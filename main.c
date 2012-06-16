/* See Copyright Notice in LICENSE.txt */

#define _BSD_SOURCE
#define _GNU_SOURCE
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
#include <IL/il.h>
#include <IL/ilu.h>
#include <libavformat/avformat.h>
#include <event.h>
#include <event2/dns.h>
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
#include "vnc.h"
#include "framebuffer.h"
#include "struct.h"

#include "kernel.h"
#include "userlib.h"

#define VERSION_STRING "Info Beamer " VERSION
#define INFO_URL "http://info-beamer.org/"

#define NODE_CODE_FILE "node.lua"

#define MAX_MEM 2000000 // KB
#define MAX_GL_PUSH 20 // glPushMatrix depth
#define MAX_CHILD_RENDERS 20 // maximum childs rendered per node
#define MAX_SNAPSHOTS 5 // maximum number of snapshots per render

// Default host/port (both udp & tcp)
#define LISTEN_ADDR  "0.0.0.0"
#define DEFAULT_PORT 4444

#ifdef DEBUG
#define MAX_RUNAWAY_TIME 10 // sec
#define MAX_PCALL_TIME  5000000 // usec
#else
#define MAX_RUNAWAY_TIME 1 // sec
#define MAX_PCALL_TIME  500000 // usec
#endif

#define NO_GL_PUSHPOP -1

#define NODE_INACTIVITY 2.0 // node considered idle after x seconds
#define NODE_CPU_BLACKLIST 60.0 // seconds a node is blacklisted if it exceeds cpu usage

typedef enum { PROFILE_BOOT, PROFILE_UPDATE, PROFILE_EVENT } profiling_bins;

typedef struct node_s {
    int wd; // inotify watch descriptor

    char *name;   // local node name
    char *path;   // full path (including node name)
    char *alias;  // alias path

    lua_State *L;

    UT_hash_handle by_wd;    // global handle for search by watch descriptor
    UT_hash_handle by_name;  // childs by name
    UT_hash_handle by_path;  // node by path
    UT_hash_handle by_alias; // node by alias

    struct node_s *parent;
    struct node_s *childs;

    int width;
    int height;

    int gl_matrix_depth;

    void *mem;
    tlsf_pool pool;

    struct client_s *clients;

    int child_render_quota;
    int snapshot_quota;

    double profiling[3];
    double last_profile;
    int num_frames;
    int num_resource_inits;
    int num_allocs;

    double last_activity;
    double blacklisted;
} node_t;

static node_t *nodes_by_wd = NULL;
static node_t *nodes_by_path = NULL;
static node_t *nodes_by_alias = NULL;
static node_t root = {0};

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
static int listen_port;

GLuint default_tex; // white default texture
struct event_base *event_base;
struct evdns_base *dns_base;

/*=== Forward declarations =====*/

static void client_write(client_t *client, const char *data, size_t data_size);
static void client_close(client_t *client);
static void node_printf(node_t *node, const char *fmt, ...);
static void node_blacklist(node_t *node, double time);
static void node_remove_alias(node_t *node);
static void node_reset_quota(node_t *node);
static int node_render_to_image(lua_State *L, node_t *node);
static void node_init(node_t *node, node_t *parent, const char *path, const char *name);
static void node_free(node_t *node);

/*======= Lua Sandboxing =======*/

static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    node_t *node = ud;
    node->num_allocs++;
    (void)osize;  /* not used */
    if (nsize == 0) {
        tlsf_free(node->pool, ptr);
        return NULL;
    } else {
        return tlsf_realloc(node->pool, ptr, nsize);
    }
}

/* execution time limiting for pcalls */
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

    fprintf(stderr, RED("[%s]") " timeout\n", global_node->path);

    if (timers_expired == 0) {
        // timer expired once? Try to solve it inside of
        // lua: set a hook that will execute deadline_stop.
        lua_sethook(global_node->L, deadline_stop,
            LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
        node_blacklist(global_node, NODE_CPU_BLACKLIST);
    } else {
        // timer expired again without lua being stopped?
        die("unstoppable runaway code in %s", global_node->path);
    }
    timers_expired++;
}

