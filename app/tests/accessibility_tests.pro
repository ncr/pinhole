CONFIG += testcase c++20 release
QT += testlib quick qml quickcontrols2 qml-private quick-private
TARGET = pinhole_accessibility_tests
TEMPLATE = app
INCLUDEPATH += ../src
SOURCES += accessibility_tests.cpp ../src/control.cpp ../src/shield.cpp ../src/keymap.cpp
HEADERS += ../src/control.h ../src/shield.h ../src/keymap.h
RESOURCES += ../src/resources.qrc
