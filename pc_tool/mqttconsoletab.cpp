// mqttconsoletab.cpp - MQTT Console implementation
#include "mqttconsoletab.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QDateTime>
#include <cstdlib>

MqttConsoleTab::MqttConsoleTab(QWidget *parent)
    : QWidget(parent)
{
    m_client = new MqttClient(this);

    // ---- 1. Top row: connection settings ----
    auto *topRow = new QHBoxLayout();
    m_hostEdit     = new QLineEdit("broker.emqx.io", this);
    m_portEdit     = new QLineEdit("1883", this);
    m_clientIdEdit = new QLineEdit("pc_demo_" + QString::number(rand() % 10000), this);
    m_connectBtn   = new QPushButton("Connect", this);
    m_disconnectBtn = new QPushButton("Disconnect", this);
    m_statusLabel  = new QLabel("Disconnected", this);
    m_statusLabel->setStyleSheet("color: gray;");

    topRow->addWidget(new QLabel("Broker:", this));
    topRow->addWidget(m_hostEdit, 2);
    topRow->addWidget(new QLabel("Port:", this));
    topRow->addWidget(m_portEdit, 1);
    topRow->addWidget(new QLabel("ClientID:", this));
    topRow->addWidget(m_clientIdEdit, 1);
    topRow->addWidget(m_connectBtn);
    topRow->addWidget(m_disconnectBtn);
    topRow->addWidget(m_statusLabel);

    // ---- 2. Subscribe row ----
    auto *subRow = new QHBoxLayout();
    m_subTopicEdit = new QLineEdit("iot/dev001/#", this);
    m_subBtn = new QPushButton("Subscribe", this);
    subRow->addWidget(new QLabel("Topic:", this));
    subRow->addWidget(m_subTopicEdit, 1);
    subRow->addWidget(m_subBtn);

    // ---- 3. Left bottom: publish area ----
    auto *pubLayout = new QVBoxLayout();
    auto *pubTopicRow = new QHBoxLayout();
    m_pubTopicEdit = new QLineEdit("iot/dev001/ota", this);
    pubTopicRow->addWidget(new QLabel("Topic:", this));
    pubTopicRow->addWidget(m_pubTopicEdit, 1);

    m_pubPayloadEdit = new QPlainTextEdit(this);
    m_pubPayloadEdit->setPlaceholderText(
        "{\"type\":\"ota\",\"major\":1,\"minor\":0,\"patch\":0,\"build\":2,\"size\":33336,\"crc\":0x34518D29}");
    m_pubBtn = new QPushButton("Send", this);

    pubLayout->addLayout(pubTopicRow);
    pubLayout->addWidget(new QLabel("Payload (JSON):", this));
    pubLayout->addWidget(m_pubPayloadEdit, 1);
    pubLayout->addWidget(m_pubBtn);

    // ---- 4. Right: colored log ----
    m_logEdit = new QPlainTextEdit(this);
    m_logEdit->setReadOnly(true);

    // ---- 5. Splitter (left = publish, right = log) ----
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    auto *leftWidget = new QWidget(this);
    leftWidget->setLayout(pubLayout);
    splitter->addWidget(leftWidget);
    splitter->addWidget(m_logEdit);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    // ---- 6. Main vertical layout ----
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(topRow);
    mainLayout->addLayout(subRow);
    mainLayout->addWidget(splitter, 1);

    // ---- 7. Button signals ----
    connect(m_connectBtn,    &QPushButton::clicked, this, &MqttConsoleTab::onConnect);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &MqttConsoleTab::onDisconnect);
    connect(m_subBtn,       &QPushButton::clicked, this, &MqttConsoleTab::onSubscribe);
    connect(m_pubBtn,       &QPushButton::clicked, this, &MqttConsoleTab::onSend);

    // MqttClient signals
    connect(m_client, &MqttClient::connected,
            this, &MqttConsoleTab::onMqttConnected);
    connect(m_client, &MqttClient::disconnected,
            this, &MqttConsoleTab::onMqttDisconnected);
    connect(m_client, &MqttClient::messageReceived,
            this, &MqttConsoleTab::onMqttMessageReceived);
    connect(m_client, &MqttClient::errorOccurred,
            this, &MqttConsoleTab::onMqttError);
}

