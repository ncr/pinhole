# Everything of the window but main(): the app (pinhole.pro) and its tests
# (tests/pinhole_tests.pro) build the same files from this one list.

QT += core gui qml quick quickcontrols2 sql network multimedia dbus concurrent gui-private qml-private multimedia-private
CONFIG += c++20
INCLUDEPATH += $$PWD

LIBS += -lturbojpeg -lwayland-client -lheif

# The pointer-gestures protocol, for the touchpad hold gesture Qt does not
# bind (see touchpadhold.h), generated from the system's protocol files.
GESTURES_XML = $$system(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml
GENERATED = $$OUT_PWD/generated
system(mkdir -p $$GENERATED && \
    wayland-scanner client-header $$GESTURES_XML $$GENERATED/pointer-gestures-unstable-v1-client-protocol.h && \
    wayland-scanner private-code $$GESTURES_XML $$GENERATED/pointer-gestures-unstable-v1-protocol.c)
# And idle-inhibit, for the screen kept on while a plugin asks (keepawake.h).
IDLE_XML = $$system(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
system(wayland-scanner client-header $$IDLE_XML $$GENERATED/idle-inhibit-unstable-v1-client-protocol.h && \
    wayland-scanner private-code $$IDLE_XML $$GENERATED/idle-inhibit-unstable-v1-protocol.c)
INCLUDEPATH += $$GENERATED
SOURCES += $$GENERATED/pointer-gestures-unstable-v1-protocol.c \
    $$GENERATED/idle-inhibit-unstable-v1-protocol.c

HEADERS += \
    $$PWD/library.h \
    $$PWD/libraryreloader.h \
    $$PWD/keymap.h \
    $$PWD/marks.h \
    $$PWD/markchoices.h \
    $$PWD/storage.h \
    $$PWD/sources.h \
    $$PWD/thumbnailloader.h \
    $$PWD/photogrid.h \
    $$PWD/scrollphysics.h \
    $$PWD/touchpadhold.h \
    $$PWD/keepawake.h \
    $$PWD/serviceclient.h \
    $$PWD/photoprovider.h \
    $$PWD/viewer.h \
    $$PWD/viewermotion.h \
    $$PWD/outgoing.h \
    $$PWD/favourites.h \
    $$PWD/history.h \
    $$PWD/commands.h \
    $$PWD/encoder.h \
    $$PWD/plugins.h \
    $$PWD/pluginstore.h \
    $$PWD/incoming.h \
    $$PWD/portal.h \
    $$PWD/selftest.h \
    $$PWD/theme.h \
    $$PWD/translator.h \
    $$PWD/framestats.h \
    $$PWD/bench.h

SOURCES += \
    $$PWD/library.cpp \
    $$PWD/libraryreloader.cpp \
    $$PWD/keymap.cpp \
    $$PWD/marks.cpp \
    $$PWD/markchoices.cpp \
    $$PWD/storage.cpp \
    $$PWD/sources.cpp \
    $$PWD/thumbnailloader.cpp \
    $$PWD/photogrid.cpp \
    $$PWD/scrollphysics.cpp \
    $$PWD/touchpadhold.cpp \
    $$PWD/keepawake.cpp \
    $$PWD/serviceclient.cpp \
    $$PWD/photoprovider.cpp \
    $$PWD/viewer.cpp \
    $$PWD/viewermotion.cpp \
    $$PWD/outgoing.cpp \
    $$PWD/favourites.cpp \
    $$PWD/history.cpp \
    $$PWD/commands.cpp \
    $$PWD/encoder.cpp \
    $$PWD/plugins.cpp \
    $$PWD/pluginstore.cpp \
    $$PWD/incoming.cpp \
    $$PWD/portal.cpp \
    $$PWD/selftest.cpp \
    $$PWD/selftestrun.cpp \
    $$PWD/selftestcovering.cpp \
    $$PWD/selftestlayers.cpp \
    $$PWD/selftestframe.cpp \
    $$PWD/selftesthidden.cpp \
    $$PWD/selfteststacks.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp \
    $$PWD/framestats.cpp \
    $$PWD/bench.cpp

