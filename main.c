#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>


#include <sys/inotify.h>

#include <GL/glfw.h>
#include <event.h>
#include <lualib.h>
#include <lauxlib.h>

#include "uthash.h"

#define MAX_MEM 2000 // KB
#define MIN_DELTA 500 // ms
#define MAX_DELTA 2000 // ms

struct node_s {
    int wd; // inotify watch descriptor
    int prio;
    const char *name;
    const char *path;
    struct node_s *childs;
    struct node_s *parent;
    lua_State *L;
    int enforce_mem;
    UT_hash_handle by_wd;
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

static void node_init(node_t *node, node_t *parent, const char *path);

static void *node_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    node_t *node = ud;
    (void)osize;  /* not used */
    if (nsize == 0) {
        free(ptr);
        return NULL;
    } else {
        if (node->enforce_mem && lua_gc(node->L, LUA_GCCOUNT, 0) > MAX_MEM);
            return NULL;
        return realloc(ptr, nsize);
    }
}


static void node_add_child(node_t* node, const char *path) {
    printf("adding child %s\n", path);
    node_t *child = xmalloc(sizeof(node_t));
    node_init(child, node, path);
}

static void node_update_content(node_t *node, const char *filename) {
    printf("updated content for %s in %s\n", filename, node->path);
}

static void node_remove(node_t *node, const char *path) {
    printf("removing content %s in %s\n", path, node->path);
}


static void node_init(node_t *node, node_t *parent, const char *path) {
    node->wd = inotify_add_watch(inotify_fd, path, IN_CLOSE_WRITE|IN_CREATE|IN_DELETE|IN_DELETE_SELF);
    if (node->wd == -1)
        die("cannot inotify_add_watch on %s", path);

    node->parent = parent;
    node->path = strdup(path);
    node->enforce_mem = 0;
    node->L = lua_newstate(node_alloc, node);

    HASH_ADD(by_wd, nodes, wd, sizeof(int), node);

    DIR *dp;
            
    dp = opendir(path);
    if (dp == NULL)
        die("cannot open directory");

    // recurse into directories
    struct dirent *ep;
    while (ep = readdir(dp)) {
        if (ep->d_name[0] == '.') 
            continue;

        if (ep->d_type == DT_DIR) {
            char child_path[PATH_MAX];
            snprintf(child_path, sizeof(child_path), "%s/%s", node->path, ep->d_name);
            node_add_child(node, child_path);
        } else if (ep->d_type == DT_REG) {
            node_update_content(node, ep->d_name);
        }
    }
    closedir(dp);
}

static void node_free(node_t *node) {
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
            die("error reading from inotfy fd");
        }

        char *pos = inotify_buffer;
        char *end = pos + size;
        while (pos < end) {
            struct inotify_event *event = (struct inotify_event*)pos;
            pos += sizeof(struct inotify_event) + event->len;

            if (event->len && event->name[0] == '.')
                continue; // ignore dot files

            node_t *node;
            HASH_FIND(by_wd, nodes, &event->wd, sizeof(int), node);
            if (!node) 
                die("node not found");

            char child_path[PATH_MAX];
            snprintf(child_path, sizeof(child_path), "%s/%s", node->path, event->name);
            printf("event for %s\n", child_path);

            printf("mask: %d\n", event->mask);
            if (event->mask & IN_CREATE) {
                struct stat stat_buf;
                if (stat(child_path, &stat_buf) == -1)
                    die("cannot stat %s", child_path);

                if (S_ISDIR(stat_buf.st_mode)) {
                    node_add_child(node, child_path);
                } else if (S_ISREG(stat_buf.st_mode)) {
                    node_update_content(node, event->name);
                }
            } else if (event->mask & IN_CLOSE_WRITE) {
                node_update_content(node, event->name);
            } else if (event->mask & IN_DELETE) {
                node_remove(node, event->name);
            }
        }
    }
}
    

static void tick(int delta) {
    check_inotify();
    event_loop(EVLOOP_NONBLOCK);
}

int main(int argc, char *argv[]) {

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd == -1)
        die("cannot open inotify");

    node_init(&root, NULL, "data");

    event_init();
    glfwInit();

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
