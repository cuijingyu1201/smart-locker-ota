# IotOtaTool.pro - qmake project file
# Generated for D14: Qt6 PC tool, 4-Tab skeleton

QT       += core gui widgets network serialport
CONFIG   += c++17

TARGET   = IotOtaTool
TEMPLATE  = app

# Source files
SOURCES  += \
    main.cpp \
    mainwindow.cpp \
    mqttclient.cpp \
    mqttconsoletab.cpp \
    sensortab.cpp

# Header files
HEADERS  += \
    mainwindow.h \
    mqttclient.h \
    mqttconsoletab.h \
    sensortab.h

# Output path
DESTDIR  = $$PWD/bin