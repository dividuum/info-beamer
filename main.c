#define _BSD_SOURCE
#include <linux/limits.h>
#include <strings.h>
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glfw.h>
#include <libavformat/avformat.h>
#include <event.h>
#include <lualib.h>
#include <lauxlib.h>

#include "uthash.h"
#include "tlsf.h"
#include "misc.h"
#include "kernel.h"
#include "image.h"
#include "video.h"
#include "font.h"
#include "framebuffer.h"
#include "struct.h"

#define MAX_CODE_SIZE 16384 // byte
#define MAX_LOADFILE_SIZE 16384 // byte
#define MAX_MEM 200000 // KB
#define MAX_DELTA 2000 // ms

#define MAX_RUNAWAY_TIME 1 // sec
#define MAX_PCALL_TIME  100000 // usec

#define UDP_HOST "0.0.0.0"
#define UDP_PORT 4444

#ifdef DEBUG_OPENGL
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

    UT_hash_handle by_wd;   // global handle for search by watch descriptor
    UT_hash_handle by_name; // handle for childs by name
    UT_hash_handle by_path; // handle search by path

    struct node_s *parent;
    struct node_s *childs;

    int width;
    int height;

    void *mem;
    tlsf_pool pool;
};

typedef struct node_s node_t;

static node_t *nodes_by_wd = NULL;
static node_t *nodes_by_path = NULL;

static node_t root;
static int inotify_fd;
static double now;
static int running = 1;

/*======= Lua Sandboxing =======*/

static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    // fprintf(stderr, "%d %d\n", osize, nsize);
    tlsf_pool pool = (tlsf_pool)ud;
    (void)osize;  /* not used */
    if (nsize == 0) {
        tlsf_free(pool, ptr);
        return NULL;
    } else {
        return tlsf_realloc(pool, ptr, nsize);
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

    int ret = lua_pcall(node->L, in, out, error_handler_pos);

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

// runs code in sandbox
static void node_code(node_t *node, const char *code, size_t code_size) {
    lua_pushliteral(node->L, "code");
    lua_pushlstring(node->L, code, code_size);
    lua_node_enter(node, 2);
}

// news.<callback>(args...)
static void node_callback(node_t *node, const char *name, int args) {
    lua_pushliteral(node->L, "callback"); // [args] "callback"
    lua_pushstring(node->L, name);        // [args] "callback" name
    lua_insert(node->L, -2 - args);       // name [args] "callback"
    lua_insert(node->L, -2 - args);       // "callback" name [args]
    lua_node_enter(node, 2 + args);
}

// restart sandbox
static void node_initsandbox(node_t *node) {
    lua_pushliteral(node->L, "init_sandbox");
    lua_node_enter(node, 1);
    node->width = 0;
    node->height = 0;
}

// render node
static void node_render_self(node_t *node, int width, int height) {
    lua_pushliteral(node->L, "render_self");
    lua_pushnumber(node->L, width);
    lua_pushnumber(node->L, height);
    lua_node_enter(node, 3);
}

static int node_render_to_image(lua_State *L, node_t *node) {
    if (!node->width) {
        luaL_error(L, "child not initialized with player.setup()");
        return 0;
    }

    print_render_state();
    int prev_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

    unsigned int fbo, tex;
    make_framebuffer(node->width, node->height, &tex, &fbo);
    print_render_state();

    glBindTexture(GL_TEXTURE_2D, 0);

    // Clear with transparent color
    glClearColor(1, 1, 1, 0);
    glClear(GL_COLOR_BUFFER_BIT);

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

    // glRotatef(glfwGetTime()*30, 0, 0, 1);

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
    return node_render_to_image(L, node);
}

static int luaRenderChild(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);

    node_t *child;
    HASH_FIND(by_name, node->childs, name, strlen(name), child);
    if (!child)
        luaL_error(L, "child not found");
    return node_render_to_image(L, child);
}

