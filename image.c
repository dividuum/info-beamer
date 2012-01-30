#include <stdio.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lualib.h>

#define IMAGE "image"

typedef struct {
    int tex;
    int width;
    int height;
} image_t;

static image_t *to_image(lua_State *L, int index) {
    image_t *image = (image_t *)lua_touserdata(L, index);
    if (!image) luaL_typerror(L, index, IMAGE);
    return image;
}

static image_t *checked_image(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TUSERDATA);
    image_t *image = (image_t *)luaL_checkudata(L, index, IMAGE);
    if (!image) luaL_typerror(L, index, IMAGE);
    return image;
}

static image_t *push_image(lua_State *L) {
    image_t *image = (image_t *)lua_newuserdata(L, sizeof(image_t));
    luaL_getmetatable(L, IMAGE);
    lua_setmetatable(L, -2);
    return image;
}

static int image_new(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    printf("creaintgg\n");
    image_t *image = push_image(L);
    image->tex = 1234;
    return 1;
}

static int image_draw(lua_State *L) {
    image_t *image = checked_image(L, 1);
    printf("??? tex: %d\n", image->tex);
    return 0;
}

static const luaL_reg image_methods[] = {
  {"new",           image_new},
  {"draw",          image_draw},
  {0,0}
};

static int image_gc(lua_State *L) {
    image_t *image = to_image(L, 1);
    printf("discarding tex: %d\n", image->tex);
    return 0;
}

static int image_tostring(lua_State *L) {
    lua_pushfstring(L, "image: %p", lua_touserdata(L, 1));
    return 1;
}

static const luaL_reg image_meta[] = {
    {"__gc",       image_gc},
    {"__tostring", image_tostring},
    {0, 0}
};


int image_register (lua_State *L) {
    luaL_openlib(L, IMAGE, image_methods, 0);  /* create methods table,
                                                  add it to the globals */
    luaL_newmetatable(L, IMAGE);        /* create metatable for Image,
                                           add it to the Lua registry */
    luaL_openlib(L, 0, image_meta, 0);  /* fill metatable */
    lua_pushliteral(L, "__index");
    lua_pushvalue(L, -3);               /* dup methods table*/
    lua_rawset(L, -3);                  /* metatable.__index = methods */
    lua_pushliteral(L, "__metatable");
    lua_pushvalue(L, -3);               /* dup methods table*/
    lua_rawset(L, -3);                  /* hide metatable:
                                           metatable.__metatable = methods */
    lua_pop(L, 1);                      /* drop metatable */
    return 1;                           /* return methods on the stack */
}
