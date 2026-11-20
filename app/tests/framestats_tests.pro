QT += core gui quick testlib
CONFIG += testcase c++20
TEMPLATE = app
include($$PWD/../headerdeps.pri)
TARGET = framestats_tests
INCLUDEPATH += ../src
SOURCES += framestats_tests.cpp ../src/framestats.cpp ../src/framemonitor.cpp
