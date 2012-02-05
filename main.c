#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>

#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glfw.h>
#include <GL/glew.h>
#include <event.h>
#include <lualib.h>
#include <lauxlib.h>

#include "uthash.h"
#include "kernel.h"
#include "image.h"
#include "video.h"
#include "font.h"

#define MAX_CODE_SIZE 16384 // byte
#define MAX_MEM 2000 // KB
#define MIN_DELTA 20 // ms
#define MAX_DELTA 2000 // ms

#define MAX_RUNAWAY_TIME 5 // sec
#define MAX_PCALL_TIME  20000000 // usec

#define UDP_HOST "0.0.0.0"
#define UDP_PORT 4444

#if 0
#define print_render_state() \
    {\
        int pd, md, fb, tex;\
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);\
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);\
        glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &pd);\
        glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &md);\
        printf("=== depth: model: %d, projection %d, fbo: %d, tex: %d\n", md, pd, fb, tex);\
    };
#else
#define print_render_state()
#endif

#define print_lua_stack(L) do { \
    int idx;\
    for (idx = 0; idx <= lua_gettop(L); idx++) {\
        printf("%3d - %s '%s'\n", idx, \
            lua_typename(L, lua_type(L, idx)),\
            lua_tostring(L, idx)\
       );\
    }\
    printf("\n");\
} while(0)

struct node_s {
    int wd; // inotify watch descriptor

    const char *name; // local node name
    const char *path; // full path (including node name)

    lua_State *L;
    int enforce_mem;

    UT_hash_handle by_wd;   // global handle for search by watch descriptor
    UT_hash_handle by_name; // handle for childs by name
    UT_hash_handle by_path; // handle search by path

    struct node_s *parent;
    struct node_s *childs;

    int width;
    int height;
};

typedef struct node_s node_t;

static node_t *nodes_by_wd = NULL;
static node_t *nodes_by_path = NULL;

static node_t root;
static int inotify_fd;
static double now;

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

/*======= Lua Sandboxing =======*/

static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
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

static int lua_panic(lua_State *L) {
    die("node panic!");
    return 0;
}

static const char *lua_safe_error_message(lua_State *L) {
    const char *message = lua_tostring(L, -1);
    if (!message)
        die("<null> error message");
    return message;
}

static void lua_node_enter(node_t *node, int args) {
    lua_State *L = node->L;
    int old_top = lua_gettop(L) - args;
    lua_gc(L, LUA_GCSTEP, 10);
    lua_pushliteral(L, "execute");              // [args] "execute"
    lua_rawget(L, LUA_REGISTRYINDEX);           // [args] execute
    lua_insert(L, -1 - args);                   // execute [args]
    lua_pushliteral(L, "traceback");            // execute [args] "traceback"
    lua_rawget(L, LUA_REGISTRYINDEX);           // execute [args] traceback
    const int error_handler_pos = lua_gettop(L) - 1 - args;
    lua_insert(L, error_handler_pos);           // traceback execute [args]
    switch (lua_timed_pcall(node, args, 0, error_handler_pos)) {
        // Erfolgreich ausgefuehrt
        case 0:                                 // traceback
            lua_remove(L, error_handler_pos);   //
            if (lua_gettop(L) != old_top)
                die("unbalanced call (success)");
            return;
        // Fehler beim Ausfuehren
        case LUA_ERRRUN:
            fprintf(stderr, "runtime error: %s\n", lua_safe_error_message(L));
            break;
        case LUA_ERRMEM:
            fprintf(stderr, "memory error: %s\n", lua_safe_error_message(L));
            break;
        case LUA_ERRERR:
            fprintf(stderr, "error handling error: %s\n", lua_safe_error_message(L));
            break;
        default:
            die("wtf?");
    };                                          // traceback "error"
    lua_pop(L, 2);                              // 
    if (lua_gettop(L) != old_top)
        die("unbalanced call");
}

