#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/gl.h>
#include <lauxlib.h>
#include <lualib.h>
#include <png.h>
#include <jpeglib.h>

#define IMAGE "image"

typedef struct {
    int tex;
    int width;
    int height;
} image_t;

static int load_jpeg(const char *filename, int *width, int *height) {
    /* JPEG error manager */
    struct my_error_mgr {
        /* "public" fields */
        struct jpeg_error_mgr pub;
        /* for return to caller */
        jmp_buf setjmp_buffer;
    };

    typedef struct my_error_mgr* my_error_ptr;

    void err_exit (j_common_ptr cinfo) {
        /* Get error manager */
        my_error_ptr jerr = (my_error_ptr)(cinfo->err);

        /* Display error message */
        (*cinfo->err->output_message) (cinfo);

        /* Return control to the setjmp point */
        longjmp (jerr->setjmp_buffer, 1);
    }

    GLint internalFormat;
    GLenum format;
    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;
    JSAMPROW j;
    int i;
    char *pixels = NULL;

    /* Open image file */
    FILE *fp = fopen(filename, "rb"); 
    if (!fp) {
        fprintf(stderr, "error: couldn't open \"%s\"!\n", filename);
        return 0;
    }

    /* Create and configure decompressor */
    jpeg_create_decompress (&cinfo);
    cinfo.err = jpeg_std_error (&jerr.pub);
    jpeg_stdio_src(&cinfo, fp);

    jerr.pub.error_exit = err_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress (&cinfo);
        if (pixels) 
            free(pixels);
        return 0;
    }

    /* Read header and prepare for decompression */
    jpeg_read_header (&cinfo, TRUE);
    jpeg_start_decompress (&cinfo);

    /* Initialize image's member variables */
    *width = cinfo.image_width;
    *height = cinfo.image_height;
    internalFormat = cinfo.num_components;

    if (cinfo.num_components == 1)
        format = GL_LUMINANCE;
    else
        format = GL_RGB;

    pixels = (GLubyte *)malloc(
        sizeof (GLubyte) * (*width) * (*height) * internalFormat
    );

    /* Extract each scanline of the image */
    for (i = 0; i < *height; ++i) {
        j = (pixels + (
            (*height - (i + 1)) * (*width) * internalFormat
        ));
        jpeg_read_scanlines(&cinfo, &j, 1);
    }

    /* Finish decompression and release memory */
    jpeg_finish_decompress (&cinfo);
    jpeg_destroy_decompress (&cinfo);

    fclose(fp);

    GLuint tex;
    /* Generate texture */
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    /* Setup some parameters for texture filters and mipmapping */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLint alignment;
    glGetIntegerv (GL_UNPACK_ALIGNMENT, &alignment);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);

#if 0
    glTexImage2D (GL_TEXTURE_2D, 0, jpeg_tex->internalFormat,
            jpeg_tex->width, jpeg_tex->height, 0, jpeg_tex->format,
            GL_UNSIGNED_BYTE, jpeg_tex->texels);
#else
    gluBuild2DMipmaps(GL_TEXTURE_2D, internalFormat,
            *width, *height, format, GL_UNSIGNED_BYTE, pixels);
#endif


    /* OpenGL has its own copy of texture data */
    free(pixels);

    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    return tex;
}

static int load_png(const char *filename, int *width, int *height) {
    FILE *png_file = fopen(filename, "rb");
    if (!png_file) {
        fprintf(stderr, "cannot open %s\n", filename);
        return 0;
    }

    const int PNG_SIG_BYTES = 8;
    unsigned char header[PNG_SIG_BYTES];

    fread(header, 1, PNG_SIG_BYTES, png_file);
    if (png_sig_cmp(header, 0, PNG_SIG_BYTES)) {
        fprintf(stderr, "no png?\n");
        goto error_info;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fprintf(stderr, "cannot create read_struct?\n");
        goto error_info;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        fprintf(stderr, "cannot create info_struct?\n");
        goto error_info;
    }

    png_infop end_info = png_create_info_struct(png_ptr);
    if (!end_info) {
        fprintf(stderr, "cannot create info_struct?\n");
        goto error_info;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        fprintf(stderr, "cannot read file\n");
        goto error_read;
    }

    png_init_io(png_ptr, png_file);
    png_set_sig_bytes(png_ptr, PNG_SIG_BYTES);
    png_read_info(png_ptr, info_ptr);

    *width = png_get_image_width(png_ptr, info_ptr);
    *height = png_get_image_height(png_ptr, info_ptr);

    png_uint_32 bit_depth, color_type;
    bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);

    if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_gray_1_2_4_to_8(png_ptr);

    if (bit_depth == 16)
        png_set_strip_16(png_ptr);

    if(color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    else if(color_type == PNG_COLOR_TYPE_GRAY ||
            color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    {
        png_set_gray_to_rgb(png_ptr);
    }

    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    else
        png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);

    png_read_update_info(png_ptr, info_ptr);

    png_uint_32 rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    png_uint_32 numbytes = rowbytes*(*height);
    png_byte* pixels = malloc(numbytes);
    png_byte** row_ptrs = malloc((*height) * sizeof(png_byte*));

    int i;
    for (i=0; i<(*height); i++)
        row_ptrs[i] = pixels + ((*height) - 1 - i)*rowbytes;

    png_read_image(png_ptr, row_ptrs);

    free(row_ptrs);
    png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
    fclose(png_file);

    GLint alignment;
    glGetIntegerv (GL_UNPACK_ALIGNMENT, &alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, *width, *height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);

    glPixelStorei (GL_UNPACK_ALIGNMENT, alignment);

    fprintf(stderr, "got png: %dx%d\n", *width, *height);
    return tex;
    // XXX: Fehlerhandling scheint mir noch inkorrekt
error_read:
    png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
error_info:
    fclose(png_file);
    return 0;
}


