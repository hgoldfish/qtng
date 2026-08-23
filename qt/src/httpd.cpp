#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "bridge/stream_bridge.h"
#include "httpd.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace qtng_bridge {

qtng_core::PosixPath toCorePosixPath(const QDir &dir)
{
    return qtng_core::PosixPath(toStdString(dir.path()));
}

qtng_core::PosixPath toCorePosixPath(const QFileInfo &fileInfo)
{
    return qtng_core::PosixPath(toStdString(fileInfo.absoluteFilePath()));
}

QDir toQtDir(const qtng_core::PosixPath &path)
{
    return QDir(toQString(path.path()));
}

QFileInfo toQtFileInfo(const qtng_core::PosixPath &path)
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
class QtHttpRequestHandlerCoreBridge : public qtng_core::StaticHttpRequestHandler
{
public:
    explicit QtHttpRequestHandlerCoreBridge(QTNETWORKNG_NAMESPACE::BaseHttpRequestHandler *q)
        : q(q)
    {
    }

    // ---- non-virtual core helpers, called from the Qt default implementations ----
    void handleCore() { qtng_core::BaseHttpRequestHandler::handle(); }
    void handleOneRequestCore() { qtng_core::BaseHttpRequestHandler::handleOneRequest(); }
    bool parseRequestCore()
    {
        const bool ok = qtng_core::BaseHttpRequestHandler::parseRequest();
        syncToQt();
        return ok;
    }
    void doMethodCore() { qtng_core::BaseHttpRequestHandler::doMethod(); }
    void doGETCore() { qtng_core::BaseHttpRequestHandler::doGET(); }
    void doPOSTCore() { qtng_core::BaseHttpRequestHandler::doPOST(); }
    void doPUTCore() { qtng_core::BaseHttpRequestHandler::doPUT(); }
    void doDELETECore() { qtng_core::BaseHttpRequestHandler::doDELETE(); }
    void doPATCHCore() { qtng_core::BaseHttpRequestHandler::doPATCH(); }
    void doHEADCore() { qtng_core::BaseHttpRequestHandler::doHEAD(); }
    void doOPTIONSCore() { qtng_core::BaseHttpRequestHandler::doOPTIONS(); }
    void doTRACECore() { qtng_core::BaseHttpRequestHandler::doTRACE(); }
    void doCONNECTCore() { qtng_core::BaseHttpRequestHandler::doCONNECT(); }
    bool sendErrorCore(qtng_core::HttpStatus status, const string &longMessage)
    {
        return qtng_core::BaseHttpRequestHandler::sendError(status, longMessage);
    }
    bool sendResponseCore(qtng_core::HttpStatus status, const string &longMessage)
    {
        return qtng_core::BaseHttpRequestHandler::sendResponse(status, longMessage);
    }
    string errorMessageCore(qtng_core::HttpStatus status, const string &shortMessage, const string &longMessage)
    {
        return qtng_core::BaseHttpRequestHandler::errorMessage(status, shortMessage, longMessage);
    }
    string errorMessageContentTypeCore() { return qtng_core::BaseHttpRequestHandler::errorMessageContentType(); }
    string dateTimeStringCore() { return qtng_core::BaseHttpRequestHandler::dateTimeString(); }
    string serverNameCore() { return qtng_core::BaseHttpRequestHandler::serverName(); }
    void logRequestCore(qtng_core::HttpStatus status, int bodySize)
    {
        qtng_core::BaseHttpRequestHandler::logRequest(status, bodySize);
    }
    void logErrorCore(qtng_core::HttpStatus status, const string &shortMessage, const string &longMessage)
    {
        qtng_core::BaseHttpRequestHandler::logError(status, shortMessage, longMessage);
    }
    shared_ptr<qtng_core::FileLike> bodyAsFileCore(bool processEncoding)
    {
        return qtng_core::BaseHttpRequestHandler::bodyAsFile(processEncoding);
    }
    bool switchToWebSocketCore() { return qtng_core::BaseHttpRequestHandler::switchToWebSocket(); }
    string tryToHandleMagicCodeCore(bool &done) { return qtng_core::BaseHttpRequestHandler::tryToHandleMagicCode(done); }