static int lua_timed_pcall(node_t *node, int in, int out,
        int error_handler_pos)
{
    node_t *old_global_node = global_node;
    struct itimerval old_timer;

    struct itimerval deadline;
    deadline.it_interval.tv_sec = MAX_RUNAWAY_TIME;
    deadline.it_interval.tv_usec = 0;
    deadline.it_value.tv_sec =  MAX_PCALL_TIME / 1000000;
    deadline.it_value.tv_usec = MAX_PCALL_TIME % 1000000;
    setitimer(ITIMER_VIRTUAL, &deadline, &old_timer);

    global_node = node;
    timers_expired = 0;
    int ret = lua_pcall(node->L, in, out, error_handler_pos);

    setitimer(ITIMER_VIRTUAL, &old_timer, NULL);
    global_node = old_global_node;
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

static void lua_node_enter(node_t *node, int args, profiling_bins bin) {
    node_reset_quota(node);
    lua_State *L = node->L;
    lua_pushliteral(L, "execute");              // [args] "execute"
    lua_rawget(L, LUA_REGISTRYINDEX);           // [args] execute
    lua_insert(L, -1 - args);                   // execute [args]
    lua_pushliteral(L, "traceback");            // execute [args] "traceback"
    lua_rawget(L, LUA_REGISTRYINDEX);           // execute [args] traceback
    const int error_handler_pos = lua_gettop(L) - 1 - args;
    lua_insert(L, error_handler_pos);           // traceback execute [args]
    struct timeval before, after;
    gettimeofday(&before, NULL);
    int status = lua_timed_pcall(node, args, 0, error_handler_pos);
    if (status == 0) {
        // success                              // traceback
        lua_remove(L, error_handler_pos);       //
    } else {
        // error                                // traceback "error"
        char *err = status == LUA_ERRRUN ? "runtime error" :
                    status == LUA_ERRMEM ? "memory error"  :
                    status == LUA_ERRERR ? "error handling error" : NULL;
        assert(err);
        node_printf(node, "%s: %s\n", err, lua_safe_error_message(L));
        lua_pop(L, 2);                          //
    }
    gettimeofday(&after, NULL);
    lua_gc(node->L, LUA_GCSTEP, 5);
    node->profiling[bin] += time_delta(&before, &after);
    node->last_activity = now;
}

/*======= Lua entry points =======*/

// reinit sandbox, load usercode and user code
static void node_boot(node_t *node) {
    lua_pushliteral(node->L, "boot");
    lua_node_enter(node, 1, PROFILE_BOOT);
}

// notify of child update 
static void node_child_update(node_t *node, const char *name, int added) {
    lua_pushliteral(node->L, "child_update");
    lua_pushstring(node->L, name);
    lua_pushboolean(node->L, added);
    lua_node_enter(node, 3, PROFILE_UPDATE);
}

// notify of content update 
static void node_content_update(node_t *node, const char *name, int added) {
    fprintf(stderr, YELLOW("[%s]")" update %c%s\n", node->path, added ? '+' : '-', name);
    lua_pushliteral(node->L, "content_update");
    lua_pushstring(node->L, name);
    lua_pushboolean(node->L, added);
    if (!strcmp(name, NODE_CODE_FILE)) {
        // reset blacklisted flag
        node->blacklisted = 0;

        // reset node dimensions
        node->width = 0;
        node->height = 0;

        // remove existing node alias
        node_remove_alias(node);
    }
    lua_node_enter(node, 3, PROFILE_UPDATE);
}

// event.<event_name>(args...)
static void node_event(node_t *node, const char *name, int args) {
    lua_pushliteral(node->L, "event"); // [args] "event_name"
    lua_pushstring(node->L, name);     // [args] "event_name" name
    lua_insert(node->L, -2 - args);    // name [args] "event_name"
    lua_insert(node->L, -2 - args);    // "event_name" name [args]
    lua_node_enter(node, 2 + args, PROFILE_EVENT);
}

// render node
static void node_render_self(node_t *node, int width, int height) {
    lua_pushliteral(node->L, "render_self");
    lua_pushnumber(node->L, width);
    lua_pushnumber(node->L, height);
    lua_node_enter(node, 3, PROFILE_EVENT);
}

/*===== node macros =======*/

#define node_setup_completed(node) ((node)->width != 0)
#define node_is_idle(node) (now > (node)->last_activity + NODE_INACTIVITY)
#define node_is_blacklisted(node) (now < (node)->blacklisted)
#define node_is_rendering(node) ((node)->gl_matrix_depth != NO_GL_PUSHPOP)

/*===== Lua bindings ======*/

static node_t *get_rendering_node(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    if (!node_is_rendering(node))
        luaL_error(L, "only callable in node.render");
    return node;
}

static int luaRenderSelf(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    return node_render_to_image(L, node);
}

static int luaRenderChild(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    if (node->child_render_quota-- <= 0)
        return luaL_error(L, "too many childs rendered");

    const char *name = luaL_checkstring(L, 1);

    node_t *child;
    HASH_FIND(by_name, node->childs, name, strlen(name), child);
    if (!child)
        return luaL_error(L, "child %s not found", name);
    return node_render_to_image(L, child);
}

static int luaSetup(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    if (node_is_rendering(node))
        return luaL_error(L, "cannot change width or height while rendering");
    int width = (int)luaL_checknumber(L, 1);
    int height = (int)luaL_checknumber(L, 2);
    if (width < 32 || width > 2048)
        luaL_argerror(L, 1, "invalid width. must be within [32,2048]");
    if (height < 32 || height > 2048)
        luaL_argerror(L, 2, "invalid height. must be within [32,2048]");
    node->width = width;
    node->height = height;
    return 0;
}

static int luaGlOrtho(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, node->width,
            node->height, 0,
            -1000, 1000);
    glMatrixMode(GL_MODELVIEW);
    return 0;
}

