#ifndef QTNG_WEBSOCKET_H
#define QTNG_WEBSOCKET_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "qtng/utils/platform.h"

namespace qtng {

class WebSocketConfigurationPrivate;
class WebSocketConfiguration
{
public:
    WebSocketConfiguration();
    ~WebSocketConfiguration();
public:
    void setKeepaliveInterval(float interval);
    float keepaliveInterval() const;
    void setKeepaliveTimeout(float timeout);
    float keepaliveTimeout() const;
    std::uint32_t sendingQueueCapacity() const;
    void setSendingQueueCapacity(std::uint32_t capacity);
    std::uint32_t receivingQueueCapacity() const;
    void setReceivingQueueCapacity(std::uint32_t capacity);
    std::int32_t maxPayloadSize() const;
    void setMaxPayloadSize(std::int32_t size);
    std::vector<std::string> protocols() const;
    void setProtocols(const std::vector<std::string> &protocols);
    void setOutgoingSize(std::int32_t size);
    std::int32_t outgoingSize() const;
private:
    WebSocketConfigurationPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(WebSocketConfiguration);
};

class Event;
class SocketLike;
class WebSocketConnectionPrivate;
class HttpResponse;
class WebSocketConnection
{
public:
    enum FrameType { Unknown = 0, Binary, Text };
    enum State { Closed = 0, Open, Closing };
    enum Side { Client = 0, Server };
    enum WebSocketError {
        NoError = 0,
        NormalClosure = 1000,
        GoingAway = 1001,
        ProtocolError = 1002,
        UnsupportedData = 1003,
        NoStatusRcvd = 1005,
        AbnormalClosure = 1006,
        InvalidData = 1007,
        PolicyViolation = 1008,
        MessageTooBig = 1009,
        MandatoryExtension = 1010,
        InternalError = 1011,
        ServiceRestart = 1012,
        TryAgainLater = 1013,
        BadGateway = 1014,
        TlsHandshake = 1015,
    };
public:
    WebSocketConnection(std::shared_ptr<SocketLike> connection, const std::string &headBytes, Side side = Client,
                        const WebSocketConfiguration &config = WebSocketConfiguration());
    ~WebSocketConnection();
public:
    std::shared_ptr<Event> disconnected;
public:
    void setConfiguration(const WebSocketConfiguration &config);
    bool send(const std::string &packet);
    bool sendText(const std::string &text);
    bool post(const std::string &packet);
    std::string recv(FrameType *type = nullptr);
    void close();
    void abort();
public:
    std::string id() const;
    Side side() const;
    State state() const;
    int closeCode() const;
    std::string closeReason() const;
    std::string toString() const;
    void setDebugLevel(int level);
    int debugLevel() const;
    void setMustMask(bool yes);
    bool mustMask() const;
    std::string origin() const;
    std::string url() const;
    const HttpResponse &response() const;
private:
    WebSocketConnectionPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(WebSocketConnection);
    friend class HttpSessionPrivate;
};

}  // namespace qtng

#endif
