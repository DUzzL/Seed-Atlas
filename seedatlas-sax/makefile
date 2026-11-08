# seedatlas-sax: the engine's C ABI for the mods (see README.md).
#   make          libseedatlas-sax.a (the engine and the ABI, linked statically)
#   make test     builds and runs the ABI and Nether reference tests

CC ?= cc
AR ?= ar
ENGINE := ../seedatlas-engine
ENGINE_SOURCES := finders.c generator.c layers.c biomenoise.c biomes.c noise.c util.c quadbase.c
# the flags of the engine's makefile: wrapping signed arithmetic, no fused
# multiply-adds (so every platform rounds alike)
SAX_CFLAGS := -std=gnu11 -O3 -fwrapv -ffp-contract=off -DSAX_STATIC -I$(ENGINE) -I.

OBJECTS := $(addprefix build/,$(ENGINE_SOURCES:.c=.o)) build/seedatlas_sax.o

all: libseedatlas-sax.a

build/%.o: $(ENGINE)/%.c | build
	$(CC) $(SAX_CFLAGS) $(CFLAGS) -c -o $@ $<

build/seedatlas_sax.o: seedatlas_sax.c seedatlas_sax.h | build
	$(CC) $(SAX_CFLAGS) -Wall -Wextra $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

libseedatlas-sax.a: $(OBJECTS)
	$(AR) rcs $@ $^

build/sax_test: tests/sax_test.c libseedatlas-sax.a
	$(CC) $(SAX_CFLAGS) -UNDEBUG $(CFLAGS) -o $@ $< libseedatlas-sax.a -lm

build/nether_reference_test: tests/nether_reference_test.c seedatlas_sax.c $(filter-out build/seedatlas_sax.o,$(OBJECTS))
	$(CC) $(SAX_CFLAGS) -UNDEBUG $(CFLAGS) -o $@ $< $(filter-out build/seedatlas_sax.o,$(OBJECTS)) -lm

test: build/sax_test build/nether_reference_test
	./build/sax_test
	./build/nether_reference_test

clean:
	$(RM) -r build libseedatlas-sax.a

.PHONY: all test clean
