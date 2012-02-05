# CFLAGS=-I/usr/include/lua5.1 -I/usr/include/freetype2/ -O0 -ggdb
# LDFLAGS=-llua5.1 -levent -lglfw -lGLEW -lGLU -lpng -ljpeg -lftgl
#
CFLAGS=-I/usr/include/lua5.1 -I/usr/include/freetype2/ -O3 -std=c99 
LDFLAGS=-llua5.1 -levent -lglfw -lGLEW -lGLU -lpng -ljpeg -lftgl -lavformat -lavcodec -lavutil -lswscale -lz -lbz2

all: gpn-info

main.o: main.c kernel.h

gpn-info: main.o image.o font.o video.o
	$(CC) -o $@ $^ $(LDFLAGS) 

bin2c: bin2c.c
	$(CC) $^ -o $@

kernel.h: kernel.lua bin2c $(LUAC)
	luac -p $<
	./bin2c $* < $< > $@

.PHONY: clean

clean:
	rm -f *.o gpn-info