/*======= Node =======*/

static void node_code(node_t *node, const char *code, size_t code_size) {
    lua_pushliteral(node->L, "code");
    lua_pushlstring(node->L, code, code_size);
    lua_node_enter(node, 2);
}

static void node_callback(node_t *node, const char *name, int args) {
    lua_pushliteral(node->L, "callback"); // [args] "callback"
    lua_pushstring(node->L, name);        // [args] "callback" name
    lua_insert(node->L, -2 - args);       // name [args] "callback"
    lua_insert(node->L, -2 - args);       // "callback" name [args]
    lua_node_enter(node, 2 + args);
}

static void node_initsandbox(node_t *node) {
    lua_pushliteral(node->L, "init_sandbox");
    lua_node_enter(node, 1);
    node->width = 0;
    node->height = 0;
}

static void node_render_self(node_t *node, int width, int height) {
    lua_pushliteral(node->L, "render_self");
    lua_pushnumber(node->L, width);
    lua_pushnumber(node->L, height);
    lua_node_enter(node, 3);
}

static int node_render_in_state(lua_State *L, node_t *node) {
    /* Neuen Framebuffer und zugehoerige Texture anlegen.
     * Dort wird dann das Child reingerendert. Das Ergebnis
     * ist ein Image. 
     *
     * Von der Performance duerfte das nicht gerade super sein,
     * da bei folgendem Code
     *
     *   gfx.render_child("child"):draw(10, 10, 300, 400)
     *
     * ein Framebuffer, eine Texture angelegt, einmalig verwendet
     * und dann wieder (durch lua gesteuert) geloescht werden.
     * Aber nunja...
     */
    if (!node->width) {
        luaL_error(L, "child not initialized with gfx.setup()");
        return 0;
    }

    print_render_state();
    int prev_fbo, fbo, tex;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, node->width, node->height, 0, GL_RGBA, GL_INT, NULL);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Render into new Texture */
    print_render_state();

    glPushAttrib(GL_ALL_ATTRIB_BITS);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glViewport(0, 0, node->width, node->height);
    glOrtho(0, node->width,
            node->height, 0,
            -1000, 1000);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glPushMatrix();

    node_callback(node, "on_render", 0);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);

    glPopAttrib();

    print_render_state();

    return image_create(L, tex, fbo, node->width, node->height);
}

static int luaRenderSelf(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    return node_render_in_state(L, node);
}

static int luaRenderChild(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);

    node_t *child;
    HASH_FIND(by_name, node->childs, name, strlen(name), child);
    if (!child)
        luaL_error(L, "child not found");
    return node_render_in_state(L, child);
}

static int luaSetup(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    int width = (int)luaL_checknumber(L, 1);
    int height = (int)luaL_checknumber(L, 2);
    if (width < 32 || width > 2048)
        luaL_error(L, "invalid width [32,2048]");
    if (height < 32 || height > 2048)
        luaL_error(L, "invalid height [32,2048]");
    node->width = width;
    node->height = height;
    return 0;
}

static int luaListChilds(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    int num_childs = HASH_CNT(by_name, node->childs);
    if (!lua_checkstack(L, num_childs))
        luaL_error(L, "too many childs");
    node_t *child, *tmp; HASH_ITER(by_name, node->childs, child, tmp) {
        lua_pushstring(L, child->name);
    }
    return num_childs;
}

static int luaLoadImage(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    if (index(name, '/'))
        luaL_error(L, "invalid resource name");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", node->path, name);
    return image_load(L, path, name);
}

static int luaLoadVideo(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    if (index(name, '/'))
        luaL_error(L, "invalid resource name");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", node->path, name);
    return video_load(L, path, name);
}

static int luaLoadFont(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    if (index(name, '/'))
        luaL_error(L, "invalid resource name");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", node->path, name);
    return font_new(L, path, name);
}

