/* See Copyright Notice in LICENSE.txt */

#define _BSD_SOURCE
#define _GNU_SOURCE
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
#include "utlist.h"
#include "tlsf.h"
#include "misc.h"
#include "image.h"
#include "video.h"
#include "font.h"
#include "shader.h"
#include "framebuffer.h"
#include "struct.h"

#include "kernel.h"
#include "userlib.h"

#define VERSION_STRING "Info Beamer " VERSION
#define INFO_URL "http://dividuum.de/info-beamer"

#define MAX_CODE_SIZE 16384 // byte
#define MAX_LOADFILE_SIZE 16384 // byte
#define MAX_MEM 2000000 // KB
#define MAX_GL_PUSH 20 // glPushMatrix depth

// Default host/port (both udp & tcp)
#define HOST "0.0.0.0"
#define PORT 4444

#ifdef DEBUG
#define MAX_RUNAWAY_TIME 10 // sec
#define MAX_PCALL_TIME  100000000 // usec
#else
#define MAX_RUNAWAY_TIME 1 // sec
#define MAX_PCALL_TIME  100000 // usec
#endif

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

#define NO_GL_PUSHPOP -1
#define LITERAL_SIZE(x) (sizeof(x) - 1)

typedef struct node_s {
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

    int gl_matrix_depth;

    void *mem;
    tlsf_pool pool;

    struct client_s *clients;
} node_t;

static node_t *nodes_by_wd = NULL;
static node_t *nodes_by_path = NULL;
static node_t root;

typedef struct client_s {
    int fd;
    node_t *node;
    struct bufferevent *buf_ev;

    struct client_s *next;
    struct client_s *prev;
} client_t;

static int inotify_fd;
static double now;
static int running = 1;


/*=== Forward declarations =====*/

static void client_write(client_t *client, const char *data, size_t data_size);
static void client_close(client_t *client);
static void node_printf(node_t *node, const char *fmt, ...);
static void node_init(node_t *node, node_t *parent, const char *path, const char *name);
static void node_free(node_t *node);

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
        die("unstoppable runaway code in %s", global_node->path);
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
            assert(lua_gettop(L) == old_top);
            return;
        // Fehler beim Ausfuehren
        case LUA_ERRRUN:
            node_printf(node, "runtime error: %s\n", lua_safe_error_message(L));
            break;
        case LUA_ERRMEM:
            node_printf(node, "memory error: %s\n", lua_safe_error_message(L));
            break;
        case LUA_ERRERR:
            node_printf(node, "error handling error: %s\n", lua_safe_error_message(L));
            break;
        default:
            die("wtf?");
    };                                          // traceback "error"
    lua_pop(L, 2);                              // 
    assert(lua_gettop(L) == old_top);
}

/*======= Node =======*/

// reinit sandbox, load usercode and user code
static void node_boot(node_t *node) {
    lua_pushliteral(node->L, "boot");
    lua_node_enter(node, 1);
}

// notify of content update 
static void node_update_content(node_t *node, const char *name, int added) {
    lua_pushliteral(node->L, "update_content");
    lua_pushstring(node->L, name);
    lua_pushboolean(node->L, added);
    lua_node_enter(node, 3);
}

// event.<event_name>(args...)
static void node_event(node_t *node, const char *name, int args) {
    lua_pushliteral(node->L, "event"); // [args] "event_name"
    lua_pushstring(node->L, name);     // [args] "event_name" name
    lua_insert(node->L, -2 - args);    // name [args] "event_name"
    lua_insert(node->L, -2 - args);    // "event_name" name [args]
    lua_node_enter(node, 2 + args);
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
        luaL_error(L, "node not initialized with gl.setup()");
        return 0;
    }
    // fprintf(stderr, "rendering %s\n", node->path);

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

    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushMatrix();

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glViewport(0, 0, node->width, node->height);
    glOrtho(0, node->width,
            node->height, 0,
            -1000, 1000);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    node->gl_matrix_depth = 0;
    node_event(node, "render", 0);
    while (node->gl_matrix_depth > 0) {
        glPopMatrix();
        node->gl_matrix_depth--;
    }
    node->gl_matrix_depth = NO_GL_PUSHPOP;

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);

    glPopAttrib();
    glPopClientAttrib();

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
    node_event(child, "msg", 1);
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
    node_t *child, *tmp; 
    HASH_ITER(by_name, node->childs, child, tmp) {
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
        return luaL_error(L, "cannot open file '%s'", path);

    size_t data_size = read(fd, data, sizeof(data));
    close(fd);
    lua_pushlstring(L, data, data_size);
    return 1;
}

