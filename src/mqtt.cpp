#include <cassert>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qtng/mqtt.h"
#include "qtng/coroutine_utils.h"
#include "qtng/eventloop.h"
#include "qtng/locks.h"
#include "qtng/random.h"
#include "qtng/socket.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/logging.h"

#ifndef QTNG_NO_CRYPTO
#  include "qtng/ssl.h"
#endif


using namespace std;

NG_LOGGER("qtng.mqtt");

namespace qtng {

namespace {

enum PacketType : uint8_t {
    Reserved0 = 0,
    CONNECT = 1,
    CONNACK = 2,
    PUBLISH = 3,
    PUBACK = 4,
    PUBREC = 5,
    PUBREL = 6,
    PUBCOMP = 7,
    SUBSCRIBE = 8,
    SUBACK = 9,
    UNSUBSCRIBE = 10,
    UNSUBACK = 11,
    PINGREQ = 12,
    PINGRESP = 13,
    DISCONNECT = 14,
};

bool encodeRemainingLength(uint32_t length, string &out)
{
    if (length > 268435455u) {
        return false;
    }
    do {
        uint8_t encoded = static_cast<uint8_t>(length % 128);
        length /= 128;
        if (length > 0) {
            encoded |= 0x80;
        }
        out.push_back(static_cast<char>(encoded));
    } while (length > 0);
    return true;
}

bool decodeRemainingLength(const char *data, size_t size, size_t &consumed, uint32_t &length)
{
    length = 0;
    consumed = 0;
    uint32_t multiplier = 1;
    for (size_t i = 0; i < 4; ++i) {
        if (i >= size) {
            return false;
        }
        uint8_t encoded = static_cast<uint8_t>(data[i]);
        length += static_cast<uint32_t>(encoded & 0x7f) * multiplier;
        ++consumed;
        if ((encoded & 0x80) == 0) {
            return true;
        }
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128) {
            return false;
        }
    }
    return false;
}

void appendUint16(string &out, uint16_t value)
{
    char buf[2];
    ngToBigEndian(value, buf);
    out.append(buf, 2);
}

void appendMqttString(string &out, const string &value)
{
    appendUint16(out, static_cast<uint16_t>(value.size()));
    out.append(value);
}

bool readUint16(const string &payload, size_t &offset, uint16_t &value)
{
    if (offset + 2 > payload.size()) {
        return false;
    }
    value = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data() + offset));
    offset += 2;
    return true;
}

bool readMqttString(const string &payload, size_t &offset, string &value)
{
    uint16_t len = 0;
    if (!readUint16(payload, offset, len)) {
        return false;
    }
    if (offset + len > payload.size()) {
        return false;
    }
    value.assign(payload.data() + offset, len);
    offset += len;
    return true;
}

string makeFixedHeader(PacketType type, uint8_t flags, uint32_t remainingLength)
{
    string out;
    out.push_back(static_cast<char>((static_cast<uint8_t>(type) << 4) | (flags & 0x0f)));
    if (!encodeRemainingLength(remainingLength, out)) {
        return string();
    }
    return out;
}

string encodeConnect(const string &clientId, bool cleanSession, uint16_t keepAlive, const string &username,
                     const string &password, bool hasWill, const MqttMessage &will)
{
    string variable;
    appendMqttString(variable, "MQTT");
    variable.push_back(0x04);  // protocol level 4 = MQTT 3.1.1

    uint8_t connectFlags = 0;
    if (cleanSession) {
        connectFlags |= 0x02;
    }
    if (hasWill) {
        connectFlags |= 0x04;
        connectFlags |= (static_cast<uint8_t>(will.qos) & 0x03) << 3;
        if (will.retain) {
            connectFlags |= 0x20;
        }
    }
    if (!password.empty()) {
        connectFlags |= 0x40;
    }
    if (!username.empty()) {
        connectFlags |= 0x80;
    }
    variable.push_back(static_cast<char>(connectFlags));
    appendUint16(variable, keepAlive);
    appendMqttString(variable, clientId);
    if (hasWill) {
        appendMqttString(variable, will.topic);
        appendMqttString(variable, will.payload);
    }
    if (!username.empty()) {
        appendMqttString(variable, username);
    }
    if (!password.empty()) {
        appendMqttString(variable, password);
    }

    string packet = makeFixedHeader(CONNECT, 0, static_cast<uint32_t>(variable.size()));
    if (packet.empty()) {
        return string();
    }
    packet.append(variable);
    return packet;
}

