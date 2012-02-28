/* See Copyright Notice in LICENSE.txt */

#define _BSD_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <GL/glew.h>
#include <GL/gl.h>
#include <lauxlib.h>
#include <lualib.h>
#include <event.h>

#include "misc.h"

typedef struct vnc_s vnc_t;
typedef void(*protocol_handler)(vnc_t *);

typedef struct {
    uint8_t bpp;
    uint8_t depth;
    uint8_t bigendian;
    uint8_t truecolor;
    uint16_t red_max;
    uint16_t green_max;
    uint16_t blue_max;
    uint8_t red_shift;
    uint8_t green_shift;
    uint8_t blue_shift;
    uint8_t padding[3];
} pixelformat_t;

typedef struct vnc_s {
    GLuint tex;
    int width;
    int height;
    struct bufferevent *buf_ev;

    char *host;
    int port;
    int alive;

    protocol_handler handler;
    int num_bytes;

    pixelformat_t pixelformat;

    // state for rect update
    int num_rects;
    int rect_x;
    int rect_y;
    int rect_w;
    int rect_h;
} vnc_t;

LUA_TYPE_DECL(vnc);

/* Instance methods */

static int vnc_size(lua_State *L) {
    vnc_t *vnc = checked_vnc(L, 1);
    lua_pushnumber(L, vnc->width);
    lua_pushnumber(L, vnc->height);
    return 2;
}

static int vnc_draw(lua_State *L) {
    vnc_t *vnc = checked_vnc(L, 1);
    GLfloat x1 = luaL_checknumber(L, 2);
    GLfloat y1 = luaL_checknumber(L, 3);
    GLfloat x2 = luaL_checknumber(L, 4);
    GLfloat y2 = luaL_checknumber(L, 5);
    GLfloat alpha = luaL_optnumber(L, 6, 1.0);

    glBindTexture(GL_TEXTURE_2D, vnc->tex);
    glColor4f(1.0, 1.0, 1.0, alpha);

    glBegin(GL_QUADS); 
        glTexCoord2f(0.0, 1.0); glVertex3f(x1, y1, 0);
        glTexCoord2f(1.0, 1.0); glVertex3f(x2, y1, 0);
        glTexCoord2f(1.0, 0.0); glVertex3f(x2, y2, 0);
        glTexCoord2f(0.0, 0.0); glVertex3f(x1, y2, 0);
    glEnd();

    return 0;
}

static int vnc_alive(lua_State *L) {
    vnc_t *vnc = checked_vnc(L, 1);
    lua_pushboolean(L, vnc->alive);
    return 1;
}

static int vnc_texid(lua_State *L) {
    vnc_t *vnc = checked_vnc(L, 1);
    lua_pushnumber(L, vnc->tex);
    return 1;
}

static const luaL_reg vnc_methods[] = {
    {"draw",    vnc_draw},
    {"size",    vnc_size},
    {"alive",   vnc_alive},
    {"texid",   vnc_texid},
    {0,0}
};

/* Protocol utils */

static void vnc_printf(vnc_t *vnc, const char *fmt, ...) {
    char buffer[16384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    fprintf(stderr, CYAN("[vnc@%s:%d]")" %s", vnc->host, vnc->port, buffer);
}

static void vnc_set_handler(vnc_t *vnc, protocol_handler handler, int num_bytes) {
    vnc->handler = handler;
    vnc->num_bytes = num_bytes;
    if (evbuffer_get_length(vnc->buf_ev->input) >= vnc->num_bytes)
        vnc->handler(vnc);
}

static void vnc_close(vnc_t *vnc) {
    if (vnc->buf_ev) {
        vnc_printf(vnc, "connection closed\n");
        bufferevent_free(vnc->buf_ev);
        vnc->buf_ev = NULL;
    }
    if (vnc->tex) {
        glDeleteTextures(1, &vnc->tex);
        vnc->tex = 0;
    }
    vnc->alive = 0;
}

static void vnc_read(struct bufferevent *bev, void *arg) {
    vnc_t *vnc = arg;
    if (evbuffer_get_length(bev->input) >= vnc->num_bytes)
        vnc->handler(vnc);
}

static void vnc_event(struct bufferevent *bev, short events, void *arg) {
    vnc_t *vnc = arg;
    if (events & BEV_EVENT_CONNECTED) {
        vnc_printf(vnc, "connected!\n");
    } else if (events & BEV_EVENT_ERROR) {
        int err = bufferevent_socket_get_dns_error(bev);
        if (err) {
            vnc_printf(vnc, "dns error: %s\n", evutil_gai_strerror(err));
        } else {
            vnc_printf(vnc, "connection error!\n");
        }
        return vnc_close(vnc);
    } else if (events & BEV_EVENT_EOF) {
        vnc_printf(vnc, "eof!\n");
        return vnc_close(vnc);
    }
}

static const int endian_test = 1;
#define is_bigendian() ((*(char*)&endian_test) == 0)
#define swap32(v) ((((v) & 0xff000000) >> 24) | \
                   (((v) & 0x00ff0000) >> 8)  | \
                   (((v) & 0x0000ff00) << 8)  | \
                   (((v) & 0x000000ff) << 24))

