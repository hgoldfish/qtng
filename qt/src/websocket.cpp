#include "bridge/core_access.h"
#include "bridge/http_access.h"
#include "bridge/io_bridge.h"
#include "websocket.h"
#include "http.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

namespace {
qtng_core::WebSocketConfiguration toCoreConfig(const WebSocketConfiguration &config)
{
    qtng_core::WebSocketConfiguration coreConfig;
    coreConfig.setKeepaliveInterval(config.keepaliveInterval());
    coreConfig.setKeepaliveTimeout(config.keepaliveTimeout());
    coreConfig.setSendingQueueCapacity(config.sendingQueueCapacity());
    coreConfig.setReceivingQueueCapacity(config.receivingQueueCapacity());
    coreConfig.setMaxPayloadSize(config.maxPayloadSize());
    std::vector<std::string> protocols;
    for (const QString &p : config.protocols()) {
        protocols.push_back(toStdString(p));
    }
    coreConfig.setProtocols(protocols);
    coreConfig.setOutgoingSize(config.outgoingSize());
    return coreConfig;
}
}  // namespace

class WebSocketConfigurationPrivate
{
public:
    qtng_core::WebSocketConfiguration core;
    qtng_core::WebSocketConfiguration *external = nullptr;

    qtng_core::WebSocketConfiguration &config() { return external ? *external : core; }
    const qtng_core::WebSocketConfiguration &config() const { return external ? *external : core; }

    static void bind(WebSocketConfiguration *config, qtng_core::WebSocketConfiguration *core)
    {
        if (config) {
            config->d_ptr->external = core;
        }
    }
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
    d->config().setKeepaliveInterval(interval);
}

float WebSocketConfiguration::keepaliveInterval() const
{
    Q_D(const WebSocketConfiguration);
    return d->config().keepaliveInterval();
}

void WebSocketConfiguration::setKeepaliveTimeout(float timeout)
{
    Q_D(WebSocketConfiguration);
    d->config().setKeepaliveTimeout(timeout);
}

float WebSocketConfiguration::keepaliveTimeout() const
{
    Q_D(const WebSocketConfiguration);
    return d->config().keepaliveTimeout();
}

quint32 WebSocketConfiguration::sendingQueueCapacity() const
{
    Q_D(const WebSocketConfiguration);
    return d->config().sendingQueueCapacity();
}

void WebSocketConfiguration::setSendingQueueCapacity(quint32 capacity)
{
    Q_D(WebSocketConfiguration);
    d->config().setSendingQueueCapacity(capacity);
}

quint32 WebSocketConfiguration::receivingQueueCapacity() const
{
    Q_D(const WebSocketConfiguration);
    return d->config().receivingQueueCapacity();
}

void WebSocketConfiguration::setReceivingQueueCapacity(quint32 capacity)
{
    Q_D(WebSocketConfiguration);
    d->config().setReceivingQueueCapacity(capacity);
}

qint32 WebSocketConfiguration::maxPayloadSize() const
{
    Q_D(const WebSocketConfiguration);
    return d->config().maxPayloadSize();
}

void WebSocketConfiguration::setMaxPayloadSize(qint32 size)
{
    Q_D(WebSocketConfiguration);
    d->config().setMaxPayloadSize(size);
}

QStringList WebSocketConfiguration::protocols() const
{
    Q_D(const WebSocketConfiguration);
    QStringList result;
    for (const string &p : d->config().protocols()) {
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
    d->config().setProtocols(coreProtocols);
}

void WebSocketConfiguration::setOutgoingSize(qint32 size)
{
    Q_D(WebSocketConfiguration);
    d->config().setOutgoingSize(size);
}

qint32 WebSocketConfiguration::outgoingSize() const
{
    Q_D(const WebSocketConfiguration);
    return d->config().outgoingSize();
}

class WebSocketConnectionPrivate
{
public:
    shared_ptr<qtng_core::WebSocketConnection> core;
    QSharedPointer<Event> disconnectedEvent;
    mutable HttpResponse response;
    mutable bool responseReady = false;
};

WebSocketConnection::WebSocketConnection(QSharedPointer<SocketLike> connection, const QByteArray &headBytes, Side side,
                                         const WebSocketConfiguration &config)
    : d_ptr(new WebSocketConnectionPrivate)
{
    Q_D(WebSocketConnection);
    d->core = make_shared<qtng_core::WebSocketConnection>(
            toCoreSocketLike(connection), toStdString(headBytes),
            static_cast<qtng_core::WebSocketConnection::Side>(side), toCoreConfig(config));
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
    d->core->setConfiguration(toCoreConfig(config));
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
    Q_D(const WebSocketConnection);
    if (!d->responseReady) {
        d->response = qtng_bridge::httpResponseFromCore(d->core->response());
        d->responseReady = true;
    }
    return d->response;
}

}  // namespace QTNETWORKNG_NAMESPACE

namespace qtng_bridge {

void bindWebSocketConfiguration(QTNETWORKNG_NAMESPACE::WebSocketConfiguration *config, qtng_core::WebSocketConfiguration *core)
{
    QTNETWORKNG_NAMESPACE::WebSocketConfigurationPrivate::bind(config, core);
}

}  // namespace qtng_bridge
