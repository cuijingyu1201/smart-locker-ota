# IotOtaTool.pro - qmake project file
# Generated for D14: Qt6 PC tool, 4-Tab skeleton

QT       += core gui widgets network
CONFIG   += c++17

TARGET   = IotOtaTool
TEMPLATE  = app

# Source files
SOURCES  += \
    main.cpp \
    mainwindow.cpp \
    sensortab.cpp

# Header files
HEADERS  += \
    mainwindow.h \
    sensortab.h

# Output path
DESTDIR  = $$PWD/bin