static int vnc_decode(vnc_t *vnc, const unsigned char *pixels) {
    unsigned char *converted = malloc(vnc->rect_w * vnc->rect_h * 4);

    assert(vnc->pixelformat.bpp == 32);
    int row_size = vnc->rect_w * 4;

    for (int row = 0; row < vnc->rect_h; row++) {
        uint32_t *src = (uint32_t*)(pixels + row * row_size);
        uint32_t *dest = (uint32_t*)(converted + (vnc->rect_h - row - 1) * row_size);
        for (int col = 0; col < vnc->rect_w; col++) {
            uint32_t raw = *src;
            if (is_bigendian() ^ vnc->pixelformat.bigendian) {
                raw = swap32(raw);
            }
            uint32_t r = 
                (raw >> vnc->pixelformat.red_shift) & 
                vnc->pixelformat.red_max;
            uint32_t g = 
                (raw >> vnc->pixelformat.green_shift) & 
                vnc->pixelformat.green_max;
            uint32_t b = 
                (raw >> vnc->pixelformat.blue_shift) & 
                vnc->pixelformat.blue_max;
            *dest = 255 << 24 | b << 16 | g << 8 | r;
            dest++, src++;
        }
    }

    glBindTexture(GL_TEXTURE_2D, vnc->tex);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        vnc->rect_x,
        vnc->height - vnc->rect_y - vnc->rect_h,
        vnc->rect_w,
        vnc->rect_h,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        converted 
    );
    glGenerateMipmap(GL_TEXTURE_2D);
    free(converted);
    return 1;
}

/* Protocol */
static void vnc_read_msg_header(vnc_t *vnc);
static void vnc_read_rect(vnc_t *vnc);
static void vnc_send_update_request(vnc_t *vnc, int x, int y, int w, int h, int incremental);

static void vnc_read_cut_text(vnc_t *vnc) {
    evbuffer_drain(vnc->buf_ev->input, vnc->num_bytes);
    return vnc_set_handler(vnc, vnc_read_msg_header, 1);
}

static void vnc_read_cut(vnc_t *vnc) {
    struct {
        uint8_t msg_type;
        uint8_t padding[3];
        uint32_t text_len;
    } server_cut_text;

    evbuffer_remove(vnc->buf_ev->input, &server_cut_text, sizeof(server_cut_text));

    uint32_t text_len = ntohl(server_cut_text.text_len);
    if (text_len > 2048) {
        vnc_printf(vnc, "too large server cut text\n");
        return vnc_close(vnc);
    }

    return vnc_set_handler(vnc, vnc_read_cut_text, text_len);
}

static void vnc_read_rect_data(vnc_t *vnc) {
    unsigned char *pixels = evbuffer_pullup(vnc->buf_ev->input, vnc->num_bytes);
    if (!vnc_decode(vnc, pixels)) {
        vnc_printf(vnc, "decoding failed\n");
        return vnc_close(vnc);
    }

    evbuffer_drain(vnc->buf_ev->input, vnc->num_bytes);

    if (--vnc->num_rects == 0) {
        return vnc_send_update_request(vnc, 0, 0, vnc->width, vnc->height, 1);
    } else {
        return vnc_set_handler(vnc, vnc_read_rect, 12);
    }
}

