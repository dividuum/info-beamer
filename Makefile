# See Copyright Notice in LICENSE.txt

RELEASE = 1.0pre3

VERSION = $(RELEASE).$(shell git rev-parse --short=6 HEAD)

ifdef DEBUG
CFLAGS ?= -ggdb -DDEBUG
else
CFLAGS ?= -O3 -DNDEBUG
endif

ifdef USE_LUAJIT
LUA_CFLAGS  ?= -I/usr/include/luajit-2.0
LUA_LDFLAGS ?= -lluajit-5.1
LUA_LUAC    ?= luac
CFLAGS      += -DUSE_LUAJIT=1
else
#################################################
# 
# If you have compile/link problems related to lua, try
# setting these variables while running make. For example:
#
# $ LUA_LDFLAGS=-llua make
#
#################################################
LUA_CFLAGS  ?= -I/usr/include/lua5.1
LUA_LDFLAGS ?= -L/usr/lib -llua5.1
LUA_LUAC    ?= luac
endif

CFLAGS  += -DVERSION='"$(VERSION)"'
CFLAGS  += $(LUA_CFLAGS) -I/usr/include/freetype2/ -I/usr/include/ffmpeg -std=c99 -Wall
LDFLAGS += $(LUA_LDFLAGS) -levent -lglfw -lGL -lGLU -lGLEW -lftgl -lIL -lILU -lavformat -lavcodec -lavutil -lswscale -lz 

prefix 		?= /usr/local
exec_prefix ?= $(prefix)
bindir 		?= $(exec_prefix)/bin

all: info-beamer

info-beamer: main.o image.o font.o video.o shader.o vnc.o framebuffer.o misc.o tlsf.o struct.o
	$(CC) -o $@ $^ $(LDFLAGS) 

main.o: main.c kernel.h userlib.h

info-beamer.1: info-beamer.1.ronn
	ronn $< -r --pipe > $@

bin2c: bin2c.c
	$(CC) $^ -o $@

ifdef USE_LUAJIT
%.h: %.lua bin2c
	$(LUA_LUAC) -p $<
	./bin2c $* < $< > $@
else
%.h: %.lua bin2c
	$(LUA_LUAC) -o $<.compiled $<
	./bin2c $* < $<.compiled > $@
endif

doc:
	markdown_py -x toc -x tables -x codehilite doc/manual.md > doc/manual.html

install: info-beamer
	install -D -o root -g root -m 755 $< $(DESTDIR)$(bindir)/$<

clean:
	rm -f *.o info-beamer kernel.h userlib.h bin2c *.compiled doc/manual.html info-beamer.1

.PHONY: clean doc install
