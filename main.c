#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>

#include <GL/glfw.h>
#include <event.h>
#include <lualib.h>
#include <lauxlib.h>

#include "uthash.h"
#include "utlist.h"
#include "kernel.h"

#define MAX_CODE_SIZE 16384 // byte
#define MAX_MEM 2000 // KB
#define MIN_DELTA 500 // ms
#define MAX_DELTA 2000 // ms
#define MAX_RUNAWAY_TIME 1 // sec
#define MAX_PCALL_TIME  20000 // usec


struct node_s {
    int wd; // inotify watch descriptor
    int prio;
    const char *name;
    const char *path;
    lua_State *L;
    int enforce_mem;
    UT_hash_handle by_wd;
    struct node_s *parent;
    struct node_s *childs;

    struct node_s *prev, *next; // same level
};

typedef struct node_s node_t;

static node_t *nodes = NULL;

static node_t root;
static int inotify_fd;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("CRITICAL ERROR: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t size) {
    void *ptr = calloc(1, size);
    if (!ptr) die("cannot malloc");
    return ptr;
}

static void node_init(node_t *node, node_t *parent, const char *path, const char *name);
static void node_free(node_t *node);

static void *node_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    node_t *node = ud;
    (void)osize;  /* not used */
    if (nsize == 0) {
        free(ptr);
        return NULL;
    } else {
        if (node->enforce_mem && lua_gc(node->L, LUA_GCCOUNT, 0) > MAX_MEM)
            return NULL;
        return realloc(ptr, nsize);
    }
}

/* Zeitbegrenzung fuer pcalls */
static node_t *global_node = NULL;
static int timers_expired = 0;

static void deadline_stop(lua_State *L, lua_Debug *ar) {
    lua_sethook(L, NULL, 0, 0);
    lua_pushliteral(L, "alarm");
    lua_gettable(L, LUA_REGISTRYINDEX);
    lua_call(L, 0, 0);
}

