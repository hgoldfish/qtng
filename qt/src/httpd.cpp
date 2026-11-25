#include "bridge/core_access.h"
#include "bridge/http_access.h"
#include "bridge/io_bridge.h"
#include "bridge/stream_bridge.h"
#include "httpd.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace qtng_bridge {

QTNG_NAMESPACE::PosixPath toCorePosixPath(const QDir &dir)
{
    return QTNG_NAMESPACE::PosixPath(toStdString(dir.path()));
}

QTNG_NAMESPACE::PosixPath toCorePosixPath(const QFileInfo &fileInfo)
{
    return QTNG_NAMESPACE::PosixPath(toStdString(fileInfo.absoluteFilePath()));
}

QDir toQtDir(const QTNG_NAMESPACE::PosixPath &path)
{
    return QDir(toQString(path.path()));
}

QFileInfo toQtFileInfo(const QTNG_NAMESPACE::PosixPath &path)
{
    return QFileInfo(toQString(path.path()));
}

}  // namespace qtng_bridge

namespace QTNETWORKNG_NAMESPACE {

// The bridge translates the HTTP server protocol handled by the core into the Qt binding:
//  - every core virtual method is overridden to forward to the corresponding Qt virtual method
//    so that user subclasses overriding the Qt API keep working;
//  - the Qt default implementations call back into the non-virtual core helpers (…Core()),
//    which hold the real protocol logic implemented in the core library.
class QtHttpRequestHandlerCoreBridge : public QTNG_NAMESPACE::StaticHttpRequestHandler
{
public:
    explicit QtHttpRequestHandlerCoreBridge(QTNETWORKNG_NAMESPACE::BaseHttpRequestHandler *q)
        : q(q)
        , lastSyncedClose(Maybe)
    {
    }