static int luaCreateShader(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    if (node->parent)
        luaL_error(L, "shader only allowed in toplevel node");
    const char *vertex = luaL_checkstring(L, 1);
    const char *fragment = luaL_checkstring(L, 2);
    return shader_new(L, vertex, fragment);
}

static int luaPrint(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int n = lua_gettop(L);
    lua_getglobal(L, "tostring");
    for (int i = 1; i <= n; i++) {
        lua_pushvalue(L, n + 1); 
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        if (!lua_isstring(L, -1))
            return luaL_error(L, "tostring must return a string to print");
        if (i > 1)
            luaL_addchar(&b, '\t');
        luaL_addvalue(&b);
    }
    luaL_addchar(&b, '\n');
    luaL_pushresult(&b);
    node_printf(node, "%s", lua_tostring(L, -1));
    return 0;
}

static int luaGlPushMatrix(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    if (node->gl_matrix_depth == NO_GL_PUSHPOP)
        return luaL_error(L, "only callable in event.render");
    if (node->gl_matrix_depth > MAX_GL_PUSH)
        return luaL_error(L, "Too may pushes");
    glPushMatrix();
    node->gl_matrix_depth++;
    return 0;
}

static int luaGlPopMatrix(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    if (node->gl_matrix_depth == NO_GL_PUSHPOP)
        return luaL_error(L, "only callable in event.render");
    if (node->gl_matrix_depth == 0)
        return luaL_error(L, "Nothing to pop");
    glPopMatrix();
    node->gl_matrix_depth--;
    return 0;
}

static int luaGlRotate(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    if (node->gl_matrix_depth == NO_GL_PUSHPOP)
        return luaL_error(L, "only callable in event.render");
    double angle = luaL_checknumber(L, 1);
    double x = luaL_checknumber(L, 2);
    double y = luaL_checknumber(L, 3);
    double z = luaL_checknumber(L, 4);
    glRotated(angle, x, y, z);
    return 0;
}

