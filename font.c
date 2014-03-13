/* See Copyright Notice in LICENSE.txt */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/gl.h>
#include <FTGL/ftgl.h>
#include <lauxlib.h>
#include <lualib.h>

#include "misc.h"
#include "shader.h"

typedef struct {
    FTGLfont *font;
} font_t;

LUA_TYPE_DECL(font)

/* Instance methods */

static int font_write(lua_State *L) {
    font_t *font = checked_font(L, 1);
    GLfloat x = luaL_checknumber(L, 2);
    GLfloat y = luaL_checknumber(L, 3);
    const char *text = luaL_checkstring(L, 4);

    // Protect FTGL
    if (!check_utf8(text))
        return luaL_error(L, "invalid utf8");

    GLfloat size = luaL_checknumber(L, 5) / 1000.0;

    int type = lua_type(L, 6);
    if (type == LUA_TNUMBER) {
        GLfloat r = luaL_checknumber(L, 6);
        GLfloat g = luaL_checknumber(L, 7);
        GLfloat b = luaL_checknumber(L, 8);
        GLfloat a = luaL_optnumber(L, 9, 1.0);

        shader_set_gl_color(r, g, b, a);
        glBindTexture(GL_TEXTURE_2D, default_tex);
    } else if (type == LUA_TUSERDATA || type == LUA_TTABLE) {
        lua_pushliteral(L, "texid");
        lua_gettable(L, 6);
        if (lua_type(L, -1) != LUA_TFUNCTION)
            return luaL_argerror(L, 6, "no texid() function");
        lua_pushvalue(L, 6);
        lua_call(L, 1, 1);
        if (lua_type(L, -1) != LUA_TNUMBER)
            return luaL_argerror(L, 6, "texid() did not return number");
        int tex_id = lua_tonumber(L, -1);
        lua_pop(L, 1);

        shader_set_gl_color(1.0, 1.0, 1.0, 1.0);
        glBindTexture(GL_TEXTURE_2D, tex_id);
    } else {
        return luaL_argerror(L, 6, "unsupported value. must be RGBA or texturelike");
    }

    glPushMatrix();
        glTranslatef(x, y, 0);
        glTranslatef(0, size * 800, 0);
        glScalef(size, -size, 1.0);
        ftglRenderFont(font->font, text, FTGL_RENDER_ALL);
    glPopMatrix();

    lua_pushnumber(L, ftglGetFontAdvance(font->font, text) * size);
    return 1;
}

static const luaL_reg font_methods[] = {
    {"write",       font_write},
    {0,0}
};

/* Lifecycle */

int font_new(lua_State *L, const char *path, const char *name) {
    FTGLfont *ftgl_font = ftglCreatePolygonFont(path);
    if (!ftgl_font)
        return luaL_error(L, "cannot load font file %s", path);

    ftglSetFontDisplayList(ftgl_font, 1);
    ftglSetFontFaceSize(ftgl_font, 1000, 1000);
    ftglSetFontCharMap(ftgl_font, ft_encoding_unicode);

    font_t *font = push_font(L);
    font->font = ftgl_font;
    return 1;
}

static int font_gc(lua_State *L) {
    font_t *font = to_font(L, 1);
    ftglDestroyFont(font->font);
    fprintf(stderr, INFO("gc'ing font\n"));
    return 0;
}

LUA_TYPE_IMPL(font)