    // ---- non-virtual core helpers, called from the Qt default implementations ----
    void handleCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::handle(); }
    void handleOneRequestCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::handleOneRequest(); }
    bool parseRequestCore()
    {
        const bool ok = QTNG_NAMESPACE::BaseHttpRequestHandler::parseRequest();
        syncToQt();
        return ok;
    }
    void doMethodCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doMethod(); }
    void doGETCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doGET(); }
    void doPOSTCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doPOST(); }
    void doQUERYCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doQUERY(); }
    void doPUTCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doPUT(); }
    void doDELETECore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doDELETE(); }
    void doPATCHCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doPATCH(); }
    void doHEADCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doHEAD(); }
    void doOPTIONSCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doOPTIONS(); }
    void doTRACECore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doTRACE(); }
    void doCONNECTCore() { QTNG_NAMESPACE::BaseHttpRequestHandler::doCONNECT(); }
    bool sendErrorCore(QTNG_NAMESPACE::HttpStatus status, const string &longMessage)
    {
        pushCloseConnection();
        const bool ok = QTNG_NAMESPACE::BaseHttpRequestHandler::sendError(status, longMessage);
        pullCloseConnection();
        return ok;
    }
    bool sendResponseCore(QTNG_NAMESPACE::HttpStatus status, const string &longMessage)
    {
        pushCloseConnection();
        const bool ok = QTNG_NAMESPACE::BaseHttpRequestHandler::sendResponse(status, longMessage);
        pullCloseConnection();
        return ok;
    }
    string errorMessageCore(QTNG_NAMESPACE::HttpStatus status, const string &shortMessage, const string &longMessage)
    {
        return QTNG_NAMESPACE::BaseHttpRequestHandler::errorMessage(status, shortMessage, longMessage);
    }
    string errorMessageContentTypeCore() { return QTNG_NAMESPACE::BaseHttpRequestHandler::errorMessageContentType(); }
    string dateTimeStringCore() { return QTNG_NAMESPACE::BaseHttpRequestHandler::dateTimeString(); }
    string serverNameCore() { return QTNG_NAMESPACE::BaseHttpRequestHandler::serverName(); }
    void logRequestCore(QTNG_NAMESPACE::HttpStatus status, int bodySize)
    {
        QTNG_NAMESPACE::BaseHttpRequestHandler::logRequest(status, bodySize);
    }
    void logErrorCore(QTNG_NAMESPACE::HttpStatus status, const string &shortMessage, const string &longMessage)
    {
        QTNG_NAMESPACE::BaseHttpRequestHandler::logError(status, shortMessage, longMessage);
    }
    shared_ptr<QTNG_NAMESPACE::FileLike> bodyAsFileCore(bool processEncoding)
    {
        pushCloseConnection();
        shared_ptr<QTNG_NAMESPACE::FileLike> file = QTNG_NAMESPACE::BaseHttpRequestHandler::bodyAsFile(processEncoding);
        pullCloseConnection();
        return file;
    }
    bool switchToWebSocketCore() { return QTNG_NAMESPACE::BaseHttpRequestHandler::switchToWebSocket(); }
    string tryToHandleMagicCodeCore(bool &done) { return QTNG_NAMESPACE::BaseHttpRequestHandler::tryToHandleMagicCode(done); }

    // non-virtual static-file helpers, called from the Qt default implementations.
    shared_ptr<QTNG_NAMESPACE::FileLike> serveStaticFilesCore(const QTNG_NAMESPACE::PosixPath &dir, const string &subPath)
    {
        return QTNG_NAMESPACE::StaticHttpRequestHandler::serveStaticFiles(dir, subPath);
    }
    shared_ptr<QTNG_NAMESPACE::FileLike> listDirectoryCore(const QTNG_NAMESPACE::PosixPath &dir, const string &displayDir)
    {
        return QTNG_NAMESPACE::StaticHttpRequestHandler::listDirectory(dir, displayDir);
    }
    bool loadMissingFileCore(const QTNG_NAMESPACE::PosixPath &fileInfo)
    {
        return QTNG_NAMESPACE::StaticHttpRequestHandler::loadMissingFile(fileInfo);
    }
    QTNG_NAMESPACE::PosixPath getIndexFileCore(const QTNG_NAMESPACE::PosixPath &dir)
    {
        return QTNG_NAMESPACE::StaticHttpRequestHandler::getIndexFile(dir);
    }

    // non-virtual protocol utilities, called from the Qt default implementations.
    void sendCommandLine(QTNG_NAMESPACE::HttpStatus status, const string &shortMessage)
    {
        pushCloseConnection();
        QTNG_NAMESPACE::BaseHttpRequestHandler::sendCommandLine(status, shortMessage);
        pullCloseConnection();
    }
    void sendHeader(const string &name, const string &value)
    {
        pushCloseConnection();
        QTNG_NAMESPACE::BaseHttpRequestHandler::sendHeader(name, value);
        pullCloseConnection();
    }
    bool endHeader()
    {
        pushCloseConnection();
        const bool ok = QTNG_NAMESPACE::BaseHttpRequestHandler::endHeader();
        pullCloseConnection();
        return ok;
    }
    bool readBody()
    {
        pushCloseConnection();
        const bool ok = QTNG_NAMESPACE::BaseHttpRequestHandler::readBody();
        pullCloseConnection();
        return ok;
    }
    bool writeServerResponseCore(const QTNG_NAMESPACE::HttpResponse &response)
    {
        pushCloseConnection();
        const bool ok = QTNG_NAMESPACE::BaseHttpRequestHandler::writeServerResponse(response);
        pullCloseConnection();
        return ok;
    }
    vector<string> webSocketProtocols() { return QTNG_NAMESPACE::BaseHttpRequestHandler::webSocketProtocols(); }

    // ---- virtual methods: forward to the Qt side so that user overrides are honored ----
    void handle() override { q->handle(); }
    // The core loop reads closeConnection right after this returns to decide whether to
    // serve another request, so a handler that set it without calling any of the synced
    // writers (a raw-socket hijack, say) must still have that verdict pushed across.
    void handleOneRequest() override
    {
        q->handleOneRequest();
        pushCloseConnection();
    }
    bool parseRequest() override { return q->parseRequest(); }
    void doMethod() override { q->doMethod(); }
    void doGET() override { q->doGET(); }
    void doPOST() override { q->doPOST(); }
    void doQUERY() override { q->doQUERY(); }
    void doPUT() override { q->doPUT(); }
    void doDELETE() override { q->doDELETE(); }
    void doPATCH() override { q->doPATCH(); }
    void doHEAD() override { q->doHEAD(); }
    void doOPTIONS() override { q->doOPTIONS(); }
    void doTRACE() override { q->doTRACE(); }
    void doCONNECT() override { q->doCONNECT(); }
    bool sendError(QTNG_NAMESPACE::HttpStatus status, const string &longMessage) override
    {
        return q->sendError(static_cast<HttpStatus>(status), toQString(longMessage));
    }
    bool sendResponse(QTNG_NAMESPACE::HttpStatus status, const string &longMessage) override
    {
        return q->sendResponse(static_cast<HttpStatus>(status), toQString(longMessage));
    }
    string errorMessage(QTNG_NAMESPACE::HttpStatus status, const string &shortMessage, const string &longMessage) override
    {
        return toStdString(q->errorMessage(static_cast<HttpStatus>(status), toQString(shortMessage), toQString(longMessage)));
    }
    string errorMessageContentType() override { return toStdString(q->errorMessageContentType()); }
    string dateTimeString() override { return toStdString(q->dateTimeString()); }
    string serverName() override { return toStdString(q->serverName()); }
    void logRequest(QTNG_NAMESPACE::HttpStatus status, int bodySize) override
    {
        q->logRequest(static_cast<HttpStatus>(status), bodySize);
    }
    void logError(QTNG_NAMESPACE::HttpStatus status, const string &shortMessage, const string &longMessage) override
    {
        q->logError(static_cast<HttpStatus>(status), toQString(shortMessage), toQString(longMessage));
    }
    shared_ptr<QTNG_NAMESPACE::FileLike> bodyAsFile(bool processEncoding) override
    {
        return toCoreFileLike(q->bodyAsFile(processEncoding));
    }
    bool switchToWebSocket() override { return q->switchToWebSocket(); }
    string tryToHandleMagicCode(bool &done) override { return toStdString(q->tryToHandleMagicCode(done)); }

    shared_ptr<QTNG_NAMESPACE::FileLike> serveStaticFiles(const QTNG_NAMESPACE::PosixPath &dir, const string &subPath) override
    {
        return toCoreFileLike(static_cast<QTNETWORKNG_NAMESPACE::StaticHttpRequestHandler *>(q)
                                      ->serveStaticFiles(toQtDir(dir), toQString(subPath)));
    }
    shared_ptr<QTNG_NAMESPACE::FileLike> listDirectory(const QTNG_NAMESPACE::PosixPath &dir, const string &displayDir) override
    {
        return toCoreFileLike(static_cast<QTNETWORKNG_NAMESPACE::StaticHttpRequestHandler *>(q)
                                      ->listDirectory(toQtDir(dir), toQString(displayDir)));
    }
    bool loadMissingFile(const QTNG_NAMESPACE::PosixPath &fileInfo) override
    {
        return static_cast<QTNETWORKNG_NAMESPACE::StaticHttpRequestHandler *>(q)->loadMissingFile(toQtFileInfo(fileInfo));
    }
    QTNG_NAMESPACE::PosixPath getIndexFile(const QTNG_NAMESPACE::PosixPath &dir) override
    {
        return toCorePosixPath(static_cast<QTNETWORKNG_NAMESPACE::StaticHttpRequestHandler *>(q)
                                       ->getIndexFile(toQtDir(dir)));
    }

    // ---- state synchronization ----
    void setServerVersion(QTNG_NAMESPACE::HttpVersion v) { serverVersion = v; }
    void setRequestTimeout(float t) { requestTimeout = t; }
    void setMaxBodySize(std::int32_t s) { maxBodySize = s; }
    void setEnableDirectoryListing(bool b) { enableDirectoryListing = b; }

    void syncToQt()
    {
        q->method = toQString(method);
        q->path = toQString(path);
        q->body = toQByteArray(body);
        q->version = static_cast<HttpVersion>(version);
        pullCloseConnection();
        QList<HttpHeader> qtHeaders;
        for (const QTNG_NAMESPACE::HttpHeader &h : allHeaders()) {
            qtHeaders.append(HttpHeader(toQString(h.name()), toQByteArray(h.value())));
        }
        q->setHeaders(qtHeaders);
    }

