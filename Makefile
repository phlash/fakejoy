# Build fake joystick driver..

INCLUDE=$(shell pkg-config --cflags fuse3)
LIBS=$(shell pkg-config --libs fuse3)

all: bin bin/fakejoy bin/evdump

clean:
	rm -rf bin

bin:
	mkdir -p bin

bin/%: bin/%.o
	$(CC) -g -o $@ $< $(LIBS)

bin/%.o: %.c
	$(CC) -g -c -o $@ $(INCLUDE) $<
