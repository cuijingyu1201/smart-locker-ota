// mqttclient.cpp - Manual MQTT 3.1.1 implementation
#include "mqttclient.h"
#include <QtEndian>
#include <QDebug>

MqttClient::MqttClient(QObject *parent)
    : QObject(parent)
    , m_state(MQTT_DISCONNECTED)
    , m_packetId(0)
{
    m_socket = new QTcpSocket(this);
    m_pingTimer = new QTimer(this);
    m_pingTimer->setInterval(30000);   // 30s keep-alive (broker default 60s)

    connect(m_socket, &QTcpSocket::connected,
            this, &MqttClient::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &MqttClient::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &MqttClient::onSocketReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &MqttClient::onSocketError);
    connect(m_pingTimer, &QTimer::timeout,
            this, &MqttClient::onPingTimer);
}

MqttClient::~MqttClient()
{
    if (m_socket->isOpen()) m_socket->close();
}

// ==================================================================
// Public API
// ==================================================================

void MqttClient::connectToHost(const QString &host, quint16 port,
                               const QString &clientId,
                               const QString &username,
                               const QString &password)
{
    m_host = host;
    m_port = port;
    m_clientId = clientId;
    m_username = username;
    m_password = password;
    m_state = MQTT_CONNECTING;
    m_rxBuffer.clear();
    m_socket->connectToHost(host, port);
}

void MqttClient::disconnectFromHost()
{
    if (m_state == MQTT_CONNECTED) {
        m_pingTimer->stop();
        sendPacket(buildDisconnectPacket());
    }
    m_socket->close();
    m_state = MQTT_DISCONNECTED;
}

int MqttClient::subscribe(const QString &topic, quint8 qos)
{
    if (m_state != MQTT_CONNECTED) return -1;
    quint16 id = ++m_packetId;
    QByteArray pkt = buildSubscribePacket(topic, qos, id);
    sendPacket(pkt);
    return id;
}

int MqttClient::publish(const QString &topic, const QByteArray &payload,
                        quint8 qos)
{
    if (m_state != MQTT_CONNECTED) return -1;
    quint16 id = (qos > 0) ? ++m_packetId : 0;
    QByteArray pkt = buildPublishPacket(topic, payload, qos, id);
    sendPacket(pkt);
    return id;
}

// ==================================================================
// Socket slots
// ==================================================================

void MqttClient::onSocketConnected()
{
    // Send CONNECT packet immediately after TCP connect
    QByteArray pkt = buildConnectPacket();
    sendPacket(pkt);
    // CONNACK will arrive asynchronously; we wait for it in parseIncoming()
}

void MqttClient::onSocketDisconnected()
{
    m_pingTimer->stop();
    if (m_state == MQTT_CONNECTED) {
        m_state = MQTT_DISCONNECTED;
        emit disconnected();
    } else {
        m_state = MQTT_DISCONNECTED;
    }
}

void MqttClient::onSocketReadyRead()
{
    m_rxBuffer.append(m_socket->readAll());
    parseIncoming();
}

void MqttClient::onSocketError(QAbstractSocket::SocketError err)
{
    Q_UNUSED(err);
    emit errorOccurred(m_socket->errorString());
    m_state = MQTT_DISCONNECTED;
}

void MqttClient::onPingTimer()
{
    if (m_state == MQTT_CONNECTED) {
        sendPacket(buildPingreqPacket());
    }
}

// ==================================================================
// Packet encoding
// ==================================================================

// MQTT variable-length encoding for "Remaining Length" field
// Values 0~127: 1 byte
// Values 128~16383: 2 bytes
// Values up to 268435455: 3-4 bytes
QByteArray MqttClient::encodeRemLen(quint32 len)
{
    QByteArray out;
    quint8 digit;
    do {
        digit = len % 128;
        len /= 128;
        if (len > 0) digit |= 0x80;   // continuation bit
        out.append((char)digit);
    } while (len > 0 && out.size() < 4);
    return out;
}

// Encode UTF-8 string as: 2-byte big-endian length + UTF-8 bytes
QByteArray MqttClient::encodeString(const QString &str)
{
    QByteArray utf8 = str.toUtf8();
    QByteArray out;
    quint16 len = utf8.size();
    out.append((char)(len >> 8));
    out.append((char)(len & 0xFF));
    out.append(utf8);
    return out;
}

// CONNECT packet (MQTT 3.1.1)
// Fixed header: 0x10
// Variable header: Protocol Name "MQTT"(4) + Protocol Level(4) + Connect Flags + Keep Alive
// Payload: Client ID + (Username? + Password?)
QByteArray MqttClient::buildConnectPacket()
{
    QByteArray varHeader;
    varHeader.append(encodeString("MQTT"));   // protocol name
    varHeader.append((char)0x04);             // protocol level (4 = 3.1.1)

    // Connect Flags byte
    quint8 flags = 0x02;   // Clean Session = 1 (bit1)
    if (!m_username.isEmpty()) flags |= 0x80;   // Username Flag (bit7)
    if (!m_password.isEmpty()) flags |= 0x40;   // Password Flag (bit6)
    varHeader.append((char)flags);

    varHeader.append((char)0x00);   // Keep Alive MSB
    varHeader.append((char)0x3C);   // Keep Alive LSB (60 seconds)

    QByteArray payload;
    payload.append(encodeString(m_clientId));
    if (!m_username.isEmpty()) payload.append(encodeString(m_username));
    if (!m_password.isEmpty()) payload.append(encodeString(m_password));

    QByteArray packet;
    packet.append((char)MQTT_CONNECT);   // 0x10
    packet.append(encodeRemLen(varHeader.size() + payload.size()));
    packet.append(varHeader);
    packet.append(payload);
    return packet;
}

