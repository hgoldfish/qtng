#ifndef QTNG_WEBSOCKET_H
#define QTNG_WEBSOCKET_H

#include <memory>
#include <QtCore/qstring.h>
#include <QtCore/qurl.h>
#include <QtCore/qsharedpointer.h>
#include "config.h"

QTNETWORKNG_NAMESPACE_BEGIN

class WebSocketConfigurationPrivate;
class WebSocketConfiguration
{
public:
    WebSocketConfiguration();
    WebSocketConfiguration(const WebSocketConfiguration &other);
    ~WebSocketConfiguration();
    WebSocketConfiguration &operator=(const WebSocketConfiguration &other);
public:
    void setKeepaliveInterval(float interval);
    float keepaliveInterval() const;
    void setKeepaliveTimeout(float timeout);
    float keepaliveTimeout() const;
    quint32 sendingQueueCapacity() const;
    void setSendingQueueCapacity(quint32 capacity);
    quint32 receivingQueueCapacity() const;
    void setReceivingQueueCapacity(quint32 capacity);
    qint32 maxPayloadSize() const;
    void setMaxPayloadSize(qint32 size);
    QStringList protocols() const;
    void setProtocols(const QStringList &protocols);
    void setOutgoingSize(qint32 size);
    qint32 outgoingSize() const;
private:
    WebSocketConfigurationPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(WebSocketConfiguration);
    friend class WebSocketConfigurationPrivate;
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
    WebSocketConnection(QSharedPointer<SocketLike> connection, const QByteArray &headBytes, Side side = Client,
                        const WebSocketConfiguration &config = WebSocketConfiguration());
    ~WebSocketConnection();
public:
    QSharedPointer<Event> disconnected() const;
public:
    void setConfiguration(const WebSocketConfiguration &config);
    bool send(const QByteArray &packet);
    bool send(const QString &text);
    bool post(const QByteArray &packet);
    bool post(const QString &text);
    QByteArray recv(FrameType *type = nullptr);
    void close();
    void abort();
public:
    QByteArray id() const;
    Side side() const;
    State state() const;
    int closeCode() const;
    QString closeReason() const;
    QString toString() const;
    void setDebugLevel(int level);
    int debugLevel() const;
    void setMustMask(bool yes);
    bool mustMask() const;
    QString origin() const;
    QUrl url() const;
    std::shared_ptr<const HttpResponse> response() const;
private:
    WebSocketConnectionPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(WebSocketConnection);
    friend class WebSocketConnectionPrivate;
    // 接管已填充好的 d（仅供内部 bridge 使用）。
    WebSocketConnection(WebSocketConnectionPrivate *d);
    Q_DISABLE_COPY(WebSocketConnection)
    WebSocketConnection(WebSocketConnection &&) = delete;
    WebSocketConnection &operator=(WebSocketConnection &&) = delete;
};

QTNETWORKNG_NAMESPACE_END

#endif
