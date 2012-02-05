/* video.c
 * (C) Copyright 2010 Michael Meeuwisse
 *
 * Adapted from avcodec_sample.0.5.0.c, license unknown
 *
 * ffmpeg_test is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ffmpeg_test is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ffmpeg_test. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/gl.h>
#include <lauxlib.h>
#include <lualib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>

#define VIDEO "video"

typedef struct {
    AVFormatContext *pFormatCtx;
    AVCodecContext *pCtx;
    AVCodec *pCodec;
    AVFrame *pRaw;
    AVFrame *pDat;
    uint8_t *buffer;
    struct SwsContext *Sctx;
    int videoStream, width, height, format;
    int tex;
    double fps;
} video_t;

static void video_free(video_t *video) {
	if (video->Sctx)
	     sws_freeContext(video->Sctx);
	if (video->pRaw)
	     av_free(video->pRaw);
	if (video->pDat)
	     av_free(video->pDat);
	
	if (video->pCtx)
	     avcodec_close(video->pCtx);
	if (video->pFormatCtx)
		av_close_input_file(video->pFormatCtx);
	
	free(video->buffer);
}

static int video_open(video_t *video, const char *filename) {
	video->format = PIX_FMT_RGB24;
	
	/* Open file, check usability */
	if (av_open_input_file(&video->pFormatCtx, filename, NULL, 0, NULL) ||
		av_find_stream_info(video->pFormatCtx) < 0) {
        fprintf(stderr, "cannot open video stream %s\n", filename);
        goto failed;
    }
	
	/* Find the first video stream */
	video->videoStream = -1;
	int i;
	for (i = 0; i < video->pFormatCtx->nb_streams; i++) {
		if (video->pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			video->videoStream = i;
			break;
		}
    }
	
	if (video->videoStream == -1) {
        fprintf(stderr, "cannot find video stream\n");
		goto failed;
    }

    AVStream *stream = video->pFormatCtx->streams[video->videoStream];

    // http://libav-users.943685.n4.nabble.com/Retrieving-Frames-Per-Second-FPS-td946533.html
    // fprintf(stderr, "%d/%d\n", stream->time_base.num, stream->time_base.den);
    // fprintf(stderr, "%d/%d\n", stream->r_frame_rate.num, stream->r_frame_rate.den);
    // fprintf(stderr, "%d/%d\n", stream->codec->time_base.num, stream->codec->time_base.den);
    if ((stream->time_base.den != stream->r_frame_rate.num) ||
        (stream->time_base.num != stream->r_frame_rate.den)) {
        video->fps = 1.0 / stream->r_frame_rate.den * stream->r_frame_rate.num;
    } else {
        video->fps = 1.0 / stream->time_base.num * stream->time_base.den;
    }
	
	/* Get context for codec, pin down target width/height, find codec */
	video->pCtx = stream->codec;
	video->width = video->pCtx->width;
	video->height = video->pCtx->height;
	video->pCodec = avcodec_find_decoder(video->pCtx->codec_id);

    //fprintf(stderr, "

	if (!video->pCodec || avcodec_open(video->pCtx, video->pCodec) < 0) {
        fprintf(stderr, "no codec found\n");
        goto failed;
    }
	
	/* Frame rate fix for some codecs */
	if (video->pCtx->time_base.num > 1000 && video->pCtx->time_base.den == 1)
		video->pCtx->time_base.den = 1000;
	
	/* Get framebuffers */
	video->pRaw = avcodec_alloc_frame();
	video->pDat = avcodec_alloc_frame();
	
	if (!video->pRaw || !video->pDat) {
        fprintf(stderr, "cannot ???\n");
        goto failed;
    }
	
	/* Create data buffer */
	video->buffer = malloc(avpicture_get_size(video->format, 
		video->pCtx->width, video->pCtx->height));
	
	/* Init buffers */
	avpicture_fill((AVPicture *) video->pDat, video->buffer, video->format, 
		video->pCtx->width, video->pCtx->height);
	
	/* Init scale & convert */
	video->Sctx = sws_getContext(video->pCtx->width, video->pCtx->height, video->pCtx->pix_fmt,
		video->width, video->height, video->format, SWS_BICUBIC, NULL, NULL, NULL);
	
	if (!video->Sctx) {
        fprintf(stderr, "scaling failed\n");
        goto failed;
    }
	
	/* Give some info on stderr about the file & stream */
	dump_format(video->pFormatCtx, 0, filename, 0);
	return 1;
failed:
    video_free(video);
    return 0;
}

