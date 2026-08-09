#ifndef QTNG_MQTT_H
#define QTNG_MQTT_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/utils/platform.h"

#ifndef QTNG_NO_CRYPTO
#  include "qtng/ssl.h"
#endif

namespace qtng {

class Event;
class SocketLike;

enum class MqttQos : std::uint8_t {
    AtMostOnce = 0,
    AtLeastOnce = 1,
    ExactlyOnce = 2,
};

class MqttMessage
{
public:
    MqttMessage();
    MqttMessage(const std::string &topic, const std::string &payload, MqttQos qos = MqttQos::AtMostOnce,
                bool retain = false);
public:
    std::string topic;
    std::string payload;
    MqttQos qos;
    bool retain;
    bool dup;
};

class MqttConfigurationPrivate;
class MqttConfiguration
{
public:
    MqttConfiguration();
    MqttConfiguration(const MqttConfiguration &other);
    MqttConfiguration &operator=(const MqttConfiguration &other);
    ~MqttConfiguration();
public:
    void setClientId(const std::string &clientId);
    std::string clientId() const;
    void setCleanSession(bool cleanSession);
    bool cleanSession() const;
    void setKeepAlive(std::uint16_t seconds);
    std::uint16_t keepAlive() const;
    void setUsername(const std::string &username);
    std::string username() const;
    void setPassword(const std::string &password);
    std::string password() const;
    void setWill(const MqttMessage &will);
    MqttMessage will() const;
    bool hasWill() const;
    void clearWill();
    void setSendingQueueCapacity(std::uint32_t capacity);
    std::uint32_t sendingQueueCapacity() const;
    void setReceivingQueueCapacity(std::uint32_t capacity);
    std::uint32_t receivingQueueCapacity() const;
    void setMaxPacketSize(std::int32_t size);
    std::int32_t maxPacketSize() const;
    void setConnectTimeout(float seconds);
    float connectTimeout() const;
private:
    MqttConfigurationPrivate *d_ptr;
    NG_DECLARE_PRIVATE(MqttConfiguration);
};

class MqttClientPrivate;
class MqttClient
{
    NG_DISABLE_COPY(MqttClient)
public:
    enum State {
        Closed = 0,
        Connecting,
        Connected,
        Disconnecting,
    };
    enum MqttError {
        NoError = 0,
        ConnectionRefusedProtocol = 1,
        ConnectionRefusedIdentifier = 2,
        ConnectionRefusedServer = 3,
        ConnectionRefusedCredentials = 4,
        ConnectionRefusedNotAuthorized = 5,
        ProtocolError = 6,
        NetworkError = 7,
        KeepaliveTimeout = 8,
        PacketTooLarge = 9,
        UserShutdown = 10,
        InternalError = 11,
        TimeoutError = 12,
    };
public:
    explicit MqttClient(std::shared_ptr<SocketLike> connection,
                        const MqttConfiguration &config = MqttConfiguration());
    ~MqttClient();
public:
    static std::shared_ptr<MqttClient> connect(const std::string &host, std::uint16_t port = 1883,
                                               const MqttConfiguration &config = MqttConfiguration());
#ifndef QTNG_NO_CRYPTO
    static std::shared_ptr<MqttClient> connectTls(const std::string &host, std::uint16_t port = 8883,
                                                  const MqttConfiguration &config = MqttConfiguration(),
                                                  const SslConfiguration &ssl = SslConfiguration());
#endif
public:
    std::shared_ptr<Event> disconnected;
public:
    bool isConnected() const;
    State state() const;
    MqttError error() const;
    std::string errorString() const;
    bool publish(const MqttMessage &msg);
    bool publishAsync(const MqttMessage &msg);
    bool subscribe(const std::string &topic, MqttQos qos = MqttQos::AtMostOnce);
    bool unsubscribe(const std::string &topic);
    MqttMessage recv();
    void disconnect();
    void abort();
private:
    MqttClientPrivate *const d_ptr;
    NG_DECLARE_PRIVATE(MqttClient);
};

}  // namespace qtng

#endif  // QTNG_MQTT_H
