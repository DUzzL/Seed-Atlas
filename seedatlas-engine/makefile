AR ?= ar
ARFLAGS = rcs
LDLIBS ?= -lm

# Make predefines CC as "cc", so "CC ?= gcc" would never take effect. Only
# replace that built-in default; an explicit CC from the caller still wins.
ifeq ($(origin CC),default)
CC = gcc
endif

SOURCES := finders.c generator.c layers.c biomenoise.c biomes.c noise.c util.c quadbase.c
# The generators rely on wrapping signed arithmetic, and the noise must round
# like Java's doubles: -ffp-contract=off stops compilers from fusing a*b+c into
# FMA instructions (Clang does so by default on arm64, e.g. Apple Silicon).
# "override" keeps these flags (and the optimisation level below) even when the
# caller passes CFLAGS on the command line, as the qmake build does.
override CFLAGS += -fwrapv -ffp-contract=off

# Every kind of build compiles its own objects, with its own flags, so no build
# can pick up objects compiled for another one (e.g. the app linking objects
# that a test target compiled without optimisation or without
# STRUCT_CONFIG_OVERRIDE):
#   release (default, used by the app)  *.o next to the sources, -O3 -g3
#   debug                                obj-debug/, -O0 -g3 -DDEBUG
#   test-versions                        obj-test-<CFLAGS>/, the caller's CFLAGS
# The test directories are named after the caller's CFLAGS, so tests built with
# other CFLAGS (e.g. with and without -DSTRUCT_CONFIG_OVERRIDE=1) never share
# objects either. The CFLAGS of release and debug builds (from qmake) are not
# tracked: after changing them, run "make clean".
RELEASE_FLAGS := -O3 -g3
DEBUG_FLAGS := -O0 -g3 -DDEBUG

OBJECTS := $(SOURCES:.c=.o)
empty :=
space := $(empty) $(empty)
FLAGS_ID := $(subst $(space),_,$(subst /,+,$(subst =,-,$(subst ",,$(strip $(CFLAGS))))))
TEST_DIR := obj-test-$(FLAGS_ID)
DEBUG_OBJECTS := $(addprefix obj-debug/,$(OBJECTS))
TEST_OBJECTS := $(addprefix $(TEST_DIR)/,$(OBJECTS))

# The archive is the one the app links. It is updated on every call (that takes
# milliseconds) with the objects of the requested kind, so switching between
# debug and release always replaces all of its members (both kinds have the
# same member names).
ifneq ($(filter debug,$(MAKECMDGOALS)),)
LIB_OBJECTS := $(DEBUG_OBJECTS)
else
LIB_OBJECTS := $(OBJECTS)
endif

release: libseedatlas-engine.a

debug: libseedatlas-engine.a

libseedatlas-engine.a: $(LIB_OBJECTS) FORCE
	$(AR) $(ARFLAGS) $@ $(LIB_OBJECTS)

FORCE:

# -MMD -MP records header dependencies, so editing a header rebuilds the
# objects that include it instead of silently reusing stale ones. Compilers
# may refuse dependency output for several -arch flags (macOS universal builds).
DEPFLAGS = -MMD -MP
ifneq ($(word 2,$(filter -arch,$(CFLAGS))),)
DEPFLAGS =
endif

%.o: %.c
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) -Wall -Wextra $(DEPFLAGS) -c -o $@ $<

obj-debug/%.o: %.c | obj-debug
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) -Wall -Wextra $(DEPFLAGS) -c -o $@ $<

$(TEST_DIR)/%.o: %.c | $(TEST_DIR)
	$(CC) $(CFLAGS) -Wall -Wextra $(DEPFLAGS) -c -o $@ $<

obj-debug $(TEST_DIR):
	mkdir $@

test-versions: tests_versions.c $(TEST_OBJECTS)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ $^ $(LDLIBS)

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) libseedatlas-engine.a test-versions test-versions.exe
	$(RM) -r obj-debug obj-test-*

.PHONY: release debug clean test-versions FORCE

-include $(OBJECTS:.o=.d) $(DEBUG_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
