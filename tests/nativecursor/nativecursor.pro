QT -= core gui

TEMPLATE = app
TARGET = native-cursor-test
CONFIG += console c++17 testcase
CONFIG -= app_bundle qt

SOURCES += \
    main.cpp \
    ../../app/platform/windows/nativecursor_win.cpp

HEADERS += \
    ../../app/platform/windows/nativecursor_win.h

INCLUDEPATH += ../../app
LIBS += user32.lib gdi32.lib