static int video_next_frame(video_t *video) {
	AVPacket packet;
    av_init_packet(&packet);

again:
    /* Can we read a frame? */
    if (av_read_frame(video->pFormatCtx, &packet)) {
        fprintf(stderr, "no next frame\n");
        av_free_packet(&packet);
        return 0;
    }
	
    /* Is it what we're trying to parse? */
    if (packet.stream_index != video->videoStream) {
        // fprintf(stderr, "not video\n");
        goto again;
    }
	
    /* Decode it! */
    int finished = 0;
    avcodec_decode_video2(video->pCtx, video->pRaw, &finished, &packet);

    /* Succes? If not, drop packet. */
    if (!finished) {
        fprintf(stderr, "not complete\n");
        goto again;
    }

    sws_scale(video->Sctx, (const uint8_t* const *) video->pRaw->data, video->pRaw->linesize, 
            0, video->pCtx->height, video->pDat->data, video->pDat->linesize);
    av_free_packet(&packet);
    // fprintf(stderr, "got next frame\n");
    return 1;
}

static video_t *to_video(lua_State *L, int index) {
    video_t *video = (video_t *)lua_touserdata(L, index);
    if (!video) luaL_typerror(L, index, VIDEO);
    return video;
}

static video_t *checked_video(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TUSERDATA);
    video_t *video = (video_t *)luaL_checkudata(L, index, VIDEO);
    if (!video) luaL_typerror(L, index, VIDEO);
    return video;
}

static video_t *push_video(lua_State *L) {
    video_t *video = (video_t *)lua_newuserdata(L, sizeof(video_t));
    luaL_getmetatable(L, VIDEO);
    lua_setmetatable(L, -2);
    return video;
}

static int video_size(lua_State *L) {
    video_t *video = checked_video(L, 1);
    lua_pushnumber(L, video->width);
    lua_pushnumber(L, video->height);
    return 2;
}

static int video_fps(lua_State *L) {
    video_t *video = checked_video(L, 1);
    lua_pushnumber(L, video->fps);
    return 1;
}

static int video_next(lua_State *L) {
    video_t *video = checked_video(L, 1);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_UNPACK_LSB_FIRST,  GL_TRUE );
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (!video_next_frame(video)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int prev_tex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glBindTexture(GL_TEXTURE_2D, video->tex);
    glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            video->width,
            video->height,
            GL_RGB,
            GL_UNSIGNED_BYTE,
            video->buffer 
    );
    glBindTexture(GL_TEXTURE_2D, prev_tex);

    lua_pushboolean(L, 1);
    return 1;
}

static int video_draw(lua_State *L) {
    video_t *video = checked_video(L, 1);
    GLfloat x1 = luaL_checknumber(L, 2);
    GLfloat y1 = luaL_checknumber(L, 3);
    GLfloat x2 = luaL_checknumber(L, 4);
    GLfloat y2 = luaL_checknumber(L, 5);

    int prev_tex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glBindTexture(GL_TEXTURE_2D, video->tex);
    glColor4f(1.0, 1.0, 1.0, 1.0);
    glBegin(GL_QUADS); 
        glTexCoord2f(0.0, 0.0); glVertex3f(x1, y1, 0);
        glTexCoord2f(1.0, 0.0); glVertex3f(x2, y1, 0);
        glTexCoord2f(1.0, 1.0); glVertex3f(x2, y2, 0);
        glTexCoord2f(0.0, 1.0); glVertex3f(x1, y2, 0);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    return 0;
}

static const luaL_reg video_methods[] = {
  {"draw",          video_draw},
  {"next",          video_next},
  {"size",          video_size},
  {"fps",           video_fps},
  {0,0}
};

static int video_gc(lua_State *L) {
    video_t *video = to_video(L, 1);
    fprintf(stderr, "gc'ing video: tex id: %d\n", video->tex);
    glDeleteTextures(1, &video->tex);
    video_free(video);
    return 0;
}

static int video_tostring(lua_State *L) {
    lua_pushfstring(L, "video: %p", lua_touserdata(L, 1));
    return 1;
}

static const luaL_reg video_meta[] = {
    {"__gc",       video_gc},
    {"__tostring", video_tostring},
    {0, 0}
};


int video_register(lua_State *L) {
    luaL_openlib(L, VIDEO, video_methods, 0);  /* create methods table,
                                                  add it to the globals */
    luaL_newmetatable(L, VIDEO);        /* create metatable for Image,
                                           add it to the Lua registry */
    luaL_openlib(L, 0, video_meta, 0);  /* fill metatable */
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

int video_load(lua_State *L, const char *path, const char *name) {
    video_t video;
    memset(&video, 0, sizeof(video_t));

    glGenTextures(1, &video.tex);

    if (!video_open(&video, path))
        luaL_error(L, "cannot open video %s", name);

    int prev_tex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glBindTexture(GL_TEXTURE_2D, video.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(
            GL_TEXTURE_2D,  
            0,
            GL_RGB, 
            video.width,
            video.height,
            0,
            GL_RGB,
            GL_UNSIGNED_BYTE,
            NULL 
    );
    glBindTexture(GL_TEXTURE_2D, prev_tex);

    *push_video(L) = video;
    return 1;
}