static int luaSendChild(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    const char *msg = luaL_checkstring(L, 2);

    node_t *child;
    HASH_FIND(by_name, node->childs, name, strlen(name), child);
    if (!child)
        luaL_error(L, "child not found");
    lua_pushstring(child->L, msg);
    node_callback(child, "on_msg", 1);
    return 0;
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

static int luaLoadFile(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    if (index(name, '/'))
        luaL_error(L, "invalid resource name");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", node->path, name);

    char data[MAX_LOADFILE_SIZE];
    int fd = open(path, O_RDONLY);
    if (fd == -1)
        return luaL_error(L, "cannot open file");

    size_t data_size = read(fd, data, sizeof(data));
    close(fd);
    lua_pushlstring(L, data, data_size);
    return 1;
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

static void node_tree_gc(node_t *node) {
    lua_gc(node->L, LUA_GCSTEP, 10);
    node_t *child, *tmp; HASH_ITER(by_name, node->childs, child, tmp) {
        node_tree_gc(child);
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
    while ((ep = readdir(dp))) {
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

    node->mem = malloc(MAX_MEM);
    node->pool = tlsf_create(node->mem, MAX_MEM);

    // link by watch descriptor & path
    HASH_ADD(by_wd, nodes_by_wd, wd, sizeof(int), node);
    HASH_ADD_KEYPTR(by_path, nodes_by_path, node->path, strlen(node->path), node);

    // create lua state
    node->L = lua_newstate(lua_alloc, node->pool);
    if (!node->L)
        die("cannot create lua");

    lua_atpanic(node->L, lua_panic);
    luaL_openlibs(node->L);
    image_register(node->L);
    video_register(node->L);
    font_register(node->L);
    luaopen_struct(node->L);

   if (luaL_loadbuffer(node->L, kernel, kernel_size, "<kernel>") != 0)
       die("kernel load");
   if (lua_pcall(node->L, 0, 0, 0) != 0)
       die("kernel run %s", lua_tostring(node->L, 1));

    lua_register_node_func(node, "setup", luaSetup);
    lua_register_node_func(node, "render_self", luaRenderSelf);
    lua_register_node_func(node, "render_child", luaRenderChild);
    lua_register_node_func(node, "send_child", luaSendChild);
    lua_register_node_func(node, "list_childs", luaListChilds);
    lua_register_node_func(node, "load_image", luaLoadImage);
    lua_register_node_func(node, "load_video", luaLoadVideo);
    lua_register_node_func(node, "load_font", luaLoadFont);
    lua_register_node_func(node, "load_file", luaLoadFile);
    lua_register(node->L, "clear", luaClear);
    lua_register(node->L, "now", luaNow);

    lua_pushstring(node->L, path);
    lua_setglobal(node->L, "PATH");

    lua_pushstring(node->L, name);
    lua_setglobal(node->L, "NAME");

    node_initsandbox(node);
    node_recursive_search(node);
    lua_gc(node->L, LUA_GCSTOP, 0);
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
    tlsf_destroy(node->pool);
    free(node->mem);
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
                running = 0;
                break;
        }
    }
}

/*===== UDP (osc) Handling ========*/

static void udp_read(int fd, short event, void *arg) {
    char buf[1500];
    int len;
    unsigned int size = sizeof(struct sockaddr);
    struct sockaddr_in client_addr;
 
    memset(buf, 0, sizeof(buf));
    len = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &size);
 
    if (len == -1) {
        die("recvfrom");
    } else {
        assert(len > 0);
        // own format:  <path>:<payload>
        int is_osc = 0;
        char payload_separator = ':';
        int initial_offset = 0;

        // If data starts with /, assume it's osc
        // format: /<path>0x00<payload>
        if (*buf == '/') {
            is_osc = 1;
            payload_separator = '\0';
            initial_offset = 1;
        };

        char *sep = memchr(buf, payload_separator, len);
        if (!sep) {
            sendto(fd, "fmt\n", 4, 0, (struct sockaddr *)&client_addr, size);
            return;
        }

        // Terminate by NUL
        *sep = '\0';

        char *path = buf + initial_offset;
        char *data = sep + 1;
        if (is_osc) {
            // round up to next multiple of 4
            data += 3 - (data - buf - 1) % 4;
        }

        int data_len = buf + len - data;
        if (data_len < 0) {
            sendto(fd, "wtf\n", 4, 0, (struct sockaddr *)&client_addr, size);
            return;
        }

        // fprintf(stderr, "%s -> %*s (%d)\n", path, data_len, data, data_len);
        
        node_t *node;
        HASH_FIND(by_path, nodes_by_path, path, strlen(path), node);
        if (!node) {
            sendto(fd, "404\n", 4, 0, (struct sockaddr *)&client_addr, size);
            return;
        }

        lua_pushlstring(node->L, data, data_len);
        lua_pushboolean(node->L, is_osc);
        node_callback(node, "on_raw_data", 2);
    }
}

static void open_udp(struct event *event) {
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

#ifdef DEBUG_PERFORMANCE
#define test(point) \
    do {\
        double next = glfwGetTime();\
        fprintf(stdout, " %7.2f", (next - now)*100000);\
        now = next;\
    } while(0);
#else
#define test(point)
#endif

static void print_free_video_mem() {
    int mem;
    glGetIntegerv(0x9049, &mem);
    fprintf(stderr, "free video mem: %d\n", mem);
}
    
static void tick() {
    double now = glfwGetTime();
    // static int loop = 1;
    // fprintf(stdout, "%d", loop++);

    check_inotify();
    test("inotify");

    event_loop(EVLOOP_NONBLOCK);
    test("io loop");


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

    test("render setup");

    node_render_self(&root, win_w, win_h);

    test("render");

    glfwSwapBuffers();
    test("swap buffer");

    glfwPollEvents();
    test("eventloop");

    node_tree_gc(&root);
    test("gc");

    test("complete");
    // fprintf(stdout, "\n");
    // node_tree_print(&root, 0);

    if (!glfwGetWindowParam(GLFW_OPENED))
        running = 0;
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
    glfwDisable(GLFW_AUTO_POLL_EVENTS);

    signal(SIGVTALRM, deadline_signal);

    node_init(&root, NULL, argv[1], argv[1]);

    double last = glfwGetTime();
    while (running) {
        now = glfwGetTime();
        int delta = (now - last) * 1000;
        last = now;
        if (delta > MAX_DELTA)
            continue;
        tick();
    }

    // no cleanup :-}
    return 0;
}