private:
    // closeConnection never crosses the layer as a bare int: lastSyncedClose is a core-typed
    // copy of the value the core last reported, and the only currency between the two enums
    // is the enumeration's numeric value. Hence the check below — a reordered Qt enum would
    // otherwise silently turn "keep-alive" into "close" with no diagnostic anywhere.
    static_assert(static_cast<int>(QTNG_NAMESPACE::BaseHttpRequestHandler::Yes)
                              == static_cast<int>(QTNETWORKNG_NAMESPACE::BaseHttpRequestHandler::Yes)
                          && static_cast<int>(QTNG_NAMESPACE::BaseHttpRequestHandler::No)
                                  == static_cast<int>(QTNETWORKNG_NAMESPACE::BaseHttpRequestHandler::No)
                          && static_cast<int>(QTNG_NAMESPACE::BaseHttpRequestHandler::Maybe)
                                  == static_cast<int>(QTNETWORKNG_NAMESPACE::BaseHttpRequestHandler::Maybe),
                  "closeConnection values must agree between the core and Qt enums");

    // closeConnection is the handler's intent on the Qt side, while the core
    // recomputes it from the headers and bodies it actually emits (a Connection
    // header, an oversized body, chunked framing). Both directions matter:
    //
    // - pull: copy the core's verdict back, so a handler sees what the protocol
    //   decided (and so nothing stale can be pushed later).
    // - push: copy the handler's *edit* back in — only when the Qt value differs
    //   from what the core last reported. An unconditional push would be wrong,
    //   not merely wasteful: core's bodyAsFile() sets Yes and then sends the
    //   error through the virtual sendError(), which lands back on the Qt side;
    //   pushing the stale value there would erase the decision it is in the
    //   middle of acting on.
    void pushCloseConnection()
    {
        const CloseConnectionStatus requested = static_cast<CloseConnectionStatus>(q->closeConnection);
        if (requested != lastSyncedClose) {
            closeConnection = requested;
        }
    }
    void pullCloseConnection()
    {
        lastSyncedClose = closeConnection;
        q->closeConnection = static_cast<QTNETWORKNG_NAMESPACE::BaseHttpRequestHandler::CloseConnectionStatus>(
                closeConnection);
    }

    QTNETWORKNG_NAMESPACE::BaseHttpRequestHandler * const q;
    CloseConnectionStatus lastSyncedClose;
};

