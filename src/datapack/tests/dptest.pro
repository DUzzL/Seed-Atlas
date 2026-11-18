# Console tests of the datapack world generator (src/datapack).
#
# Build in a directory outside the repository:
#   qmake6 <repo>/src/datapack/tests/dptest.pro && make -j8
# Usage: see the comment at the top of dptest.cpp (./dptest without arguments
# prints it). No game data is part of the repository: every test takes the
# paths of its data as arguments.

QT       = core gui
CONFIG  += console c++17 release
CONFIG  -= app_bundle
TARGET   = dptest

# the flags of the app (seed-atlas.pro): no fused multiply-adds
QMAKE_CFLAGS            = -fwrapv -ffp-contract=off
QMAKE_CXXFLAGS          = $$QMAKE_CFLAGS -std=gnu++17
QMAKE_CFLAGS_RELEASE    = -O3
QMAKE_CXXFLAGS_RELEASE  = -O3

DP      = $$PWD/..
ENGINE  = $$PWD/../../../seedatlas-engine

INCLUDEPATH += $$DP $$ENGINE

# the engine is compiled into this build directory (its own object
# directories are not touched)
SOURCES += \
    $$ENGINE/noise.c \
    $$ENGINE/biomenoise.c \
    $$ENGINE/biomes.c \
    $$ENGINE/layers.c \
    $$ENGINE/generator.c \
    $$ENGINE/finders.c \
    $$ENGINE/quadbase.c \
    $$ENGINE/util.c

include($$DP/datapack.pri)

SOURCES += dptest.cpp
