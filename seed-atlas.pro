#-------------------------------------------------
#
# Project created by QtCreator 2020-07-11T11:37:33
#
#-------------------------------------------------

QT += core widgets

# uncomment to override the profile compiler
#QMAKE_CC = clang
#QMAKE_CXX = clang++

CHARSET                 = -finput-charset=UTF-8 -fexec-charset=UTF-8
# -ffp-contract=off: no fused multiply-adds, so every platform rounds alike
QMAKE_CFLAGS            = $$CHARSET -fwrapv -ffp-contract=off -DSTRUCT_CONFIG_OVERRIDE=1
# x86-64 builds (Windows, Linux) require AVX2 (x86-64-v3: Intel Haswell, AMD
# Excavator/Zen and newer); macOS builds both architectures in one pass and
# keeps the compiler default
!macx:contains(QT_ARCH, x86_64) {
    QMAKE_CFLAGS += -march=x86-64-v3
}
QMAKE_CXXFLAGS          = $$QMAKE_CFLAGS
QMAKE_CXXFLAGS_RELEASE  *= -O3 -g3

!versionAtLeast(QT_VERSION, 6.3.0) {
    error("Seed Atlas requires Qt 6.3 or newer (the sources use Qt 6 and C++17 APIs).")
}
QMAKE_CXXFLAGS += -std=gnu++17
DEFINES += QT_DISABLE_DEPRECATED_UP_TO=0x060000

win32: {
    CONFIG += static_gnu
    RC_ICONS = rc/app_icon.ico

    # thank you nullprogram for dealing with the Windows UTF-16 nonsense
    LIBWINSANE          = $$PWD/src/libwinsane
    libwinsane.target   = libwinsane
    libwinsane.output   = $$LIBWINSANE/libwinsane.o
    libwinsane.commands = $(MAKE) -C $$shell_quote($$LIBWINSANE) -f $$shell_quote($$LIBWINSANE/Makefile)
    QMAKE_EXTRA_TARGETS += libwinsane
    PRE_TARGETDEPS      += libwinsane
    LIBS                += $$LIBWINSANE/libwinsane.o
} else {
    DEFINES += "LUA_USE_POSIX=1"
}

macx {
    ICON = rc/app_icon.icns
    QMAKE_TARGET_BUNDLE_PREFIX = org.seedatlas
}

wasm: {
    DEFINES += "WASM=1"
    #QT_WASM_SOURCE_MAP=1
    QT_WASM_INITIAL_MEMORY = 256MB
    QT_WASM_PTHREAD_POOL_SIZE = 32
    CONFIG(debug, debug|release): {
        #QMAKE_CFLAGS += -O3 -gsource-map
    }
}
#CONFIG += sanitizer
#CONFIG += sanitize_undefined
#CONFIG += sanitize_thread

static_gnu: {
    LIBS += -static -static-libgcc -static-libstdc++
}

CONFIG(debug, debug|release): {
    CUTARGET = debug
} else {
    CUTARGET = release
}

