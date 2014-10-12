/* See Copyright Notice in LICENSE.txt */

#ifndef IMAGE_H
#define IMAGE_H

int image_register(lua_State *L);
int image_create(lua_State *L, int tex, int fbo, int width, int height);
int image_from_current_framebuffer(lua_State *L, int x, int y, int width, int height, int mipmap);
int image_from_color(lua_State *L, GLfloat r, GLfloat g, GLfloat b, GLfloat a);
int image_load(lua_State *L, const char *path, const char *name);

#endif
