#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "qtng/coroutine.h"
#include "qtng/coroutine_utils.h"
#include "qtng/hostaddress.h"
#include "qtng/mqtt.h"
#include "qtng/socket.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/platform.h"

using namespace std;
using namespace qtng;

namespace {

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

string makeFixedHeader(uint8_t type, uint8_t flags, uint32_t remainingLength)
{
    string out;
    out.push_back(static_cast<char>((type << 4) | (flags & 0x0f)));
    if (!encodeRemainingLength(remainingLength, out)) {
        return string();
    }
    return out;
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

bool readExact(SocketLike *conn, string &out, size_t n)
{
    out.clear();
    while (out.size() < n) {
        string chunk = conn->recv(static_cast<int32_t>(n - out.size()));
        if (chunk.empty()) {
            return false;
        }
        out.append(chunk);
    }
    return true;
}

bool readMqttPacket(SocketLike *conn, uint8_t &type, uint8_t &flags, string &payload)
{
    string first;
    if (!readExact(conn, first, 1)) {
        return false;
    }
    type = static_cast<uint8_t>((static_cast<uint8_t>(first[0]) >> 4) & 0x0f);
    flags = static_cast<uint8_t>(first[0]) & 0x0f;
    string rlBytes;
    uint32_t remaining = 0;
    size_t consumed = 0;
    for (int i = 0; i < 4; ++i) {
        string b;
        if (!readExact(conn, b, 1)) {
            return false;
        }
        rlBytes.append(b);
        if (decodeRemainingLength(rlBytes.data(), rlBytes.size(), consumed, remaining)) {
            break;
        }
        if (rlBytes.size() >= 4) {
            return false;
        }
    }
    if (consumed == 0) {
        return false;
    }
    if (remaining == 0) {
        payload.clear();
        return true;
    }
    return readExact(conn, payload, remaining);
}

string encodeConnack(uint8_t returnCode)
{
    string payload;
    payload.push_back(0x00);
    payload.push_back(static_cast<char>(returnCode));
    string packet = makeFixedHeader(2, 0, static_cast<uint32_t>(payload.size()));
    packet.append(payload);
    return packet;
}

string encodePublish(const string &topic, const string &body, uint8_t qos, uint16_t packetId, bool retain = false)
{
    string variable;
    appendMqttString(variable, topic);
    if (qos > 0) {
        appendUint16(variable, packetId);
    }
    variable.append(body);
    uint8_t flags = static_cast<uint8_t>((qos & 0x03) << 1);
    if (retain) {
        flags |= 0x01;
    }
    string packet = makeFixedHeader(3, flags, static_cast<uint32_t>(variable.size()));
    packet.append(variable);
    return packet;
}

string encodePacketId(uint8_t type, uint8_t flags, uint16_t packetId)
{
    string payload;
    appendUint16(payload, packetId);
    string packet = makeFixedHeader(type, flags, static_cast<uint32_t>(payload.size()));
    packet.append(payload);
    return packet;
}

string encodeSuback(uint16_t packetId, uint8_t returnCode)
{
    string payload;
    appendUint16(payload, packetId);
    payload.push_back(static_cast<char>(returnCode));
    string packet = makeFixedHeader(9, 0, static_cast<uint32_t>(payload.size()));
    packet.append(payload);
    return packet;
}

string encodeUnsuback(uint16_t packetId)
{
    return encodePacketId(11, 0, packetId);
}

struct StubBroker {
    set<string> subscriptions;
    map<uint16_t, string> qos2Payloads;
};

void runBrokerSession(shared_ptr<SocketLike> conn, StubBroker *broker)
{
    while (true) {
        uint8_t type = 0;
        uint8_t flags = 0;
        string payload;
        if (!readMqttPacket(conn.get(), type, flags, payload)) {
            return;
        }
        switch (type) {
        case 1: {  // CONNECT
            string ack = encodeConnack(0);
            if (conn->sendall(ack) != static_cast<int32_t>(ack.size())) {
                return;
            }
            break;
        }
        case 3: {  // PUBLISH
            size_t offset = 0;
            if (payload.size() < 2) {
                return;
            }
            uint16_t topicLen = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
            offset = 2;
            if (offset + topicLen > payload.size()) {
                return;
            }
            string topic(payload.data() + offset, topicLen);
            offset += topicLen;
            uint8_t qos = (flags >> 1) & 0x03;
            uint16_t packetId = 0;
            if (qos > 0) {
                if (offset + 2 > payload.size()) {
                    return;
                }
                packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data() + offset));
                offset += 2;
            }
            string body(payload.data() + offset, payload.size() - offset);
            if (qos == 1) {
                string ack = encodePacketId(4, 0, packetId);
                conn->sendall(ack);
            } else if (qos == 2) {
                broker->qos2Payloads[packetId] = body;
                string ack = encodePacketId(5, 0, packetId);  // PUBREC
                conn->sendall(ack);
            }
            if (broker->subscriptions.count(topic) || broker->subscriptions.count("#")) {
                // Echo to same client (simple stub).
                string echo = encodePublish(topic, body, qos, packetId ? packetId : 1);
                // For qos0 echo without packet id collision concerns use qos0.
                if (qos == 0) {
                    echo = encodePublish(topic, body, 0, 0);
                    conn->sendall(echo);
                }
            }
            break;
        }
        case 6: {  // PUBREL
            if (payload.size() < 2) {
                return;
            }
            uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
            string ack = encodePacketId(7, 0, packetId);  // PUBCOMP
            conn->sendall(ack);
            break;
        }
        case 8: {  // SUBSCRIBE
            if (payload.size() < 5) {
                return;
            }
            uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
            size_t offset = 2;
            uint16_t topicLen = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data() + offset));
            offset += 2;
            if (offset + topicLen + 1 > payload.size()) {
                return;
            }
            string topic(payload.data() + offset, topicLen);
            broker->subscriptions.insert(topic);
            uint8_t requestedQos = static_cast<uint8_t>(payload[offset + topicLen]);
            string ack = encodeSuback(packetId, requestedQos);
            conn->sendall(ack);
            break;
        }
        case 10: {  // UNSUBSCRIBE
            if (payload.size() < 4) {
                return;
            }
            uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
            size_t offset = 2;
            uint16_t topicLen = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data() + offset));
            offset += 2;
            if (offset + topicLen > payload.size()) {
                return;
            }
            string topic(payload.data() + offset, topicLen);
            broker->subscriptions.erase(topic);
            string ack = encodeUnsuback(packetId);
            conn->sendall(ack);
            break;
        }
        case 12: {  // PINGREQ
            string pong = makeFixedHeader(13, 0, 0);
            conn->sendall(pong);
            break;
        }
        case 14:  // DISCONNECT
            return;
        default:
            return;
        }
    }
}

