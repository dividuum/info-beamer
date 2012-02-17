/* See Copyright Notice in LICENSE.txt */

#ifndef FONT_H
#define FONT_H

int font_register (lua_State *L);
int font_new(lua_State *L, const char *path, const char *name);

#endif
