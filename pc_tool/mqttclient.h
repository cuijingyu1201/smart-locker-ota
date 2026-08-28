// mqttclient.h - Manual MQTT 3.1.1 client using QTcpSocket
#ifndef MQTTCLIENT_H
#define MQTTCLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>
#include <QString>

// MQTT 3.1.1 Packet Types (high nibble of byte 1)
enum MqttPacketType {
    MQTT_CONNECT     = 0x10,
    MQTT_CONNACK     = 0x20,
    MQTT_PUBLISH     = 0x30,
    MQTT_PUBACK      = 0x40,
    MQTT_SUBSCRIBE   = 0x80,
    MQTT_SUBACK      = 0x90,
    MQTT_PINGREQ     = 0xC0,
    MQTT_PINGRESP    = 0xD0,
    MQTT_DISCONNECT  = 0xE0
};

// Connection state
enum MqttState {
    MQTT_DISCONNECTED = 0,
    MQTT_CONNECTING   = 1,
    MQTT_CONNECTED    = 2
};

class MqttClient : public QObject
{
    Q_OBJECT

public:
    explicit MqttClient(QObject *parent = nullptr);
    ~MqttClient();

    // ---- Public API ----
    void connectToHost(const QString &host, quint16 port,
                       const QString &clientId,
                       const QString &username = "",
                       const QString &password = "");
    void disconnectFromHost();
    int  subscribe(const QString &topic, quint8 qos = 0);   // returns packet id
    int  publish(const QString &topic, const QByteArray &payload,
                quint8 qos = 0);                           // returns packet id
    MqttState state() const { return m_state; }

signals:
    void connected();
    void disconnected();
    void messageReceived(const QByteArray &payload, const QString &topic);
    void errorOccurred(const QString &errMsg);

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketReadyRead();
    void onSocketError(QAbstractSocket::SocketError err);
    void onPingTimer();

private:
    QTcpSocket *m_socket;
    QTimer      *m_pingTimer;     // keep-alive ping
    MqttState    m_state;

    // Connection params (saved for reconnection)
    QString m_host;
    quint16 m_port;
    QString m_clientId;
    QString m_username;
    QString m_password;

    // Receive buffer (incoming TCP data)
    QByteArray  m_rxBuffer;

    // Packet ID counter (for SUBSCRIBE/PUBLISH QoS1)
    quint16      m_packetId;

    // ---- MQTT encoding helpers ----
    void sendPacket(const QByteArray &packet);

    // Encode remaining length (MQTT variable-length encoding)
    QByteArray encodeRemLen(quint32 len);

    // Encode UTF-8 string (2-byte big-endian length + UTF-8 data)
    QByteArray encodeString(const QString &str);

    // Build specific packet types
    QByteArray buildConnectPacket();
    QByteArray buildSubscribePacket(const QString &topic, quint8 qos, quint16 pktId);
    QByteArray buildPublishPacket(const QString &topic, const QByteArray &payload,
                                  quint8 qos, quint16 pktId);
    QByteArray buildPingreqPacket();
    QByteArray buildDisconnectPacket();

    // Parse incoming data from m_rxBuffer
    void parseIncoming();

    // Decode remaining length from buffer starting at pos
    // Returns the length value, advances pos past the remlen bytes
    quint32 decodeRemLen(const QByteArray &buf, int &pos);
};

#endif // MQTTCLIENT_H