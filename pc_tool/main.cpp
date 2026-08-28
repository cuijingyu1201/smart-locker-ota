// main.cpp - Program entry point
#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    // QApplication: manages GUI application's control flow and main settings
    QApplication app(argc, argv);

    // Create and show the main window
    MainWindow w;
    w.show();

    // Enter the main event loop (equivalent to while(1) in RTOS)
    return app.exec();
}