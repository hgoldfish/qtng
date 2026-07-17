#include <memory>
#include <sstream>
#include <string>
#include <vector>


#ifdef QTNG_HAVE_ZLIB
#include "qtng/gzip.h"
#endif

#include "qtng/httpd.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/mime.h"
#include "qtng/utils/url.h"
#include "qtng/md.h"
#include "qtng/io_utils.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.httpd")

namespace qtng {

static const string DEFAULT_ERROR_MESSAGE =
        "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\"\n"
                            "        \"http://www.w3.org/TR/html4/strict.dtd\">\n<html>\n"
                            "    <head>\n"
                            "        <meta http-equiv=\"Content-Type\" content=\"text/html;charset=utf-8\">\n"
                            "        <title>Error response</title>\n"
                            "    </head>\n"
                            "    <body>\n"
                            "        <h1>Error response</h1>\n"
                            "        <p>Error code: %1</p>\n"
                            "        <p>Message: %2.</p>\n"
                            "        <p>Error code explanation: %1 - %3.</p>\n"
                            "    </body>\n"
                            "</html>\n";
static const string DEFAULT_ERROR_CONTENT_TYPE = "text/html;charset=utf-8";

//#define DEBUG_HTTP_PROTOCOL 1

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
    // do not close the request, because it can be keep by other module.
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
        const string message = "HTTP request handler is timeout.";
        logError(HttpStatus::Gone, message, message);
        closeConnection = Yes;
    }
}

string BaseHttpRequestHandler::normalizePath(const string &path)
{
    utils::Url url(path);
    return url.path();
}

bool BaseHttpRequestHandler::parseRequest()
{
    bool done = false;
    const string &buf = tryToHandleMagicCode(done);
    if (done) {
        return false;
    }
    HeaderSplitter headerSplitter(request, buf);
    HeaderSplitter::Error headerSplitterError;
    string firstLine = headerSplitter.nextLine(&headerSplitterError);
    if (firstLine.empty() || headerSplitterError != HeaderSplitter::NoError) {
        return false;
    }
#ifdef DEBUG_HTTP_PROTOCOL
    ngDebug() << "first line is" << firstLine;
#endif

    const string &commandLine = firstLine;
    vector<string> words;
    {
        istringstream iss(commandLine);
        string word;
        while (iss >> word) {
            words.push_back(word);
        }
    }
    if (words.empty()) {
        return false;
    }
    if (words.size() == 3) {
        method = words[0];
        path = words[1];
        const string &versionStr = words[2];
        if (versionStr == "HTTP/1.0") {
            version = Http1_0;
        } else if (versionStr == "HTTP/1.1") {
            version = Http1_1;
        } else {
            sendError(HttpStatus::BadRequest, utils::formatMessage("Bad request version (%1", {versionStr}));
            return false;
        }
    } else if (words.size() == 2) {
        method = words[0];
        path = words[1];
        version = Http1_0;
    } else if (words.empty()) {
        return false;
    } else {
        sendError(HttpStatus::BadRequest, utils::formatMessage("Bad request syntax (%1)", {commandLine}));
        return false;
    }
    method = utils::toUpper(method);
    if (path.empty()) {
        sendError(HttpStatus::BadRequest, utils::formatMessage("Bad request path (%1)", {path}));
        return false;
    }

    const int MaxHeaders = 64;
    vector<HttpHeader> headers = headerSplitter.headers(MaxHeaders, &headerSplitterError);
    switch (headerSplitterError) {
    case HeaderSplitter::EncodingError:
        sendError(HttpStatus::BadRequest, "Bad request invalid header");
        return false;
    case HeaderSplitter::ConnectionError:
        return false;
    case HeaderSplitter::ExhausedMaxLine:
        sendError(HttpStatus::RequestHeaderFieldsTooLarge, "Too much headers");
        return false;
    case HeaderSplitter::LineTooLong:
        sendError(HttpStatus::RequestHeaderFieldsTooLarge, "Line too long");
        return false;
    default:
        break;
    }
    setHeaders(headers);
#ifdef DEBUG_HTTP_PROTOCOL
    for (const HttpHeader &header : headers) {
        ngDebug() << "header(" << header.name << ") = " << header.value;
    }
#endif
    const string &connectionType = header(ConnectionHeader);
    if (utils::toLower(connectionType) == string("close") || utils::toUpper(method) == "CONNECT") {
        closeConnection = Yes;
    } else if (utils::toLower(connectionType) == string("keep-alive") && version >= Http1_1 && serverVersion >= Http1_1) {
        closeConnection = Maybe;
    } else {
        closeConnection = Yes;
    }
    body = headerSplitter.buf;
    return true;
}

