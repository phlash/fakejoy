# Build fake joystick driver..

INCLUDE=$(shell pkg-config --cflags fuse3)
LIBS=$(shell pkg-config --libs fuse3)

all: bin bin/fakejoy bin/evdump bin/fakeev

clean:
	rm -rf bin

bin:
	mkdir -p bin

bin/fakejoy: bin/fakejoy.o
	$(CC) -g -o $@ $< $(LIBS)

bin/%: bin/%.o
	$(CC) -g -o $@ $<

bin/%.o: %.c
	$(CC) -g -c -o $@ $(INCLUDE) $<
