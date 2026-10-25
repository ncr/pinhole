QT += core gui quick testlib sql
CONFIG += c++20 testcase
TARGET = pinhole_tests
TEMPLATE = app

INCLUDEPATH += ../src
# The real catalogs, so a broken translation shows up here.
DEFINES += PINHOLE_I18N=\\\"$$PWD/../src/i18n\\\"

HEADERS += ../src/scrollphysics.h ../src/theme.h ../src/translator.h ../src/library.h
SOURCES += pinhole_tests.cpp ../src/scrollphysics.cpp ../src/theme.cpp ../src/translator.cpp ../src/library.cpp
