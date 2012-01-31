#ifndef IMAGE_H
#define IMAGE_H

int image_register (lua_State *L);
int image_create(lua_State *L, int tex, int fbo, int width, int height);
int image_load(lua_State *L, const char *path, const char *name);

#endif
