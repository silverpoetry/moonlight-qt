QT += core
QT -= gui

TEMPLATE = app
TARGET = clipboard-manifest-test
CONFIG += console c++17 testcase
CONFIG -= app_bundle

include(../../globaldefs.pri)

SOURCES += \
    main.cpp \
    ../../app/streaming/clipboardfilemanifest.cpp

HEADERS += \
    ../../app/streaming/clipboardfilemanifest.h

INCLUDEPATH += \
    ../../app/streaming \
    ../../moonlight-common-c/moonlight-common-c/src

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../../moonlight-common-c/release/ -lmoonlight-common-c
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../../moonlight-common-c/debug/ -lmoonlight-common-c
else:unix: LIBS += -L$$OUT_PWD/../../moonlight-common-c/ -lmoonlight-common-c