static image_t *to_image(lua_State *L, int index) {
    image_t *image = (image_t *)lua_touserdata(L, index);
    if (!image) luaL_typerror(L, index, IMAGE);
    return image;
}

static image_t *checked_image(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TUSERDATA);
    image_t *image = (image_t *)luaL_checkudata(L, index, IMAGE);
    if (!image) luaL_typerror(L, index, IMAGE);
    return image;
}

static image_t *push_image(lua_State *L) {
    image_t *image = (image_t *)lua_newuserdata(L, sizeof(image_t));
    luaL_getmetatable(L, IMAGE);
    lua_setmetatable(L, -2);
    return image;
}

static int image_draw(lua_State *L) {
    image_t *image = checked_image(L, 1);
    GLfloat x1 = luaL_checknumber(L, 2);
    GLfloat y1 = luaL_checknumber(L, 3);
    GLfloat x2 = luaL_checknumber(L, 4);
    GLfloat y2 = luaL_checknumber(L, 5);

    glBindTexture(GL_TEXTURE_2D, image->tex);
    glColor4f(1.0, 1.0, 1.0, 1.0);
    glBegin(GL_QUADS); 
        glTexCoord2f(0.0, 1.0); glVertex3f(x1, y1, 0);
        glTexCoord2f(1.0, 1.0); glVertex3f(x2, y1, 0);
        glTexCoord2f(1.0, 0.0); glVertex3f(x2, y2, 0);
        glTexCoord2f(0.0, 0.0); glVertex3f(x1, y2, 0);
    glEnd();
    return 0;
}

static const luaL_reg image_methods[] = {
  {"draw",          image_draw},
  {0,0}
};

static int image_gc(lua_State *L) {
    image_t *image = to_image(L, 1);
    fprintf(stderr, "discarding tex: %d\n", image->tex);
    glDeleteTextures(1, &image->tex);
    return 0;
}

static int image_tostring(lua_State *L) {
    lua_pushfstring(L, "image: %p", lua_touserdata(L, 1));
    return 1;
}

static const luaL_reg image_meta[] = {
    {"__gc",       image_gc},
    {"__tostring", image_tostring},
    {0, 0}
};


int image_register (lua_State *L) {
    luaL_openlib(L, IMAGE, image_methods, 0);  /* create methods table,
                                                  add it to the globals */
    luaL_newmetatable(L, IMAGE);        /* create metatable for Image,
                                           add it to the Lua registry */
    luaL_openlib(L, 0, image_meta, 0);  /* fill metatable */
    lua_pushliteral(L, "__index");
    lua_pushvalue(L, -3);               /* dup methods table*/
    lua_rawset(L, -3);                  /* metatable.__index = methods */
    lua_pushliteral(L, "__metatable");
    lua_pushvalue(L, -3);               /* dup methods table*/
    lua_rawset(L, -3);                  /* hide metatable:
                                           metatable.__metatable = methods */
    lua_pop(L, 1);                      /* drop metatable */
    return 1;                           /* return methods on the stack */
}

int image_new(lua_State *L, const char *path, const char *name) {
    int tex, width, height;

    char *pos = strstr(name, ".png");
    if (pos && pos == name + strlen(name) - 4) {
        fprintf(stderr, "loading png\n");
        tex = load_png(path, &width, &height);
    }

    pos = strstr(name, ".jpg");
    if (pos && pos == name + strlen(name) - 4) {
        fprintf(stderr, "loading jpg\n");
        tex = load_jpeg(path, &width, &height);
    }

    if (!tex)
        luaL_error(L, "cannot load image file %s", name);


    image_t *image = push_image(L);
    image->tex = tex;
    image->width = width;
    image->height = height;
    return 1;
}
