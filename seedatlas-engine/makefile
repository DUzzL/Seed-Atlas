AR ?= ar
ARFLAGS = rcs
LDLIBS ?= -lm

# Make predefines CC as "cc", so "CC ?= gcc" would never take effect. Only
# replace that built-in default; an explicit CC from the caller still wins.
ifeq ($(origin CC),default)
CC = gcc
endif

SOURCES := finders.c generator.c layers.c biomenoise.c biomes.c noise.c util.c quadbase.c strongholds.c eyesearch.c
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
#   test-versions, test-strongholds,     obj-test-<CFLAGS>/, the caller's CFLAGS
#   test-portal-reference
#   test-eyesearch, bench-eyesearch,     obj-test-O3-<CFLAGS>/, -O3
#   test-stronghold-positions
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
TEST_O3_DIR := obj-test-O3-$(FLAGS_ID)
DEBUG_OBJECTS := $(addprefix obj-debug/,$(OBJECTS))
TEST_OBJECTS := $(addprefix $(TEST_DIR)/,$(OBJECTS))
TEST_O3_OBJECTS := $(addprefix $(TEST_O3_DIR)/,$(OBJECTS))

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

$(TEST_O3_DIR)/%.o: %.c | $(TEST_O3_DIR)
	$(CC) $(CFLAGS) -O3 -Wall -Wextra $(DEPFLAGS) -c -o $@ $<

obj-debug $(TEST_DIR) $(TEST_O3_DIR):
	mkdir $@

test-versions: tests_versions.c $(TEST_OBJECTS)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ $^ $(LDLIBS)

test-strongholds: tests_strongholds.c $(TEST_OBJECTS)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ $^ $(LDLIBS)

# Compares against vanilla reference data; run as
#   ./test-portal-reference ../tools/portal-reference/data
test-portal-reference: tests_portal_reference.c $(TEST_OBJECTS)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ $^ $(LDLIBS)

# Stronghold positions (finders.c) against vanilla ring positions
test-stronghold-positions: tests_stronghold_positions.c $(TEST_O3_OBJECTS)
	$(CC) $(CFLAGS) -O3 -Wall -Wextra -o $@ $^ $(LDLIBS)

# scanEyeFamily() against the reference scan, on seed sub-ranges; run as
#   ./test-eyesearch [-quick] [-real ../tools/portal-reference/data]
# The objects are optimised here as well, since the reference scan is slow.
test-eyesearch: tests_eyesearch.c $(TEST_O3_OBJECTS)
	$(CC) $(CFLAGS) -O3 -Wall -Wextra -o $@ $^ $(LDLIBS)

# World type Amplified: terrain splines, density, heights, biomes; run as
#   ./test-amplified [../tools/worldgen-reference/data]
# With the directory, heights and biomes are compared with real-server data.
test-amplified: tests_amplified.c $(TEST_O3_OBJECTS)
	$(CC) $(CFLAGS) -O3 -Wall -Wextra -o $@ $^ $(LDLIBS)

# Seeds per second of scanEyeFamily(); run as ./bench-eyesearch [FAMILIES] [MC]
bench-eyesearch: bench_eyesearch.c $(TEST_O3_OBJECTS)
	$(CC) $(CFLAGS) -O3 -Wall -Wextra -o $@ $^ $(LDLIBS)

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) libseedatlas-engine.a test-versions test-versions.exe \
		test-strongholds test-strongholds.exe test-portal-reference test-portal-reference.exe \
		test-eyesearch test-eyesearch.exe bench-eyesearch bench-eyesearch.exe \
		test-stronghold-positions test-stronghold-positions.exe \
		test-amplified test-amplified.exe
	$(RM) -r obj-debug obj-test-*

.PHONY: release debug clean test-versions test-strongholds test-portal-reference \
	test-eyesearch bench-eyesearch test-stronghold-positions test-amplified FORCE

-include $(OBJECTS:.o=.d) $(DEBUG_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(TEST_O3_OBJECTS:.o=.d)