static int luaGlPerspective(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    double fov = luaL_checknumber(L, 1);
    double eye_x = luaL_checknumber(L, 2);
    double eye_y = luaL_checknumber(L, 3);
    double eye_z = luaL_checknumber(L, 4);
    double center_x = luaL_checknumber(L, 5);
    double center_y = luaL_checknumber(L, 6);
    double center_z = luaL_checknumber(L, 7);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(fov, (float)node->width / (float)node->height, 0.1, 10000);
    gluLookAt(eye_x, eye_y, eye_z, 
              center_x, center_y, center_z,
              0, -1, 0);
    glMatrixMode(GL_MODELVIEW);
    return 0;
}

static int luaSetAlias(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *alias = luaL_checkstring(L, 1);

    // already exists?
    node_t *existing_node;
    HASH_FIND(by_alias, nodes_by_alias, alias, strlen(alias), existing_node);
    if (existing_node) {
        if (existing_node == node) {
            return 0;
        } else {
            return luaL_error(L, "alias already taken by %s", existing_node->path);
        }
    }

    // remove old alias
    if (node->alias) {
        HASH_DELETE(by_alias, nodes_by_alias, node);
        free(node->alias);
    }

    // set new alias
    node->alias = strdup(alias);
    HASH_ADD_KEYPTR(by_alias, nodes_by_alias, node->alias, strlen(node->alias), node);
    return 0;
}

static int luaLoadImage(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    if (index(name, '/'))
        luaL_argerror(L, 1, "invalid resource name");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", node->path, name);
    node->num_resource_inits++;
    return image_load(L, path, name);
}

static int luaLoadVideo(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    if (index(name, '/'))
        luaL_argerror(L, 1, "invalid resource name");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", node->path, name);
    node->num_resource_inits++;
    return video_load(L, path, name);
}

static int luaLoadFont(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    if (index(name, '/'))
        luaL_argerror(L, 1, "invalid resource name");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", node->path, name);
    node->num_resource_inits++;
    return font_new(L, path, name);
}

static int luaLoadFile(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    if (index(name, '/'))
        luaL_argerror(L, 1, "invalid resource name");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", node->path, name);

    int fd = open(path, O_RDONLY);
    if (fd == -1)
        return luaL_error(L, "cannot open file '%s'", path);

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    while (1) {
        char *data = luaL_prepbuffer(&b);
        ssize_t data_size = read(fd, data, LUAL_BUFFERSIZE);
        if (data_size < 0)
            return luaL_error(L, "cannot read %s: %s", name, strerror(errno));
        if (data_size == 0)
            break;
        luaL_addsize(&b, data_size);
    }
    close(fd);
    luaL_pushresult(&b);
    node->num_resource_inits++;
    return 1;
}

static int luaCreateSnapshot(lua_State *L) {
    node_t *node = get_rendering_node(L);
    if (node->snapshot_quota-- <= 0)
        return luaL_error(L, "too many snapshots");
    node->num_resource_inits++;
    return image_from_current_framebuffer(L, node->width, node->height);
}

static int luaCreateShader(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *vertex = luaL_checkstring(L, 1);
    const char *fragment = luaL_checkstring(L, 2);
    node->num_resource_inits++;
    return shader_new(L, vertex, fragment);
}

