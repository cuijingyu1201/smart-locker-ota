// mainwindow.h - Main window class declaration
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>

// MainWindow: top-level window containing 4 tabs
class MainWindow : public QMainWindow
{
    Q_OBJECT   // Qt macro: required for signals/slots

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    QTabWidget *m_tabs;   // the tab container
};

#endif // MAINWINDOW_H