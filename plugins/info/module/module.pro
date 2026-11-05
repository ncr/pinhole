# Info's native QML module, Pinhole.Internal.Info: the map, drawn from
# OpenFreeMap's vector tiles. Built with the window (bin/build), against the
# same Qt, into ../qml, where the window finds Pinhole's own plugins' modules
# (docs/plugins.md, Native modules).
TEMPLATE = lib
CONFIG += plugin c++20 release
QT += core gui qml quick network concurrent
TARGET = pinholeinfo
DESTDIR = $$PWD/../qml/Pinhole/Internal/Info

HEADERS += maptiles.h vectortile.h
SOURCES += maptiles.cpp vectortile.cpp module.cpp
RESOURCES += module.qrc

# Its qmldir beside the library.
COPIES += qmldir
qmldir.files = $$PWD/qmldir
qmldir.path = $$DESTDIR
