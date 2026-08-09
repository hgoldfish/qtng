#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "httpd.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

BaseHttpRequestHandler::BaseHttpRequestHandler()
    : version(Http1_1)
    , serverVersion(Http1_1)
    , requestTimeout(60 * 60)
    , maxBodySize(1024 * 1024 * 32)
    , closeConnection(Maybe)
{
}

void BaseHttpRequestHandler::handle()
{
    do {
        closeConnection = Maybe;
        handleOneRequest();
    } while (closeConnection == No && request);
}

void BaseHttpRequestHandler::handleOneRequest()
{
    try {
        Timeout timeout(requestTimeout);
        if (!parseRequest()) {
            return;
        }
        doMethod();
    } catch (TimeoutException &) {
        logError(Gone, QString::fromLatin1("timeout"), QString::fromLatin1("timeout"));
        closeConnection = Yes;
    }
}

QString BaseHttpRequestHandler::normalizePath(const QString &path)
{
    return QUrl(path).path();
}

void BaseHttpRequestHandler::doMethod()
{
    if (method == QLatin1String("GET")) {
        doGET();
    } else if (method == QLatin1String("POST")) {
        doPOST();
    } else if (method == QLatin1String("PUT")) {
        doPUT();
    } else if (method == QLatin1String("DELETE")) {
        doDELETE();
    } else if (method == QLatin1String("PATCH")) {
        doPATCH();
    } else if (method == QLatin1String("HEAD")) {
        doHEAD();
    } else if (method == QLatin1String("OPTIONS")) {
        doOPTIONS();
    } else if (method == QLatin1String("TRACE")) {
        doTRACE();
    } else if (method == QLatin1String("CONNECT")) {
        doCONNECT();
    } else {
        sendError(MethodNotAllowed);
    }
}

void BaseHttpRequestHandler::doGET() { sendError(NotImplemented); }
void BaseHttpRequestHandler::doPOST() { sendError(NotImplemented); }
void BaseHttpRequestHandler::doPUT() { sendError(NotImplemented); }
void BaseHttpRequestHandler::doDELETE() { sendError(NotImplemented); }
void BaseHttpRequestHandler::doPATCH() { sendError(NotImplemented); }
void BaseHttpRequestHandler::doHEAD() { sendError(NotImplemented); }
void BaseHttpRequestHandler::doOPTIONS() { sendError(NotImplemented); }
void BaseHttpRequestHandler::doTRACE() { sendError(NotImplemented); }
void BaseHttpRequestHandler::doCONNECT() { sendError(NotImplemented); }

QString BaseHttpRequestHandler::serverName() { return QString::fromLatin1("QtNetworkNg"); }

void BaseHttpRequestHandler::logRequest(HttpStatus, int) { }
void BaseHttpRequestHandler::logError(HttpStatus, const QString &, const QString &) { }

bool BaseHttpRequestHandler::parseRequest()
{
    if (!request) {
        return false;
    }
    HeaderSplitter splitter(request);
    HeaderSplitter::Error error = HeaderSplitter::NoError;
    const QByteArray firstLine = splitter.nextLine(&error);
    if (firstLine.isEmpty() || error != HeaderSplitter::NoError) {
        return false;
    }
    const QList<QByteArray> parts = firstLine.trimmed().split(' ');
    if (parts.size() < 3) {
        return false;
    }
    method = QString::fromUtf8(parts.at(0));
    path = QString::fromUtf8(parts.at(1));
    version = Http1_1;
    headers = splitter.headers(100, &error);
    return error == HeaderSplitter::NoError;
}

bool BaseHttpRequestHandler::sendError(HttpStatus status, const QString &longMessage)
{
    return sendResponse(status, longMessage);
}

bool BaseHttpRequestHandler::sendResponse(HttpStatus status, const QString &longMessage)
{
    sendCommandLine(status, longMessage);
    endHeader();
    Q_UNUSED(longMessage);
    return true;
}

QString BaseHttpRequestHandler::errorMessage(HttpStatus status, const QString &shortMessage, const QString &longMessage)
{
    Q_UNUSED(status);
    return shortMessage + longMessage;
}

QString BaseHttpRequestHandler::errorMessageContentType() { return QString::fromLatin1("text/plain"); }
QString BaseHttpRequestHandler::dateTimeString() { return QDateTime::currentDateTimeUtc().toString(Qt::RFC2822Date); }

QSharedPointer<FileLike> BaseHttpRequestHandler::bodyAsFile(bool processEncoding)
{
    Q_UNUSED(processEncoding);
    return QSharedPointer<FileLike>();
}

bool BaseHttpRequestHandler::switchToWebSocket() { return false; }

QList<QByteArray> BaseHttpRequestHandler::webSocketProtocols()
{
    return QList<QByteArray>();
}

void BaseHttpRequestHandler::sendCommandLine(HttpStatus status, const QString &shortMessage)
{
    Q_UNUSED(status);
    Q_UNUSED(shortMessage);
}

void BaseHttpRequestHandler::sendHeader(const QByteArray &name, const QByteArray &value)
{
    headerCache.append(name + QByteArray(": ") + value + QByteArray("\r\n"));
}

bool BaseHttpRequestHandler::endHeader()
{
    headerCache.clear();
    return true;
}

bool BaseHttpRequestHandler::readBody()
{
    body.clear();
    return true;
}

QByteArray BaseHttpRequestHandler::tryToHandleMagicCode(bool &done)
{
    done = false;
    return QByteArray();
}

QSharedPointer<FileLike> StaticHttpRequestHandler::serveStaticFiles(const QDir &, const QString &)
{
    return QSharedPointer<FileLike>();
}

QSharedPointer<FileLike> StaticHttpRequestHandler::listDirectory(const QDir &, const QString &)
{
    return QSharedPointer<FileLike>();
}

bool StaticHttpRequestHandler::loadMissingFile(const QFileInfo &) { return false; }
QFileInfo StaticHttpRequestHandler::getIndexFile(const QDir &dir) { return QFileInfo(dir, QString::fromLatin1("index.html")); }

void SimpleHttpRequestHandler::doGET()
{
    sendError(NotFound);
}

void SimpleHttpRequestHandler::doHEAD()
{
    sendError(NotFound);
}

}  // namespace QTNETWORKNG_NAMESPACE
