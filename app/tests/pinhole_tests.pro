QT += core gui quick testlib sql network dbus concurrent gui-private
LIBS += -lturbojpeg -lwayland-client
CONFIG += c++20 testcase
TARGET = pinhole_tests
TEMPLATE = app

INCLUDEPATH += ../src
# The real catalogs, so a broken translation shows up here.
DEFINES += PINHOLE_I18N=\\\"$$PWD/../src/i18n\\\"

# The grid, for its cells; its touchpad hold needs the pointer-gestures
# protocol the app's build generates (app/bin/build first).
GENERATED = $$PWD/../build/generated
INCLUDEPATH += $$GENERATED
HEADERS += ../src/photogrid.h ../src/thumbnailloader.h ../src/framestats.h ../src/touchpadhold.h ../src/marks.h
SOURCES += ../src/photogrid.cpp ../src/thumbnailloader.cpp ../src/framestats.cpp ../src/touchpadhold.cpp ../src/marks.cpp \
    $$GENERATED/pointer-gestures-unstable-v1-protocol.c

HEADERS += ../src/viewermotion.h ../src/scrollphysics.h ../src/theme.h ../src/translator.h ../src/library.h \
    ../src/outgoing.h ../src/serviceclient.h ../src/portal.h ../src/viewer.h ../src/history.h ../src/libraryreloader.h
SOURCES += pinhole_tests.cpp ../src/viewermotion.cpp ../src/scrollphysics.cpp ../src/theme.cpp ../src/translator.cpp ../src/library.cpp \
    ../src/outgoing.cpp ../src/serviceclient.cpp ../src/portal.cpp ../src/viewer.cpp ../src/history.cpp ../src/libraryreloader.cpp