struct ConnectedPair {
    shared_ptr<Socket> serverSide;
    shared_ptr<Socket> clientSide;
    uint16_t port;
};

ConnectedPair makeConnectedPair()
{
    unique_ptr<Socket> listener(Socket::createServer(HostAddress::LocalHost, 0, 1));
    REQUIRE(listener);
    uint16_t port = listener->localPort();
    REQUIRE(port != 0);

    shared_ptr<Socket> client;
    shared_ptr<Socket> accepted;
    shared_ptr<Coroutine> acceptor(Coroutine::spawn([&] {
        accepted.reset(listener->accept());
    }));
    client.reset(Socket::createConnection(HostAddress::LocalHost, port));
    REQUIRE(client);
    acceptor->join();
    REQUIRE(accepted);

    ConnectedPair pair;
    pair.serverSide = accepted;
    pair.clientSide = client;
    pair.port = port;
    return pair;
}

}  // namespace

TEST_CASE("remaining length encode/decode", "[mqtt]")
{
    const uint32_t samples[] = {0, 127, 128, 16383, 16384, 2097151, 2097152, 268435455u};
    for (uint32_t value : samples) {
        string encoded;
        REQUIRE(encodeRemainingLength(value, encoded));
        size_t consumed = 0;
        uint32_t decoded = 0;
        REQUIRE(decodeRemainingLength(encoded.data(), encoded.size(), consumed, decoded));
        REQUIRE(consumed == encoded.size());
        REQUIRE(decoded == value);
    }
    string tooLarge;
    REQUIRE_FALSE(encodeRemainingLength(268435456u, tooLarge));
}

TEST_CASE("mqtt connect publish subscribe qos0", "[mqtt]")
{
    unique_ptr<Socket> listener(Socket::createServer(HostAddress::LocalHost, 0, 1));
    REQUIRE(listener);
    const uint16_t port = listener->localPort();

    StubBroker broker;
    shared_ptr<Coroutine> server(Coroutine::spawn([&] {
        shared_ptr<Socket> accepted(listener->accept());
        REQUIRE(accepted);
        runBrokerSession(asSocketLike(accepted), &broker);
    }));

    MqttConfiguration config;
    config.setClientId("test-client");
    config.setKeepAlive(30);
    shared_ptr<MqttClient> client = MqttClient::connect("127.0.0.1", port, config);
    REQUIRE(client);
    REQUIRE(client->isConnected());

    REQUIRE(client->subscribe("sensors/temp", MqttQos::AtMostOnce));
    REQUIRE(broker.subscriptions.count("sensors/temp") == 1);

    // Broker pushes a message to the client.
    // Use a second connection path: ask stub via publish echo.
    // Directly publish from a helper coroutine that writes to the accepted socket is hard;
    // instead have client publish and stub echo qos0 when subscribed.
    REQUIRE(client->publish(MqttMessage("sensors/temp", "22.5", MqttQos::AtMostOnce)));

    MqttMessage msg = client->recv();
    REQUIRE(msg.topic() == "sensors/temp");
    REQUIRE(msg.payload() == "22.5");
    REQUIRE(msg.qos() == MqttQos::AtMostOnce);

    REQUIRE(client->unsubscribe("sensors/temp"));
    client->disconnect();
    REQUIRE_FALSE(client->isConnected());
    server->join();
}

