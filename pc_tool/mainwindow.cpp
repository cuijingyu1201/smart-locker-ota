// mainwindow.cpp - Main window class implementation (4 tabs)
#include "mainwindow.h"
#include "sensortab.h"
#include "mqttconsoletab.h"
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // ---- Window title and size ----
    setWindowTitle("IotOtaTool v1.0");
    resize(900, 600);

    // ---- Create the tab container ----
    m_tabs = new QTabWidget(this);
    setCentralWidget(m_tabs);   // make tabs fill the whole window

    // ---- Tab 1: Sensor Monitor (D14 step 3: QPainter real-time curve) ----
    {
        SensorTab *page = new SensorTab(this);
        m_tabs->addTab(page, "Sensor Monitor");
    }

    // ---- Tab 2: MQTT Console (D15) ----
    {
        MqttConsoleTab *page = new MqttConsoleTab(this);
        m_tabs->addTab(page, "MQTT Console");
    }

    // ---- Tab 3: OTA WiFi Upgrade (later) ----
    {
        QWidget *page = new QWidget(this);
        QVBoxLayout *lay = new QVBoxLayout(page);
        QLabel *lbl = new QLabel(
            "Tab 3: OTA WiFi Upgrade\n"
            "Will implement: firmware info + OTA server status + progress bar",
            page);
        lbl->setAlignment(Qt::AlignCenter);
        lay->addWidget(lbl);
        m_tabs->addTab(page, "OTA WiFi Upgrade");
    }

    // ---- Tab 4: Serial IAP (Ymodem) (later) ----
    {
        QWidget *page = new QWidget(this);
        QVBoxLayout *lay = new QVBoxLayout(page);
        QLabel *lbl = new QLabel(
            "Tab 4: Serial IAP (Ymodem)\n"
            "Will implement: port settings + firmware file + send button",
            page);
        lbl->setAlignment(Qt::AlignCenter);
        lay->addWidget(lbl);
        m_tabs->addTab(page, "Serial IAP");
    }
}