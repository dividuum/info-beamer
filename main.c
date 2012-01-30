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

#define MAX_CODE_SIZE 16384 // byte
#define MAX_MEM 2000 // KB
#define MIN_DELTA 500 // ms
#define MAX_DELTA 2000 // ms

#define MAX_RUNAWAY_TIME 5 // sec
#define MAX_PCALL_TIME  20000000 // usec

#define print_projection_depth() \
    {\
        int depth;\
        glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &depth);\
        printf("projection matrix depth is %d\n", depth);\
    };

struct node_s {
    int wd; // inotify watch descriptor
    int prio;
    const char *name; // local node name
    const char *path; // full path (including node name)
    lua_State *L;
    int enforce_mem;

    UT_hash_handle by_wd;   // global handle for search by watch descriptor
    UT_hash_handle by_name; // handle for childs by name

    struct node_s *parent;
    struct node_s *childs;

    int width;
    int height;
    GLuint fbo, rbo, tex;
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

static void lua_execute(node_t *node, const char *code, size_t code_size) {
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
}

static void lua_execute_format(node_t *node, const char *fmt, ...) {
    char code[MAX_CODE_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int code_size = vsnprintf(code, sizeof(code), fmt, ap);
    va_end(ap);
    lua_execute(node, code, code_size);
}

/*======= Node =======*/

static int node_has_framebuffer(node_t *node) {
    return node->tex != 0;
}

static void node_framebuffer_remove(node_t *node) {
    glDeleteTextures(1, &node->tex);
    // glDeleteRenderbuffers(1, &node->rbo);
    glDeleteFramebuffers(1, &node->fbo);
    node->tex = node->fbo = node->width = node->height = 0;
}

static void node_framebuffer_init(node_t *node, int width, int height) {
    if (node_has_framebuffer(node))
        node_framebuffer_remove(node);

    node->width = width;
    node->height = height;

    glGenFramebuffers(1, &node->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, node->fbo);

    // glGenRenderbuffers(1, &node->rbo);
    // glBindRenderbuffer(GL_RENDERBUFFER, node->rbo);
    // glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, node->width, node->height);
    // glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, node->rbo);

    glGenTextures(1, &node->tex);
    glBindTexture(GL_TEXTURE_2D, node->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, node->width, node->height, 0, GL_RGBA, GL_INT, NULL);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, node->tex, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}


static void node_render_to_buffer(node_t *node) {
    if (!node_has_framebuffer(node)) return;

    printf("-> rendering %s into %dx%d\n", node->name, node->width, node->height);

    glPushAttrib(GL_ALL_ATTRIB_BITS);

    int prev_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, node->fbo);
    printf("switched fbo from %d to %d\n", prev_fbo, node->fbo);

    // int tex;
    // glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    // printf("Texture is %d\n", tex);

    print_projection_depth();

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glViewport(0, 0, node->width, node->height);
    glOrtho(0, node->width,
            node->height, 0,
            -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // start with white screen
    glClearColor(1.0, 1.0, 1.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    print_projection_depth();

    lua_execute_format(node, "news.on_render()");

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();

    print_projection_depth();

    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    printf("fbo is now %d again\n", prev_fbo);

    printf("<- done rendering %s\n", node->name);
}

static void node_render_to_viewport(node_t *node, int x1, int y1, int x2, int y2) {
    if (!node_has_framebuffer(node)) return;

    printf("-> drawing %s to %d,%d -> %d,%d\n", node->name, x1, y1, x2, y2);

    int prev_tex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glBindTexture(GL_TEXTURE_2D, node->tex);
    printf("switching tex from %d to %d\n", prev_tex, node->tex);

    glColor4f(1.0, 1.0, 1.0, 1.0);
    glBegin(GL_QUADS);
        glTexCoord2f(0, 0);
        glVertex3f(x1, y1, 0);
        glTexCoord2f(node->width, 0);
        glVertex3f(x2, y1, 0);
        glTexCoord2f(node->width, node->height);
        glVertex3f(x2, y2, 0);
        glTexCoord2f(0, node->height);
        glVertex3f(x1, y2, 0);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, prev_tex);
    printf("tex is now %d again\n", prev_tex);

    printf("<- done drawing %s\n", node->name);
}

static int luaSetup(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    int width = (int)luaL_checknumber(L, 1);
    int height = (int)luaL_checknumber(L, 2);
    node_framebuffer_init(node, width, height);
    return 0;
}

static int luaChildRender(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);

    node_t *child;
    HASH_FIND(by_name, node->childs, name, strlen(name), child);
    if (!child)
        luaL_error(L, "child not found");

    node_render_to_buffer(child);
    return 0;
}

static int luaChildDraw(lua_State *L) {
    node_t *node = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    int x1 = (int)luaL_checknumber(L, 2);
    int y1 = (int)luaL_checknumber(L, 3);
    int x2 = (int)luaL_checknumber(L, 4);
    int y2 = (int)luaL_checknumber(L, 5);

    node_t *child;
    HASH_FIND(by_name, node->childs, name, strlen(name), child);
    if (!child)
        luaL_error(L, "child not found");

    node_render_to_viewport(child, x1, y1, x2, y2);
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

static int luaDrawShit(lua_State *L) {
    printf("SHIT\n");
    glColor4f(1.0, 0.0, 0.0, 1.0);
    glLineWidth(3000);
    glBegin(GL_LINES);
        glVertex3f(0., 0., 0.);
        glVertex3f(300., 300., 0.);
    glEnd();
    return 0;
}

static void node_init(node_t *node, node_t *parent, const char *path, const char *name);
static void node_free(node_t *node);

#define lua_register_node_func(node,name,func) \
    (lua_pushliteral((node)->L, name), \
     lua_pushlightuserdata((node)->L, node), \
     lua_pushcclosure((node)->L, func, 1), \
     lua_settable((node)->L, LUA_GLOBALSINDEX))


static void node_init_sandbox(node_t *node) {
    lua_execute_format(node, "init_sandbox");
}

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
        node_init_sandbox(node);

        char code[MAX_CODE_SIZE];
        int fd = open(path, O_RDONLY);
        if (fd == -1) {
            fprintf(stderr, "cannot open file for reading: %s\n", path);
            return;
        }

        size_t code_size = read(fd, code, sizeof(code));
        close(fd);

        lua_execute(node, code, code_size);
    } else {
        lua_execute_format(node, "news.on_content_update(\"%s\")", name);
    }
}

