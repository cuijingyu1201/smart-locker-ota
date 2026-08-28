// mqttconsoletab.h - MQTT Console tab UI
#ifndef MQTTCONSOLETAB_H
#define MQTTCONSOLETAB_H

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include "mqttclient.h"

class MqttConsoleTab : public QWidget
{
    Q_OBJECT

public:
    explicit MqttConsoleTab(QWidget *parent = nullptr);

signals:
    void sensorDataReceived(double temp, double humi);   // 转发给 SensorTab

private slots:
    void onConnect();
    void onDisconnect();
    void onSubscribe();
    void onSend();

    // MqttClient signal handlers
    void onMqttConnected();
    void onMqttDisconnected();
    void onMqttMessageReceived(const QByteArray &payload, const QString &topic);
    void onMqttError(const QString &errMsg);

private:
    MqttClient *m_client;

    // Top: connection
    QLineEdit   *m_hostEdit;
    QLineEdit   *m_portEdit;
    QLineEdit   *m_clientIdEdit;
    QPushButton *m_connectBtn;
    QPushButton *m_disconnectBtn;
    QLabel      *m_statusLabel;

    // Middle: subscribe
    QLineEdit   *m_subTopicEdit;
    QPushButton *m_subBtn;

    // Left bottom: publish
    QLineEdit      *m_pubTopicEdit;
    QPlainTextEdit *m_pubPayloadEdit;
    QPushButton    *m_pubBtn;

    // Right: colored log
    QPlainTextEdit *m_logEdit;

    void logMsg(const QString &text, const QColor &color = Qt::black);
};

#endif // MQTTCONSOLETAB_H