string BaseHttpRequestHandler::tryToHandleMagicCode(bool &done)
{
    done = false;
    return string();
}

void BaseHttpRequestHandler::doMethod()
{
    if (method == "GET") {
        doGET();
    } else if (method == "POST") {
        doPOST();
    } else if (method == "PUT") {
        doPUT();
    } else if (method == "PATCH") {
        doPATCH();
    } else if (method == "DELETE") {
        doDELETE();
    } else if (method == "HEAD") {
        doHEAD();
    } else if (method == "OPTIONS") {
        doOPTIONS();
    } else if (method == "TRACE") {
        doTRACE();
    } else if (method == "CONNECT") {
        doCONNECT();
    } else {
        sendError(HttpStatus::NotImplemented, utils::formatMessage("Unsupported method %1", {method}));
    }
}

void BaseHttpRequestHandler::doGET()
{
    string body("hello, world!");
    sendResponse(HttpStatus::OK);
    if (!body.empty()) {
        sendHeader("Content-Type", "text/html");
        sendHeader("Content-Length", utils::number(static_cast<long long>(body.size())));
    }
    if (!endHeader()) {
        return;
    }
    if (!body.empty()) {
        request->sendall(body);
    }
}

void BaseHttpRequestHandler::doPOST()
{
    sendError(HttpStatus::NotImplemented, utils::formatMessage("Unsupported method %1", {method}));
}

void BaseHttpRequestHandler::doPUT()
{
    sendError(HttpStatus::NotImplemented, utils::formatMessage("Unsupported method %1", {method}));
}

void BaseHttpRequestHandler::doDELETE()
{
    sendError(HttpStatus::NotImplemented, utils::formatMessage("Unsupported method %1", {method}));
}

void BaseHttpRequestHandler::doPATCH()
{
    sendError(HttpStatus::NotImplemented, utils::formatMessage("Unsupported method %1", {method}));
}

void BaseHttpRequestHandler::doHEAD()
{
    sendError(HttpStatus::NotImplemented, utils::formatMessage("Unsupported method %1", {method}));
}

void BaseHttpRequestHandler::doOPTIONS()
{
    sendError(HttpStatus::NotImplemented, utils::formatMessage("Unsupported method %1", {method}));
}

void BaseHttpRequestHandler::doTRACE()
{
    sendError(HttpStatus::NotImplemented, utils::formatMessage("Unsupported method %1", {method}));
}

void BaseHttpRequestHandler::doCONNECT()
{
    sendError(HttpStatus::NotImplemented, utils::formatMessage("Unsupported method %1", {method}));
}

bool BaseHttpRequestHandler::sendError(HttpStatus status, const string &message)
{
    string shortMessage, longMessage;
    bool ok = toMessage(status, &shortMessage, &longMessage);
    if (!ok) {
        shortMessage = longMessage = "???";
    }
    if (!message.empty()) {
        longMessage = message;
    }
    logError(status, shortMessage, longMessage);
    sendCommandLine(status, shortMessage);
    sendHeader("Server", serverName());
    sendHeader("Date", dateTimeString());
    string body;
    if (status >= 200 && status != HttpStatus::NoContent && status != HttpStatus::ResetContent
        && status != HttpStatus::NotModified) {
        const string &html = errorMessage(status, shortMessage, longMessage);
        body = html;
        sendHeader("Content-Length", utils::number(static_cast<long long>(body.size())));
        sendHeader("Content-Type", errorMessageContentType());
    }
    if (!endHeader()) {
        return false;
    }
    if (utils::toUpper(method) != "HEAD" && !body.empty()) {
        return request->sendall(body) == body.size();
    }
    return true;
}

