# Everything of the window but main(): the app (pinhole.pro) and its tests
# (tests/pinhole_tests.pro) build the same files from this one list.

QT += core gui qml quick quickcontrols2 sql network multimedia dbus concurrent gui-private qml-private quick-private multimedia-private \
    multimediaquick-private waylandclient-private
CONFIG += c++20
INCLUDEPATH += $$PWD

LIBS += -lturbojpeg -lwayland-client -lheif -llcms2 -luhdr -lsqlite3

# The pointer-gestures protocol, for the touchpad hold gesture Qt does not
# bind (see touchpadhold.h), generated from the system's protocol files.
GESTURES_XML = $$system(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml
GENERATED = $$OUT_PWD/generated
system(mkdir -p $$GENERATED && \
    wayland-scanner client-header $$GESTURES_XML $$GENERATED/pointer-gestures-unstable-v1-client-protocol.h && \
    wayland-scanner private-code $$GESTURES_XML $$GENERATED/pointer-gestures-unstable-v1-protocol.c)
# Colour management, to read what each display can show (display.h).
CM_XML = $$system(pkg-config --variable=pkgdatadir wayland-protocols)/staging/color-management/color-management-v1.xml
system(wayland-scanner client-header $$CM_XML $$GENERATED/color-management-v1-client-protocol.h && \
    wayland-scanner private-code $$CM_XML $$GENERATED/color-management-v1-protocol.c)
# And idle-inhibit, for the screen kept on while a plugin asks (keepawake.h).
IDLE_XML = $$system(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
system(wayland-scanner client-header $$IDLE_XML $$GENERATED/idle-inhibit-unstable-v1-client-protocol.h && \
    wayland-scanner private-code $$IDLE_XML $$GENERATED/idle-inhibit-unstable-v1-protocol.c)
INCLUDEPATH += $$GENERATED
# Only the version readers rebuild when Git HEAD moves.
PINHOLE_BUILD_COMMIT = $$system(git -C $$shell_quote($$PWD/../..) rev-parse HEAD)
BUILD_VERSION_DATA = "$${LITERAL_HASH}pragma once"
BUILD_VERSION_DATA += "$${LITERAL_HASH}define PINHOLE_BUILD_COMMIT \"$$PINHOLE_BUILD_COMMIT\""
write_file($$GENERATED/buildversion.h, BUILD_VERSION_DATA)

# The HDR video shader (videoview.h) and the stage's photo's (stage.h),
# baked by Qt's qsb for every graphics API the window may use, the vertex
# shaders batchable as the scene graph asks; into a resource file of their
# own.
QSB = $$[QT_HOST_BINS]/qsb
QSB_TARGETS = --glsl \'100 es,120,150\' --hlsl 50 --msl 12
SHADERS_OUT = $$GENERATED/shaders
!system(mkdir -p $$SHADERS_OUT && \
    $$QSB -b $$QSB_TARGETS -o $$SHADERS_OUT/hdrvideo.vert.qsb $$PWD/shaders/hdrvideo.vert && \
    $$QSB $$QSB_TARGETS -o $$SHADERS_OUT/hdrvideo.frag.qsb $$PWD/shaders/hdrvideo.frag && \
    $$QSB -b $$QSB_TARGETS -o $$SHADERS_OUT/photo.vert.qsb $$PWD/shaders/photo.vert && \
    $$QSB $$QSB_TARGETS -o $$SHADERS_OUT/photo.frag.qsb $$PWD/shaders/photo.frag): \
    error("qsb could not bake the shaders: Qt Shader Tools (qt6-shadertools) is needed to build")
write_file($$SHADERS_OUT/shaders.qrc, $$list("<RCC><qresource prefix=\"/shaders\">" \
    "<file>hdrvideo.vert.qsb</file>" "<file>hdrvideo.frag.qsb</file>" \
    "<file>photo.vert.qsb</file>" "<file>photo.frag.qsb</file>" "</qresource></RCC>"))
RESOURCES += $$SHADERS_OUT/shaders.qrc
OTHER_FILES += $$PWD/shaders/hdrvideo.vert $$PWD/shaders/hdrvideo.frag $$PWD/shaders/photo.vert $$PWD/shaders/photo.frag
SOURCES += $$GENERATED/pointer-gestures-unstable-v1-protocol.c \
    $$GENERATED/idle-inhibit-unstable-v1-protocol.c \
    $$GENERATED/color-management-v1-protocol.c

HEADERS += \
    $$PWD/maptiles.h \
    $$PWD/library.h \
    $$PWD/timeline.h \
    $$PWD/libraryreloader.h \
    $$PWD/keymap.h \
    $$PWD/marks.h \
    $$PWD/markchoices.h \
    $$PWD/storage.h \
    $$PWD/updater.h \
    $$PWD/sources.h \
    $$PWD/thumbnailloader.h \
    $$PWD/photogrid.h \
    $$PWD/scrollphysics.h \
    $$PWD/scroller.h \
    $$PWD/shield.h \
    $$PWD/zoomer.h \
    $$PWD/touchpadhold.h \
    $$PWD/keepawake.h \
    $$PWD/serviceclient.h \
    $$PWD/photoprovider.h \
    $$PWD/colour.h \
    $$PWD/gainmap.h \
    $$PWD/display.h \
    $$PWD/sdrvideo.h \
    $$PWD/videoview.h \
    $$PWD/stage.h \
    $$PWD/viewer.h \
    $$PWD/viewermotion.h \
    $$PWD/outgoing.h \
    $$PWD/favourites.h \
    $$PWD/history.h \
    $$PWD/commands.h \
    $$PWD/encoder.h \
    $$PWD/plugins.h \
    $$PWD/pluginquery.h \
    $$PWD/pluginguide.h \
    $$PWD/pluginstore.h \
    $$PWD/incoming.h \
    $$PWD/portal.h \
    $$PWD/selftest.h \
    $$PWD/theme.h \
    $$PWD/translator.h \
    $$PWD/framestats.h \
    $$PWD/framemonitor.h \
    $$PWD/bench.h

SOURCES += \
    $$PWD/maptiles.cpp \
    $$PWD/library.cpp \
    $$PWD/pluginguide.cpp \
    $$PWD/timeline.cpp \
    $$PWD/libraryreloader.cpp \
    $$PWD/keymap.cpp \
    $$PWD/marks.cpp \
    $$PWD/markchoices.cpp \
    $$PWD/storage.cpp \
    $$PWD/updater.cpp \
    $$PWD/sources.cpp \
    $$PWD/thumbnailloader.cpp \
    $$PWD/photogrid.cpp \
    $$PWD/scrollphysics.cpp \
    $$PWD/scroller.cpp \
    $$PWD/shield.cpp \
    $$PWD/zoomer.cpp \
    $$PWD/touchpadhold.cpp \
    $$PWD/keepawake.cpp \
    $$PWD/serviceclient.cpp \
    $$PWD/photoprovider.cpp \
    $$PWD/colour.cpp \
    $$PWD/gainmap.cpp \
    $$PWD/display.cpp \
    $$PWD/sdrvideo.cpp \
    $$PWD/videoview.cpp \
    $$PWD/stage.cpp \
    $$PWD/viewer.cpp \
    $$PWD/viewermotion.cpp \
    $$PWD/outgoing.cpp \
    $$PWD/favourites.cpp \
    $$PWD/history.cpp \
    $$PWD/commands.cpp \
    $$PWD/encoder.cpp \
    $$PWD/plugins.cpp \
    $$PWD/pluginlibrary.cpp \
    $$PWD/pluginjobs.cpp \
    $$PWD/plugincheck.cpp \
    $$PWD/pluginquery.cpp \
    $$PWD/pluginstore.cpp \
    $$PWD/incoming.cpp \
    $$PWD/portal.cpp \
    $$PWD/selftest.cpp \
    $$PWD/selftestplaces.cpp \
    $$PWD/selftestquery.cpp \
    $$PWD/selftestaccessibility.cpp \
    $$PWD/selftestrun.cpp \
    $$PWD/selftestcovering.cpp \
    $$PWD/selftestlayers.cpp \
    $$PWD/selftestsources.cpp \
    $$PWD/selftestlive.cpp \
    $$PWD/selftestframe.cpp \
    $$PWD/selftesthidden.cpp \
    $$PWD/selfteststacks.cpp \
    $$PWD/selftestthrough.cpp \
    $$PWD/selftestphysics.cpp \
    $$PWD/selftestcolour.cpp \
    $$PWD/selftestguard.cpp \
    $$PWD/selftesthdr.cpp \
    $$PWD/selfteststage.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp \
    $$PWD/framestats.cpp \
    $$PWD/framemonitor.cpp \
    $$PWD/bench.cpp

# Shared control keyboard and accessibility semantics; their QML keeps the style.
HEADERS += $$PWD/control.h
SOURCES += $$PWD/control.cpp