static int luaGlTranslate(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    if (node->gl_matrix_depth == NO_GL_PUSHPOP)
        return luaL_error(L, "only callable in event.render");
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double z = luaL_optnumber(L, 3, 0.0);
    glTranslated(x, y, z);
    return 0;
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

#define lua_register_node_func(node,name,func) \
    (lua_pushliteral((node)->L, name), \
     lua_pushlightuserdata((node)->L, node), \
     lua_pushcclosure((node)->L, func, 1), \
     lua_settable((node)->L, LUA_GLOBALSINDEX))

static void node_printf(node_t *node, const char *fmt, ...) {
    char buffer[16384];
    va_list ap;
    va_start(ap, fmt);
    size_t buffer_size = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s: %s", node->path, buffer);
    client_t *client;
    DL_FOREACH(node->clients, client) {
        client_write(client, buffer, buffer_size);
    }
}

static void node_tree_print(node_t *node, int depth) {
    fprintf(stderr, "%4d %*s'- %s (%d) %p\n", lua_gc(node->L, LUA_GCCOUNT, 0), 
        depth*2, "", node->name, HASH_CNT(by_name, node->childs), node->clients);
    node_t *child, *tmp; 
    HASH_ITER(by_name, node->childs, child, tmp) {
        node_tree_print(child, depth+1);
    };
}

static void node_tree_gc(node_t *node) {
    lua_gc(node->L, LUA_GCSTEP, 100);
    node_t *child, *tmp; 
    HASH_ITER(by_name, node->childs, child, tmp) {
        node_tree_gc(child);
    };
}

static node_t *node_add_child(node_t* node, const char *path, const char *name) {
    node_t *child = xmalloc(sizeof(node_t));
    node_init(child, node, path, name);
    HASH_ADD_KEYPTR(by_name, node->childs, child->name, strlen(child->name), child);
    return child;
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

    node->gl_matrix_depth = NO_GL_PUSHPOP;

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
    shader_register(node->L);
    luaopen_struct(node->L);

   if (luaL_loadbuffer(node->L, kernel, kernel_size, "kernel.lua") != 0)
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
    lua_register_node_func(node, "create_shader", luaCreateShader);
    lua_register_node_func(node, "print", luaPrint);
    lua_register_node_func(node, "glPushMatrix", luaGlPushMatrix);
    lua_register_node_func(node, "glPopMatrix", luaGlPopMatrix);
    lua_register_node_func(node, "glRotate", luaGlRotate);
    lua_register_node_func(node, "glTranslate", luaGlTranslate);
    lua_register(node->L, "clear", luaClear);
    lua_register(node->L, "now", luaNow);

    lua_pushstring(node->L, path);
    lua_setglobal(node->L, "PATH");

    lua_pushstring(node->L, name);
    lua_setglobal(node->L, "NAME");

    lua_pushlstring(node->L, userlib, userlib_size);
    lua_setglobal(node->L, "USERLIB");
}

static void node_free(node_t *node) {
    fprintf(stderr, "<<< node del %s in %s\n", node->name, node->path);
    node_t *child, *tmp; 
    HASH_ITER(by_name, node->childs, child, tmp) {
        node_remove_child(node, child);
    }
    HASH_DELETE(by_wd, nodes_by_wd, node);
    HASH_DELETE(by_path, nodes_by_path, node);
    free((void*)node->path);
    free((void*)node->name);

    client_t *client, *tmp_client;
    DL_FOREACH_SAFE(node->clients, client, tmp_client) {
        client_close(client);
    }
    assert(node->clients == NULL);

    // inotify_rm_watch(inotify_fd, node->wd));
    lua_close(node->L);

    tlsf_destroy(node->pool);
    free(node->mem);
}


static void node_init_all(node_t *root, const char *base_path) {
    void init_recursive(node_t *node, node_t *parent, const char *path) {
        DIR *dp = opendir(node->path);
        if (!dp)
            die("cannot open directory %s", node->path);

        struct dirent *ep;
        while ((ep = readdir(dp))) {
            if (ep->d_name[0] == '.') 
                continue;

            const char *child_name = ep->d_name;
            char child_path[PATH_MAX];
            snprintf(child_path, sizeof(child_path), "%s/%s", path, child_name);

            if (ep->d_type == DT_DIR) {
                node_t *child = node_add_child(node, child_path, child_name);
                init_recursive(child, node, child_path);
            }
        }
        closedir(dp);

        node_boot(node);
    }

    node_init(root, NULL, base_path, base_path);
    init_recursive(root, NULL, base_path);
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

                if (S_ISDIR(stat_buf.st_mode)) {
                    node_t *child = node_add_child(node, path, event->name);
                    node_boot(child);
                }
            } else if (event->mask & IN_CLOSE_WRITE) {
                node_update_content(node, event->name, 1);
            } else if (event->mask & IN_DELETE_SELF) {
                if (!node->parent)
                    die("root node deleted. cannot continue");
                node_remove_child(node->parent, node);
            } else if (event->mask & IN_DELETE && !(event->mask & IN_ISDIR)) {
                node_update_content(node, event->name, 0);
            } else if (event->mask & IN_MOVED_FROM) {
                if (event->mask & IN_ISDIR) {
                    node_remove_child_by_name(node, event->name);
                } else {
                    node_update_content(node, event->name, 0);
                }
            } else if (event->mask & IN_MOVED_TO) {
                if (event->mask & IN_ISDIR) {
                    node_t *child = node_add_child(node, path, event->name);
                    node_boot(child);
                } else {
                    node_update_content(node, event->name, 1);
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

int create_socket(int type) {
    int one = 1;
    struct sockaddr_in sin;
    int fd = socket(AF_INET, type, 0);

    if (fd < 0)
        die("socket");

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0)
        die("setsockopt reuse");

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(HOST);
    sin.sin_port = htons(PORT);

    if (bind(fd, (struct sockaddr *)&sin, sizeof(struct sockaddr)) < 0)
        die("bind");

    return fd;
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

        // split a/b/c into first matching prefix:
        // a/b -> suffix: c if node a/b exists

        char *suffix = sep;
        node_t *node;
        while (1) {
            HASH_FIND(by_path, nodes_by_path, path, strlen(path), node);
            if (node)
                break;

            char *next_split = memrchr(path, '/', suffix - path);
            if (!next_split) {
                sendto(fd, "404\n", 4, 0, (struct sockaddr *)&client_addr, size);
                return;
            }
            if (suffix != sep)
                *suffix = '/';
            suffix = next_split;
            *next_split = '\0';
        }
        if (suffix != sep)
            suffix++;

        lua_pushlstring(node->L, data, data_len);
        lua_pushboolean(node->L, is_osc);
        lua_pushstring(node->L, suffix);
        node_event(node, "raw_data", 3);
    }
}

static void open_udp(struct event *event) {
    int fd = create_socket(SOCK_DGRAM); 
    event_set(event, fd, EV_READ | EV_PERSIST, &udp_read, NULL);
    if (event_add(event, NULL) == -1)
        die("event_add failed");
}

/*===== TCP Handler ========*/

static void setnonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void client_write(client_t *client, const char *data, size_t data_size) {
    bufferevent_write(client->buf_ev, data, data_size);
}

static void client_close(client_t *client) {
    if (client->node) {
        // Unlink client & node
        DL_DELETE(client->node->clients, client);
        client->node = NULL;
    }
    bufferevent_free(client->buf_ev);
    close(client->fd);
    free(client);
}

static void client_read(struct bufferevent *bev, void *arg) {
    client_t *client = arg;

    char *line = evbuffer_readline(bev->input);
    if (!line)
        return;

    if (!client->node) {
        node_t *node;
        HASH_FIND(by_path, nodes_by_path, line, strlen(line), node);
        if (!node) {
            client_write(client, "404\n", 4);
            goto done;
        }

        // Link client & node
        DL_APPEND(node->clients, client);
        client->node = node;

        client_write(client, "ok!\n", 4);
    } 
done:
    free(line);
}

static void client_error(struct bufferevent *bev, short what, void *arg) {
    client_t *client = arg;
    client_close(client);
}

static void client_create(int fd) {
    client_t *client = xmalloc(sizeof(client_t));
    client->fd = fd;
    client->buf_ev = bufferevent_new(
            fd,
            client_read,
            NULL,
            client_error,
            client);
    bufferevent_enable(client->buf_ev, EV_READ);
    client_write(client, VERSION_STRING, LITERAL_SIZE(VERSION_STRING));
    client_write(client, " (", 2);
    client_write(client, INFO_URL, LITERAL_SIZE(INFO_URL));
    client_write(client, ")", 2);
    client_write(client, ". Select your channel!\n", 23);
}

static void accept_callback(int fd, short ev, void *arg) {
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    client_fd = accept(fd,
            (struct sockaddr *)&client_addr,
            &client_len);
    if (client_fd < 0) {
        fprintf(stderr, "accept() failed\n");
        return;
    }

    setnonblock(client_fd);
    client_create(client_fd);
}

static void open_tcp(struct event *event) {
    int fd = create_socket(SOCK_STREAM);

    if (listen(fd, 5) < 0)
        die("cannot listen");

    setnonblock(fd);

    struct event accept_event;
    event_set(&accept_event,
            fd,
            EV_READ | EV_PERSIST,
            accept_callback,
            NULL);

    if (event_add(&accept_event, NULL) == -1)
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

    glClearColor(0.05, 0.05, 0.05, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(0);
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

static void update_now() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    now = tv.tv_sec;
    now += 1.0 / 1000000 * tv.tv_usec;
}

int main(int argc, char *argv[]) {
    fprintf(stdout, VERSION_STRING " (" INFO_URL ")\n");
    fprintf(stdout, "Copyright (c) 2012, Florian Wesch <fw@dividuum.de>\n");

    if (argc != 2) {
        fprintf(stderr, "\nusage: %s <root_name>\n", argv[0]);
        exit(1);
    }

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

    struct event tcp_event;
    open_tcp(&tcp_event);

    glfwInit();
    glfwOpenWindowHint(GLFW_FSAA_SAMPLES, 4);

    int mode = getenv("FULLSCREEN") ? GLFW_FULLSCREEN : GLFW_WINDOW;

    if(!glfwOpenWindow(1024, 768, 8,8,8,8, 0,0, mode))
        die("cannot open window");

    GLenum err = glewInit();
    if (err != GLEW_OK)
        die("cannot initialize glew");

    glfwSetWindowTitle(VERSION_STRING);
    glfwSwapInterval(1);
    glfwSetWindowSizeCallback(reshape);
    glfwSetKeyCallback(keypressed);
    glfwDisable(GLFW_AUTO_POLL_EVENTS);

    signal(SIGVTALRM, deadline_signal);

    update_now();

    node_init_all(&root, argv[1]);

    double last = glfwGetTime();
    while (running) {
        update_now();
        tick();
    }

    // no cleanup :-}
    return 0;
}
