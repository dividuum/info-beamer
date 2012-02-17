/* See Copyright Notice in LICENSE.txt */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/gl.h>
#include <GL/glfw.h>
#include <FTGL/ftgl.h>
#include <lauxlib.h>
#include <lualib.h>

#define FONT "font"

typedef struct {
    FTGLfont *font;
} font_t;

static font_t *to_font(lua_State *L, int index) {
    font_t *font = (font_t *)lua_touserdata(L, index);
    if (!font) luaL_typerror(L, index, FONT);
    return font;
}

static font_t *checked_font(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TUSERDATA);
    font_t *font = (font_t *)luaL_checkudata(L, index, FONT);
    if (!font) luaL_typerror(L, index, FONT);
    return font;
}

static font_t *push_font(lua_State *L) {
    font_t *font = (font_t *)lua_newuserdata(L, sizeof(font_t));
    luaL_getmetatable(L, FONT);
    lua_setmetatable(L, -2);
    return font;
}

static int font_write(lua_State *L) {
    font_t *font = checked_font(L, 1);
    GLfloat x = luaL_checknumber(L, 2);
    GLfloat y = luaL_checknumber(L, 3);
    const char *text = luaL_checkstring(L, 4);
    GLfloat size = luaL_checknumber(L, 5) / 1000.0;
    GLfloat r = luaL_checknumber(L, 6);
    GLfloat g = luaL_checknumber(L, 7);
    GLfloat b = luaL_checknumber(L, 8);
    GLfloat a = luaL_checknumber(L, 9);

    glDisable(GL_TEXTURE_2D);

    glPushMatrix();
        glTranslatef(x, y, 0);
        glTranslatef(0, size * 800, 0);
        glScalef(size, -size, 1.0);
        glColor4f(r, g, b, a);
        ftglRenderFont(font->font, text, FTGL_RENDER_ALL);
    glPopMatrix();

    glEnable(GL_TEXTURE_2D);

    lua_pushnumber(L, ftglGetFontAdvance(font->font, text) * size);
    return 1;
}

static const luaL_reg font_methods[] = {
    {"write",       font_write},
    {0,0}
};

static int font_gc(lua_State *L) {
    font_t *font = to_font(L, 1);
    ftglDestroyFont(font->font);
    fprintf(stderr, "gc'ing font\n");
    return 0;
}

static int font_tostring(lua_State *L) {
    lua_pushfstring(L, "font: %p", lua_touserdata(L, 1));
    return 1;
}

static const luaL_reg font_meta[] = {
    {"__gc",       font_gc},
    {"__tostring", font_tostring},
    {0, 0}
};


int font_register (lua_State *L) {
    luaL_openlib(L, FONT, font_methods, 0);  /* create methods table,
                                                  add it to the globals */
    luaL_newmetatable(L, FONT);        /* create metatable for Image,
                                           add it to the Lua registry */
    luaL_openlib(L, 0, font_meta, 0);  /* fill metatable */
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

int font_new(lua_State *L, const char *path, const char *name) {
    FTGLfont *ftgl_font = ftglCreatePolygonFont(path);
    // FTGLfont *ftgl_font = ftglCreateTextureFont(path);

    if (!ftgl_font)
        luaL_error(L, "cannot load font file %s", name);

    ftglSetFontFaceSize(ftgl_font, 1000, 1000);
    // ftglSetFontDepth(ftgl_font, 0.1);
    // ftglSetFontOutset(ftgl_font, 0, 3);
    ftglSetFontCharMap(ftgl_font, ft_encoding_unicode);

    font_t *font = push_font(L);
    font->font = ftgl_font;
    return 1;
}
