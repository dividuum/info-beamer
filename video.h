/* See Copyright Notice in LICENSE.txt */

#ifndef VIDEO_H
#define VIDEO_H

int video_register(lua_State *L);
int video_load(lua_State *L, const char *path, const char *name);

#endif