# compile the Seed Atlas generation engine
ENGINE_PATH         = $$PWD/seedatlas-engine
ENGINE_ARCHIVER     =
macx: ENGINE_ARCHIVER = AR=\"/usr/bin/libtool\" ARFLAGS=\"-static -o\"
QMAKE_PRE_LINK      += $(MAKE) -C $$shell_quote($$ENGINE_PATH) -f $$shell_quote($$ENGINE_PATH/makefile) CC=\"$$QMAKE_CC\" CFLAGS=\"$(CFLAGS) $$QMAKE_CFLAGS\" $$ENGINE_ARCHIVER $$CUTARGET
QMAKE_CLEAN         += $$ENGINE_PATH/*.o $$ENGINE_PATH/libseedatlas-engine.a
LIBS                += $$ENGINE_PATH/libseedatlas-engine.a -lm

LUAPATH = $$PWD/lua/src

TARGET = seed-atlas

# The application version has one source: the file VERSION.txt, e.g. "4.3.0" for
# a release or "4.3.dev0" for a development version (patch number -1 - N).
# The packaging scripts and workflows read the same file.
SEED_ATLAS_VERSION = $$cat($$PWD/VERSION.txt)
SEED_ATLAS_VERSION = $$first(SEED_ATLAS_VERSION)
SEED_ATLAS_VERSION_PARTS = $$split(SEED_ATLAS_VERSION, .)
SEED_ATLAS_VERS_MAJOR = $$member(SEED_ATLAS_VERSION_PARTS, 0)
SEED_ATLAS_VERS_MINOR = $$member(SEED_ATLAS_VERSION_PARTS, 1)
SEED_ATLAS_VERS_PATCH = $$member(SEED_ATLAS_VERSION_PARTS, 2)
contains(SEED_ATLAS_VERS_PATCH, "^dev[0-9]+$") {
    SEED_ATLAS_VERS_DEV = $$replace(SEED_ATLAS_VERS_PATCH, "^dev", "")
    SEED_ATLAS_VERS_PATCH = -$$num_add($$SEED_ATLAS_VERS_DEV, 1)
}
!contains(SEED_ATLAS_VERS_MAJOR, "^[0-9]+$")|!contains(SEED_ATLAS_VERS_MINOR, "^[0-9]+$")|!contains(SEED_ATLAS_VERS_PATCH, "^-?[0-9]+$") {
    error("VERSION must hold MAJOR.MINOR.PATCH or MAJOR.MINOR.devN, not \"$$SEED_ATLAS_VERSION\".")
}
DEFINES += SEED_ATLAS_VERS_MAJOR=$$SEED_ATLAS_VERS_MAJOR \
           SEED_ATLAS_VERS_MINOR=$$SEED_ATLAS_VERS_MINOR \
           SEED_ATLAS_VERS_PATCH=$$SEED_ATLAS_VERS_PATCH

# Public packages pass the source commit as a machine-readable build ID and its
# commit time (seconds since 1970) as the build time; the packaging scripts in
# installer/ do this. Other builds are "development" builds without update
# checks. The update check compares the build times to find a newer release.
isEmpty(SEED_ATLAS_BUILD_ID) {
    SEED_ATLAS_BUILD_ID = development
}
isEmpty(SEED_ATLAS_BUILD_TIME) {
    SEED_ATLAS_BUILD_TIME = 0
}
!contains(SEED_ATLAS_BUILD_TIME, "^[0-9]+$") {
    error("SEED_ATLAS_BUILD_TIME must be the commit time in seconds since 1970.")
}
DEFINES += SEED_ATLAS_BUILD_ID=\\\"$$SEED_ATLAS_BUILD_ID\\\"
DEFINES += SEED_ATLAS_BUILD_TIME=$${SEED_ATLAS_BUILD_TIME}LL

SOURCES += \
        $$LUAPATH/lapi.c \
        $$LUAPATH/lauxlib.c \
        $$LUAPATH/lbaselib.c \
        $$LUAPATH/lcode.c \
        $$LUAPATH/lcorolib.c \
        $$LUAPATH/lctype.c \
        $$LUAPATH/ldblib.c \
        $$LUAPATH/ldebug.c \
        $$LUAPATH/ldo.c \
        $$LUAPATH/ldump.c \
        $$LUAPATH/lfunc.c \
        $$LUAPATH/lgc.c \
        $$LUAPATH/linit.c \
        $$LUAPATH/liolib.c \
        $$LUAPATH/llex.c \
        $$LUAPATH/lmathlib.c \
        $$LUAPATH/lmem.c \
        $$LUAPATH/loadlib.c \
        $$LUAPATH/lobject.c \
        $$LUAPATH/lopcodes.c \
        $$LUAPATH/loslib.c \
        $$LUAPATH/lparser.c \
        $$LUAPATH/lstate.c \
        $$LUAPATH/lstring.c \
        $$LUAPATH/lstrlib.c \
        $$LUAPATH/ltable.c \
        $$LUAPATH/ltablib.c \
        $$LUAPATH/ltm.c \
        $$LUAPATH/lundump.c \
        $$LUAPATH/lutf8lib.c \
        $$LUAPATH/lvm.c \
        $$LUAPATH/lzio.c \
        src/aboutdialog.cpp \
        src/biomecolordialog.cpp \
        src/conditiondialog.cpp \
        src/config.cpp \
        src/configdialog.cpp \
        src/extgendialog.cpp \
        src/exportdialog.cpp \
        src/formconditions.cpp \
        src/formgen48.cpp \
        src/formsearchcontrol.cpp \
        src/fortresslayout.cpp \
        src/gotodialog.cpp \
        src/headless.cpp \
        src/maptoolsdialog.cpp \
        src/message.cpp \
        src/motioncontroller.cpp \
        src/layerdialog.cpp \
        src/mapview.cpp \
        src/rangedialog.cpp \
        src/resultlist.cpp \
        src/scripts.cpp \
        src/search.cpp \
        src/searchthread.cpp \
        src/tabbiomes.cpp \
        src/tablocations.cpp \
        src/tabslime.cpp \
        src/tabstructures.cpp \
        src/terrainmap.cpp \
        src/mainwindow.cpp \
        src/main.cpp \
        src/util.cpp \
        src/widgets.cpp \
        src/world.cpp \
        src/gpufilter.cpp

# Experimental GPU prefilter: Metal on macOS, Vulkan (loaded at run time) on
# Windows and Linux, none in WebAssembly. The kernels are generated from
# src/gpufilter.comp with tools/gen-gpu-kernels.sh.
macx {
    OBJECTIVE_SOURCES += src/gpufilter_metal.mm
    QMAKE_OBJECTIVE_CFLAGS += -fobjc-arc
    LIBS += -framework Metal -framework Foundation
} else:wasm {
    SOURCES += src/gpufilter_stub.cpp
} else {
    SOURCES += src/gpufilter_vulkan.cpp
    HEADERS += src/vkmini.h
    unix: LIBS += -ldl
}

HEADERS += \
        src/gpufilter.h \
        src/gpufilter_backend.h \
        $$ENGINE_PATH/finders.h \
        $$ENGINE_PATH/generator.h \
        $$ENGINE_PATH/layers.h \
        $$ENGINE_PATH/biomes.h \
        $$ENGINE_PATH/quadbase.h \
        $$ENGINE_PATH/util.h \
        $$LUAPATH/lapi.h \
        $$LUAPATH/lauxlib.h \
        $$LUAPATH/lcode.h \
        $$LUAPATH/lctype.h \
        $$LUAPATH/ldebug.h \
        $$LUAPATH/ldo.h \
        $$LUAPATH/lfunc.h \
        $$LUAPATH/lgc.h \
        $$LUAPATH/ljumptab.h \
        $$LUAPATH/llex.h \
        $$LUAPATH/llimits.h \
        $$LUAPATH/lmem.h \
        $$LUAPATH/lobject.h \
        $$LUAPATH/lopcodes.h \
        $$LUAPATH/lopnames.h \
        $$LUAPATH/lparser.h \
        $$LUAPATH/lprefix.h \
        $$LUAPATH/lstate.h \
        $$LUAPATH/lstring.h \
        $$LUAPATH/ltable.h \
        $$LUAPATH/ltm.h \
        $$LUAPATH/lua.h \
        $$LUAPATH/lua.hpp \
        $$LUAPATH/luaconf.h \
        $$LUAPATH/lualib.h \
        $$LUAPATH/lundump.h \
        $$LUAPATH/lvm.h \
        $$LUAPATH/lzio.h \
        src/aboutdialog.h \
        src/biomecolordialog.h \
        src/conditiondialog.h \
        src/config.h \
        src/configdialog.h \
        src/extgendialog.h \
        src/exportdialog.h \
        src/formconditions.h \
        src/formgen48.h \
        src/formsearchcontrol.h \
        src/fortresslayout.h \
        src/gotodialog.h \
        src/headless.h \
        src/maptoolsdialog.h \
        src/message.h \
        src/motioncontroller.h \
        src/layerdialog.h \
        src/mapview.h \
        src/qzipwriter.h \
        src/rangedialog.h \
        src/resultlist.h \
        src/scripts.h \
        src/search.h \
        src/searchthread.h \
        src/seedtables.h \
        src/tabbiomes.h \
        src/tablocations.h \
        src/tabslime.h \
        src/tabstructures.h \
        src/terrainmap.h \
        src/mainwindow.h \
        src/mappingworkers.h \
        src/util.h \
        src/widgets.h \
        src/world.h

FORMS += \
        src/aboutdialog.ui \
        src/biomecolordialog.ui \
        src/conditiondialog.ui \
        src/configdialog.ui \
        src/extgendialog.ui \
        src/exportdialog.ui \
        src/formconditions.ui \
        src/formgen48.ui \
        src/formsearchcontrol.ui \
        src/gotodialog.ui \
        src/maptoolsdialog.ui \
        src/layerdialog.ui \
        src/mainwindow.ui \
        src/rangedialog.ui \
        src/tabbiomes.ui \
        src/tablocations.ui \
        src/tabstructures.ui

RESOURCES += \
        rc/icons.qrc \
        rc/style.qrc \
        rc/examples.qrc \
        rc/qh.qrc


# Desktop builds check the manually maintained GitHub release for updates.
!wasm: {
    QT += network
    DEFINES += "WITH_UPDATER=1"
    SOURCES += src/updater.cpp
    HEADERS += src/updater.h
}

# enable dbus features with: qmake CONFIG+=with_dbus
with_dbus: {
    QT += dbus
    DEFINES += "WITH_DBUS=1"
}
