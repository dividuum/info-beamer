/* See Copyright Notice in LICENSE.txt */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/gl.h>
#include <GL/glfw.h>
#include <FTGL/ftgl.h>
#include <lauxlib.h>
#include <lualib.h>

#include "misc.h"

typedef struct {
    FTGLfont *font;
    GLuint tex;
} font_t;

LUA_TYPE_DECL(font);

/* Instance methods */

static unsigned char to_gl(GLfloat value) {
    value *= 255;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    return (unsigned char)value;
}

static int font_write(lua_State *L) {
    font_t *font = checked_font(L, 1);
    GLfloat x = luaL_checknumber(L, 2);
    GLfloat y = luaL_checknumber(L, 3);
    const char *text = luaL_checkstring(L, 4);
    GLfloat size = luaL_checknumber(L, 5) / 1000.0;

    // int prev_tex;
    // glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    int type = lua_type(L, 6);
    if (type == LUA_TNUMBER) {
        GLfloat r = luaL_checknumber(L, 6);
        GLfloat g = luaL_checknumber(L, 7);
        GLfloat b = luaL_checknumber(L, 8);
        GLfloat a = luaL_checknumber(L, 9);

        glColor4f(r,g,b,a);

        // XXX: HACK: Die Schrift bekommt hier
        // eine vorgefaerbte Texture verpasst.
        // Eigentlich sollte das auch ohne moeglich
        // sein, aber dann verschwinden die
        // Schriften, sobald ein Shader aktiv ist.

        glBindTexture(GL_TEXTURE_2D, font->tex);

        unsigned char texel[4] = {
            to_gl(r), to_gl(g), to_gl(b), to_gl(a)
        };
        glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                1,
                1,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                texel
        );
    } else if (type == LUA_TUSERDATA) {
        lua_pushliteral(L, "texid");
        lua_gettable(L, 6);
        if (lua_type(L, -1) != LUA_TFUNCTION)
            return luaL_error(L, "no texid() function");
        lua_pushvalue(L, 6);
        lua_call(L, 1, 1);
        if (lua_type(L, -1) != LUA_TNUMBER)
            return luaL_error(L, "texid() did not return number");
        int tex_id = lua_tonumber(L, -1);
        glColor4f(1,1,1,1);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        lua_pop(L, 1);
    } else {
        return luaL_error(L, "missing color/texture argument");
    }

    glPushMatrix();
        glTranslatef(x, y, 0);
        glTranslatef(0, size * 800, 0);
        glScalef(size, -size, 1.0);
        ftglRenderFont(font->font, text, FTGL_RENDER_ALL);
    glPopMatrix();

    // glBindTexture(GL_TEXTURE_2D, prev_tex);

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
    ftglSetFontDisplayList(ftgl_font, 1);

    if (!ftgl_font)
        luaL_error(L, "cannot load font file %s", name);

    ftglSetFontFaceSize(ftgl_font, 1000, 1000);
    // ftglSetFontDepth(ftgl_font, 0.1);
    // ftglSetFontOutset(ftgl_font, 0, 3);
    ftglSetFontCharMap(ftgl_font, ft_encoding_unicode);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    unsigned char pixels[] = {255, 255, 255, 128};
    glTexImage2D(GL_TEXTURE_2D, 0, 4, 1, 1, 0, 
        GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    font_t *font = push_font(L);
    font->font = ftgl_font;
    font->tex = tex;
    return 1;
}

static int font_gc(lua_State *L) {
    font_t *font = to_font(L, 1);
    ftglDestroyFont(font->font);
    glDeleteTextures(1, &font->tex);
    fprintf(stderr, "gc'ing font\n");
    return 0;
}

LUA_TYPE_IMPL(font);