// SUBSCRIBE packet
// Fixed header: 0x82 (SUBSCRIBE + QoS1)
// Variable header: Packet Identifier (2 bytes, big-endian)
// Payload: Topic Filter + Requested QoS (per subscription)
QByteArray MqttClient::buildSubscribePacket(const QString &topic, quint8 qos, quint16 pktId)
{
    QByteArray payload;
    payload.append((char)(pktId >> 8));
    payload.append((char)(pktId & 0xFF));
    payload.append(encodeString(topic));
    payload.append((char)qos);

    QByteArray packet;
    packet.append((char)(MQTT_SUBSCRIBE | 0x02));   // 0x82 (SUBSCRIBE, flags=2)
    packet.append(encodeRemLen(payload.size()));
    packet.append(payload);
    return packet;
}

// PUBLISH packet
// Fixed header: 0x30 | dup<<3 | qos<<1 | retain
QByteArray MqttClient::buildPublishPacket(const QString &topic, const QByteArray &payload,
                                          quint8 qos, quint16 pktId)
{
    QByteArray varHeader;
    varHeader.append(encodeString(topic));
    if (qos > 0) {
        varHeader.append((char)(pktId >> 8));
        varHeader.append((char)(pktId & 0xFF));
    }

    quint8 firstByte = MQTT_PUBLISH | ((qos & 0x03) << 1);   // 0x30 | qos<<1

    QByteArray packet;
    packet.append((char)firstByte);
    packet.append(encodeRemLen(varHeader.size() + payload.size()));
    packet.append(varHeader);
    packet.append(payload);
    return packet;
}

// PINGREQ: 0xC0 + remlen=0x00
QByteArray MqttClient::buildPingreqPacket()
{
    QByteArray pkt;
    pkt.append((char)MQTT_PINGREQ);   // 0xC0
    pkt.append((char)0x00);
    return pkt;
}

// DISCONNECT: 0xE0 + remlen=0x00
QByteArray MqttClient::buildDisconnectPacket()
{
    QByteArray pkt;
    pkt.append((char)MQTT_DISCONNECT);   // 0xE0
    pkt.append((char)0x00);
    return pkt;
}

void MqttClient::sendPacket(const QByteArray &packet)
{
    if (m_socket) m_socket->write(packet);
}

// ==================================================================
// Incoming data parsing
// ==================================================================

quint32 MqttClient::decodeRemLen(const QByteArray &buf, int &pos)
{
    quint32 multiplier = 1;
    quint32 value = 0;
    quint8 byte;
    int iterations = 0;
    do {
        if (pos >= buf.size()) return 0;   // incomplete
        byte = (quint8)buf[pos++];
        value += (byte & 0x7F) * multiplier;
        multiplier *= 128;
        iterations++;
        if (iterations > 4) break;
    } while ((byte & 0x80) != 0);
    return value;
}

void MqttClient::parseIncoming()
{
    // Loop until we can't parse a complete packet
    while (m_rxBuffer.size() >= 2) {
        quint8 firstByte = (quint8)m_rxBuffer[0];
        quint8 pktType = firstByte & 0xF0;   // high nibble
        quint8 flags   = firstByte & 0x0F;   // low nibble

        int pos = 1;
        quint32 remLen = decodeRemLen(m_rxBuffer, pos);

        // Check if we have the full packet
        if (m_rxBuffer.size() < pos + remLen) break;   // need more data

        QByteArray payload = m_rxBuffer.mid(pos, remLen);
        // Remove consumed bytes from receive buffer
        m_rxBuffer.remove(0, pos + remLen);

        // Dispatch by packet type
        switch (pktType) {
        case MQTT_CONNACK: {   // 0x20
            // CONNACK payload: 2 bytes (Connect Acknowledge Flags + Return Code)
            if (payload.size() >= 2) {
                quint8 returnCode = (quint8)payload[1];
                if (returnCode == 0) {
                    m_state = MQTT_CONNECTED;
                    m_pingTimer->start();
                    emit connected();
                } else {
                    m_state = MQTT_DISCONNECTED;
                    QString err = QString("CONNACK failed, code=%1").arg(returnCode);
                    emit errorOccurred(err);
                }
            }
            break;
        }

        case MQTT_PUBLISH: {   // 0x30
            // PUBLISH flags: QoS bits in firstByte bits 1-2
            quint8 qos = (flags >> 1) & 0x03;
            int idx = 0;

            // Topic name: 2-byte len + UTF-8
            quint16 topicLen = ((quint8)payload[idx] << 8) | (quint8)payload[idx+1];
            idx += 2;
            QString topic = QString::fromUtf8(payload.mid(idx, topicLen));
            idx += topicLen;

            // Packet ID (only for QoS 1 and 2)
            if (qos > 0) idx += 2;

            // Remaining bytes = application payload
            QByteArray appPayload = payload.mid(idx);

            emit messageReceived(appPayload, topic);

            // Send PUBACK for QoS1
            if (qos == 1 && idx >= 2) {
                quint16 pktId = ((quint8)payload[topicLen+2] << 8) |
                                (quint8)payload[topicLen+3];
                QByteArray puback;
                puback.append((char)MQTT_PUBACK);   // 0x40
                puback.append((char)0x02);          // remlen=2
                puback.append((char)(pktId >> 8));
                puback.append((char)(pktId & 0xFF));
                sendPacket(puback);
            }
            break;
        }

        case MQTT_PINGRESP: {   // 0xD0
            // Nothing to do, just keep connection alive
            break;
        }

        case MQTT_SUBACK: {   // 0x90
            // We could verify the packet ID here, but for simplicity just ignore
            break;
        }

        default:
            qDebug() << "MQTT: unhandled packet type" <<  pktType;
            break;
        }
    }
}