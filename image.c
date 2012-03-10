/* See Copyright Notice in LICENSE.txt */

/* JPEG loader is based on jpeg.c from
 * http://tfc.duke.free.fr/
 *
 * License: "All these programs are Open Source. Some old
 * works haven't been tagged yet with a licence. I assume
 * they are licensed under the MIT-license, like a majority
 * of my other works."
 *
 *
 * PNG loader is based on
 * http://files.slembcke.net/misc/PNGTexture.tar.gz
 *
 * License: unknown
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/glew.h>
#include <GL/gl.h>
#include <IL/il.h>
#include <IL/ilu.h>
#include <lauxlib.h>
#include <lualib.h>

#include "framebuffer.h"
#include "misc.h"

typedef struct {
    GLuint tex;
    GLuint fbo;
    int width;
    int height;
} image_t;

LUA_TYPE_DECL(image);


/* Instance methods */

static int image_size(lua_State *L) {
    image_t *image = checked_image(L, 1);
    lua_pushnumber(L, image->width);
    lua_pushnumber(L, image->height);
    return 2;
}

static int image_draw(lua_State *L) {
    image_t *image = checked_image(L, 1);
    GLfloat x1 = luaL_checknumber(L, 2);
    GLfloat y1 = luaL_checknumber(L, 3);
    GLfloat x2 = luaL_checknumber(L, 4);
    GLfloat y2 = luaL_checknumber(L, 5);
    GLfloat alpha = luaL_optnumber(L, 6, 1.0);

    glBindTexture(GL_TEXTURE_2D, image->tex);
    glColor4f(1.0, 1.0, 1.0, alpha);

    glBegin(GL_QUADS); 
        glTexCoord2f(0.0, 1.0); glVertex3f(x1, y1, 0);
        glTexCoord2f(1.0, 1.0); glVertex3f(x2, y1, 0);
        glTexCoord2f(1.0, 0.0); glVertex3f(x2, y2, 0);
        glTexCoord2f(0.0, 0.0); glVertex3f(x1, y2, 0);
    glEnd();

    return 0;
}

static int image_texid(lua_State *L) {
    image_t *image = checked_image(L, 1);
    lua_pushnumber(L, image->tex);
    return 1;
}

static const luaL_reg image_methods[] = {
    {"draw",    image_draw},
    {"size",    image_size},
    {"texid",   image_texid},
    {0,0}
};

/* Lifecycle */

int image_create(lua_State *L, GLuint tex, GLuint fbo, int width, int height) {
    image_t *image = push_image(L);
    image->tex = tex;
    image->fbo = fbo;
    image->width = width;
    image->height = height;
    return 1;
}

int image_from_current_framebuffer(lua_State *L, int width, int height) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

    image_t *image = push_image(L);
    image->tex = tex;
    image->fbo = 0;
    image->width = width;
    image->height = height;
    return 1;
}

int image_load(lua_State *L, const char *path, const char *name) {
    ILuint imageID;
    ilGenImages(1, &imageID);
    ilBindImage(imageID);
 
    if (!ilLoadImage(path)) {
        ilDeleteImages(1, &imageID);
        return luaL_error(L, "image loading failed: %s", 
            iluErrorString(ilGetError()));
    }
 
    ILinfo ImageInfo;
    iluGetImageInfo(&ImageInfo);

    if (ImageInfo.Origin == IL_ORIGIN_UPPER_LEFT)
        iluFlipImage();
 
    if (!ilConvertImage(IL_RGBA, IL_UNSIGNED_BYTE)) {
        ilDeleteImages(1, &imageID);
        return luaL_error(L, "converting image failed: %s", 
            iluErrorString(ilGetError()));
    }

    int width = ilGetInteger(IL_IMAGE_WIDTH);
    int height = ilGetInteger(IL_IMAGE_HEIGHT);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, ilGetInteger(IL_IMAGE_BPP), width, height, 0,
                 ilGetInteger(IL_IMAGE_FORMAT), GL_UNSIGNED_BYTE, ilGetData());
    glGenerateMipmap(GL_TEXTURE_2D);
 
    ilDeleteImages(1, &imageID);
 
    image_t *image = push_image(L);
    image->tex = tex;
    image->fbo = 0;
    image->width = width;
    image->height = height;
    return 1;
}

static int image_gc(lua_State *L) {
    image_t *image = to_image(L, 1);
    if (image->fbo) {
        // If images has attached Framebuffer, put the
        // texture and framebuffer into the recycler.
        // Allocations for new framebuffers can then
        // reuse these => Better performance.
        recycle_framebuffer(image->width, image->height, 
            image->tex, image->fbo);
    } else {
        // No Framebuffer? Just remove the texture.
        glDeleteTextures(1, &image->tex);
    }
    return 0;
}

LUA_TYPE_IMPL(image);