static void vnc_read_rect(vnc_t *vnc) {
    struct {
        uint16_t x;
        uint16_t y;
        uint16_t w;
        uint16_t h;
        uint32_t encoding;
    } server_rect;

    evbuffer_remove(vnc->buf_ev->input, &server_rect, sizeof(server_rect));

    vnc->rect_x = ntohs(server_rect.x);
    vnc->rect_y = ntohs(server_rect.y);
    vnc->rect_w = ntohs(server_rect.w);
    vnc->rect_h = ntohs(server_rect.h);

    if ((vnc->rect_x + vnc->rect_w > vnc->width) ||
        (vnc->rect_y + vnc->rect_h > vnc->height)) {
        vnc_printf(vnc, "invalid rect (out of bound)\n");
        return vnc_close(vnc);
    }
    return vnc_set_handler(vnc, vnc_read_rect_data, vnc->pixelformat.bpp / 8 * vnc->rect_w * vnc->rect_h);
}

static void vnc_read_rects(vnc_t *vnc) {
    struct {
        uint8_t msg_type;
        uint8_t padding[1];
        uint16_t num_rects;
    } server_frameupdate;

    evbuffer_remove(vnc->buf_ev->input, &server_frameupdate, sizeof(server_frameupdate));

    vnc->num_rects = ntohs(server_frameupdate.num_rects);
    if (vnc->num_rects == 0) {
        vnc_printf(vnc, "zero rect update\n");
        return vnc_close(vnc);
    }

    return vnc_set_handler(vnc, vnc_read_rect, 12);
}

static void vnc_read_msg_header(vnc_t *vnc) {
    // get without removing
    unsigned char *header = evbuffer_pullup(vnc->buf_ev->input, 1);

    uint8_t msg_type = header[0];
#define SERVER_MSG_TYPE_FRAMEBUFFER_UPDATE 0
#define SERVER_MSG_TYPE_BELL               2
#define SERVER_MSG_TYPE_CUT_TEXT           3

    if (msg_type == SERVER_MSG_TYPE_FRAMEBUFFER_UPDATE) {
        return vnc_set_handler(vnc, vnc_read_rects, 4);
    } else if (msg_type == SERVER_MSG_TYPE_BELL) {
        // ignore the bell
        evbuffer_drain(vnc->buf_ev->input, 1);
        return vnc_set_handler(vnc, vnc_read_msg_header, 1);
    } else if (msg_type == SERVER_MSG_TYPE_CUT_TEXT) {
        return vnc_set_handler(vnc, vnc_read_cut, 4);
    } else {
        vnc_printf(vnc, "unexpected msg_type\n");
        return vnc_close(vnc);
    }
}

static void vnc_send_update_request(vnc_t *vnc, int x, int y, int w, int h, int incremental) {
    struct {
        uint8_t msg_type;
#define CLIENT_MSG_TYPE_UPDATE_REQUEST 3
        uint8_t incremental;
        uint16_t x;
        uint16_t y;
        uint16_t w;
        uint16_t h;
    } update_request = {
        .msg_type = CLIENT_MSG_TYPE_UPDATE_REQUEST,
        .incremental = incremental,
        .x = htons(x),
        .y = htons(y),
        .w = htons(w),
        .h = htons(h),
    };
    bufferevent_write(vnc->buf_ev, &update_request, sizeof(update_request));
    return vnc_set_handler(vnc, vnc_read_msg_header, 1);
}

static void vnc_read_server_name(vnc_t *vnc) {
    evbuffer_drain(vnc->buf_ev->input, vnc->num_bytes);
    return vnc_send_update_request(vnc, 0, 0, vnc->width, vnc->height, 0);
}