BaseHttpRequestHandler::BaseHttpRequestHandler()
    : version(Http1_1)
    , serverVersion(Http1_1)
    , requestTimeout(60 * 60)
    , maxBodySize(1024 * 1024 * 32)
    , closeConnection(Maybe)
    , coreBridge(new QtHttpRequestHandlerCoreBridge(this))
{
}

BaseHttpRequestHandler::~BaseHttpRequestHandler()
{
    delete coreBridge;
}

void BaseHttpRequestHandler::handle()
{
    if (!request) {
        return;
    }
    coreBridge->request = toCoreSocketLike(request);
    coreBridge->setServerVersion(static_cast<QTNG_NAMESPACE::HttpVersion>(serverVersion));
    coreBridge->setRequestTimeout(requestTimeout);
    coreBridge->setMaxBodySize(maxBodySize);
    coreBridge->handleCore();
}

void BaseHttpRequestHandler::handleOneRequest()
{
    coreBridge->handleOneRequestCore();
}

QString BaseHttpRequestHandler::normalizePath(const QString &path)
{
    return QUrl::fromEncoded(path.toLatin1(), QUrl::StrictMode).toString(QUrl::NormalizePathSegments);
}

void BaseHttpRequestHandler::doMethod()
{
    coreBridge->doMethodCore();
}

void BaseHttpRequestHandler::doGET() { coreBridge->doGETCore(); }
void BaseHttpRequestHandler::doPOST() { coreBridge->doPOSTCore(); }
void BaseHttpRequestHandler::doQUERY() { coreBridge->doQUERYCore(); }
void BaseHttpRequestHandler::doPUT() { coreBridge->doPUTCore(); }
void BaseHttpRequestHandler::doDELETE() { coreBridge->doDELETECore(); }
void BaseHttpRequestHandler::doPATCH() { coreBridge->doPATCHCore(); }
void BaseHttpRequestHandler::doHEAD() { coreBridge->doHEADCore(); }
void BaseHttpRequestHandler::doOPTIONS() { coreBridge->doOPTIONSCore(); }
void BaseHttpRequestHandler::doTRACE() { coreBridge->doTRACECore(); }
void BaseHttpRequestHandler::doCONNECT() { coreBridge->doCONNECTCore(); }

QString BaseHttpRequestHandler::serverName()
{
    return toQString(coreBridge->serverNameCore());
}

void BaseHttpRequestHandler::logRequest(HttpStatus status, int bodySize)
{
    coreBridge->logRequestCore(static_cast<QTNG_NAMESPACE::HttpStatus>(status), bodySize);
}

void BaseHttpRequestHandler::logError(HttpStatus status, const QString &shortMessage, const QString &longMessage)
{
    coreBridge->logErrorCore(static_cast<QTNG_NAMESPACE::HttpStatus>(status), toStdString(shortMessage),
                             toStdString(longMessage));
}

bool BaseHttpRequestHandler::parseRequest()
{
    return coreBridge->parseRequestCore();
}

