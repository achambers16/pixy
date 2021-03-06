#-------------------------------------------------
#
# Project created by QtCreator 2012-07-30T13:51:47
#
#-------------------------------------------------

QT       += core gui

TARGET = pixymon
TEMPLATE = app
RC_FILE = resources.rc

SOURCES += main.cpp\
        mainwindow.cpp \
    videowidget.cpp \
    usblink.cpp \
    console.cpp \
    interpreter.cpp \
    renderer.cpp \
    chirpmon.cpp \
    ../libpixy/chirp.cpp \
    calc.cpp \
    blob.cpp \
    blobs.cpp \
    clut.cpp

HEADERS  += mainwindow.h \
    link.h \
    videowidget.h \
    usblink.h \
    console.h \
    interpreter.h \
    renderer.h \
    chirpmon.h \
    ../libpixy/chirp.hpp \
    calc.h \
    blobs.h \
    blob.h \
    clut.h

INCLUDEPATH += ../libpixy

FORMS    += mainwindow.ui

LIBS += ./libusb.a

RESOURCES += \
    resources.qrc

































