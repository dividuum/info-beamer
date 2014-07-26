/* See Copyright Notice in LICENSE.txt */

#ifndef MISC_H
#define MISC_H

#include <sys/time.h>
#include <stdint.h>

#include <GL/gl.h>
#include <lualib.h>

#define LITERAL_SIZE(x) (sizeof(x) - 1)
#define LITERAL_AND_SIZE(x) x, LITERAL_SIZE(x)

#define RED(string)    "[31m" string "[0m"
#define GREEN(string)  "[32m" string "[0m"
#define YELLOW(string) "[33m" string "[0m"
#define BLUE(string)   "[34m" string "[0m"
#define CYAN(string)   "[36m" string "[0m"
#define WHITE(string)  "[37m" string "[0m"

#define INFO(str) WHITE("[" __FILE__ "]") " " str
#define ERROR(str) RED("[" __FILE__ "]") " " str

#define CLAMP(val, min, max) ((val) > (max) ? (max) : ((val) < (min) ? (min) : (val)))

void die(const char *fmt, ...);
void *xmalloc(size_t size);
double time_delta(struct timeval *before, struct timeval *after);
int check_utf8(const char* s);

extern GLuint default_tex;
extern struct event_base *event_base;
extern struct evdns_base *dns_base;

// Simple Lua binder
// Based on http://lua-users.org/wiki/UserDataWithPointerExample

#define LUA_TYPE_DECL(type) \
static type##_t *to_##type(lua_State *L, int index);                    \
static type##_t *checked_##type(lua_State *L, int index);               \
static type##_t *push_##type(lua_State *L);

#define LUA_TYPE_IMPL(type) \
static type##_t *to_##type(lua_State *L, int index) {                   \
    type##_t *obj = (type##_t *)lua_touserdata(L, index);               \
    if (!obj) luaL_typerror(L, index, #type);                           \
    return obj;                                                         \
}                                                                       \
                                                                        \
static type##_t *checked_##type(lua_State *L, int index) {              \
    luaL_checktype(L, index, LUA_TUSERDATA);                            \
    type##_t *obj = (type##_t *)luaL_checkudata(L, index, #type);       \
    if (!obj) luaL_typerror(L, index, #type);                           \
    return obj;                                                         \
}                                                                       \
                                                                        \
static type##_t *push_##type(lua_State *L) {                            \
    type##_t *obj = (type##_t *)lua_newuserdata(L, sizeof(type##_t));   \
    luaL_getmetatable(L, #type);                                        \
    lua_setmetatable(L, -2);                                            \
    return obj;                                                         \
}                                                                       \
                                                                        \
static int type##_tostring(lua_State *L) {                              \
    lua_pushfstring(L, "<" #type " %p>", lua_touserdata(L, 1));         \
    return 1;                                                           \
}                                                                       \
                                                                        \
static const luaL_reg type##_meta[] = {                                 \
    {"__gc",       type##_gc},                                          \
    {"__tostring", type##_tostring},                                    \
    {0, 0}                                                              \
};                                                                      \
                                                                        \
int type##_register(lua_State *L) {                                     \
    luaL_openlib(L, #type, type##_methods, 0);                          \
    luaL_newmetatable(L, #type);                                        \
    luaL_openlib(L, 0, type##_meta, 0);                                 \
    lua_pushliteral(L, "__index");                                      \
    lua_pushvalue(L, -3);                                               \
    lua_rawset(L, -3);                                                  \
    lua_pushliteral(L, "__metatable");                                  \
    lua_pushvalue(L, -3);                                               \
    lua_rawset(L, -3);                                                  \
    lua_pop(L, 1);                                                      \
    return 1;                                                           \
}

#endif
