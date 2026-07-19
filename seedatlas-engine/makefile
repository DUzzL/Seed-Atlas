CC ?= gcc
AR ?= ar

SOURCES := finders.c generator.c layers.c biomenoise.c biomes.c noise.c util.c quadbase.c
OBJECTS := $(SOURCES:.c=.o)

release: CFLAGS += -O3 -g3
release: libseedatlas-engine.a

debug: CFLAGS += -O0 -g3 -DDEBUG
debug: libseedatlas-engine.a

libseedatlas-engine.a: $(OBJECTS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -Wall -Wextra -c -o $@ $<

test-versions: tests_versions.c $(OBJECTS)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ $^

clean:
	$(RM) $(OBJECTS) libseedatlas-engine.a test-versions

.PHONY: release debug clean test-versions
