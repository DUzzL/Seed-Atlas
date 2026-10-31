AR ?= ar
ARFLAGS = rcs
LDLIBS ?= -lm

# Make predefines CC as "cc", so "CC ?= gcc" would never take effect. Only
# replace that built-in default; an explicit CC from the caller still wins.
ifeq ($(origin CC),default)
CC = gcc
endif

SOURCES := finders.c generator.c layers.c biomenoise.c biomes.c noise.c util.c quadbase.c
OBJECTS := $(SOURCES:.c=.o)

# The generators rely on wrapping signed arithmetic, and the noise must round
# like Java's doubles: -ffp-contract=off stops compilers from fusing a*b+c into
# FMA instructions (Clang does so by default on arm64, e.g. Apple Silicon).
# "override" keeps these flags (and the optimisation level below) even when the
# caller passes CFLAGS on the command line, as the qmake build does.
override CFLAGS += -fwrapv -ffp-contract=off

release: override CFLAGS += -O3 -g3
release: libseedatlas-engine.a

debug: override CFLAGS += -O0 -g3 -DDEBUG
debug: libseedatlas-engine.a

libseedatlas-engine.a: $(OBJECTS)
	$(AR) $(ARFLAGS) $@ $^

# -MMD -MP records header dependencies, so editing a header rebuilds the
# objects that include it instead of silently reusing stale ones. Compilers
# may refuse dependency output for several -arch flags (macOS universal builds).
DEPFLAGS = -MMD -MP
ifneq ($(word 2,$(filter -arch,$(CFLAGS))),)
DEPFLAGS =
endif

%.o: %.c
	$(CC) $(CFLAGS) -Wall -Wextra $(DEPFLAGS) -c -o $@ $<

test-versions: tests_versions.c $(OBJECTS)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ $^ $(LDLIBS)

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) libseedatlas-engine.a test-versions test-versions.exe

.PHONY: release debug clean test-versions

-include $(OBJECTS:.o=.d)
