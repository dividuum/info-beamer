#ifndef IMAGE_H
#define IMAGE_H

int image_register (lua_State *L);
int image_new(lua_State *L, const char *path, const char *name);

#endif
