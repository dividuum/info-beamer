ifdef DEBUG
CFLAGS ?= -ggdb 
else
CFLAGS ?= -O3
endif

CFLAGS +=-I/usr/include/lua5.1 -I/usr/include/freetype2/ -std=c99 -Wall -Wno-deprecated-declarations -Wno-unused-function -Wno-unused-variable
LDFLAGS=-llua5.1 -levent -lglfw -lGLEW -lftgl -lpng -ljpeg -lavformat -lavcodec -lavutil -lswscale -lz -lbz2

all: gpn-info

main.o: main.c kernel.h

gpn-info: main.o image.o font.o video.o tlsf.o framebuffer.o misc.o
	$(CC) -o $@ $^ $(LDFLAGS) 

bin2c: bin2c.c
	$(CC) $^ -o $@

kernel.h: kernel.lua bin2c $(LUAC)
	luac -p $<
	./bin2c $* < $< > $@

performance: performance.csv
	gnuplot -e "plot './performance.csv' using 1:8 with lines;pause mouse key"

.PHONY: clean performance

clean:
	rm -f *.o gpn-info kernel.h bin2c
