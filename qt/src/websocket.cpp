#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "websocket.h"
#include "http.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class WebSocketConfigurationPrivate
{
public:
    qtng_core::WebSocketConfiguration core;
};

WebSocketConfiguration::WebSocketConfiguration()
    : d_ptr(new WebSocketConfigurationPrivate)
{
}

WebSocketConfiguration::~WebSocketConfiguration()
{
    delete d_ptr;
}

void WebSocketConfiguration::setKeepaliveInterval(float interval)
{
    Q_D(WebSocketConfiguration);
    d->core.setKeepaliveInterval(interval);
}

float WebSocketConfiguration::keepaliveInterval() const
{
    Q_D(const WebSocketConfiguration);
    return d->core.keepaliveInterval();
}

void WebSocketConfiguration::setKeepaliveTimeout(float timeout)
{
    Q_D(WebSocketConfiguration);
    d->core.setKeepaliveTimeout(timeout);
}

float WebSocketConfiguration::keepaliveTimeout() const
{
    Q_D(const WebSocketConfiguration);
    return d->core.keepaliveTimeout();
}

quint32 WebSocketConfiguration::sendingQueueCapacity() const
{
    Q_D(const WebSocketConfiguration);
    return d->core.sendingQueueCapacity();
}

void WebSocketConfiguration::setSendingQueueCapacity(quint32 capacity)
{
    Q_D(WebSocketConfiguration);
    d->core.setSendingQueueCapacity(capacity);
}

quint32 WebSocketConfiguration::receivingQueueCapacity() const
{
    Q_D(const WebSocketConfiguration);
    return d->core.receivingQueueCapacity();
}

void WebSocketConfiguration::setReceivingQueueCapacity(quint32 capacity)
{
    Q_D(WebSocketConfiguration);
    d->core.setReceivingQueueCapacity(capacity);
}

qint32 WebSocketConfiguration::maxPayloadSize() const
{
    Q_D(const WebSocketConfiguration);
    return d->core.maxPayloadSize();
}

void WebSocketConfiguration::setMaxPayloadSize(qint32 size)
{
    Q_D(WebSocketConfiguration);
    d->core.setMaxPayloadSize(size);
}

QStringList WebSocketConfiguration::protocols() const
{
    Q_D(const WebSocketConfiguration);
    QStringList result;
    for (const string &p : d->core.protocols()) {
        result.append(toQString(p));
    }
    return result;
}

void WebSocketConfiguration::setProtocols(const QStringList &protocols)
{
    Q_D(WebSocketConfiguration);
    vector<string> coreProtocols;
    for (const QString &p : protocols) {
        coreProtocols.push_back(toStdString(p));
    }
    d->core.setProtocols(coreProtocols);
}

void WebSocketConfiguration::setOutgoingSize(qint32 size)
{
    Q_D(WebSocketConfiguration);
    d->core.setOutgoingSize(size);
}

qint32 WebSocketConfiguration::outgoingSize() const
{
    Q_D(const WebSocketConfiguration);
    return d->core.outgoingSize();
}

class WebSocketConnectionPrivate
{
public:
    shared_ptr<qtng_core::WebSocketConnection> core;
    QSharedPointer<Event> disconnectedEvent;
};

WebSocketConnection::WebSocketConnection(QSharedPointer<SocketLike> connection, const QByteArray &headBytes, Side side,
                                         const WebSocketConfiguration &config)
    : d_ptr(new WebSocketConnectionPrivate)
{
    Q_D(WebSocketConnection);
    qtng_core::WebSocketConfiguration coreConfig;
    coreConfig.setKeepaliveInterval(config.keepaliveInterval());
    coreConfig.setKeepaliveTimeout(config.keepaliveTimeout());
    d->core = make_shared<qtng_core::WebSocketConnection>(
            toCoreSocketLike(connection), toStdString(headBytes),
            static_cast<qtng_core::WebSocketConnection::Side>(side), coreConfig);
    d->disconnectedEvent = QSharedPointer<Event>(new Event());
    disconnected = d->disconnectedEvent;
}

WebSocketConnection::~WebSocketConnection()
{
    delete d_ptr;
}

void WebSocketConnection::setConfiguration(const WebSocketConfiguration &config)
{
    Q_D(WebSocketConnection);
    qtng_core::WebSocketConfiguration coreConfig;
    coreConfig.setKeepaliveInterval(config.keepaliveInterval());
    d->core->setConfiguration(coreConfig);
}

bool WebSocketConnection::send(const QByteArray &packet) { Q_D(WebSocketConnection); return d->core->send(toStdString(packet)); }
bool WebSocketConnection::send(const QString &text) { Q_D(WebSocketConnection); return d->core->sendText(toStdString(text)); }
bool WebSocketConnection::post(const QByteArray &packet) { Q_D(WebSocketConnection); return d->core->post(toStdString(packet)); }
bool WebSocketConnection::post(const QString &text) { Q_D(WebSocketConnection); return d->core->post(toStdString(text)); }
QByteArray WebSocketConnection::recv(FrameType *type)
{
    Q_D(WebSocketConnection);
    qtng_core::WebSocketConnection::FrameType coreType = qtng_core::WebSocketConnection::Unknown;
    const string payload = d->core->recv(type ? &coreType : nullptr);
    if (type) {
        *type = static_cast<FrameType>(coreType);
    }
    return toQByteArray(payload);
}
void WebSocketConnection::close() { Q_D(WebSocketConnection); d->core->close(); }
void WebSocketConnection::abort() { Q_D(WebSocketConnection); d->core->abort(); }
QByteArray WebSocketConnection::id() const { Q_D(const WebSocketConnection); return toQByteArray(d->core->id()); }
WebSocketConnection::Side WebSocketConnection::side() const { Q_D(const WebSocketConnection); return static_cast<Side>(d->core->side()); }
WebSocketConnection::State WebSocketConnection::state() const { Q_D(const WebSocketConnection); return static_cast<State>(d->core->state()); }
int WebSocketConnection::closeCode() const { Q_D(const WebSocketConnection); return d->core->closeCode(); }
QString WebSocketConnection::closeReason() const { Q_D(const WebSocketConnection); return toQString(d->core->closeReason()); }
QString WebSocketConnection::toString() const { Q_D(const WebSocketConnection); return toQString(d->core->toString()); }
void WebSocketConnection::setDebugLevel(int level) { Q_D(WebSocketConnection); d->core->setDebugLevel(level); }
int WebSocketConnection::debugLevel() const { Q_D(const WebSocketConnection); return d->core->debugLevel(); }
void WebSocketConnection::setMustMask(bool yes) { Q_D(WebSocketConnection); d->core->setMustMask(yes); }
bool WebSocketConnection::mustMask() const { Q_D(const WebSocketConnection); return d->core->mustMask(); }
QString WebSocketConnection::origin() const { Q_D(const WebSocketConnection); return toQString(d->core->origin()); }
QUrl WebSocketConnection::url() const { Q_D(const WebSocketConnection); return toQUrl(qtng_core::utils::Url(d->core->url())); }
const HttpResponse &WebSocketConnection::response() const
{
    static HttpResponse empty;
    return empty;
}

}  // namespace QTNETWORKNG_NAMESPACE