TEST_CASE("mqtt qos1 publish", "[mqtt]")
{
    unique_ptr<Socket> listener(Socket::createServer(HostAddress::LocalHost, 0, 1));
    REQUIRE(listener);
    const uint16_t port = listener->localPort();

    StubBroker broker;
    shared_ptr<Coroutine> server(Coroutine::spawn([&] {
        shared_ptr<Socket> accepted(listener->accept());
        REQUIRE(accepted);
        runBrokerSession(asSocketLike(accepted), &broker);
    }));

    MqttConfiguration config;
    config.setClientId("qos1-client");
    shared_ptr<MqttClient> client = MqttClient::connect("127.0.0.1", port, config);
    REQUIRE(client);

    REQUIRE(client->publish(MqttMessage("orders", "id=1", MqttQos::AtLeastOnce)));
    client->disconnect();
    server->join();
}

TEST_CASE("mqtt qos2 publish", "[mqtt]")
{
    unique_ptr<Socket> listener(Socket::createServer(HostAddress::LocalHost, 0, 1));
    REQUIRE(listener);
    const uint16_t port = listener->localPort();

    StubBroker broker;
    shared_ptr<Coroutine> server(Coroutine::spawn([&] {
        shared_ptr<Socket> accepted(listener->accept());
        REQUIRE(accepted);
        runBrokerSession(asSocketLike(accepted), &broker);
    }));

    MqttConfiguration config;
    config.setClientId("qos2-client");
    shared_ptr<MqttClient> client = MqttClient::connect("127.0.0.1", port, config);
    REQUIRE(client);

    REQUIRE(client->publish(MqttMessage("orders", "id=2", MqttQos::ExactlyOnce)));
    REQUIRE_FALSE(broker.qos2Payloads.empty());
    client->disconnect();
    server->join();
}

TEST_CASE("mqtt inbound qos1 from broker", "[mqtt]")
{
    ConnectedPair sockets = makeConnectedPair();
    StubBroker broker;

    // Manually drive CONNECT on server side then inject PUBLISH.
    shared_ptr<Coroutine> server(Coroutine::spawn([&] {
        shared_ptr<SocketLike> conn = asSocketLike(sockets.serverSide);
        uint8_t type = 0;
        uint8_t flags = 0;
        string payload;
        REQUIRE(readMqttPacket(conn.get(), type, flags, payload));
        REQUIRE(type == 1);
        string ack = encodeConnack(0);
        REQUIRE(conn->sendall(ack) == static_cast<int32_t>(ack.size()));

        string pub = encodePublish("alerts", "fire", 1, 7);
        REQUIRE(conn->sendall(pub) == static_cast<int32_t>(pub.size()));

        // Expect PUBACK
        REQUIRE(readMqttPacket(conn.get(), type, flags, payload));
        REQUIRE(type == 4);
        REQUIRE(payload.size() >= 2);
        uint16_t packetId = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t *>(payload.data()));
        REQUIRE(packetId == 7);

        // Wait for client disconnect or ping; read until closed.
        readMqttPacket(conn.get(), type, flags, payload);
    }));

    MqttConfiguration config;
    config.setClientId("inbound-qos1");
    shared_ptr<MqttClient> client = make_shared<MqttClient>(asSocketLike(sockets.clientSide), config);
    REQUIRE(client->isConnected());

    MqttMessage msg = client->recv();
    REQUIRE(msg.topic() == "alerts");
    REQUIRE(msg.payload() == "fire");
    REQUIRE(msg.qos() == MqttQos::AtLeastOnce);

    client->disconnect();
    server->join();
}

TEST_CASE("mqtt connack refusal", "[mqtt]")
{
    ConnectedPair sockets = makeConnectedPair();
    shared_ptr<Coroutine> server(Coroutine::spawn([&] {
        shared_ptr<SocketLike> conn = asSocketLike(sockets.serverSide);
        uint8_t type = 0;
        uint8_t flags = 0;
        string payload;
        REQUIRE(readMqttPacket(conn.get(), type, flags, payload));
        REQUIRE(type == 1);
        string ack = encodeConnack(4);  // bad credentials
        conn->sendall(ack);
    }));

    MqttConfiguration config;
    config.setClientId("bad-auth");
    config.setUsername("u");
    config.setPassword("p");
    shared_ptr<MqttClient> client = make_shared<MqttClient>(asSocketLike(sockets.clientSide), config);
    REQUIRE_FALSE(client->isConnected());
    REQUIRE(client->error() == MqttClient::ConnectionRefusedCredentials);
    server->join();
}
