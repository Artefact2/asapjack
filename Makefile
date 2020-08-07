JACK_FLAGS=$(shell pkg-config --cflags --libs jack)
SRC_FLAGS=$(shell pkg-config --cflags --libs samplerate)

default: build/asapjack

build/asapjack: src/asapjack.c build Makefile
	clang -g -O2 -Wall -Wno-unused-parameter -pthread -lm $(JACK_FLAGS) $(SRC_FLAGS) -o $@ $< /usr/lib/libasap.a

build:
	mkdir $@

clean:
	rm -Rf build

.PHONY: clean
