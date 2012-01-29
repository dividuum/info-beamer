CFLAGS=-I/usr/include/lua5.1
LDFLAGS=-llua5.1 -levent -lglfw

all: gpn-info

gpn-info: main.o
	$(CC) -o $@ $^ $(LDFLAGS) 

.PHONY: clean

clean:
	rm -f *.o gpn-info