string encodePublish(const MqttMessage &msg, uint16_t packetId)
{
    string variable;
    appendMqttString(variable, msg.topic);
    if (msg.qos != MqttQos::AtMostOnce) {
        appendUint16(variable, packetId);
    }
    variable.append(msg.payload);

    uint8_t flags = 0;
    if (msg.dup) {
        flags |= 0x08;
    }
    flags |= (static_cast<uint8_t>(msg.qos) & 0x03) << 1;
    if (msg.retain) {
        flags |= 0x01;
    }

    string packet = makeFixedHeader(PUBLISH, flags, static_cast<uint32_t>(variable.size()));
    if (packet.empty()) {
        return string();
    }
    packet.append(variable);
    return packet;
}

string encodePacketIdOnly(PacketType type, uint8_t flags, uint16_t packetId)
{
    string variable;
    appendUint16(variable, packetId);
    string packet = makeFixedHeader(type, flags, static_cast<uint32_t>(variable.size()));
    if (packet.empty()) {
        return string();
    }
    packet.append(variable);
    return packet;
}

string encodeSubscribe(uint16_t packetId, const string &topic, MqttQos qos)
{
    string variable;
    appendUint16(variable, packetId);
    appendMqttString(variable, topic);
    variable.push_back(static_cast<char>(static_cast<uint8_t>(qos) & 0x03));
    string packet = makeFixedHeader(SUBSCRIBE, 0x02, static_cast<uint32_t>(variable.size()));
    if (packet.empty()) {
        return string();
    }
    packet.append(variable);
    return packet;
}

string encodeUnsubscribe(uint16_t packetId, const string &topic)
{
    string variable;
    appendUint16(variable, packetId);
    appendMqttString(variable, topic);
    string packet = makeFixedHeader(UNSUBSCRIBE, 0x02, static_cast<uint32_t>(variable.size()));
    if (packet.empty()) {
        return string();
    }
    packet.append(variable);
    return packet;
}

string encodePingReq()
{
    return makeFixedHeader(PINGREQ, 0, 0);
}

string encodeDisconnect()
{
    return makeFixedHeader(DISCONNECT, 0, 0);
}

bool parsePublish(const string &payload, uint8_t flags, MqttMessage &msg, uint16_t &packetId, bool &hasPacketId)
{
    size_t offset = 0;
    if (!readMqttString(payload, offset, msg.topic)) {
        return false;
    }
    msg.dup = (flags & 0x08) != 0;
    msg.qos = static_cast<MqttQos>((flags >> 1) & 0x03);
    msg.retain = (flags & 0x01) != 0;
    hasPacketId = msg.qos != MqttQos::AtMostOnce;
    packetId = 0;
    if (hasPacketId) {
        if (!readUint16(payload, offset, packetId)) {
            return false;
        }
    }
    msg.payload.assign(payload.data() + offset, payload.size() - offset);
    return true;
}

string defaultClientId()
{
    return string("qtng-") + RandomGenerator::global().generateHex(8);
}

}  // namespace

MqttMessage::MqttMessage()
    : qos(MqttQos::AtMostOnce)
    , retain(false)
    , dup(false)
{
}

MqttMessage::MqttMessage(const string &topic, const string &payload, MqttQos qos, bool retain)
    : topic(topic)
    , payload(payload)
    , qos(qos)
    , retain(retain)
    , dup(false)
{
}

class MqttConfigurationPrivate
{
public:
    MqttConfigurationPrivate();
public:
    string clientId;
    string username;
    string password;
    MqttMessage will;
    bool hasWill;
    bool cleanSession;
    uint16_t keepAlive;
    uint32_t sendingQueueCapacity;
    uint32_t receivingQueueCapacity;
    int32_t maxPacketSize;
    float connectTimeout;
};

MqttConfigurationPrivate::MqttConfigurationPrivate()
    : hasWill(false)
    , cleanSession(true)
    , keepAlive(60)
    , sendingQueueCapacity(256)
    , receivingQueueCapacity(256)
    , maxPacketSize(256 * 1024)
    , connectTimeout(30.0f)
{
}

MqttConfiguration::MqttConfiguration()
    : d_ptr(new MqttConfigurationPrivate())
{
}

MqttConfiguration::MqttConfiguration(const MqttConfiguration &other)
    : d_ptr(new MqttConfigurationPrivate(*other.d_func()))
{
}

MqttConfiguration &MqttConfiguration::operator=(const MqttConfiguration &other)
{
    if (this != &other) {
        *d_func() = *other.d_func();
    }
    return *this;
}

MqttConfiguration::~MqttConfiguration()
{
    delete d_ptr;
}

