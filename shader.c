/* See Copyright Notice in LICENSE.txt */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/glfw.h>
#include <FTGL/ftgl.h>
#include <lauxlib.h>
#include <lualib.h>

#include "misc.h"

typedef struct {
    GLuint fs;
    GLuint vs;
    GLuint po;
} shader_t;

LUA_TYPE_DECL(shader);

/* Instance methods */

static int shader_use(lua_State *L) {
    shader_t *shader = checked_shader(L, 1);
    glUseProgram(shader->po);

    // No variables?
    if (lua_type(L, 2) == LUA_TNIL)
        return 0;
    
    luaL_checktype(L, 2, LUA_TTABLE);

    lua_pushnil(L);
    while (lua_next(L, -2)) {
        // name kopieren und nach string konvertieren
        // => [name] [value] [converted name]
        lua_pushvalue(L, -2); 
        const char *name = lua_tostring(L, -1);

        int type = lua_type(L, -2);
        if (type == LUA_TNUMBER) {
            GLfloat value = lua_tonumber(L, -2);
            GLint loc = glGetUniformLocation(shader->po, name);
            if (loc == -1)
                return luaL_error(L, "unknown uniform name %s", name);
            glUniform1f(loc, value);
        } else {
            return luaL_error(L, "unsupported value type");
        }
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    return 0;
}

static const luaL_reg shader_methods[] = {
    {"use",       shader_use},
    {0,0}
};

/* Lifecycle */

int shader_new(lua_State *L, const char *vertex, const char *fragment) {
    char *fault = "";
    char log[1024];
    GLint status;
    GLsizei log_len;
    GLuint fs = 0, vs = 0, po = 0;

    // Pixel
    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertex, NULL);
    glCompileShader(vs);

    glGetObjectParameterivARB(vs, GL_COMPILE_STATUS, &status);
    if (!status) {
        fault = "compiling pixel shader";
        glGetShaderInfoLog(vs, sizeof(log), &log_len, log);
        if (log_len > 0) 
            goto error;
    }

    // Fragment
    fs =  glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragment, NULL);
    glCompileShader(fs);

    glGetObjectParameterivARB(fs, GL_COMPILE_STATUS, &status);
    if (!status) {
        fault = "compiling fragment shader";
        glGetShaderInfoLog(fs, sizeof(log), &log_len, log);
        if (log_len > 0) 
            goto error;
    }

    // Program Object
    po = glCreateProgram();
    glAttachShader(po, vs);
    glAttachShader(po, fs);
    glLinkProgram(po);

    glGetProgramiv(po, GL_LINK_STATUS, &status);
    if (!status) {
        fault = "linking program";
        glGetProgramInfoLog(fs, sizeof(log), &log_len, log);
        if (log_len > 0) 
            goto error;
    }

    shader_t *shader = push_shader(L);
    shader->fs = fs;
    shader->vs = vs;
    shader->po = po;
    return 1;

error:
    if (po)
        glDeleteProgram(po);
    if (vs)
        glDeleteShader(vs);
    if (fs)
        glDeleteShader(fs);
    return luaL_error(L, "While %s: %s", fault, log);
}

static int shader_gc(lua_State *L) {
    shader_t *shader = to_shader(L, 1);
    glDeleteProgram(shader->po);
    glDeleteShader(shader->vs);
    glDeleteShader(shader->fs);
    return 0;
}

LUA_TYPE_IMPL(shader);

