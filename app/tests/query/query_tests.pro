QT = core concurrent testlib qml sql
CONFIG += testcase console c++20 release
TARGET = pluginquery_tests
TEMPLATE = app
include($$PWD/../../headerdeps.pri)
INCLUDEPATH += ../../src
SOURCES += tst_pluginquery.cpp ../../src/pluginquery.cpp
HEADERS += ../../src/pluginquery.h
LIBS += -lsqlite3