static int luaClear(lua_State *L) {
    GLdouble r = luaL_checknumber(L, 1);
    GLdouble g = luaL_checknumber(L, 2);
    GLdouble b = luaL_checknumber(L, 3);
    GLdouble a = luaL_checknumber(L, 4);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    return 0;
}

static int luaNow(lua_State *L) {
    lua_pushnumber(L, now);
    return 1;
}

static void node_init(node_t *node, node_t *parent, const char *path, const char *name);
static void node_free(node_t *node);

#define lua_register_node_func(node,name,func) \
    (lua_pushliteral((node)->L, name), \
     lua_pushlightuserdata((node)->L, node), \
     lua_pushcclosure((node)->L, func, 1), \
     lua_settable((node)->L, LUA_GLOBALSINDEX))


static void node_tree_print(node_t *node, int depth) {
    fprintf(stderr, "%4d %*s'- %s (%d)\n", lua_gc(node->L, LUA_GCCOUNT, 0), 
        depth*2, "", node->name, HASH_CNT(by_name, node->childs));
    node_t *child, *tmp; HASH_ITER(by_name, node->childs, child, tmp) {
        node_tree_print(child, depth+1);
    };
}


static void node_add_child(node_t* node, const char *path, const char *name) {
    node_t *child = xmalloc(sizeof(node_t));
    node_init(child, node, path, name);
    HASH_ADD_KEYPTR(by_name, node->childs, child->name, strlen(child->name), child);
}

static void node_remove_child(node_t* node, node_t* child) {
    HASH_DELETE(by_name, node->childs, child);
    node_free(child);
    free(child);
}

static void node_remove_child_by_name(node_t* node, const char *name) {
    node_t *child;
    HASH_FIND(by_name, node->childs, name, strlen(name), child);
    if (!child)
        die("child not found: %s", name);
    node_remove_child(node, child); 
}

static void node_update_content(node_t *node, const char *path, const char *name) {
    fprintf(stderr, ">>> content add %s in %s\n", name, node->path);
    if (strcmp(name, "script.lua") == 0) {
        node_initsandbox(node);

        char code[MAX_CODE_SIZE];
        int fd = open(path, O_RDONLY);
        if (fd == -1) {
            fprintf(stderr, "cannot open file for reading: %s\n", path);
            return;
        }

        size_t code_size = read(fd, code, sizeof(code));
        close(fd);

        node_code(node, code, code_size);
    } else {
        lua_pushstring(node->L, name);
        node_callback(node, "on_content_update", 1);
    }
}

static void node_remove(node_t *node, const char *name) {
    fprintf(stderr, "<<< content del %s in %s\n", name, node->path);
    if (strcmp(name, "script.lua") == 0) {
        node_initsandbox(node);
    } else {
        lua_pushstring(node->L, name);
        node_callback(node, "on_content_update", 1);
    }
}

static void node_recursive_search(node_t *node) {
    // search for existing script.lua
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/script.lua", node->path);

    struct stat stat_buf;
    if (stat(path, &stat_buf) != -1 && 
            S_ISREG(stat_buf.st_mode)) {
        node_update_content(node, path, "script.lua");
    }

    // recursivly add remaining files (except script.lua) and 
    // directories
    DIR *dp = opendir(node->path);
    if (!dp)
        die("cannot open directory");
    struct dirent *ep;
    while (ep = readdir(dp)) {
        if (ep->d_name[0] == '.') 
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", node->path, ep->d_name);

        if (ep->d_type == DT_DIR) {
            node_add_child(node, path, ep->d_name);
        } else if (ep->d_type == DT_REG &&
                strcmp(ep->d_name, "script.lua") != 0) {
            node_update_content(node, path, ep->d_name);
        }
    }
    closedir(dp);
}

