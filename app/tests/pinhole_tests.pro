QT += core gui quick testlib sql network dbus
CONFIG += c++20 testcase
TARGET = pinhole_tests
TEMPLATE = app

INCLUDEPATH += ../src
# The real catalogs, so a broken translation shows up here.
DEFINES += PINHOLE_I18N=\\\"$$PWD/../src/i18n\\\"

HEADERS += ../src/viewermotion.h ../src/scrollphysics.h ../src/theme.h ../src/translator.h ../src/library.h \
    ../src/outgoing.h ../src/serviceclient.h ../src/portal.h
SOURCES += pinhole_tests.cpp ../src/viewermotion.cpp ../src/scrollphysics.cpp ../src/theme.cpp ../src/translator.cpp ../src/library.cpp \
    ../src/outgoing.cpp ../src/serviceclient.cpp ../src/portal.cpp
