CFLAGS=-I/usr/include/lua5.1 -O0 -ggdb
LDFLAGS=-llua5.1 -levent -lglfw -lGLEW

all: gpn-info

main.o: main.c kernel.h

gpn-info: main.o
	$(CC) -o $@ $^ $(LDFLAGS) 

bin2c: bin2c.c
	$(CC) $^ -o $@

kernel.h: kernel.lua bin2c $(LUAC)
	luac -p $<
	./bin2c $* < $< > $@

.PHONY: clean

clean:
	rm -f *.o gpn-info
