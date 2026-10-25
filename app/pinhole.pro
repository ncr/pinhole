QT += core gui qml quick quickcontrols2 sql network multimedia dbus concurrent gui-private

CONFIG += c++20 release
TARGET = pinhole
TEMPLATE = app

LIBS += -lturbojpeg -lwayland-client -lheif

# The pointer-gestures protocol, for the touchpad hold gesture Qt does not
# bind (see touchpadhold.h), generated from the system's protocol files.
GESTURES_XML = $$system(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml
GENERATED = $$OUT_PWD/generated
system(mkdir -p $$GENERATED && \
    wayland-scanner client-header $$GESTURES_XML $$GENERATED/pointer-gestures-unstable-v1-client-protocol.h && \
    wayland-scanner private-code $$GESTURES_XML $$GENERATED/pointer-gestures-unstable-v1-protocol.c)
INCLUDEPATH += $$GENERATED
SOURCES += $$GENERATED/pointer-gestures-unstable-v1-protocol.c

HEADERS += \
    src/library.h \
    src/thumbnailloader.h \
    src/photogrid.h \
    src/scrollphysics.h \
    src/touchpadhold.h \
    src/serviceclient.h \
    src/photoprovider.h \
    src/viewer.h \
    src/viewermotion.h \
    src/outgoing.h \
    src/portal.h \
    src/selftest.h \
    src/theme.h \
    src/translator.h \
    src/framestats.h \
    src/bench.h

SOURCES += \
    src/main.cpp \
    src/library.cpp \
    src/thumbnailloader.cpp \
    src/photogrid.cpp \
    src/scrollphysics.cpp \
    src/touchpadhold.cpp \
    src/serviceclient.cpp \
    src/photoprovider.cpp \
    src/viewer.cpp \
    src/viewermotion.cpp \
    src/outgoing.cpp \
    src/portal.cpp \
    src/selftest.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/framestats.cpp \
    src/bench.cpp

RESOURCES += src/resources.qrc
