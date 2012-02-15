#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

void make_framebuffer(int width, int height, unsigned int *tex, unsigned int *fbo);
void recycle_framebuffer(int width, int height, unsigned int tex, unsigned int fbo);

#endif
