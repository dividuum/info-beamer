/* See Copyright Notice in LICENSE.txt */

#ifndef MISC_H
#define MISC_H

#include <GL/gl.h>

void die(const char *fmt, ...);
void *xmalloc(size_t size);

extern GLuint default_tex;

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