static int luaCreateVnc(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *host = luaL_checkstring(L, 1);
    int port = luaL_optnumber(L, 2, 5900);
    node->num_resource_inits++;
    return vnc_create(L, host, port);
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

static int luaGlClear(lua_State *L) {
    get_rendering_node(L);
    GLdouble r = luaL_checknumber(L, 1);
    GLdouble g = luaL_checknumber(L, 2);
    GLdouble b = luaL_checknumber(L, 3);
    GLdouble a = luaL_checknumber(L, 4);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(0);
    return 0;
}

static int luaGlPushMatrix(lua_State *L) {
    node_t *node = get_rendering_node(L);
    if (node->gl_matrix_depth > MAX_GL_PUSH)
        return luaL_error(L, "Too may pushes");
    glPushMatrix();
    node->gl_matrix_depth++;
    return 0;
}

static int luaGlPopMatrix(lua_State *L) {
    node_t *node = get_rendering_node(L);
    if (node->gl_matrix_depth == 0)
        return luaL_error(L, "Nothing to pop");
    glPopMatrix();
    node->gl_matrix_depth--;
    return 0;
}

static int luaGlRotate(lua_State *L) {
    get_rendering_node(L);
    double angle = luaL_checknumber(L, 1);
    double x = luaL_checknumber(L, 2);
    double y = luaL_checknumber(L, 3);
    double z = luaL_checknumber(L, 4);
    glRotated(angle, x, y, z);
    return 0;
}

static int luaGlTranslate(lua_State *L) {
    get_rendering_node(L);
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double z = luaL_optnumber(L, 3, 0.0);
    glTranslated(x, y, z);
    return 0;
}

static int luaGlScale(lua_State *L) {
    get_rendering_node(L);
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double z = luaL_optnumber(L, 3, 1.0);
    glScaled(x, y, z);
    return 0;
}

static int luaNow(lua_State *L) {
    lua_pushnumber(L, now);
    return 1;
}

/*==== Node functions =====*/

static int node_render_to_image(lua_State *L, node_t *node) {
    // save current gl state
    int prev_fbo, prev_prog;
    GLdouble prev_projection[16];
    GLdouble prev_modelview[16];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetDoublev(GL_PROJECTION_MATRIX, prev_projection);
    glGetDoublev(GL_MODELVIEW_MATRIX, prev_modelview);

    glPushAttrib(GL_ALL_ATTRIB_BITS);

    int width = 1, height = 1;
    if (node_setup_completed(node))
        width = node->width, height = node->height;

    // get new framebuffer and associated texture from recycler
    unsigned int fbo, tex;
    make_framebuffer(width, height, &tex, &fbo);

    // initialize gl state
    glUseProgram(0);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    glViewport(0, 0, width, height);
    glOrtho(0, width,
            height, 0,
            -1000, 1000);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    if (!node_setup_completed(node)) {
        node_printf(node, "node not initialized with gl.setup()\n");
        glClearColor(0.5, 0.5, 0.5, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    } else if (node_is_blacklisted(node)) {
        node_printf(node, "node is blacklisted\n");
        glClearColor(0.5, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        // clear with transparent color
        glClearColor(1, 1, 1, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        // render node
        node->gl_matrix_depth = 0;

        node->num_frames++;
        node_event(node, "render", 0);

        while (node->gl_matrix_depth-- > 0)
            glPopMatrix();
        node->gl_matrix_depth = NO_GL_PUSHPOP;
    }

    // rebind to framebuffer texture
    glBindTexture(GL_TEXTURE_2D, tex);
    glGenerateMipmap(GL_TEXTURE_2D);

    // restore previous state
    glPopAttrib();

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixd(prev_projection);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixd(prev_modelview);
    glUseProgram(prev_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);

    return image_create(L, tex, fbo, width, height);
}

static void node_printf(node_t *node, const char *fmt, ...) {
    char buffer[16384];
    va_list ap;
    va_start(ap, fmt);
    size_t buffer_size = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    fprintf(stderr, GREEN("[%s]")" %s", node->path, buffer);
    client_t *client;
    DL_FOREACH(node->clients, client) {
        client_write(client, buffer, buffer_size);
    }
}

static void node_blacklist(node_t *node, double time) {
    node->blacklisted = now + time;
    node_printf(node, "blacklisted for %.0f seconds\n", time);
}

static void node_remove_alias(node_t *node) {
    if (node->alias) {
        HASH_DELETE(by_alias, nodes_by_alias, node);
        free(node->alias);
        node->alias = NULL;
    }
}

static void node_tree_gc(node_t *node) {
    if (!node_is_idle(node))
        lua_gc(node->L, LUA_GCSTEP, 30);
    node_t *child, *tmp; 
    HASH_ITER(by_name, node->childs, child, tmp) {
        node_tree_gc(child);
    };
}

static node_t *node_add_child(node_t* node, const char *path, const char *name) {
    fprintf(stderr, YELLOW("[%s]")" adding new child node %s\n", node->name, name);
    node_t *child = xmalloc(sizeof(node_t));
    node_init(child, node, path, name);
    HASH_ADD_KEYPTR(by_name, node->childs, child->name, strlen(child->name), child);
    return child;
}

static void node_remove_child(node_t* node, node_t* child) {
    fprintf(stderr, YELLOW("[%s]")" removing child node %s\n", node->name, child->name);
    node_child_update(node, child->name, 0);
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

static void node_reset_quota(node_t *node) {
    node->child_render_quota = MAX_CHILD_RENDERS;
    node->snapshot_quota = MAX_SNAPSHOTS;
}

static void node_reset_profiler(node_t *node) {
    node->last_profile = now;
    node->profiling[PROFILE_BOOT] = 0.0;
    node->profiling[PROFILE_UPDATE] = 0.0;
    node->profiling[PROFILE_EVENT] = 0.0;
    node->num_frames = 0;
    node->num_resource_inits = 0;
    node->num_allocs = 0;
}

#define lua_register_node_func(node,name,func) \
    (lua_pushliteral((node)->L, name), \
     lua_pushlightuserdata((node)->L, node), \
     lua_pushcclosure((node)->L, func, 1), \
     lua_settable((node)->L, LUA_GLOBALSINDEX))

static void node_init(node_t *node, node_t *parent, const char *path, const char *name) {
    // add directory watcher
    node->wd = inotify_add_watch(inotify_fd, path, 
        IN_CLOSE_WRITE|IN_CREATE|IN_DELETE|IN_DELETE_SELF|
        IN_MOVE);
    if (node->wd == -1)
        die("cannot start watching directory %s: %s", path, strerror(errno));

    node->parent = parent;
    node->path = strdup(path);
    node->name = strdup(name);
    node->alias = NULL;
    node->width = 0;
    node->height = 0;

    node_reset_profiler(node);

    node->last_activity = now;

    node->gl_matrix_depth = NO_GL_PUSHPOP;

    // link by watch descriptor & path
    HASH_ADD(by_wd, nodes_by_wd, wd, sizeof(int), node);
    HASH_ADD_KEYPTR(by_path, nodes_by_path, node->path, strlen(node->path), node);

    // create lua state
#ifdef USE_LUAJIT
    node->L = luaL_newstate();
#else
    node->mem = malloc(MAX_MEM);
    node->pool = tlsf_create(node->mem, MAX_MEM);
    node->L = lua_newstate(lua_alloc, node);
#endif

    if (!node->L)
        die("cannot create lua");

    lua_atpanic(node->L, lua_panic);
    luaL_openlibs(node->L);
    image_register(node->L);
    video_register(node->L);
    font_register(node->L);
    shader_register(node->L);
    vnc_register(node->L);
    luaopen_struct(node->L);

    lua_register_node_func(node, "setup", luaSetup);
    lua_register_node_func(node, "print", luaPrint);
    lua_register_node_func(node, "set_alias", luaSetAlias);

    lua_register_node_func(node, "render_self", luaRenderSelf);
    lua_register_node_func(node, "render_child", luaRenderChild);
    lua_register_node_func(node, "load_image", luaLoadImage);
    lua_register_node_func(node, "load_video", luaLoadVideo);
    lua_register_node_func(node, "load_font", luaLoadFont);
    lua_register_node_func(node, "load_file", luaLoadFile);
    lua_register_node_func(node, "create_snapshot", luaCreateSnapshot);
    lua_register_node_func(node, "create_shader", luaCreateShader);
    lua_register_node_func(node, "create_vnc", luaCreateVnc);

    lua_register_node_func(node, "glClear", luaGlClear);
    lua_register_node_func(node, "glPushMatrix", luaGlPushMatrix);
    lua_register_node_func(node, "glPopMatrix", luaGlPopMatrix);
    lua_register_node_func(node, "glRotate", luaGlRotate);
    lua_register_node_func(node, "glTranslate", luaGlTranslate);
    lua_register_node_func(node, "glScale", luaGlScale);
    lua_register_node_func(node, "glOrtho", luaGlOrtho);
    lua_register_node_func(node, "glPerspective", luaGlPerspective);

    lua_register(node->L, "now", luaNow);

    lua_pushstring(node->L, path);
    lua_setglobal(node->L, "PATH");

    lua_pushstring(node->L, name);
    lua_setglobal(node->L, "NAME");

    lua_pushlstring(node->L, userlib, userlib_size);
    lua_setglobal(node->L, "USERLIB");

    lua_pushliteral(node->L, NODE_CODE_FILE);
    lua_setglobal(node->L, "NODE_CODE_FILE");

    if (luaL_loadbuffer(node->L, kernel, kernel_size, "kernel.lua") != 0) {
        const char *error =  lua_tostring(node->L, -1);
        // If kernel.lua was procompiled with an incompatible lua
        // version, loading the embedded code fail here. Try to
        // detect this...
        die("cannot load kernel.lua: %s%s",
            lua_tostring(node->L, -1),
            strstr(error, "bad header") ? " (See 'kernel load error' in the docs)" : ""
        );
    }
    if (lua_pcall(node->L, 0, 0, 0) != 0)
        die("kernel run %s", lua_tostring(node->L, -1));
}

static void node_free(node_t *node) {
    node_t *child, *tmp; 
    HASH_ITER(by_name, node->childs, child, tmp) {
        node_remove_child(node, child);
    }
    HASH_DELETE(by_wd, nodes_by_wd, node);
    HASH_DELETE(by_path, nodes_by_path, node);
    free(node->path);
    free(node->name);

    node_remove_alias(node);

    client_t *client, *tmp_client;
    DL_FOREACH_SAFE(node->clients, client, tmp_client) {
        client_close(client);
    }
    assert(node->clients == NULL);

    lua_close(node->L);

#ifndef USE_LUAJIT
    tlsf_destroy(node->pool);
    free(node->mem);
#endif
}

static void node_search_and_boot(node_t *node) {
    DIR *dp = opendir(node->path);
    if (!dp)
        die("cannot open directory %s: %s", node->path, strerror(errno));

    struct dirent *ep;
    while ((ep = readdir(dp))) {
        if (ep->d_name[0] == '.') 
            continue;

        const char *child_name = ep->d_name;
        char child_path[PATH_MAX];
        snprintf(child_path, sizeof(child_path), "%s/%s", node->path, child_name);

        if (ep->d_type == DT_DIR) {
            node_t *child = node_add_child(node, child_path, child_name);
            node_search_and_boot(child);
            node_child_update(node, child->name, 1);
        } else if (ep->d_type == DT_REG && strcmp(child_name, NODE_CODE_FILE)) {
            node_content_update(node, child_name, 1);
        }
    }
    closedir(dp);

    node_boot(node);
}

static void node_init_root(node_t *root, const char *base_path) {
    node_init(root, NULL, base_path, base_path);
    node_search_and_boot(root);
}

static node_t *node_find_by_path_or_alias(const char *needle) {
    size_t needle_size = strlen(needle);
    node_t *node;
    HASH_FIND(by_path, nodes_by_path, needle, needle_size, node);
    if (node)
        return node;
    HASH_FIND(by_alias, nodes_by_alias, needle, needle_size, node);
    return node;
}

static void node_print_profile(node_t *node, int depth) {
    node_t *child, *tmp; 
    double delta = (now - node->last_profile) * 1000;
    fprintf(stderr, "%c%4dkb %3.0f %5.1f %6.1f %5d  %5d %5.1lf%% %5.1lf%% %5.1lf%% %*s '- %s (%s)\n", 
        node_is_blacklisted(node) ? 'X' : node_is_idle(node) ? ' ' : '*',
        lua_gc(node->L, LUA_GCCOUNT, 0),
        node->num_frames * 1000 / delta,
        (double)node->num_resource_inits * 1000 / delta,
        node->num_frames ?  (double)node->num_allocs / node->num_frames : 0.0,
        node->width, node->height,
        100 / delta * node->profiling[PROFILE_BOOT],
        100 / delta * node->profiling[PROFILE_UPDATE],
        100 / delta * node->profiling[PROFILE_EVENT],
        depth*3, "", node->name, node->alias ? node->alias : "-"
    );
    node_reset_profiler(node);
    HASH_ITER(by_name, node->childs, child, tmp) {
        node_print_profile(child, depth+1);
    };
}

static void node_profiler() {
    fprintf(stderr, "    mem fps   rps allocs width height   boot update  event     name (alias)\n");
    fprintf(stderr, "---------------------------------------------------------------------------\n");
    node_print_profile(&root, 0);
    fprintf(stderr, "---------------------------------------------------------------------------\n");
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
            // fprintf(stderr, "event for %s (%s), mask: %08x\n", path, event->name, event->mask);

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
                    node_search_and_boot(child);
                    node_child_update(node, child->name, 1);
                } else if (S_ISREG(stat_buf.st_mode)) {
                    node_content_update(node, event->name, 1);
                }
            } else if (event->mask & IN_CLOSE_WRITE) {
                node_content_update(node, event->name, 1);
            } else if (event->mask & IN_DELETE_SELF) {
                if (!node->parent)
                    die("root node deleted. cannot continue");
                node_remove_child(node->parent, node);
            } else if (event->mask & IN_DELETE && !(event->mask & IN_ISDIR)) {
                node_content_update(node, event->name, 0);
            } else if (event->mask & IN_MOVED_FROM) {
                if (event->mask & IN_ISDIR) {
                    node_remove_child_by_name(node, event->name);
                } else {
                    node_content_update(node, event->name, 0);
                }
            } else if (event->mask & IN_MOVED_TO) {
                if (event->mask & IN_ISDIR) {
                    node_t *child = node_add_child(node, path, event->name);
                    node_search_and_boot(child);
                    node_child_update(node, child->name, 1);
                } else {
                    node_content_update(node, event->name, 1);
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
            case GLFW_KEY_SPACE:
                node_profiler();
                break;
            case GLFW_KEY_ESC:
                running = 0;
                break;
        }
    }
}

/*===== Util ========*/

static int create_socket(int type) {
    int one = 1;
    struct sockaddr_in sin;
    int fd = socket(AF_INET, type, 0);

    if (fd < 0)
        die("socket failed: %s", strerror(errno));

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0)
        die("setsockopt reuse failed: %s", strerror(errno));

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(LISTEN_ADDR);
    sin.sin_port = htons(listen_port);

    if (bind(fd, (struct sockaddr *)&sin, sizeof(struct sockaddr)) < 0)
        die("binding to %s port %d failed: %s",
            type == SOCK_DGRAM ? "udp" : "tcp",
            listen_port,
            strerror(errno)
        );

    return fd;
}

/*===== UDP (osc) Handling ========*/

static void udp_read(int fd, short event, void *arg) {
    char buf[1500];
    int len;
    unsigned int size = sizeof(struct sockaddr);
    struct sockaddr_in client_addr;

    len = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &size);

    if (len == -1)
        die("recvfrom");

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
        sendto(fd, LITERAL_AND_SIZE("fmt\n"), 0, (struct sockaddr *)&client_addr, size);
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
        sendto(fd, LITERAL_AND_SIZE("wtf\n"), 0, (struct sockaddr *)&client_addr, size);
        return;
    }

    // split a/b/c into first matching prefix:
    // a/b -> suffix: c if node a/b exists

    // fprintf(stderr, "udp event: %s: %*s\n", path, data_len, data);

    char *suffix = sep;
    node_t *node;
    while (1) {
        node = node_find_by_path_or_alias(path);
        if (node)
            break;

        char *next_split = memrchr(path, '/', suffix - path);
        if (!next_split) {
            sendto(fd, LITERAL_AND_SIZE("404\n"), 0, (struct sockaddr *)&client_addr, size);
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

static void open_udp(struct event *event) {
    int fd = create_socket(SOCK_DGRAM); 
    event_set(event, fd, EV_READ | EV_PERSIST, &udp_read, NULL);
    if (event_add(event, NULL) == -1)
        die("event_add failed");
}

/*===== TCP Handler ========*/

static void client_write(client_t *client, const char *data, size_t data_size) {
    bufferevent_write(client->buf_ev, data, data_size);
}

static void client_close(client_t *client) {
    if (client->node) {
        // unlink client & node
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

    if (client->node) {
        lua_pushstring(client->node->L, line);
        node_event(client->node, "input", 1);
    } else {
        node_t *node = node_find_by_path_or_alias(line);
        if (!node) {
            client_write(client, LITERAL_AND_SIZE("404\n"));
        } else {
            // link client & node
            DL_APPEND(node->clients, client);
            client->node = node;
            client_write(client, LITERAL_AND_SIZE("ok!\n"));
        }
    } 
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
    client_write(client, LITERAL_AND_SIZE(VERSION_STRING));
    client_write(client, LITERAL_AND_SIZE(" ("));
    client_write(client, LITERAL_AND_SIZE(INFO_URL));
    client_write(client, LITERAL_AND_SIZE(") [pid "));
    char pid[12];
    snprintf(pid, sizeof(pid), "%d", getpid());
    client_write(client, pid, strlen(pid));
    client_write(client, LITERAL_AND_SIZE("]. Select your channel!\n"));
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

    evutil_make_socket_nonblocking(client_fd);
    client_create(client_fd);
}

static void open_tcp(struct event *event) {
    int fd = create_socket(SOCK_STREAM);

    if (listen(fd, 5) < 0)
        die("listen failed: %s", strerror(errno));

    evutil_make_socket_nonblocking(fd);

    event_set(event,
            fd,
            EV_READ | EV_PERSIST,
            accept_callback,
            NULL);

    if (event_add(event, NULL) == -1)
        die("event_add failed");
}

static void tick() {
	now = glfwGetTime();

    check_inotify();

    event_loop(EVLOOP_NONBLOCK);

    glEnable(GL_TEXTURE_2D);

    glEnable(GL_BLEND);
    glBlendFuncSeparate(
        GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE_MINUS_DST_ALPHA, GL_ONE
    );

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

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

    node_tree_gc(&root);

    if (!glfwGetWindowParam(GLFW_OPENED))
        running = 0;
}

static void init_default_texture() {
    glGenTextures(1, &default_tex);
    glBindTexture(GL_TEXTURE_2D, default_tex);
    unsigned char white_pixel[] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, 4, 1, 1, 0, 
        GL_RGBA, GL_UNSIGNED_BYTE, white_pixel);
}

int main(int argc, char *argv[]) {
    fprintf(stdout, VERSION_STRING " (" INFO_URL ")\n");
    fprintf(stdout, "Copyright (c) 2012, Florian Wesch <fw@dividuum.de>\n\n");

    if (argc != 2 || (argc == 2 && !strcmp(argv[1], "-h"))) {
        fprintf(stderr, 
            "Usage: %s <root_name>\n"
            "\n"
            "Optional environment variables:\n"
            "\n"
            "  INFOBEAMER_FULLSCREEN=1  # Fullscreen mode\n"
            "  INFOBEAMER_PORT=<port>   # Listen on alternative port (tcp & udp, default %d)\n"
            "  INFOBEAMER_PRECOMPILED=1 # Allow precompiled code\n"
            "                             Warning: unsafe for untrusted code\n"
            "\n",
            argv[0], DEFAULT_PORT);
        exit(1);
    }

    char *root_name = realpath(argv[1], NULL);
    if (!root_name)
        die("cannot canonicalize path: %s", strerror(errno));

    char *split = rindex(root_name, '/');
    if (split) {
        *split = '\0';
        fprintf(stderr, INFO("chdir %s\n"), root_name);
        if (chdir(root_name) == -1)
            die("cannot chdir(%s): %s", root_name, strerror(errno));
        root_name = split+1;
    }

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd == -1)
        die("cannot open inotify: %s", strerror(errno));

    av_register_all();

    event_base = event_init();
    dns_base = evdns_base_new(event_base, 1);

    const char *port = getenv("INFOBEAMER_PORT");
    listen_port = port ? atoi(port) : DEFAULT_PORT;
    fprintf(stderr, INFO("tcp/udp port is %d\n"), listen_port);

    struct event udp_event;
    open_udp(&udp_event);

    struct event tcp_event;
    open_tcp(&tcp_event);

    glfwInit();
    glfwOpenWindowHint(GLFW_FSAA_SAMPLES, 4);

    int mode = getenv("INFOBEAMER_FULLSCREEN") ? GLFW_FULLSCREEN : GLFW_WINDOW;

    if(!glfwOpenWindow(1024, 768, 8,8,8,8, 0,0, mode))
        die("cannot open window");

    GLenum err = glewInit();
    if (err != GLEW_OK)
        die("cannot initialize glew");
    if (!glewIsSupported("GL_VERSION_2_0"))
        die("need opengl 2.0 support\n");

    glfwSetWindowTitle(VERSION_STRING);
    glfwSwapInterval(1);
    glfwSetWindowSizeCallback(reshape);
    glfwSetKeyCallback(keypressed);

    if (mode == GLFW_FULLSCREEN)
        glfwDisable(GLFW_MOUSE_CURSOR);

    ilInit();
    iluInit();

    signal(SIGVTALRM, deadline_signal);

    init_default_texture();

    now = glfwGetTime();
    node_init_root(&root, root_name);

    fprintf(stderr, INFO("initialization completed\n"));

    while (running) {
        tick();
    }

    // no cleanup :-}
    return 0;
}