void MqttConfiguration::setClientId(const string &clientId)
{
    NG_D(MqttConfiguration);
    d->clientId = clientId;
}

string MqttConfiguration::clientId() const
{
    NG_D(const MqttConfiguration);
    return d->clientId;
}

void MqttConfiguration::setCleanSession(bool cleanSession)
{
    NG_D(MqttConfiguration);
    d->cleanSession = cleanSession;
}

bool MqttConfiguration::cleanSession() const
{
    NG_D(const MqttConfiguration);
    return d->cleanSession;
}

void MqttConfiguration::setKeepAlive(uint16_t seconds)
{
    NG_D(MqttConfiguration);
    d->keepAlive = seconds;
}

uint16_t MqttConfiguration::keepAlive() const
{
    NG_D(const MqttConfiguration);
    return d->keepAlive;
}

void MqttConfiguration::setUsername(const string &username)
{
    NG_D(MqttConfiguration);
    d->username = username;
}

string MqttConfiguration::username() const
{
    NG_D(const MqttConfiguration);
    return d->username;
}

void MqttConfiguration::setPassword(const string &password)
{
    NG_D(MqttConfiguration);
    d->password = password;
}

string MqttConfiguration::password() const
{
    NG_D(const MqttConfiguration);
    return d->password;
}

void MqttConfiguration::setWill(const MqttMessage &will)
{
    NG_D(MqttConfiguration);
    d->will = will;
    d->hasWill = true;
}

MqttMessage MqttConfiguration::will() const
{
    NG_D(const MqttConfiguration);
    return d->will;
}

bool MqttConfiguration::hasWill() const
{
    NG_D(const MqttConfiguration);
    return d->hasWill;
}

void MqttConfiguration::clearWill()
{
    NG_D(MqttConfiguration);
    d->hasWill = false;
    d->will = MqttMessage();
}

void MqttConfiguration::setSendingQueueCapacity(uint32_t capacity)
{
    NG_D(MqttConfiguration);
    d->sendingQueueCapacity = capacity;
}

uint32_t MqttConfiguration::sendingQueueCapacity() const
{
    NG_D(const MqttConfiguration);
    return d->sendingQueueCapacity;
}

void MqttConfiguration::setReceivingQueueCapacity(uint32_t capacity)
{
    NG_D(MqttConfiguration);
    d->receivingQueueCapacity = capacity;
}

uint32_t MqttConfiguration::receivingQueueCapacity() const
{
    NG_D(const MqttConfiguration);
    return d->receivingQueueCapacity;
}

void MqttConfiguration::setMaxPacketSize(int32_t size)
{
    NG_D(MqttConfiguration);
    d->maxPacketSize = size;
}

int32_t MqttConfiguration::maxPacketSize() const
{
    NG_D(const MqttConfiguration);
    return d->maxPacketSize;
}

void MqttConfiguration::setConnectTimeout(float seconds)
{
    NG_D(MqttConfiguration);
    d->connectTimeout = seconds;
}

float MqttConfiguration::connectTimeout() const
{
    NG_D(const MqttConfiguration);
    return d->connectTimeout;
}

enum OutboundKind {
    OutboundRaw = 0,
    OutboundPublish,
    OutboundSubscribe,
    OutboundUnsubscribe,
    OutboundPubRel,
};

class PacketToWrite
{
public:
    PacketToWrite()
        : kind(OutboundRaw)
        , packetId(0)
        , qos(MqttQos::AtMostOnce)
    {
    }
    PacketToWrite(const string &raw)
        : kind(OutboundRaw)
        , raw(raw)
        , packetId(0)
        , qos(MqttQos::AtMostOnce)
    {
    }
    PacketToWrite(OutboundKind kind, const string &raw, uint16_t packetId, MqttQos qos,
                  shared_ptr<ValueEvent<bool>> done)
        : kind(kind)
        , raw(raw)
        , packetId(packetId)
        , qos(qos)
        , done(done)
    {
    }
public:
    OutboundKind kind;
    string raw;
    uint16_t packetId;
    MqttQos qos;
    shared_ptr<ValueEvent<bool>> done;
};