    // non-virtual static-file helpers, called from the Qt default implementations.
    shared_ptr<qtng_core::FileLike> serveStaticFilesCore(const qtng_core::PosixPath &dir, const string &subPath)
    {
        return qtng_core::StaticHttpRequestHandler::serveStaticFiles(dir, subPath);
    }
    shared_ptr<qtng_core::FileLike> listDirectoryCore(const qtng_core::PosixPath &dir, const string &displayDir)
    {
        return qtng_core::StaticHttpRequestHandler::listDirectory(dir, displayDir);
    }
    bool loadMissingFileCore(const qtng_core::PosixPath &fileInfo)
    {
        return qtng_core::StaticHttpRequestHandler::loadMissingFile(fileInfo);
    }
    qtng_core::PosixPath getIndexFileCore(const qtng_core::PosixPath &dir)
    {
        return qtng_core::StaticHttpRequestHandler::getIndexFile(dir);
    }

    // non-virtual protocol utilities, called from the Qt default implementations.
    void sendCommandLine(qtng_core::HttpStatus status, const string &shortMessage)
    {
        qtng_core::BaseHttpRequestHandler::sendCommandLine(status, shortMessage);
    }
    void sendHeader(const string &name, const string &value)
    {
        qtng_core::BaseHttpRequestHandler::sendHeader(name, value);
    }
    bool endHeader() { return qtng_core::BaseHttpRequestHandler::endHeader(); }
    bool readBody() { return qtng_core::BaseHttpRequestHandler::readBody(); }
    vector<string> webSocketProtocols() { return qtng_core::BaseHttpRequestHandler::webSocketProtocols(); }

    // ---- virtual methods: forward to the Qt side so that user overrides are honored ----
    void handle() override { q->handle(); }
    void handleOneRequest() override { q->handleOneRequest(); }
    bool parseRequest() override { return q->parseRequest(); }
    void doMethod() override { q->doMethod(); }
    void doGET() override { q->doGET(); }
    void doPOST() override { q->doPOST(); }
    void doPUT() override { q->doPUT(); }
    void doDELETE() override { q->doDELETE(); }
    void doPATCH() override { q->doPATCH(); }
    void doHEAD() override { q->doHEAD(); }
    void doOPTIONS() override { q->doOPTIONS(); }
    void doTRACE() override { q->doTRACE(); }
    void doCONNECT() override { q->doCONNECT(); }
    bool sendError(qtng_core::HttpStatus status, const string &longMessage) override
    {
        return q->sendError(static_cast<HttpStatus>(status), toQString(longMessage));
    }
    bool sendResponse(qtng_core::HttpStatus status, const string &longMessage) override
    {
        return q->sendResponse(static_cast<HttpStatus>(status), toQString(longMessage));
    }
    string errorMessage(qtng_core::HttpStatus status, const string &shortMessage, const string &longMessage) override
    {
        return toStdString(q->errorMessage(static_cast<HttpStatus>(status), toQString(shortMessage), toQString(longMessage)));
    }
    string errorMessageContentType() override { return toStdString(q->errorMessageContentType()); }
    string dateTimeString() override { return toStdString(q->dateTimeString()); }
    string serverName() override { return toStdString(q->serverName()); }
    void logRequest(qtng_core::HttpStatus status, int bodySize) override
    {
        q->logRequest(static_cast<HttpStatus>(status), bodySize);
    }
    void logError(qtng_core::HttpStatus status, const string &shortMessage, const string &longMessage) override
    {
        q->logError(static_cast<HttpStatus>(status), toQString(shortMessage), toQString(longMessage));
    }
    shared_ptr<qtng_core::FileLike> bodyAsFile(bool processEncoding) override
    {
        return toCoreFileLike(q->bodyAsFile(processEncoding));
    }
    bool switchToWebSocket() override { return q->switchToWebSocket(); }
    string tryToHandleMagicCode(bool &done) override { return toStdString(q->tryToHandleMagicCode(done)); }

    shared_ptr<qtng_core::FileLike> serveStaticFiles(const qtng_core::PosixPath &dir, const string &subPath) override
    {
        return toCoreFileLike(static_cast<QTNETWORKNG_NAMESPACE::StaticHttpRequestHandler *>(q)
                                      ->serveStaticFiles(toQtDir(dir), toQString(subPath)));
    }
    shared_ptr<qtng_core::FileLike> listDirectory(const qtng_core::PosixPath &dir, const string &displayDir) override
    {
        return toCoreFileLike(static_cast<QTNETWORKNG_NAMESPACE::StaticHttpRequestHandler *>(q)
                                      ->listDirectory(toQtDir(dir), toQString(displayDir)));
    }
    bool loadMissingFile(const qtng_core::PosixPath &fileInfo) override
    {
        return static_cast<QTNETWORKNG_NAMESPACE::StaticHttpRequestHandler *>(q)->loadMissingFile(toQtFileInfo(fileInfo));
    }
    qtng_core::PosixPath getIndexFile(const qtng_core::PosixPath &dir) override
    {
        return toCorePosixPath(static_cast<QTNETWORKNG_NAMESPACE::StaticHttpRequestHandler *>(q)
                                       ->getIndexFile(toQtDir(dir)));
    }