bool BaseHttpRequestHandler::sendResponse(HttpStatus status, const string &message)
{
    string shortMessage, longMessage;
    bool ok = toMessage(status, &shortMessage, &longMessage);
    if (!ok) {
        shortMessage = longMessage = "???";
    }
    if (!message.empty()) {
        longMessage = message;
    }
    logRequest(status, 0);
    sendCommandLine(status, shortMessage);
    sendHeader(string("Server"), serverName());
    sendHeader(string("Date"), dateTimeString());
    return true;
}

string BaseHttpRequestHandler::errorMessage(HttpStatus status, const string &shortMessage, const string &longMessage)
{
    return utils::formatMessage(DEFAULT_ERROR_MESSAGE, {utils::number(static_cast<int>(status)), shortMessage, longMessage});
}

string BaseHttpRequestHandler::errorMessageContentType()
{
    return DEFAULT_ERROR_CONTENT_TYPE;
}

void BaseHttpRequestHandler::sendCommandLine(HttpStatus status, const string &shortMessage)
{
    string versionStr;
    if (serverVersion == Http1_0 || version == Http1_0) {
        versionStr = "HTTP/1.0";
    } else {
        versionStr = "HTTP/1.1";
    }
    const string &firstLine =
            utils::formatMessage("%1 %2 %3\r\n", {versionStr, utils::number(static_cast<int>(status)), shortMessage});
    headers.insert(headers.begin(), firstLine);
}

void BaseHttpRequestHandler::sendHeader(const string &name, const string &value)
{
    const string &line = name + ": " + value + "\r\n";
    headers.push_back(line);
    if (utils::toLower(name) == string("transfer-encoding") && utils::toLower(value) == string("chunked")) {
        closeConnection = Yes;
    } else if (utils::toLower(name) == string("connection")) {
        if (utils::toLower(value) == "keep-alive" && closeConnection != Yes) {
            closeConnection = No;
        } else {
            closeConnection = Yes;
        }
    }
}

bool BaseHttpRequestHandler::endHeader()
{
    if (closeConnection == Maybe) {
        closeConnection = No;
        headers.push_back(string("Connection: keep-alive\r\n"));
    }
    headers.push_back("\r\n");
    const string &data = utils::join(headers, string());
    headers.clear();
    return request->sendall(data) == data.size();
}

shared_ptr<FileLike> BaseHttpRequestHandler::bodyAsFile(bool processEncoding)
{
    int64_t contentLength = getContentLength();

    shared_ptr<FileLike> bodyFile;
    if (contentLength >= 0) {
        if (contentLength >= INT_MAX || (maxBodySize >= 0 && contentLength > maxBodySize)) {
            closeConnection = Yes;
            sendError(HttpStatus::RequestEntityTooLarge);
            return shared_ptr<FileLike>();
        } else {
            if (body.size() > contentLength) {
                ngWarning() << "request body got too much bytes.";
                bodyFile = FileLike::bytes(body);
            } else if (body.size() < contentLength) {
                bodyFile = make_shared<PlainBodyFile>(contentLength, body, request);
            } else {
                bodyFile = FileLike::bytes(body);
            }
        }
    } else {  // if (contentLength < 0) without `Content-Length` header.
        const string &transferEncodingHeader = header("Transfer-Encoding");
        bool isChunked = (utils::toLower(transferEncodingHeader) == string("chunked"));
        if (isChunked && processEncoding) {
            removeHeader("Transfer-Encoding");
            bodyFile = make_shared<ChunkedBodyFile>(maxBodySize, body, request);
        } else {
            // if the client does not send content length, it mean no content.
            // this is not the same as client side.
            bodyFile = FileLike::bytes(string());
        }
    }
    body.clear();

    if (processEncoding) {
        const string &contentEncodingHeader = header("Content-Encoding");
        const string &transferEncodingHeader = header("Transfer-Encoding");
#ifdef QTNG_HAVE_ZLIB
        if (utils::toLower(contentEncodingHeader) == string("gzip")
            || utils::toLower(contentEncodingHeader) == string("deflate")) {
            removeHeader("Content-Encoding");
            bodyFile = make_shared<GzipFile>(bodyFile, GzipFile::Decompress);
        } else if (utils::toLower(transferEncodingHeader) == string("gzip")
                   || utils::toLower(transferEncodingHeader) == string("deflate")) {
            removeHeader("Transfer-Encoding");
            bodyFile = make_shared<GzipFile>(bodyFile, GzipFile::Decompress);
        } else if (utils::toLower(transferEncodingHeader) == string("qt")) {
            ngWarning() << "unsupported qt transfer encoding.";
            closeConnection = Yes;
            return shared_ptr<FileLike>();
        } else
#endif
                if (!contentEncodingHeader.empty() || !transferEncodingHeader.empty()) {
            ngWarning() << "unsupported content encoding." << contentEncodingHeader << transferEncodingHeader;
            closeConnection = Yes;
        }
    }
    return bodyFile;
}