class MqttClientPrivate
{
public:
    MqttClientPrivate(shared_ptr<SocketLike> connection, const MqttConfiguration &config, MqttClient *q);
    ~MqttClientPrivate();
public:
    bool handshake();
    void doSend();
    void doReceive();
    void doKeepalive();
    void abort(MqttClient::MqttError err, const string &message);
    void disconnectGracefully();
    uint16_t nextPacketId();
    bool enqueueRaw(const string &packet, shared_ptr<ValueEvent<bool>> done = shared_ptr<ValueEvent<bool>>());
    bool enqueueOutbound(OutboundKind kind, const string &raw, uint16_t packetId, MqttQos qos,
                         shared_ptr<ValueEvent<bool>> done);
    bool readExact(string &out, size_t n);
    bool readPacket(uint8_t &type, uint8_t &flags, string &payload);
    void handleIncoming(uint8_t type, uint8_t flags, const string &payload);
    void completeWaiter(map<uint16_t, shared_ptr<ValueEvent<bool>>> &waiters, uint16_t packetId, bool ok);
    void failAllWaiters();
    void noteActivity();
public:
    CoroutineGroup *operations;
    shared_ptr<SocketLike> const connection;
    Queue<MqttMessage> receivingQueue;
    Queue<PacketToWrite> sendingQueue;
    map<uint16_t, shared_ptr<ValueEvent<bool>>> waitPubAck;
    map<uint16_t, shared_ptr<ValueEvent<bool>>> waitPubRec;
    map<uint16_t, shared_ptr<ValueEvent<bool>>> waitPubComp;
    map<uint16_t, shared_ptr<ValueEvent<bool>>> waitSubAck;
    map<uint16_t, shared_ptr<ValueEvent<bool>>> waitUnsubAck;
    map<uint16_t, MqttMessage> incomingQos2;
    MqttConfiguration config;
    MqttClient::State state;
    MqttClient::MqttError errorCode;
    string errorString;
    uint16_t nextId;
    int32_t maxPacketSize;
    int64_t lastActiveTimestamp;
    int64_t lastOutboundTimestamp;
    int64_t keepAliveMs;
    bool awaitingPingResp;
    bool operationsStarted;
private:
    MqttClient *const q_ptr;
    NG_DECLARE_PUBLIC(MqttClient);
};

MqttClientPrivate::MqttClientPrivate(shared_ptr<SocketLike> connection, const MqttConfiguration &config, MqttClient *q)
    : operations(new CoroutineGroup())
    , connection(connection)
    , receivingQueue(config.receivingQueueCapacity())
    , sendingQueue(config.sendingQueueCapacity())
    , config(config)
    , state(MqttClient::Connecting)
    , errorCode(MqttClient::NoError)
    , nextId(1)
    , maxPacketSize(config.maxPacketSize())
    , lastActiveTimestamp(utils::DateTime::currentMSecsSinceEpoch())
    , lastOutboundTimestamp(lastActiveTimestamp)
    , keepAliveMs(static_cast<int64_t>(config.keepAlive()) * 1000)
    , awaitingPingResp(false)
    , operationsStarted(false)
    , q_ptr(q)
{
}

MqttClientPrivate::~MqttClientPrivate()
{
    abort(MqttClient::UserShutdown, "destroyed");
    delete operations;
}

uint16_t MqttClientPrivate::nextPacketId()
{
    uint16_t id = nextId;
    if (nextId == 0xffff) {
        nextId = 1;
    } else {
        ++nextId;
    }
    if (id == 0) {
        id = nextPacketId();
    }
    return id;
}

void MqttClientPrivate::noteActivity()
{
    lastActiveTimestamp = utils::DateTime::currentMSecsSinceEpoch();
}

bool MqttClientPrivate::readExact(string &out, size_t n)
{
    out.clear();
    out.reserve(n);
    while (out.size() < n) {
        string chunk = connection->recv(static_cast<int32_t>(n - out.size()));
        if (chunk.empty()) {
            return false;
        }
        out.append(chunk);
    }
    return true;
}

bool MqttClientPrivate::readPacket(uint8_t &type, uint8_t &flags, string &payload)
{
    string first;
    if (!readExact(first, 1)) {
        return false;
    }
    type = static_cast<uint8_t>((static_cast<uint8_t>(first[0]) >> 4) & 0x0f);
    flags = static_cast<uint8_t>(first[0]) & 0x0f;

    string rlBytes;
    uint32_t remaining = 0;
    size_t consumed = 0;
    for (int i = 0; i < 4; ++i) {
        string b;
        if (!readExact(b, 1)) {
            return false;
        }
        rlBytes.append(b);
        if (!decodeRemainingLength(rlBytes.data(), rlBytes.size(), consumed, remaining)) {
            if (rlBytes.size() >= 4) {
                return false;
            }
            continue;
        }
        break;
    }
    if (consumed == 0) {
        return false;
    }
    if (maxPacketSize > 0 && static_cast<int32_t>(1 + consumed + remaining) > maxPacketSize) {
        abort(MqttClient::PacketTooLarge, "mqtt packet exceeds maxPacketSize");
        return false;
    }
    if (remaining == 0) {
        payload.clear();
        noteActivity();
        return true;
    }
    if (!readExact(payload, remaining)) {
        return false;
    }
    noteActivity();
    return true;
}