    // ---- state synchronization ----
    void setServerVersion(qtng_core::HttpVersion v) { serverVersion = v; }
    void setRequestTimeout(float t) { requestTimeout = t; }
    void setMaxBodySize(std::int32_t s) { maxBodySize = s; }
    void setEnableDirectoryListing(bool b) { enableDirectoryListing = b; }

    void syncToQt()
    {
        q->method = toQString(method);
        q->path = toQString(path);
        q->body = toQByteArray(body);
        q->version = static_cast<HttpVersion>(version);
        q->closeConnection =
                static_cast<QTNETWORKNG_NAMESPACE::BaseHttpRequestHandler::CloseConnectionStatus>(closeConnection);
        QList<HttpHeader> qtHeaders;
        for (const qtng_core::HttpHeader &h : allHeaders()) {
            qtHeaders.append(HttpHeader(toQString(h.name), toQByteArray(h.value)));
        }
        q->setHeaders(qtHeaders);
    }

    int qtCloseConnection() const { return static_cast<int>(closeConnection); }

private:
    QTNETWORKNG_NAMESPACE::BaseHttpRequestHandler * const q;
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
    coreBridge->setServerVersion(static_cast<qtng_core::HttpVersion>(serverVersion));
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
    return QUrl(path).path();
}

void BaseHttpRequestHandler::doMethod()
{
    coreBridge->doMethodCore();
}

void BaseHttpRequestHandler::doGET() { coreBridge->doGETCore(); }
void BaseHttpRequestHandler::doPOST() { coreBridge->doPOSTCore(); }
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
    coreBridge->logRequestCore(static_cast<qtng_core::HttpStatus>(status), bodySize);
}

void BaseHttpRequestHandler::logError(HttpStatus status, const QString &shortMessage, const QString &longMessage)
{
    coreBridge->logErrorCore(static_cast<qtng_core::HttpStatus>(status), toStdString(shortMessage),
                             toStdString(longMessage));
}

bool BaseHttpRequestHandler::parseRequest()
{
    return coreBridge->parseRequestCore();
}

bool BaseHttpRequestHandler::sendError(HttpStatus status, const QString &longMessage)
{
    return coreBridge->sendErrorCore(static_cast<qtng_core::HttpStatus>(status), toStdString(longMessage));
}

bool BaseHttpRequestHandler::sendResponse(HttpStatus status, const QString &longMessage)
{
    return coreBridge->sendResponseCore(static_cast<qtng_core::HttpStatus>(status), toStdString(longMessage));
}

QString BaseHttpRequestHandler::errorMessage(HttpStatus status, const QString &shortMessage, const QString &longMessage)
{
    return toQString(coreBridge->errorMessageCore(static_cast<qtng_core::HttpStatus>(status), toStdString(shortMessage),
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
    coreBridge->sendCommandLine(static_cast<qtng_core::HttpStatus>(status), toStdString(shortMessage));
    closeConnection = static_cast<CloseConnectionStatus>(coreBridge->qtCloseConnection());
}

void BaseHttpRequestHandler::sendHeader(const QByteArray &name, const QByteArray &value)
{
    coreBridge->sendHeader(toStdString(name), toStdString(value));
    closeConnection = static_cast<CloseConnectionStatus>(coreBridge->qtCloseConnection());
}

bool BaseHttpRequestHandler::endHeader()
{
    const bool ok = coreBridge->endHeader();
    closeConnection = static_cast<CloseConnectionStatus>(coreBridge->qtCloseConnection());
    return ok;
}

bool BaseHttpRequestHandler::readBody()
{
    return coreBridge->readBody();
}

QByteArray BaseHttpRequestHandler::tryToHandleMagicCode(bool &done)
{
    return toQByteArray(coreBridge->tryToHandleMagicCodeCore(done));
}

QSharedPointer<FileLike> StaticHttpRequestHandler::serveStaticFiles(const QDir &dir, const QString &subPath)
{
    coreBridge->setEnableDirectoryListing(enableDirectoryListing);
    const shared_ptr<qtng_core::FileLike> core = coreBridge->serveStaticFilesCore(toCorePosixPath(dir), toStdString(subPath));
    return toQtFileLike(core);
}

QSharedPointer<FileLike> StaticHttpRequestHandler::listDirectory(const QDir &dir, const QString &displayDir)
{
    const shared_ptr<qtng_core::FileLike> core =
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