// ==================================================================
// Button slots
// ==================================================================

void MqttConsoleTab::onConnect()
{
    logMsg(QString("TCP %1:%2 connecting...")
               .arg(m_hostEdit->text()).arg(m_portEdit->text()), Qt::darkGray);
    m_client->connectToHost(m_hostEdit->text().trimmed(),
                            m_portEdit->text().toInt(),
                            m_clientIdEdit->text().trimmed());
}

void MqttConsoleTab::onDisconnect()
{
    logMsg("Disconnecting...", Qt::darkGray);
    m_client->disconnectFromHost();
}

void MqttConsoleTab::onSubscribe()
{
    if (m_client->state() != MQTT_CONNECTED) {
        logMsg("ERROR: Not connected!", QColor(200, 0, 0));
        return;
    }
    QString topic = m_subTopicEdit->text().trimmed();
    int id = m_client->subscribe(topic, 0);
    if (id >= 0) {
        logMsg(QString("Subscribe sent: %1").arg(topic), Qt::darkBlue);
    } else {
        logMsg("Subscribe FAILED", QColor(200, 0, 0));
    }
}

void MqttConsoleTab::onSend()
{
    if (m_client->state() != MQTT_CONNECTED) {
        logMsg("ERROR: Not connected!", QColor(200, 0, 0));
        return;
    }
    QString topic = m_pubTopicEdit->text().trimmed();
    QString payload = m_pubPayloadEdit->toPlainText().trimmed();
    int id = m_client->publish(topic, payload.toUtf8(), 1);
    if (id >= 0) {
        logMsg(QString("TX [%1]: %2").arg(topic).arg(payload), QColor(0, 120, 0));
    } else {
        logMsg("Publish FAILED", QColor(200, 0, 0));
    }
}

// ==================================================================
// MqttClient callbacks
// ==================================================================

void MqttConsoleTab::onMqttConnected()
{
    logMsg("MQTT CONNACK success!", QColor(0, 120, 0));
    m_statusLabel->setText("Online");
    m_statusLabel->setStyleSheet("color: green;");
}

void MqttConsoleTab::onMqttDisconnected()
{
    logMsg("MQTT disconnected", QColor(200, 100, 0));
    m_statusLabel->setText("Disconnected");
    m_statusLabel->setStyleSheet("color: gray;");
}

void MqttConsoleTab::onMqttMessageReceived(const QByteArray &payload,
                                           const QString &topic)
{
    QColor color;
    if (topic.contains("sensor"))       color = QColor(0, 80, 160);    // blue
    else if (topic.contains("ota"))     color = QColor(140, 0, 140);   // purple
    else                                color = Qt::black;

    logMsg(QString("RX [%1]: %2").arg(topic).arg(QString::fromUtf8(payload)), color);

    // Parse sensor JSON and emit signal for SensorTab (D15 enhancement)
    if (topic.contains("sensor")) {
        QByteArray p = payload;
        // Simple regex-less parse: find "temp":xx.x and "humi":xx.x
        // We just forward raw JSON for now; SensorTab will parse later
        emit sensorDataReceived(0, 0);   // placeholder (real parsing in step 4)
    }
}

void MqttConsoleTab::onMqttError(const QString &errMsg)
{
    logMsg(QString("ERROR: %1").arg(errMsg), QColor(200, 0, 0));
    m_statusLabel->setText("Error");
    m_statusLabel->setStyleSheet("color: red;");
}

// ==================================================================
// Colored log
// ==================================================================

void MqttConsoleTab::logMsg(const QString &text, const QColor &color)
{
    QString ts = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString line = QString("[%1] %2").arg(ts).arg(text);

    QTextCharFormat fmt;
    fmt.setForeground(color);

    QTextCursor cursor(m_logEdit->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(line + "\n", fmt);
    m_logEdit->ensureCursorVisible();
}