void MqttClientPrivate::completeWaiter(map<uint16_t, shared_ptr<ValueEvent<bool>>> &waiters, uint16_t packetId, bool ok)
{
    auto it = waiters.find(packetId);
    if (it == waiters.end()) {
        return;
    }
    shared_ptr<ValueEvent<bool>> done = it->second;
    waiters.erase(it);
    if (done) {
        done->send(ok);
    }
}

void MqttClientPrivate::failAllWaiters()
{
    auto failMap = [](map<uint16_t, shared_ptr<ValueEvent<bool>>> &waiters) {
        for (auto &pair : waiters) {
            if (pair.second) {
                pair.second->send(false);
            }
        }
        waiters.clear();
    };
    failMap(waitPubAck);
    failMap(waitPubRec);
    failMap(waitPubComp);
    failMap(waitSubAck);
    failMap(waitUnsubAck);
}

bool MqttClientPrivate::handshake()
{
    string clientId = config.clientId();
    if (clientId.empty()) {
        clientId = defaultClientId();
    }
    if (clientId.size() > 65535) {
        abort(MqttClient::ProtocolError, "client id too long");
        return false;
    }

    string connectPacket =
            encodeConnect(clientId, config.cleanSession(), config.keepAlive(), config.username(), config.password(),
                          config.hasWill(), config.will());
    if (connectPacket.empty()) {
        abort(MqttClient::ProtocolError, "failed to encode CONNECT");
        return false;
    }

    Timeout timeout(config.connectTimeout());
    (void) timeout;
    try {
        if (connection->sendall(connectPacket) != static_cast<int32_t>(connectPacket.size())) {
            abort(MqttClient::NetworkError, "failed to send CONNECT");
            return false;
        }
        lastOutboundTimestamp = utils::DateTime::currentMSecsSinceEpoch();

        uint8_t type = 0;
        uint8_t flags = 0;
        string payload;
        if (!readPacket(type, flags, payload)) {
            if (errorCode == MqttClient::NoError) {
                abort(MqttClient::NetworkError, "failed to read CONNACK");
            }
            return false;
        }
        if (type != CONNACK || payload.size() < 2) {
            abort(MqttClient::ProtocolError, "expected CONNACK");
            return false;
        }
        uint8_t returnCode = static_cast<uint8_t>(payload[1]);
        if (returnCode != 0) {
            MqttClient::MqttError err = MqttClient::ConnectionRefusedServer;
            switch (returnCode) {
            case 1:
                err = MqttClient::ConnectionRefusedProtocol;
                break;
            case 2:
                err = MqttClient::ConnectionRefusedIdentifier;
                break;
            case 3:
                err = MqttClient::ConnectionRefusedServer;
                break;
            case 4:
                err = MqttClient::ConnectionRefusedCredentials;
                break;
            case 5:
                err = MqttClient::ConnectionRefusedNotAuthorized;
                break;
            default:
                err = MqttClient::ProtocolError;
                break;
            }
            abort(err, "CONNACK refused");
            return false;
        }
    } catch (TimeoutException &) {
        abort(MqttClient::TimeoutError, "CONNECT timed out");
        return false;
    }

    state = MqttClient::Connected;
    operations->spawnWithName("send", [this] { doSend(); });
    operations->spawnWithName("receive", [this] { doReceive(); });
    operations->spawnWithName("keepalive", [this] { doKeepalive(); });
    operationsStarted = true;
    return true;
}

bool MqttClientPrivate::enqueueRaw(const string &packet, shared_ptr<ValueEvent<bool>> done)
{
    if (state != MqttClient::Connected && state != MqttClient::Disconnecting) {
        if (done) {
            done->send(false);
        }
        return false;
    }
    sendingQueue.put(PacketToWrite(OutboundRaw, packet, 0, MqttQos::AtMostOnce, done));
    return true;
}

bool MqttClientPrivate::enqueueOutbound(OutboundKind kind, const string &raw, uint16_t packetId, MqttQos qos,
                                        shared_ptr<ValueEvent<bool>> done)
{
    if (state != MqttClient::Connected) {
        if (done) {
            done->send(false);
        }
        return false;
    }
    sendingQueue.put(PacketToWrite(kind, raw, packetId, qos, done));
    return true;
}

