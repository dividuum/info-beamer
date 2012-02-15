#ifndef TEXTURE_H
#define TEXTURE_H

int make_framebuffer(int width, int height, int *tex, int *fbo);
int recycle_framebuffer(int width, int height, int tex, int fbo);

#endif