static void deadline_signal(int i) {
    if (!global_node)
        die("urg. timer expired and no global_node");

    if (timers_expired == 0) {
        // Beim ersten expiren wird versucht das Problem
        // innerhalb von Lua zu loesen.
        lua_sethook(global_node->L, deadline_stop,
            LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
    } else {
        // Lua wollte sich nicht beenden. Hier
        // kann nichts anderes mehr gemacht werden.
        die("unstoppable runaway code in %s", global_node->name);
    }
    timers_expired++;
}

static int lua_timed_pcall(node_t *node, int in, int out,
        int error_handler_pos)
{
    struct itimerval deadline;
    deadline.it_interval.tv_sec = MAX_RUNAWAY_TIME;
    deadline.it_interval.tv_usec = 0;
    deadline.it_value.tv_sec = 0;
    deadline.it_value.tv_usec = MAX_PCALL_TIME;
    setitimer(ITIMER_VIRTUAL, &deadline, NULL);

    global_node = node;
    timers_expired = 0;

    node->enforce_mem = 1;
    int ret = lua_pcall(node->L, in, out, error_handler_pos);
    node->enforce_mem = 0;

    deadline.it_value.tv_usec = 0;
    setitimer(ITIMER_VIRTUAL, &deadline, NULL);
    global_node = NULL;
    return ret;
}

static int node_panic(lua_State *L) {
    die("node panic!");
    return 0;
}

#define lua_register_node_func(node,name,func) \
    (lua_pushliteral((node)->L, name), \
     lua_pushlightuserdata((node)->L, node), \
     lua_pushcclosure((node)->L, func, 1), \
     lua_settable((node)->L, LUA_GLOBALSINDEX))

static const char *node_safe_error_message(lua_State *L) {
    const char *message = lua_tostring(L, -1);
    if (!message)
        die("<null> error message");
    return message;
}

static void node_execute(node_t *node, const char *code, size_t code_size) {
    lua_State *L = node->L;
    lua_gc(L, LUA_GCSTEP, 100);
    lua_pushliteral(L, "execute");              // "fun"
    lua_rawget(L, LUA_REGISTRYINDEX);           // fun
    lua_pushlstring(L, code, code_size);        // fun <arg>
    lua_pushliteral(L, "traceback");            // fun <arg> "traceback"
    lua_rawget(L, LUA_REGISTRYINDEX);           // fun <arg> traceback
    const int error_handler_pos = lua_gettop(L) - 2;
    lua_insert(L, error_handler_pos);           // traceback fun <arg>
    switch (lua_timed_pcall(node, 1, 0, error_handler_pos)) {
        // Erfolgreich ausgefuehrt
        case 0:                                 // traceback
            lua_remove(L, error_handler_pos);   //
            if (lua_gettop(L) != 0)
                die("top is not zero");
            return;
        // Fehler beim Ausfuehren
        case LUA_ERRRUN:
            fprintf(stderr, "runtime error: %s\n", node_safe_error_message(L));
            break;
        case LUA_ERRMEM:
            fprintf(stderr, "memory error: %s\n", node_safe_error_message(L));
            break;
        case LUA_ERRERR:
            fprintf(stderr, "error handling error: %s\n", node_safe_error_message(L));
            break;
        default:
            die("wtf?");
    };                                          // traceback "error"
    lua_pop(L, 2);                              // 
}

static void node_init_sandbox(node_t *node) {
    node_execute(node, "init_sandbox", 12);
}

static void node_tree_tick(node_t *node, int delta) {
    char code[128];
    size_t code_size = snprintf(code, sizeof(code), "news.on_tick(\"%d\")", delta);
    node_execute(node, code, code_size);

    node_t *child; DL_FOREACH(node->childs, child) {
        node_tree_tick(child, delta);
    };
}

static void node_tree_print(node_t *node, int depth) {
    fprintf(stderr, "%4d %*s'- %s\n", lua_gc(node->L, LUA_GCCOUNT, 0), depth*2, "", node->name);

    node_t *child; DL_FOREACH(node->childs, child) {
        node_tree_print(child, depth+1);
    };
}


static void node_add_child(node_t* node, const char *path, const char *name) {
    node_t *child = xmalloc(sizeof(node_t));
    node_init(child, node, path, name);

    DL_APPEND(node->childs, child);
}

static void node_remove_child(node_t* node, node_t* child) {
    node_free(child);
    DL_DELETE(node->childs, child);
    free(child);
}

static void node_remove_child_by_name(node_t* node, const char *name) {
    int search(node_t *a, node_t *b) {
        return strcmp(a->name, b->name);
    }
    node_t needle = {.name = name};
    node_t *child;
    DL_SEARCH(node->childs, child, &needle, search);
    if (!child)
        die("child not found: %s", name);
    node_remove_child(node, child); 
}

static void node_update_content(node_t *node, const char *filename) {
    fprintf(stderr, ">>> content add %s in %s\n", filename, node->path);
    if (strcmp(filename, "script.lua") == 0) {
        node_init_sandbox(node);

        char fullname[PATH_MAX];
        snprintf(fullname, sizeof(fullname), "%s/%s", node->path, filename);

        char code[MAX_CODE_SIZE];
        int fd = open(fullname, O_RDONLY);
        if (fd == -1) {
            fprintf(stderr, "cannot open file for reading: %s\n", filename);
            return;
        }

        size_t code_size = read(fd, code, sizeof(code));
        close(fd);

        node_execute(node, code, code_size);
    } else {
        char code[MAX_CODE_SIZE];
        size_t code_size = snprintf(code, sizeof(code), "news.on_content_update(\"%s\")", filename);
        node_execute(node, code, code_size);
    }
}

static void node_remove(node_t *node, const char *filename) {
    fprintf(stderr, "<<< content del %s in %s\n", filename, node->path);
    if (strcmp(filename, "script.lua") == 0) {
        node_init_sandbox(node);
    } else {
        char code[MAX_CODE_SIZE];
        size_t code_size = snprintf(code, sizeof(code), "news.on_content_update(\"%s\")", filename);
        node_execute(node, code, code_size);
    }
}

static void node_init(node_t *node, node_t *parent, const char *path, const char *name) {
    fprintf(stderr, ">>> node add %s in %s\n", name, path);

    node->wd = inotify_add_watch(inotify_fd, path, 
        IN_CLOSE_WRITE|IN_CREATE|IN_DELETE|IN_DELETE_SELF|
        IN_MOVE);
    if (node->wd == -1)
        die("cannot inotify_add_watch on %s", path);
    HASH_ADD(by_wd, nodes, wd, sizeof(int), node);

    node->parent = parent;
    node->path = strdup(path);
    node->name = strdup(name);
    node->enforce_mem = 0;
    node->L = lua_newstate(node_alloc, node);
    lua_atpanic(node->L, node_panic);

    if (!node->L)
        die("cannot create lua");

    luaL_openlibs(node->L);

   if (luaL_loadbuffer(node->L, kernel, kernel_size, "<kernel>") != 0)
       die("kernel load");
   if (lua_pcall(node->L, 0, 0, 0) != 0)
       die("kernel run %s", lua_tostring(node->L, 1));

    node_init_sandbox(node);

    // search for existing script.lua
    char script_path[PATH_MAX];
    snprintf(script_path, sizeof(script_path), "%s/script.lua", node->path);

    struct stat stat_buf;
    if (stat(script_path, &stat_buf) != -1 && 
            S_ISREG(stat_buf.st_mode)) {
        node_update_content(node, "script.lua");
    }

    // recursivly add remaining files / directories
    DIR *dp = opendir(path);
    if (!dp)
        die("cannot open directory");
    struct dirent *ep;
    while (ep = readdir(dp)) {
        if (ep->d_name[0] == '.') 
            continue;

        if (ep->d_type == DT_DIR) {
            char child_path[PATH_MAX];
            snprintf(child_path, sizeof(child_path), "%s/%s", node->path, ep->d_name);
            node_add_child(node, child_path, ep->d_name);
        } else if (ep->d_type == DT_REG &&
                strcmp(ep->d_name, "script.lua") != 0) {
            node_update_content(node, ep->d_name);
        }
    }
    closedir(dp);
}

static void node_free(node_t *node) {
    fprintf(stderr, "<<< node del %s in %s\n", node->name, node->path);
    node_t *child; DL_FOREACH(node->childs, child) {
        node_remove_child(node, child);
    }
    free((void*)node->path);
    free((void*)node->name);
    inotify_rm_watch(inotify_fd, node->wd);
    lua_close(node->L);
    HASH_DELETE(by_wd, nodes, node);
}

static void check_inotify() {
    static char inotify_buffer[sizeof(struct inotify_event) + PATH_MAX + 1];
    while (1) {
        size_t size = read(inotify_fd, &inotify_buffer, sizeof(inotify_buffer));
        if (size == -1) {
            if (errno == EAGAIN)
                break;
            die("error reading from inotify fd");
        }

        char *pos = inotify_buffer;
        char *end = pos + size;
        while (pos < end) {
            struct inotify_event *event = (struct inotify_event*)pos;
            pos += sizeof(struct inotify_event) + event->len;

            // printf("%s %08x\n", event->name, event->mask);

            if (event->len && event->name[0] == '.')
                continue; // ignore dot files

            if (event->mask & IN_IGNORED)
                continue;

            node_t *node;
            HASH_FIND(by_wd, nodes, &event->wd, sizeof(int), node);
            if (!node) 
                die("node not found: %s", event->name);

            char child_path[PATH_MAX];
            snprintf(child_path, sizeof(child_path), "%s/%s", node->path, event->name);

            if (event->mask & IN_CREATE) {
                struct stat stat_buf;
                if (stat(child_path, &stat_buf) == -1) {
                    fprintf(stderr, "cannot stat %s\n", child_path);
                    continue;
                }

                if (S_ISDIR(stat_buf.st_mode))
                    node_add_child(node, child_path, event->name);
            } else if (event->mask & IN_CLOSE_WRITE) {
                node_update_content(node, event->name);
            } else if (event->mask & IN_DELETE_SELF) {
                if (!node->parent)
                    die("data deleted. cannot continue");
                node_remove_child(node->parent, node);
            } else if (event->mask & IN_DELETE && !(event->mask & IN_ISDIR)) {
                node_remove(node, event->name);
            } else if (event->mask & IN_MOVED_FROM) {
                if (event->mask & IN_ISDIR) {
                    node_remove_child_by_name(node, event->name);
                } else {
                    node_remove(node, event->name);
                }
            } else if (event->mask & IN_MOVED_TO) {
                if (event->mask & IN_ISDIR) {
                    node_add_child(node, child_path, event->name);
                } else {
                    node_update_content(node, event->name);
                }
            }
        }
    }
}
    
static void tick(int delta) {
    check_inotify();
    event_loop(EVLOOP_NONBLOCK);
    node_tree_tick(&root, delta);
    node_tree_print(&root, 0);
}

int main(int argc, char *argv[]) {
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd == -1)
        die("cannot open inotify");


    event_init();
    glfwInit();

    signal(SIGVTALRM, deadline_signal);

    node_init(&root, NULL, "data", "data");

    double last = glfwGetTime();
    while (1) {
        double current = glfwGetTime();
        int delta = (current - last) * 1000;
        if (delta < MIN_DELTA) {
            glfwSleep((MIN_DELTA-delta)/1000.0);
            continue;
        }
        last = current;
        if (delta > MAX_DELTA)
            continue;
        tick(delta);
    }
    return 0;
}