static void node_remove(node_t *node, const char *filename) {
    fprintf(stderr, "<<< content del %s in %s\n", filename, node->path);
    if (strcmp(filename, "script.lua") == 0) {
        node_init_sandbox(node);
    } else {
        lua_execute_format(node, "news.on_content_update(\"%s\")", filename);
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
    HASH_ADD(by_wd, nodes, wd, sizeof(int), node);

    // init node structure
    node->parent = parent;
    node->path = strdup(path);
    node->name = strdup(name);
    node->enforce_mem = 0;

    // create lua state
    node->L = lua_newstate(lua_alloc, node);
    if (!node->L)
        die("cannot create lua");

    lua_atpanic(node->L, lua_panic);
    luaL_openlibs(node->L);

   if (luaL_loadbuffer(node->L, kernel, kernel_size, "<kernel>") != 0)
       die("kernel load");
   if (lua_pcall(node->L, 0, 0, 0) != 0)
       die("kernel run %s", lua_tostring(node->L, 1));

    lua_register_node_func(node, "setup", luaSetup);
    lua_register_node_func(node, "child_draw", luaChildDraw);
    lua_register_node_func(node, "child_render", luaChildRender);
    lua_register(node->L, "clear", luaClear);
    lua_register(node->L, "shit", luaDrawShit);

    lua_pushstring(node->L, path);
    lua_setglobal(node->L, "PATH");

    lua_pushstring(node->L, name);
    lua_setglobal(node->L, "NAME");

    node_init_sandbox(node);

    node_recursive_search(node);
}

static void node_free(node_t *node) {
    fprintf(stderr, "<<< node del %s in %s\n", node->name, node->path);
    node_t *child, *tmp; HASH_ITER(by_name, node->childs, child, tmp) {
        node_remove_child(node, child);
    }
    free((void*)node->path);
    free((void*)node->name);
    // inotify_rm_watch(inotify_fd, node->wd));
    lua_close(node->L);
    HASH_DELETE(by_wd, nodes, node);
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
            HASH_FIND(by_wd, nodes, &event->wd, sizeof(int), node);
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
    
static void tick(int delta) {
    check_inotify();
    event_loop(EVLOOP_NONBLOCK);
    glfwPollEvents();
    node_tree_print(&root, 0);

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
            -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    node_render_to_buffer(&root);

    // Screen background
    glClearColor(0.4, 0.4, 0.4, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    // Render root on screen
    node_render_to_viewport(&root, 100, 100, win_w - 100, win_h - 100);

    glfwSwapBuffers();

    if (!glfwGetWindowParam(GLFW_OPENED))
        die("window closed");
}

int main(int argc, char *argv[]) {
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd == -1)
        die("cannot open inotify");

    event_init();

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

    signal(SIGVTALRM, deadline_signal);

    node_init(&root, NULL, "news", "news");

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
