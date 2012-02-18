/* See Copyright Notice in LICENSE.txt */

#ifndef SHADER_H
#define SHADER_H

int shader_register(lua_State *L);
int shader_new(lua_State *L, const char *vertex, const char *fragment);

#endif
