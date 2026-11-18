# The datapack world generator (namespace dp). Include this file from a qmake
# project; it needs Qt Core, QColor from Qt GUI (dpworld.h) and the engine
# headers (seedatlas-engine) on the include path.

HEADERS += \
    $$PWD/dpworld.h \
    $$PWD/dpworld_p.h \
    $$PWD/dpinternal.h \
    $$PWD/dpclimate.h \
    $$PWD/dpdensity.h \
    $$PWD/dpeval.h \
    $$PWD/dplistorder.h \
    $$PWD/dpaquifer.h \
    $$PWD/dpsurface.h \
    $$PWD/dpterrain.h \
    $$PWD/dppack.h \
    $$PWD/dpstructures.h \
    $$PWD/dpzip.h

SOURCES += \
    $$PWD/dpworld.cpp \
    $$PWD/dpbiomes.cpp \
    $$PWD/dpclimate.cpp \
    $$PWD/dpdensity.cpp \
    $$PWD/dpformats.cpp \
    $$PWD/dpnative.cpp \
    $$PWD/dpaquifer.cpp \
    $$PWD/dpsurface.cpp \
    $$PWD/dpterrain.cpp \
    $$PWD/dppack.cpp \
    $$PWD/dpstructures.cpp \
    $$PWD/dpzip.cpp
