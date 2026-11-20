CONFIG += testcase
QT += testlib
TARGET = pinhole_tests
TEMPLATE = app
include($$PWD/../headerdeps.pri)

# The window's own files, every one (src/sources.pri), and the tests.
include(../src/sources.pri)
SOURCES += pinhole_tests.cpp
# Info's map module's vector tile decoder and photo grouping (plugins/info/module).
INCLUDEPATH += $$PWD/../../plugins/info/module
HEADERS += $$PWD/../../plugins/info/module/vectortile.h $$PWD/../../plugins/info/module/places.h
SOURCES += $$PWD/../../plugins/info/module/vectortile.cpp $$PWD/../../plugins/info/module/places.cpp
# QML and images some of them load.
RESOURCES += ../src/resources.qrc
# The real catalogs, so a broken translation shows up here.
DEFINES += PINHOLE_I18N=\\\"$$PWD/../src/i18n\\\"
# Colour profiles of real photos (colour tests).
DEFINES += PINHOLE_TEST_DATA=\\\"$$PWD/data\\\"