static void node_init(node_t *node, node_t *parent, const char *path, const char *name) {
    fprintf(stderr, ">>> node add %s in %s\n", name, path);

    // add directory watcher
    node->wd = inotify_add_watch(inotify_fd, path, 
        IN_CLOSE_WRITE|IN_CREATE|IN_DELETE|IN_DELETE_SELF|
        IN_MOVE);
    if (node->wd == -1)
        die("cannot inotify_add_watch on %s", path);

    // init node structure
    node->parent = parent;
    node->path = strdup(path);
    node->name = strdup(name);
    node->enforce_mem = 0;

    // link by watch descriptor & path
    HASH_ADD(by_wd, nodes_by_wd, wd, sizeof(int), node);
    HASH_ADD_KEYPTR(by_path, nodes_by_path, node->path, strlen(node->path), node);

    // create lua state
    node->L = lua_newstate(lua_alloc, node);
    if (!node->L)
        die("cannot create lua");

    lua_atpanic(node->L, lua_panic);
    luaL_openlibs(node->L);
    image_register(node->L);
    video_register(node->L);
    font_register(node->L);

   if (luaL_loadbuffer(node->L, kernel, kernel_size, "<kernel>") != 0)
       die("kernel load");
   if (lua_pcall(node->L, 0, 0, 0) != 0)
       die("kernel run %s", lua_tostring(node->L, 1));

    lua_register_node_func(node, "setup", luaSetup);
    lua_register_node_func(node, "render_self", luaRenderSelf);
    lua_register_node_func(node, "render_child", luaRenderChild);
    lua_register_node_func(node, "list_childs", luaListChilds);
    lua_register_node_func(node, "load_image", luaLoadImage);
    lua_register_node_func(node, "load_video", luaLoadVideo);
    lua_register_node_func(node, "load_font", luaLoadFont);
    lua_register(node->L, "clear", luaClear);
    lua_register(node->L, "now", luaNow);

    lua_pushstring(node->L, path);
    lua_setglobal(node->L, "PATH");

    lua_pushstring(node->L, name);
    lua_setglobal(node->L, "NAME");

    node_initsandbox(node);

    node_recursive_search(node);
}

static void node_free(node_t *node) {
    fprintf(stderr, "<<< node del %s in %s\n", node->name, node->path);
    node_t *child, *tmp; HASH_ITER(by_name, node->childs, child, tmp) {
        node_remove_child(node, child);
    }
    HASH_DELETE(by_wd, nodes_by_wd, node);
    HASH_DELETE(by_path, nodes_by_path, node);
    free((void*)node->path);
    free((void*)node->name);
    // inotify_rm_watch(inotify_fd, node->wd));
    lua_close(node->L);
}

/*======= inotify ==========*/

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

            // printf("%s %08x %d\n", event->name, event->mask, event->wd);

            // ignore dot-files (including parent and current directory)
            if (event->len && event->name[0] == '.')
                continue; // ignore dot files

            // notifies, that wd was removed from kernel.
            // can be ignored (since it is handled in 
            // IN_DELETE_SELF).
            if (event->mask & IN_IGNORED)
                continue;

            node_t *node;
            HASH_FIND(by_wd, nodes_by_wd, &event->wd, sizeof(int), node);
            if (!node) 
                die("node not found: %s", event->name);

            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", node->path, event->name);

            if (event->mask & IN_CREATE) {
                struct stat stat_buf;
                if (stat(path, &stat_buf) == -1) {
                    // file/path can be gone (race between inotify and 
                    // user actions)
                    fprintf(stderr, "cannot stat %s\n", path);
                    continue;
                }

                if (S_ISDIR(stat_buf.st_mode))
                    node_add_child(node, path, event->name);
            } else if (event->mask & IN_CLOSE_WRITE) {
                node_update_content(node, path, event->name);
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
                    node_add_child(node, path, event->name);
                } else {
                    node_update_content(node, path, event->name);
                }
            }
        }
    }
}

/*============ GUI ===========*/

static int win_w, win_h;

static void GLFWCALL reshape(int width, int height) {
    win_w = width;
    win_h = height;
}

