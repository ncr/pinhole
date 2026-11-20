CONFIG += release qtquickcompiler
TARGET = pinhole
TEMPLATE = app
include($$PWD/headerdeps.pri)

include(src/sources.pri)
SOURCES += src/main.cpp

RESOURCES += src/resources.qrc
