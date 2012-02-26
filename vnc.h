#ifndef VNC_H
#define VNC_H

int vnc_create(lua_State *L, const char *host, int port);
int vnc_register (lua_State *L);

#endif