bool BaseHttpRequestHandler::sendError(HttpStatus status, const QString &longMessage)
{
    return coreBridge->sendErrorCore(static_cast<QTNG_NAMESPACE::HttpStatus>(status), toStdString(longMessage));
}

bool BaseHttpRequestHandler::sendResponse(HttpStatus status, const QString &longMessage)
{
    return coreBridge->sendResponseCore(static_cast<QTNG_NAMESPACE::HttpStatus>(status), toStdString(longMessage));
}

QString BaseHttpRequestHandler::errorMessage(HttpStatus status, const QString &shortMessage, const QString &longMessage)
{
    return toQString(coreBridge->errorMessageCore(static_cast<QTNG_NAMESPACE::HttpStatus>(status), toStdString(shortMessage),
                                                  toStdString(longMessage)));
}

QString BaseHttpRequestHandler::errorMessageContentType()
{
    return toQString(coreBridge->errorMessageContentTypeCore());
}

QString BaseHttpRequestHandler::dateTimeString()
{
    return toQString(coreBridge->dateTimeStringCore());
}

QSharedPointer<FileLike> BaseHttpRequestHandler::bodyAsFile(bool processEncoding)
{
    return toQtFileLike(coreBridge->bodyAsFileCore(processEncoding));
}

bool BaseHttpRequestHandler::switchToWebSocket()
{
    return coreBridge->switchToWebSocketCore();
}

QBYTEARRAYLIST BaseHttpRequestHandler::webSocketProtocols()
{
    QBYTEARRAYLIST result;
    for (const string &p : coreBridge->webSocketProtocols()) {
        result.append(toQByteArray(p));
    }
    return result;
}

void BaseHttpRequestHandler::sendCommandLine(HttpStatus status, const QString &shortMessage)
{
    coreBridge->sendCommandLine(static_cast<QTNG_NAMESPACE::HttpStatus>(status), toStdString(shortMessage));
}

void BaseHttpRequestHandler::sendHeader(const QByteArray &name, const QByteArray &value)
{
    coreBridge->sendHeader(toStdString(name), toStdString(value));
}

bool BaseHttpRequestHandler::endHeader()
{
    return coreBridge->endHeader();
}

bool BaseHttpRequestHandler::readBody()
{
    const bool ok = coreBridge->readBody();
    // core's readBody() fills the core-side `body` member; copy it back to the Qt
    // wrapper, otherwise the Qt `body` member still holds the header-splitter
    // leftovers (a large POST would report the wrong length).
    coreBridge->syncToQt();
    return ok;
}

bool BaseHttpRequestHandler::writeServerResponse(const HttpResponse &response)
{
    return coreBridge->writeServerResponseCore(httpResponseToCore(response));
}

QByteArray BaseHttpRequestHandler::tryToHandleMagicCode(bool &done)
{
    return toQByteArray(coreBridge->tryToHandleMagicCodeCore(done));
}

QSharedPointer<FileLike> StaticHttpRequestHandler::serveStaticFiles(const QDir &dir, const QString &subPath)
{
    coreBridge->setEnableDirectoryListing(enableDirectoryListing);
    const shared_ptr<QTNG_NAMESPACE::FileLike> core = coreBridge->serveStaticFilesCore(toCorePosixPath(dir), toStdString(subPath));
    return toQtFileLike(core);
}

QSharedPointer<FileLike> StaticHttpRequestHandler::listDirectory(const QDir &dir, const QString &displayDir)
{
    const shared_ptr<QTNG_NAMESPACE::FileLike> core =
            coreBridge->listDirectoryCore(toCorePosixPath(dir), toStdString(displayDir));
    return toQtFileLike(core);
}

bool StaticHttpRequestHandler::loadMissingFile(const QFileInfo &fileInfo)
{
    return coreBridge->loadMissingFileCore(toCorePosixPath(fileInfo));
}

QFileInfo StaticHttpRequestHandler::getIndexFile(const QDir &dir)
{
    return toQtFileInfo(coreBridge->getIndexFileCore(toCorePosixPath(dir)));
}

void SimpleHttpRequestHandler::doGET()
{
    QSharedPointer<FileLike> f = serveStaticFiles(rootDir, path);
    if (f) {
        if (!sendfile(f, request)) {
            request->close();
        }
        f->close();
    }
}

void SimpleHttpRequestHandler::doHEAD()
{
    QSharedPointer<FileLike> f = serveStaticFiles(rootDir, path);
    if (f) {
        f->close();
    }
}

}  // namespace QTNETWORKNG_NAMESPACE
