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
    std::shared_ptr<qtng_core::WebSocketConfiguration> core = std::make_shared<qtng_core::WebSocketConfiguration>();

    qtng_core::WebSocketConfiguration &config() { return *core; }
    const qtng_core::WebSocketConfiguration &config() const { return *core; }

    static void bindCore(WebSocketConfiguration *config, std::shared_ptr<qtng_core::WebSocketConfiguration> core)
    {
        if (config) {
            config->d_ptr->core = std::move(core);
        }
    }

    static std::shared_ptr<qtng_core::WebSocketConfiguration> sharedCoreOf(const WebSocketConfiguration &config)
    {
        return config.d_ptr->core;
    }
};

WebSocketConfiguration::WebSocketConfiguration()
    : d_ptr(new WebSocketConfigurationPrivate)
{
}

WebSocketConfiguration::WebSocketConfiguration(const WebSocketConfiguration &other)
    : d_ptr(new WebSocketConfigurationPrivate(*other.d_ptr))
{
}

WebSocketConfiguration::~WebSocketConfiguration()
{
    delete d_ptr;
}

WebSocketConfiguration &WebSocketConfiguration::operator=(const WebSocketConfiguration &other)
{
    if (this != &other) {
        *d_ptr = *other.d_ptr;
    }
    return *this;
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

    static WebSocketConnection *create(std::shared_ptr<qtng_core::WebSocketConnection> core);
};

namespace {
// 桥接 core 的 disconnected 事件到 Qt 层 Event：core 连接断开（abort）时同步转发。
// 捕获事件对象副本而非 this/d，因为 core 析构（abort 路径）可能晚于 d_ptr 释放。
void attachDisconnectedBridge(WebSocketConnectionPrivate *d)
{
    d->disconnectedEvent = QSharedPointer<Event>(new Event());
    QSharedPointer<Event> disconnected = d->disconnectedEvent;
    d->core->setDisconnectedNotifier([disconnected]() { disconnected->set(); });
}
}  // namespace

WebSocketConnection *WebSocketConnectionPrivate::create(std::shared_ptr<qtng_core::WebSocketConnection> core)
{
    WebSocketConnectionPrivate *d = new WebSocketConnectionPrivate;
    d->core = std::move(core);
    attachDisconnectedBridge(d);
    return new WebSocketConnection(d);
}

WebSocketConnection::WebSocketConnection(QSharedPointer<SocketLike> connection, const QByteArray &headBytes, Side side,
                                         const WebSocketConfiguration &config)
    : d_ptr(new WebSocketConnectionPrivate)
{
    Q_D(WebSocketConnection);
    d->core = make_shared<qtng_core::WebSocketConnection>(
            toCoreSocketLike(connection), toStdString(headBytes),
            static_cast<qtng_core::WebSocketConnection::Side>(side), toCoreConfig(config));
    attachDisconnectedBridge(d);
}

WebSocketConnection::WebSocketConnection(WebSocketConnectionPrivate *d)
    : d_ptr(d)
{
}

WebSocketConnection::~WebSocketConnection()
{
    delete d_ptr;
}

QSharedPointer<Event> WebSocketConnection::disconnected() const
{
    Q_D(const WebSocketConnection);
    return d->disconnectedEvent;
}

void WebSocketConnection::setConfiguration(const WebSocketConfiguration &config)
{
    Q_D(WebSocketConnection);
    d->core->setConfiguration(toCoreConfig(config));
}

bool WebSocketConnection::send(const QByteArray &packet) { Q_D(WebSocketConnection); return d->core->send(toStdString(packet)); }
bool WebSocketConnection::send(const QString &text) { Q_D(WebSocketConnection); return d->core->sendText(toStdString(text)); }
bool WebSocketConnection::post(const QByteArray &packet) { Q_D(WebSocketConnection); return d->core->post(toStdString(packet)); }
bool WebSocketConnection::post(const QString &text) { Q_D(WebSocketConnection); return d->core->postText(toStdString(text)); }
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
std::shared_ptr<const HttpResponse> WebSocketConnection::response() const
{
    Q_D(const WebSocketConnection);
    const shared_ptr<const qtng_core::HttpResponse> coreResponse = d->core->response();
    if (!coreResponse) {
        return std::shared_ptr<const HttpResponse>();
    }
    return std::make_shared<const HttpResponse>(httpResponseFromCore(*coreResponse));
}

}  // namespace QTNETWORKNG_NAMESPACE

namespace qtng_bridge {

QSharedPointer<QTNETWORKNG_NAMESPACE::WebSocketConnection>
webSocketConnectionFromCore(std::shared_ptr<qtng_core::WebSocketConnection> core)
{
    if (!core) {
        return QSharedPointer<QTNETWORKNG_NAMESPACE::WebSocketConnection>();
    }
    return QSharedPointer<QTNETWORKNG_NAMESPACE::WebSocketConnection>(
            QTNETWORKNG_NAMESPACE::WebSocketConnectionPrivate::create(std::move(core)));
}

std::shared_ptr<QTNETWORKNG_NAMESPACE::WebSocketConfiguration>
webSocketConfigurationFromCore(std::shared_ptr<qtng_core::WebSocketConfiguration> core)
{
    std::shared_ptr<QTNETWORKNG_NAMESPACE::WebSocketConfiguration> config =
            std::make_shared<QTNETWORKNG_NAMESPACE::WebSocketConfiguration>();
    QTNETWORKNG_NAMESPACE::WebSocketConfigurationPrivate::bindCore(config.get(), std::move(core));
    return config;
}

std::shared_ptr<qtng_core::WebSocketConfiguration>
webSocketConfigurationToCore(const std::shared_ptr<QTNETWORKNG_NAMESPACE::WebSocketConfiguration> &config)
{
    if (!config) {
        return std::shared_ptr<qtng_core::WebSocketConfiguration>();
    }
    return QTNETWORKNG_NAMESPACE::WebSocketConfigurationPrivate::sharedCoreOf(*config);
}

}  // namespace qtng_bridge
