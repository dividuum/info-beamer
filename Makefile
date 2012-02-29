# See Copyright Notice in LICENSE.txt

VERSION="rev-$(shell git rev-parse --short=6 HEAD)"

ifdef DEBUG
CFLAGS ?= -ggdb -DDEBUG
else
CFLAGS ?= -O3
endif

CFLAGS += -DVERSION='$(VERSION)'
CFLAGS +=-I/usr/include/lua5.1 -I/usr/include/freetype2/ -I/usr/include/ffmpeg -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations 
LDFLAGS=-llua5.1 -levent -lglfw -lGL -lGLU -lGLEW -lftgl -lpng -ljpeg -lavformat -lavcodec -lavutil -lswscale -lz 

all: info-beamer
	$(MAKE) -C doc

main.o: main.c kernel.h userlib.h

info-beamer: main.o image.o font.o video.o shader.o vnc.o framebuffer.o misc.o tlsf.o struct.o
	$(CC) -o $@ $^ $(LDFLAGS) 

bin2c: bin2c.c
	$(CC) $^ -o $@

kernel.h: kernel.lua bin2c $(LUAC)
	luac -o $<.compiled $<
	./bin2c $* < $<.compiled > $@

userlib.h: userlib.lua bin2c $(LUAC)
	luac -o $<.compiled $<
	./bin2c $* < $<.compiled > $@

performance: performance.csv
	gnuplot -e "plot './performance.csv' using 1:8 with lines;pause mouse key"

.PHONY: clean performance

clean:
	rm -f *.o info-beamer kernel.h userlib.h bin2c *.compiled
	$(MAKE) -C doc clean
