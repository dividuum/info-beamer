# See Copyright Notice in LICENSE.txt

RELEASE = 0.1-beta

VERSION = $(RELEASE).$(shell git rev-parse --short=6 HEAD)

ifdef DEBUG
CFLAGS ?= -ggdb -DDEBUG
else
CFLAGS ?= -O3 -DNDEBUG
endif

ifdef USE_LUAJIT
LUA_CFLAGS  ?= -I/usr/include/luajit-2.0
LUA_LDFLAGS ?= -lluajit-5.1
else
LUA_CFLAGS  ?= $(shell pkg-config lua5.1 --cflags)
LUA_LDFLAGS ?= $(shell pkg-config lua5.1 --libs)
endif

CFLAGS += -DVERSION='"$(VERSION)"'
CFLAGS += $(LUA_CFLAGS) -I/usr/include/freetype2/ -I/usr/include/ffmpeg -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations 
LDFLAGS = $(LUA_LDFLAGS) -levent -lglfw -lGL -lGLU -lGLEW -lftgl -lIL -lILU -lILUT -lavformat -lavcodec -lavutil -lswscale -lz 

all: info-beamer

info-beamer: main.o image.o font.o video.o shader.o vnc.o framebuffer.o misc.o tlsf.o struct.o
	$(CC) -o $@ $^ $(LDFLAGS) 

main.o: main.c kernel.h userlib.h

bin2c: bin2c.c
	$(CC) $^ -o $@

ifdef USE_LUAJIT
%.h: %.lua bin2c $(LUAC)
	luac -p $<
	./bin2c $* < $< > $@
else
%.h: %.lua bin2c $(LUAC)
	luac -o $<.compiled $<
	./bin2c $* < $<.compiled > $@
endif

doc:
	$(MAKE) -C doc

install: info-beamer
	install -o root -g root -m 755 $< /usr/local/bin/

.PHONY: clean doc install

clean:
	rm -f *.o info-beamer kernel.h userlib.h bin2c *.compiled
