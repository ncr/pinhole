QT += core gui qml quick quickcontrols2 sql

CONFIG += c++20 release
TARGET = pinhole
TEMPLATE = app

LIBS += -lturbojpeg

HEADERS += \
    src/library.h \
    src/thumbnailloader.h \
    src/photogrid.h \
    src/framestats.h \
    src/bench.h

SOURCES += \
    src/main.cpp \
    src/library.cpp \
    src/thumbnailloader.cpp \
    src/photogrid.cpp \
    src/framestats.cpp \
    src/bench.cpp

RESOURCES += src/resources.qrc