static void GLFWCALL keypressed(int key, int action) {
    if (action == GLFW_PRESS) {
        switch (key) {
            case GLFW_KEY_ESC:
                die("exit!");
                break;
        }
    }
}

void udp_read(int fd, short event, void *arg) {
    char buf[1500];
    int len;
    int size = sizeof(struct sockaddr);
    struct sockaddr_in client_addr;
 
    memset(buf, 0, sizeof(buf));
    len = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &size);
 
    if (len == -1) {
        die("recvfrom");
    } else {
        char *sep = memchr(buf, ':', len);
        if (!sep) {
            sendto(fd, "fmt\n", 4, 0, (struct sockaddr *)&client_addr, size);
            return;
        }
        *sep = '\0';
        char *path = buf;
        char *data = sep + 1;
        int data_len = len - (data - path);
        fprintf(stderr, "%s -> %*s (%d)\n", path, data_len, data, data_len);
        
        node_t *node;
        HASH_FIND(by_path, nodes_by_path, path, strlen(path), node);
        if (!node) {
            sendto(fd, "404\n", 4, 0, (struct sockaddr *)&client_addr, size);
            return;
        }

        lua_pushlstring(node->L, data, data_len);
        node_callback(node, "on_data", 1);
    }
}

void open_udp(struct event *event) {
    int sock_fd;
    int one = 1;
    struct sockaddr_in sin;
 
    if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        die("socket");
 
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0)
        die("setsockopt reuse");
 
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(UDP_HOST);
    sin.sin_port = htons(UDP_PORT);
 
    if (bind(sock_fd, (struct sockaddr *)&sin, sizeof(struct sockaddr)) < 0)
        die("bind");
 
    event_set(event, sock_fd, EV_READ | EV_PERSIST, &udp_read, NULL);
    if (event_add(event, NULL) == -1)
        die("event_add failed");
}
    
static void tick() {
    check_inotify();
    event_loop(EVLOOP_NONBLOCK);
    glfwPollEvents();
    // node_tree_print(&root, 0);

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glViewport(0, 0, win_w, win_h);
    glOrtho(0, win_w,
            win_h, 0,
            -1000, 1000);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(0.05, 0.05, 0.05, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    node_render_self(&root, win_w, win_h);

    glfwSwapBuffers();

    if (!glfwGetWindowParam(GLFW_OPENED))
        die("window closed");
}

int main(int argc, char *argv[]) {
    if (argc != 2)
        die("%s <root_name>", argv[0]);

    char *root_name = argv[1];
    if (index(root_name, '/'))
        die("<root_name> cannot contain /");

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd == -1)
        die("cannot open inotify");

    av_register_all();

    event_init();

    struct event udp_event;
    open_udp(&udp_event);

    glfwInit();
    // glfwSetTime(time(0));
    glfwOpenWindowHint(GLFW_FSAA_SAMPLES, 4);
    int mode = getenv("FULLSCREEN") ? GLFW_FULLSCREEN : GLFW_WINDOW;

    if(!glfwOpenWindow(1024, 768, 8,8,8,8, 0,0, mode))
        die("cannot open window");

    GLenum err = glewInit();
    if (err != GLEW_OK)
        die("cannot initialize glew");

    glfwSetWindowTitle("GPN Info");
    glfwSwapInterval(1);
    glfwSetWindowSizeCallback(reshape);
    glfwSetKeyCallback(keypressed);

    signal(SIGVTALRM, deadline_signal);

    node_init(&root, NULL, argv[1], argv[1]);

    double last = glfwGetTime();
    while (1) {
        now = glfwGetTime();
        int delta = (now - last) * 1000;
        if (delta < MIN_DELTA) {
            glfwSleep((MIN_DELTA-delta)/1000.0);
            continue;
        }
        last = now;
        if (delta > MAX_DELTA)
            continue;
        tick();
    }
    return 0;
}
