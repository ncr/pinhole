CONFIG += release qtquickcompiler
TARGET = pinhole
TEMPLATE = app

include(src/sources.pri)
SOURCES += src/main.cpp

RESOURCES += src/resources.qrc