void MqttClientPrivate::doSend()
{
    while (true) {
        PacketToWrite packet;
        try {
            packet = sendingQueue.get();
        } catch (CoroutineExitException &) {
            return;
        } catch (...) {
            ngCritical() << "unknown error in MqttClientPrivate::doSend()";
            return abort(MqttClient::InternalError, "send loop exception");
        }
        if (packet.raw.empty()) {
            if (packet.done) {
                packet.done->send(false);
            }
            continue;
        }
        if (connection->sendall(packet.raw) != static_cast<int32_t>(packet.raw.size())) {
            if (packet.done) {
                packet.done->send(false);
            }
            return abort(MqttClient::NetworkError, "send failed");
        }
        lastOutboundTimestamp = utils::DateTime::currentMSecsSinceEpoch();
        noteActivity();

        // QoS>0 and subscribe waiters are registered before enqueue to avoid ACK races.
        if (packet.kind == OutboundPublish && packet.qos == MqttQos::AtMostOnce) {
            if (packet.done) {
                packet.done->send(true);
            }
        } else if (packet.kind == OutboundRaw && packet.done) {
            packet.done->send(true);
        }
    }
}

void MqttClientPrivate::handleIncoming(uint8_t type, uint8_t flags, const string &payload)
{
    switch (type) {
    case PUBLISH: {
        MqttMessage msg;
        uint16_t packetId = 0;
        bool hasPacketId = false;
        if (!parsePublish(payload, flags, msg, packetId, hasPacketId)) {
            return abort(MqttClient::ProtocolError, "invalid PUBLISH");
        }
        if (msg.qos == MqttQos::AtMostOnce) {
            receivingQueue.put(msg);
        } else if (msg.qos == MqttQos::AtLeastOnce) {
            receivingQueue.put(msg);
            enqueueRaw(encodePacketIdOnly(PUBACK, 0, packetId));
        } else if (msg.qos == MqttQos::ExactlyOnce) {
            incomingQos2[packetId] = msg;
            enqueueRaw(encodePacketIdOnly(PUBREC, 0, packetId));
        } else {
            return abort(MqttClient::ProtocolError, "invalid PUBLISH qos");
        }
        break;
    }
    case PUBACK: {
        if (payload.size() < 2) {
            return abort(MqttClient::ProtocolError, "invalid PUBACK");
        }
        uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
        completeWaiter(waitPubAck, packetId, true);
        break;
    }
    case PUBREC: {
        if (payload.size() < 2) {
            return abort(MqttClient::ProtocolError, "invalid PUBREC");
        }
        uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
        auto it = waitPubRec.find(packetId);
        if (it == waitPubRec.end()) {
            break;
        }
        shared_ptr<ValueEvent<bool>> done = it->second;
        waitPubRec.erase(it);
        waitPubComp[packetId] = done;
        string pubrel = encodePacketIdOnly(PUBREL, 0x02, packetId);
        if (!enqueueOutbound(OutboundPubRel, pubrel, packetId, MqttQos::ExactlyOnce, done)) {
            waitPubComp.erase(packetId);
            if (done) {
                done->send(false);
            }
        }
        break;
    }
    case PUBREL: {
        if ((flags & 0x02) == 0 || payload.size() < 2) {
            return abort(MqttClient::ProtocolError, "invalid PUBREL");
        }
        uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
        auto it = incomingQos2.find(packetId);
        if (it != incomingQos2.end()) {
            receivingQueue.put(it->second);
            incomingQos2.erase(it);
        }
        enqueueRaw(encodePacketIdOnly(PUBCOMP, 0, packetId));
        break;
    }
    case PUBCOMP: {
        if (payload.size() < 2) {
            return abort(MqttClient::ProtocolError, "invalid PUBCOMP");
        }
        uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
        completeWaiter(waitPubComp, packetId, true);
        break;
    }
    case SUBACK: {
        if (payload.size() < 3) {
            return abort(MqttClient::ProtocolError, "invalid SUBACK");
        }
        uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
        uint8_t returnCode = static_cast<uint8_t>(payload[2]);
        completeWaiter(waitSubAck, packetId, returnCode != 0x80);
        break;
    }
    case UNSUBACK: {
        if (payload.size() < 2) {
            return abort(MqttClient::ProtocolError, "invalid UNSUBACK");
        }
        uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
        completeWaiter(waitUnsubAck, packetId, true);
        break;
    }
    case PINGRESP:
        awaitingPingResp = false;
        break;
    case DISCONNECT:
        abort(MqttClient::UserShutdown, "broker disconnected");
        break;
    default:
        abort(MqttClient::ProtocolError, "unexpected packet type");
        break;
    }
}