bool BaseHttpRequestHandler::readBody()
{
    shared_ptr<FileLike> bodyFile = bodyAsFile();
    if (!bodyFile) {
        return false;
    }
    bool ok;
    body = bodyFile->readall(&ok);
    return ok;
}

bool BaseHttpRequestHandler::switchToWebSocket()
{
    if (this->method != "GET") {
        return false;
    }
    const string &upgradeHeader = header(UpgradeHeader);
    const string &connectionHeader = header(ConnectionHeader);
    if (utils::toLower(upgradeHeader) != "websocket" || utils::toLower(connectionHeader) != "upgrade") {
        return false;
    }

    const string &itsKey = header("Sec-WebSocket-Key");
    const string &itsVersion = header("Sec-WebSocket-Version");
    if (itsKey.empty() || itsVersion != "13") {
        return false;
    }

    const string uuid("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    const string &t = itsKey + uuid;
    const string &myKey = utils::bytesToBase64(MessageDigest::hash(t, MessageDigest::Sha1));

    sendResponse(HttpStatus::SwitchProtocol);
    sendHeader(UpgradeHeader, "websocket");
    sendHeader(ConnectionHeader, "Upgrade");
    sendHeader("Sec-WebSocket-Accept", myKey);

    // it's the responsibility of caller to call endHeader().
    return true;
}

vector<string> BaseHttpRequestHandler::webSocketProtocols()
{
    const vector<string> &lines = multiHeader("Sec-WebSocket-Protocol");
    vector<string> result;
    for (const string &line : lines) {
        const vector<string> &protocols = utils::split(line, ',');
        for (const string &protocol : protocols) {
            const string &t = utils::trimmed(protocol);
            if (!t.empty()) {
                result.push_back(t);
            }
        }
    }
    return result;
}

string BaseHttpRequestHandler::serverName()
{
    return "QtNetworkNg";
}

string BaseHttpRequestHandler::dateTimeString()
{
    return toHttpDate(utils::DateTime::currentDateTimeUtc());
}

void BaseHttpRequestHandler::logRequest(HttpStatus status, int bodySize)
{
    const string msg = utils::formatMessage("%1 -- %2 %3", {request->peerAddress().toString(), utils::DateTime::currentDateTimeUtc().toString("%Y-%m-%dT%H:%M:%SZ"), utils::formatMessage("%1 %2 %3 %4", {method, path, utils::number(static_cast<int>(status)), utils::number(bodySize)})});
    printf("%s\n", msg.c_str());
}

void BaseHttpRequestHandler::logError(HttpStatus status, const string &shortMessage, const string &)
{
    const string msg = utils::formatMessage("%1 -- %2 %3", {request->peerAddress().toString(), utils::DateTime::currentDateTimeUtc().toString("%Y-%m-%dT%H:%M:%SZ"), utils::formatMessage("%1 %2 %3 %4", {method, path, utils::number(static_cast<int>(status)), shortMessage})});
    printf("%s\n", msg.c_str());
}

shared_ptr<FileLike> StaticHttpRequestHandler::serveStaticFiles(const PosixPath &dir, const string &subPath)
{
    utils::Url url(subPath);
    const pair<string, string> joined = safeJoinPath(dir.path(), url.path());
    PosixPath fileInfo(joined.first);
#ifdef DEBUG_HTTP_PROTOCOL
    ngDebug() << "serve path" << subPath << "from" << fileInfo.path();
#endif
    if (!fileInfo.exists() && !loadMissingFile(fileInfo)) {
        sendError(HttpStatus::NotFound, "File not found");
        return shared_ptr<FileLike>();
    }

    if (fileInfo.isDir()) {
        const string &p = url.path();
        if (!utils::endsWith(p, "/")) {
            url.setPath(p + "/");
            sendResponse(HttpStatus::MovedPermanently);
            sendHeader("Location", url.toString());
            endHeader();
            return shared_ptr<FileLike>();
        } else {
            PosixPath indexDir(fileInfo.path());
            const PosixPath &t = getIndexFile(indexDir);
            if (t.isFile()) {
                fileInfo = t;
            } else if (enableDirectoryListing) {
                return listDirectory(indexDir, p);
            } else {
                sendError(HttpStatus::NotFound, "File Not Found");
                return shared_ptr<FileLike>();
            }
        }
    }

    string contentType;

#ifdef NG_OS_ANDROID
    const string &ext = utils::toLower(fileInfo.completeSuffix());
    if (ext == "txt") {
        contentType = "text/plain";
    } else if (ext == "html" || ext == "htm") {
        contentType = "text/html";
    } else if (ext == "js") {
        contentType = "application/javascript";
    } else if (ext == "css") {
        contentType = "text/css";
    } else {
        contentType = "application/octet-stream";
    }
#else
    contentType = utils::mimeTypeForFileName(fileInfo.name());
    if (contentType.empty()) {
        contentType = "application/octet-stream";
    }
#endif
    shared_ptr<FileLike> f = FileLike::open(fileInfo.path());
    if (!f) {
        sendError(HttpStatus::NotFound, "File not found");
        return shared_ptr<FileLike>();
    }
    sendResponse(HttpStatus::OK);
    sendHeader(string("Content-Type"), contentType);
    sendHeader(string("Content-Length"), utils::number(static_cast<long long>(f->size())));
    sendHeader(string("Last-Modified"),
               utils::DateTime::fromMSecsSinceEpoch(fileInfo.lastModifiedMsecsSinceEpoch()).toHttpDate());
    if (!endHeader()) {
        return shared_ptr<FileLike>();
    }
    return f;
}

shared_ptr<FileLike> StaticHttpRequestHandler::listDirectory(const PosixPath &dir, const string &displayDir)
{
    const vector<PosixPath> list = dir.children();
    const string &title = "Directory listing for " + displayDir;
    vector<string> html;
    html.push_back("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://www.w3.org/TR/html4/strict.dtd\">");
    html.push_back("<html>\n<head>");
    html.push_back("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">");
    html.push_back(utils::formatMessage("<title>%1</title>\n</head>", {title}));
    html.push_back(utils::formatMessage("<body>\n<h1>%1</h1>", {title}));
    html.push_back("<hr>\n<ul>");
    for (const PosixPath &entry : list) {
        string name = entry.name();
        string link = entry.name();
        if (entry.isDir()) {
            name.push_back('/');
            link.push_back('/');
        }
        if (entry.isSymLink()) {
            name = entry.name() + "@";
        }
        const string &htmlName = utils::htmlEscape(name);
        const string &htmlLink = utils::Url::toEncodedComponent(link);
        html.push_back(utils::formatMessage("<li><a href=\"%1\">%2</a></li>", {htmlLink, htmlName}));
    }
    html.push_back("</ul>\n<hr>\n</body>\n</html>");
    const string &data = utils::join(html, "\n");
    sendResponse(HttpStatus::OK);
    sendHeader(string("Content-Type"), string("text/html; charset=utf-8"));
    sendHeader(string("Content-Length"), utils::number(static_cast<long long>(data.size())));
    if (!endHeader()) {
        return shared_ptr<FileLike>();
    }
    return FileLike::bytes(data);
}

bool StaticHttpRequestHandler::loadMissingFile(const PosixPath &)
{
    return false;
}

PosixPath StaticHttpRequestHandler::getIndexFile(const PosixPath &dir)
{
    PosixPath indexHtml = dir / "index.html";
    if (indexHtml.exists()) {
        return indexHtml;
    }
    PosixPath indexHtm = dir / "index.htm";
    if (indexHtm.exists()) {
        return indexHtm;
    }
    return PosixPath();
}

void SimpleHttpRequestHandler::doGET()
{
    shared_ptr<FileLike> f = serveStaticFiles(rootDir, path);
    if (f) {
        if (!sendfile(f, request)) {
            request->close();
        }
        f->close();
    }
}

void SimpleHttpRequestHandler::doHEAD()
{
    shared_ptr<FileLike> f = serveStaticFiles(rootDir, path);
    if (f) {
        f->close();
    }
}

}  // namespace qtng
