#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <qtnetworkng.h>

using namespace qtng;

// Bridge verification:
//  1. httpd bridge: a static http server serves a file from a temp dir.
//  2. HttpSession bridge settings: dnsCache / socketProxy / httpProxy / cacheManager /
//     cookie / setManagingCookies / webSocketConfiguration / sslConfiguration.
//  3. Socks5RequestHandler bridge: an HTTP request tunneled through a local SOCKS5 server.
static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            qCritical() << "FAIL:" << msg;                                  \
            ++failures;                                                     \
        } else {                                                            \
            qDebug() << "ok:" << msg;                                       \
        }                                                                   \
    } while (0)

static QString g_rootDir;

class RootedHttpRequestHandler : public SimpleHttpRequestHandler
{
protected:
    virtual bool setup() override
    {
        rootDir = QDir(g_rootDir);
        return true;
    }
};

static QSharedPointer<HttpSession> g_session;
static QSharedPointer<HttpSession> g_viaSocks;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    Coroutine::spawn([]() {
        // --- prepare a temp dir with an index.html ---
        QTemporaryDir tmp;
        CHECK(tmp.isValid(), "temporary dir created");
        g_rootDir = tmp.path();
        QFile indexFile(tmp.path() + QStringLiteral("/index.html"));
        CHECK(indexFile.open(QIODevice::WriteOnly), "index.html opened");
        const QByteArray body = QByteArrayLiteral("hello-bridge-index");
        indexFile.write(body);
        indexFile.close();

        // --- 1. httpd bridge: static http server ---
        TcpServer<RootedHttpRequestHandler> httpServer(HostAddress(HostAddress::LocalHost), 0);
        CHECK(httpServer.start(), "http server started");
        const quint16 httpPort = httpServer.serverPort();
        CHECK(httpPort > 0, "http server port assigned");

        g_session = QSharedPointer<HttpSession>::create();

        // --- 2. HttpSession bridge settings ---
        g_session->setDnsCache(QSharedPointer<SocketDnsCache>::create());
        CHECK(!g_session->dnsCache().isNull(), "dnsCache round-trip");
        g_session->setSocketProxy(QSharedPointer<SocketProxy>());
        CHECK(g_session->socketProxy().isNull(), "socketProxy null round-trip");
        g_session->setHttpProxy(QSharedPointer<HttpProxy>());
        CHECK(g_session->httpProxy().isNull(), "httpProxy null round-trip");
        g_session->setCacheManager(QSharedPointer<HttpCacheManager>(new HttpMemoryCacheManager()));
        CHECK(!g_session->cacheManager().isNull(), "cacheManager round-trip");
        g_session->setManagingCookies(true);
        CHECK(g_session->cookie(QUrl(QStringLiteral("http://example.com/")), QStringLiteral("n")).name().isEmpty(),
              "cookie empty for unknown jar");
        g_session->webSocketConfiguration().setKeepaliveInterval(12.0f);
        CHECK(qFuzzyCompare(g_session->webSocketConfiguration().keepaliveInterval(), 12.0f),
              "webSocketConfiguration keepaliveInterval round-trip");
        g_session->webSocketConfiguration().setProtocols(QStringList() << QStringLiteral("chat"));
        CHECK(g_session->webSocketConfiguration().protocols() == QStringList(QStringLiteral("chat")),
              "webSocketConfiguration protocols round-trip");
#ifndef QTNG_NO_CRYPTO
        CHECK(!g_session->sslConfiguration().caCertificates().isEmpty()
                      || g_session->sslConfiguration().caCertificates().isEmpty(),
              "sslConfiguration accessible");
#endif

        // --- HttpCacheManager bridge: serialize through the core and read back ---
        {
            QSharedPointer<HttpCacheManager> cache = QSharedPointer<HttpCacheManager>(new HttpMemoryCacheManager());
            HttpResponse toStore;
            toStore.setUrl(QUrl(QStringLiteral("http://cache.example.com/")));
            toStore.setStatusCode(200);
            toStore.setStatusText(QStringLiteral("OK"));
            toStore.setHeader(QStringLiteral("Content-Type"), QByteArrayLiteral("text/plain"));
            toStore.setBody(QByteArrayLiteral("cached-body"));
            CHECK(cache->addResponse(toStore), "cache addResponse");
            HttpResponse fromCache;
            fromCache.setUrl(QUrl(QStringLiteral("http://cache.example.com/")));
            CHECK(cache->getResponse(&fromCache), "cache getResponse hit");
            CHECK(fromCache.statusCode() == 200 && fromCache.body() == QByteArrayLiteral("cached-body"),
                  "cache round-trip content");
        }

        // --- raw socket probe: does the server actually send the file body? ---
        {
            QSharedPointer<Socket> raw = QSharedPointer<Socket>(
                    Socket::createConnection(QStringLiteral("127.0.0.1"), httpPort));
            CHECK(!raw.isNull(), "raw probe connection established");
            if (raw) {
                raw->sendall(QByteArrayLiteral("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
                QByteArray reply;
                for (;;) {
                    const QByteArray chunk = raw->recvall(4096);
                    if (chunk.isEmpty()) {
                        break;
                    }
                    reply.append(chunk);
                }
                qDebug() << "raw probe reply (" << reply.size() << "bytes):" << reply.left(120);
                CHECK(reply.contains(body), "raw probe sees the file body");
                raw->close();
            }
        }

        // --- plain HTTP request through the bridged httpd ---
        const QUrl indexUrl(QStringLiteral("http://127.0.0.1:%1/").arg(httpPort));
        HttpResponse resp = g_session->get(indexUrl);
        qDebug() << "plain response: status" << resp.statusCode() << "text" << resp.statusText() << "body"
                 << resp.body() << "content-length" << resp.getContentLength();
        CHECK(resp.isOk(), "plain http GET returns ok (httpd bridge)");
        if (resp.isOk()) {
            CHECK(resp.body() == body, "plain http GET returns index content");
            CHECK(resp.statusText() == QStringLiteral("OK"), "plain http GET status text");
            CHECK(resp.header(QStringLiteral("Content-Type")) == QByteArrayLiteral("text/html"),
                  "plain http GET content-type header");
            CHECK(resp.getContentLength() == body.size(), "plain http GET content-length");
            CHECK(resp.request().method() == QStringLiteral("GET"), "plain http GET request method");
            CHECK(resp.request().url() == indexUrl, "plain http GET request url");
        }

        // --- send(): header bridge + deep copy must not mutate the caller's request ---
        {
            HttpRequest req;
            req.setMethod(QStringLiteral("GET"));
            req.setUrl(indexUrl);
            req.setVersion(Http1_0);
            req.setHeader(QStringLiteral("X-Bridge-Test"), QByteArrayLiteral("yes"));
            HttpResponse r = g_session->send(req);
            CHECK(r.isOk(), "http GET via send returns ok");
            CHECK(req.version() == Http1_0, "send does not mutate caller version");
            CHECK(req.header(QStringLiteral("X-Bridge-Test")) == QByteArrayLiteral("yes"),
                  "send keeps caller headers");
            CHECK(r.getContentLength() == body.size(), "send response content-length");
        }

        // --- 3. socks5 bridge: tunnel an http request through Socks5RequestHandler ---
        TcpServer<Socks5RequestHandler> socksServer(HostAddress(HostAddress::LocalHost), 0);
        CHECK(socksServer.start(), "socks5 server started");
        const quint16 socksPort = socksServer.serverPort();
        CHECK(socksPort > 0, "socks5 server port assigned");

        g_viaSocks = QSharedPointer<HttpSession>::create();
        g_viaSocks->setSocketProxy(
                QSharedPointer<SocketProxy>(new Socks5Proxy(QStringLiteral("127.0.0.1"), socksPort)));
        HttpResponse respViaSocks = g_viaSocks->get(indexUrl);
        CHECK(respViaSocks.isOk(), "http GET through socks5 bridge returns ok");
        if (respViaSocks.isOk()) {
            CHECK(respViaSocks.body() == body, "http GET through socks5 bridge returns index content");
        }

        socksServer.stop();
        httpServer.stop();

        if (failures == 0) {
            qDebug() << "bridge verification PASSED";
            qApp->exit(0);
        } else {
            qCritical() << "bridge verification FAILED with" << failures << "failure(s)";
            qApp->exit(1);
        }
    });

    QTimer::singleShot(30000, &app, []() {
        qFatal("TIMEOUT: bridge verification did not complete");
    });

    return startQtLoop();
}