void MqttClientPrivate::doReceive()
{
    while (state == MqttClient::Connected || state == MqttClient::Disconnecting) {
        uint8_t type = 0;
        uint8_t flags = 0;
        string payload;
        try {
            if (!readPacket(type, flags, payload)) {
                if (errorCode == MqttClient::NoError) {
                    abort(MqttClient::NetworkError, "connection closed");
                }
                return;
            }
            handleIncoming(type, flags, payload);
        } catch (CoroutineExitException &) {
            return;
        } catch (...) {
            ngCritical() << "unknown error in MqttClientPrivate::doReceive()";
            return abort(MqttClient::InternalError, "receive loop exception");
        }
    }
}

void MqttClientPrivate::doKeepalive()
{
    if (keepAliveMs <= 0) {
        return;
    }
    while (state == MqttClient::Connected) {
        try {
            Coroutine::sleep(0.5f);
        } catch (CoroutineExitException &) {
            return;
        }
        const int64_t now = utils::DateTime::currentMSecsSinceEpoch();
        if (awaitingPingResp) {
            if (now - lastOutboundTimestamp > keepAliveMs) {
                return abort(MqttClient::KeepaliveTimeout, "PINGRESP timeout");
            }
            continue;
        }
        if (now - lastOutboundTimestamp >= keepAliveMs) {
            awaitingPingResp = true;
            if (!enqueueRaw(encodePingReq())) {
                return;
            }
        }
    }
}

void MqttClientPrivate::abort(MqttClient::MqttError err, const string &message)
{
    if (state == MqttClient::Closed) {
        return;
    }
    state = MqttClient::Closed;
    if (errorCode == MqttClient::NoError || err != MqttClient::UserShutdown) {
        errorCode = err;
        errorString = message;
    }
    failAllWaiters();
    receivingQueue.putForcedly(MqttMessage());
    if (connection) {
        connection->abort();
    }
    if (operations) {
        operations->killall();
    }
    q_func()->disconnected->set();
}

void MqttClientPrivate::disconnectGracefully()
{
    if (state != MqttClient::Connected) {
        abort(MqttClient::UserShutdown, "disconnect");
        return;
    }
    state = MqttClient::Disconnecting;
    shared_ptr<ValueEvent<bool>> done = make_shared<ValueEvent<bool>>();
    enqueueRaw(encodeDisconnect(), done);
    done->tryWait(static_cast<uint32_t>(config.connectTimeout() * 1000));
    abort(MqttClient::UserShutdown, "disconnected");
}

MqttClient::MqttClient(shared_ptr<SocketLike> connection, const MqttConfiguration &config)
    : disconnected(new Event())
    , d_ptr(new MqttClientPrivate(connection, config, this))
{
    NG_D(MqttClient);
    if (!connection) {
        d->abort(NetworkError, "null connection");
        return;
    }
    d->handshake();
}

MqttClient::~MqttClient()
{
    delete d_ptr;
}

shared_ptr<MqttClient> MqttClient::connect(const string &host, uint16_t port, const MqttConfiguration &config)
{
    Socket::SocketError sockError = Socket::NoError;
    unique_ptr<Socket> raw(Socket::createConnection(host, port, &sockError));
    if (!raw) {
        return shared_ptr<MqttClient>();
    }
    shared_ptr<MqttClient> client = make_shared<MqttClient>(asSocketLike(shared_ptr<Socket>(raw.release())), config);
    if (!client->isConnected()) {
        return shared_ptr<MqttClient>();
    }
    return client;
}

#ifndef QTNG_NO_CRYPTO
shared_ptr<MqttClient> MqttClient::connectTls(const string &host, uint16_t port, const MqttConfiguration &config,
                                              const SslConfiguration &ssl)
{
    Socket::SocketError sockError = Socket::NoError;
    unique_ptr<SslSocket> raw(SslSocket::createConnection(host, port, ssl, &sockError));
    if (!raw) {
        return shared_ptr<MqttClient>();
    }
    shared_ptr<MqttClient> client = make_shared<MqttClient>(asSocketLike(shared_ptr<SslSocket>(raw.release())), config);
    if (!client->isConnected()) {
        return shared_ptr<MqttClient>();
    }
    return client;
}
#endif

bool MqttClient::isConnected() const
{
    NG_D(const MqttClient);
    return d->state == Connected;
}

MqttClient::State MqttClient::state() const
{
    NG_D(const MqttClient);
    return d->state;
}

