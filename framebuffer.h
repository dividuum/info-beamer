/* See Copyright Notice in LICENSE.txt */

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <GL/gl.h>

void make_framebuffer(int width, int height, GLuint *tex, GLuint *fbo);
void recycle_framebuffer(int width, int height, GLuint tex, GLuint fbo);

#endif