static void vnc_read_server_init(vnc_t *vnc) {
    struct {
        uint16_t width;
        uint16_t height;
        pixelformat_t pixelformat;
        uint32_t name_len;
    } server_init;

    evbuffer_remove(vnc->buf_ev->input, &server_init, sizeof(server_init));

    uint32_t name_len = ntohl(server_init.name_len);
    if (name_len > 512) {
        vnc_printf(vnc, "name too long\n");
        return vnc_close(vnc);
    }

    if (server_init.pixelformat.bpp != 32) {
        vnc_printf(vnc, "invalid bpp (only 32bit supported)\n");
        return vnc_close(vnc);
    }

    vnc->width = ntohs(server_init.width);
    vnc->height = ntohs(server_init.height);
    vnc->pixelformat = server_init.pixelformat;
    vnc->pixelformat.red_max = ntohs(vnc->pixelformat.red_max);
    vnc->pixelformat.green_max = ntohs(vnc->pixelformat.green_max);
    vnc->pixelformat.blue_max = ntohs(vnc->pixelformat.blue_max);

    if (vnc->width > 1920 || vnc->height > 1080) {
        vnc_printf(vnc, "screen too large\n");
        return vnc_close(vnc);
    }

    glGenTextures(1, &vnc->tex);
    glBindTexture(GL_TEXTURE_2D, vnc->tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexImage2D(
        GL_TEXTURE_2D, 
        0, 
        GL_RGBA, 
        vnc->width, 
        vnc->height, 
        0, 
        GL_RGBA, 
        GL_UNSIGNED_BYTE,
        NULL
    );
    
    vnc_printf(vnc, "got screen: %dx%d\n", ntohs(server_init.width), ntohs(server_init.height));

    return vnc_set_handler(vnc, vnc_read_server_name, name_len);
}

static void vnc_read_security_type(vnc_t *vnc) {
    struct {
        uint32_t security_type;
#define SECURITY_NO_AUTH 1
    } auth;

    evbuffer_remove(vnc->buf_ev->input, &auth, sizeof(auth));
    if (ntohl(auth.security_type) != SECURITY_NO_AUTH) {
        vnc_printf(vnc, "unexpected security type\n");
        return vnc_close(vnc);
    }

    struct {
        uint8_t shared;
    } client_init = {
        .shared = 1
    };

    bufferevent_write(vnc->buf_ev, &client_init, sizeof(client_init));
    return vnc_set_handler(vnc, vnc_read_server_init, 24);
}

static void vnc_read_handshake(vnc_t *vnc) {
    uint8_t handshake[12];
    evbuffer_remove(vnc->buf_ev->input, handshake, sizeof(handshake));
    if (handshake[0] != 'R' || handshake[1] != 'F' || handshake[2] != 'B') {
        vnc_printf(vnc, "unexpected handshake packet\n");
        return vnc_close(vnc);
    }

    bufferevent_write(vnc->buf_ev, LITERAL_AND_SIZE("RFB 003.003\n"));
    return vnc_set_handler(vnc, vnc_read_security_type, 4);
}

/* Lifecycle */

int vnc_create(lua_State *L, const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return luaL_error(L, "cannot create new client socket");

    vnc_t *vnc = push_vnc(L);
    vnc->tex = 0;
    vnc->width = 0;
    vnc->height = 0;
    vnc->buf_ev = NULL;
    vnc->alive = 1;

    vnc->host = strdup(host);
    vnc->port = port;

    evutil_make_socket_nonblocking(fd);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(host);
    sin.sin_port = htons(port);

    vnc_printf(vnc, "connecting...\n");
    vnc->buf_ev = bufferevent_socket_new(event_base, -1, BEV_OPT_CLOSE_ON_FREE);
    vnc_set_handler(vnc, vnc_read_handshake, 12);

    bufferevent_setcb(vnc->buf_ev, vnc_read, NULL, vnc_event, vnc);
    bufferevent_enable(vnc->buf_ev, EV_READ | EV_WRITE);
    bufferevent_socket_connect_hostname(vnc->buf_ev, dns_base, AF_UNSPEC, host, port);
    return 1;
}

static int vnc_gc(lua_State *L) {
    vnc_t *vnc = to_vnc(L, 1);
    vnc_close(vnc);
    free(vnc->host);
    return 0;
}

LUA_TYPE_IMPL(vnc);