MqttClient::MqttError MqttClient::error() const
{
    NG_D(const MqttClient);
    return d->errorCode;
}

string MqttClient::errorString() const
{
    NG_D(const MqttClient);
    return d->errorString;
}

bool MqttClient::publish(const MqttMessage &msg)
{
    NG_D(MqttClient);
    if (d->state != Connected) {
        return false;
    }
    if (msg.topic.empty() || msg.topic.size() > 65535) {
        return false;
    }
    uint16_t packetId = 0;
    if (msg.qos != MqttQos::AtMostOnce) {
        packetId = d->nextPacketId();
    }
    string raw = encodePublish(msg, packetId);
    if (raw.empty()) {
        return false;
    }
    if (d->maxPacketSize > 0 && static_cast<int32_t>(raw.size()) > d->maxPacketSize) {
        return false;
    }
    shared_ptr<ValueEvent<bool>> done = make_shared<ValueEvent<bool>>();
    if (msg.qos == MqttQos::AtLeastOnce) {
        d->waitPubAck[packetId] = done;
    } else if (msg.qos == MqttQos::ExactlyOnce) {
        d->waitPubRec[packetId] = done;
    }
    if (!d->enqueueOutbound(OutboundPublish, raw, packetId, msg.qos, done)) {
        d->waitPubAck.erase(packetId);
        d->waitPubRec.erase(packetId);
        return false;
    }
    return done->tryWait();
}

bool MqttClient::publishAsync(const MqttMessage &msg)
{
    NG_D(MqttClient);
    if (d->state != Connected) {
        return false;
    }
    if (msg.topic.empty() || msg.topic.size() > 65535) {
        return false;
    }
    uint16_t packetId = 0;
    if (msg.qos != MqttQos::AtMostOnce) {
        packetId = d->nextPacketId();
    }
    string raw = encodePublish(msg, packetId);
    if (raw.empty()) {
        return false;
    }
    if (d->maxPacketSize > 0 && static_cast<int32_t>(raw.size()) > d->maxPacketSize) {
        return false;
    }
    if (msg.qos == MqttQos::AtLeastOnce) {
        d->waitPubAck[packetId] = shared_ptr<ValueEvent<bool>>();
    } else if (msg.qos == MqttQos::ExactlyOnce) {
        d->waitPubRec[packetId] = shared_ptr<ValueEvent<bool>>();
    }
    return d->enqueueOutbound(OutboundPublish, raw, packetId, msg.qos, shared_ptr<ValueEvent<bool>>());
}

bool MqttClient::subscribe(const string &topic, MqttQos qos)
{
    NG_D(MqttClient);
    if (d->state != Connected || topic.empty() || topic.size() > 65535) {
        return false;
    }
    uint16_t packetId = d->nextPacketId();
    string raw = encodeSubscribe(packetId, topic, qos);
    if (raw.empty()) {
        return false;
    }
    shared_ptr<ValueEvent<bool>> done = make_shared<ValueEvent<bool>>();
    d->waitSubAck[packetId] = done;
    if (!d->enqueueOutbound(OutboundSubscribe, raw, packetId, qos, done)) {
        d->waitSubAck.erase(packetId);
        return false;
    }
    return done->tryWait();
}

bool MqttClient::unsubscribe(const string &topic)
{
    NG_D(MqttClient);
    if (d->state != Connected || topic.empty() || topic.size() > 65535) {
        return false;
    }
    uint16_t packetId = d->nextPacketId();
    string raw = encodeUnsubscribe(packetId, topic);
    if (raw.empty()) {
        return false;
    }
    shared_ptr<ValueEvent<bool>> done = make_shared<ValueEvent<bool>>();
    d->waitUnsubAck[packetId] = done;
    if (!d->enqueueOutbound(OutboundUnsubscribe, raw, packetId, MqttQos::AtMostOnce, done)) {
        d->waitUnsubAck.erase(packetId);
        return false;
    }
    return done->tryWait();
}

MqttMessage MqttClient::recv()
{
    NG_D(MqttClient);
    if (d->state != Connected && d->receivingQueue.isEmpty()) {
        return MqttMessage();
    }
    try {
        return d->receivingQueue.get();
    } catch (CoroutineExitException &) {
        return MqttMessage();
    }
}

void MqttClient::disconnect()
{
    NG_D(MqttClient);
    d->disconnectGracefully();
}

void MqttClient::abort()
{
    NG_D(MqttClient);
    d->abort(UserShutdown, "aborted");
}

}  // namespace qtng
