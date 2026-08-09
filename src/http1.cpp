#include "qtng/private/http_protocol_p.h"
#include "qtng/private/http_p.h"
#include "qtng/http_utils.h"
#include "qtng/io_utils.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/url.h"
#include "qtng/utils/logging.h"
#include "qtng/coroutine.h"

using namespace std;

NG_LOGGER("qtng.http1");

namespace qtng {

namespace {

string urlResourcePath(const utils::Url &url)
{
    string path = url.path();
    if (!url.query().empty()) {
        path += '?' + url.query();
    }
    return path;
}

vector<string> splitWhitespace(const string &text)
{
    vector<string> parts;
    string current;
    for (char ch : text) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

string joinWithSeparator(char sep, const vector<string> &parts, size_t start = 0)
{
    string result;
    for (size_t i = start; i < parts.size(); ++i) {
        if (i > start) {
            result.push_back(sep);
        }
        result += parts[i];
    }
    return result;
}

string joinLines(const vector<string> &lines)
{
    string buf;
    buf.reserve(1024 * 4);
    for (const string &line : lines) {
        buf += line;
    }
    return buf;
}

RequestError *toRequestError(HeaderSplitter::Error error)
{
    switch (error) {
    case HeaderSplitter::ConnectionError:
        return new ConnectionError();
    case HeaderSplitter::EncodingError:
        return new InvalidHeader();
    case HeaderSplitter::ExhausedMaxLine:
        return new InvalidHeader();
    case HeaderSplitter::LineTooLong:
        return new InvalidHeader();
    default:
        return nullptr;
    }
}

class SendRequestBodyCoroutine : public Coroutine
{
public:
    SendRequestBodyCoroutine(Coroutine *parentCoroutine, shared_ptr<SocketLike> connection, shared_ptr<FileLike> body)
        : parentCoroutine(parentCoroutine)
        , connection(connection)
        , body(body)
    {
    }
    virtual void run() override
    {
        if (!sendfile(body, connection) && parentCoroutine) {
            parentCoroutine->kill(new CoroutineInterruptedException());
        }
    }
private:
    Coroutine *parentCoroutine;
    shared_ptr<SocketLike> connection;
    shared_ptr<FileLike> body;
};

}  // namespace

vector<HttpHeader> Http1Protocol::makeHeaders(HttpSessionPrivate *session, HttpRequest &request, const string &urlStr)
{
    const utils::Url url(urlStr);
    vector<HttpHeader> allHeaders = request.allHeaders();

    if (!request.hasHeader("Connection") && request.version() == Http1_1) {
        if (session->keepAlive) {
            allHeaders.insert(allHeaders.begin(), HttpHeader("Connection", string("keep-alive")));
        } else {
            allHeaders.insert(allHeaders.begin(), HttpHeader("Connection", string("close")));
        }
    }
    if (!request.hasHeader("Content-Length") && request.d->body) {
        int64_t requestBodySize = request.d->body->size();
        if (requestBodySize > 0) {
            allHeaders.insert(allHeaders.begin(),
                              HttpHeader("Content-Length", utils::number(static_cast<long long>(requestBodySize))));
        }
    }
    if (!request.hasHeader("User-Agent")) {
        if (request.userAgent().empty()) {
            allHeaders.insert(allHeaders.begin(), HttpHeader("User-Agent", session->defaultUserAgent));
        } else {
            allHeaders.insert(allHeaders.begin(), HttpHeader("User-Agent", request.userAgent()));
        }
    }
    if (!request.hasHeader("Host")) {
        string httpHost = url.host();
        if (url.port() != -1) {
            httpHost += ":" + utils::number(url.port());
        }
        allHeaders.insert(allHeaders.begin(), HttpHeader("Host", httpHost));
    }
    if (!request.hasHeader("Accept")) {
        allHeaders.push_back(HttpHeader("Accept", string("*/*")));
    }
    if (!request.hasHeader("Accept-Language")) {
        allHeaders.push_back(HttpHeader("Accept-Language", string("en-US,en;q=0.5")));
    }
    if (!request.hasHeader("Accept-Encoding")) {
#ifdef QTNG_HAVE_ZLIB
        allHeaders.push_back(HttpHeader("Accept-Encoding", string("gzip, deflate")));
#else
        allHeaders.push_back(HttpHeader("Accept-Encoding", string("identity")));
#endif
    }
    if (!request.d->cookies.empty() && !request.hasHeader("Cookie")) {
        string result;
        bool first = true;
        for (const HttpCookie &cookie : request.d->cookies) {
            if (!first)
                result += "; ";
            first = false;
            result += cookie.toRawForm(HttpCookie::NameAndValueOnly);
        }
        allHeaders.push_back(HttpHeader("Cookie", result));
    }
    return allHeaders;
}

void Http1Protocol::exchange(HttpSessionPrivate *session, HttpRequest &request, HttpResponse &response,
                             shared_ptr<SocketLike> connection, unique_ptr<ScopedLock<Semaphore>> &ptrLock)
{
    RequestError *error = nullptr;
    utils::Url &url = request.d->url;
    vector<HttpHeader> allHeaders = Http1Protocol::makeHeaders(session, request, url.toString());

    string versionBytes;
    if (request.d->version == HttpVersion::Http1_0) {
        versionBytes = "HTTP/1.0";
    } else if (request.d->version == HttpVersion::Http1_1) {
        versionBytes = "HTTP/1.1";
    } else {
        response.setError(new UnsupportedVersion());
        return;
    }

    vector<string> lines;
    string resourcePath = urlResourcePath(url);
    if (resourcePath.empty()) {
        resourcePath = "/";
    }
    const string &commandLine =
            utils::toUpper(request.d->method) + string(" ") + resourcePath + string(" ") + versionBytes + string("\r\n");
    lines.push_back(commandLine);
    for (size_t i = 0; i < allHeaders.size(); ++i) {
        const HttpHeader &header = allHeaders.at(i);
        lines.push_back(header.name + string(": ") + header.value + string("\r\n"));
    }
    lines.push_back(string("\r\n"));
    if (session->debugLevel > 0) {
        for (const string &line : lines) {
            ngDebug() << "sending headers:" << line;
        }
    }
    const string headerBytes = joinLines(lines);

    if (connection->sendall(headerBytes) != static_cast<int32_t>(headerBytes.size())) {
        response.setError(new ConnectionError());
        return;
    }

    HeaderSplitter headerSplitter(connection, session->debugLevel);
    HeaderSplitter::Error headerSplitterError;
    unique_ptr<Coroutine> sendingRequestBodyCoroutine(
            new SendRequestBodyCoroutine(Coroutine::current(), connection, request.d->body));
    if (request.d->body) {
        if (session->debugLevel > 0) {
            ngDebug() << "sending body:" << request.d->body->size();
        }
        sendingRequestBodyCoroutine->start();
        try {
            headerSplitter.buf = connection->recv(1024 * 8);
            if (sendingRequestBodyCoroutine->isRunning()) {
                sendingRequestBodyCoroutine->kill();
            }
            sendingRequestBodyCoroutine->join();
            sendingRequestBodyCoroutine.reset();
        } catch (CoroutineInterruptedException &) {
            if (session->debugLevel > 0) {
                ngDebug() << "the server terminated connection while sending body." << headerSplitter.buf.size();
            }
            sendingRequestBodyCoroutine->join();
            if (headerSplitter.buf.empty()) {
                response.setError(new ConnectionError());
                return;
            }
        } catch (...) {
            if (sendingRequestBodyCoroutine->isRunning()) {
                sendingRequestBodyCoroutine->kill();
            }
            sendingRequestBodyCoroutine->join();
            throw;
        }
    }

    const string &firstLine = headerSplitter.nextLine(&headerSplitterError);
    error = toRequestError(headerSplitterError);
    if (error != nullptr) {
        if (session->debugLevel > 0) {
            ngDebug() << "read http response header error:" << error->what();
        }
        response.setError(error);
        return;
    }
    vector<string> commands = splitWhitespace(firstLine);
    if (commands.size() < 3) {
        response.setError(new InvalidHeader());
        return;
    }
    if (commands.at(0) == "HTTP/1.0") {
        response.d->version = Http1_0;
    } else if (commands.at(0) == "HTTP/1.1") {
        response.d->version = Http1_1;
    } else {
        response.setError(new InvalidHeader());
        return;
    }
    bool ok;
    response.d->statusCode = utils::parseInt(commands.at(1), &ok);
    if (!ok) {
        response.setError(new InvalidHeader());
        return;
    }
    response.d->statusText = joinWithSeparator(' ', commands, 2);

    const int MaxHeaders = 64;
    vector<HttpHeader> headers = headerSplitter.headers(MaxHeaders, &headerSplitterError);
    if (headerSplitterError != HeaderSplitter::NoError) {
        response.setError(toRequestError(headerSplitterError));
        return;
    }
    response.setHeaders(headers);
    if (session->debugLevel > 0) {
        for (const HttpHeader &header : headers) {
            ngDebug() << "receiving header:" << header.name << header.value;
        }
    }

    if (session->managingCookies && response.hasHeader("Set-Cookie")) {
        for (const string &value : response.multiHeader("Set-Cookie")) {
            const vector<HttpCookie> &cookies = HttpCookie::parseCookies(value);
            if (session->debugLevel > 0 && !cookies.empty()) {
                ngDebug() << "receiving cookie:" << cookies[0].toRawForm();
            }
            response.d->cookies.insert(response.d->cookies.end(), cookies.begin(), cookies.end());
        }
        session->cookieJar.setCookiesFromUrl(response.d->cookies, response.d->url.toString());
    }

    response.d->body = headerSplitter.buf;
    response.d->stream = connection;
    if (!request.streamResponse() && response.d->statusCode != HttpStatus::NoContent) {
        if (utils::toUpper(request.method()) == "HEAD") {
            response.d->consumed = true;
            response.d->body.clear();
        } else {
            const string &body = response.body();
            if (response.d->error) {
                return;
            }
            if (session->debugLevel == 1 && !body.empty()) {
                ngDebug() << "receiving body:" << body.size();
            } else if (session->debugLevel > 1 && !body.empty()) {
                ngDebug() << "receiving body:" << body;
            }
            if (ptrLock && connection->isValid() && response.statusCode() >= 200
                && utils::equalsIgnoreCase(response.header(KnownHeader::ConnectionHeader), "keep-alive")
                && session->keepAlive) {
                session->recycle(response.d->url.toString(), connection);
            }
        }
        response.d->stream.reset();
    }

    if (response.d->statusCode >= 400) {
        response.setError(new HTTPError(response.d->statusCode));
    } else {
        const string rm = utils::toUpper(request.method());
        if ((rm == "GET" || rm == "HEAD" || rm == "OPTIONS") && session->cacheManager && !request.streamResponse()) {
            bool doCache = true;
            const string &requestHeader = utils::toLower(request.header(KnownHeader::CacheControlHeader));
            if (requestHeader.find("no-cache") != string::npos || requestHeader.find("no-store") != string::npos) {
                doCache = false;
            } else {
                const string &responseHeader = utils::toLower(response.header(KnownHeader::CacheControlHeader));
                if (responseHeader.find("public") != string::npos || responseHeader.find("private") != string::npos) {
                    doCache = true;
                } else if (responseHeader.find("no-cache") != string::npos
                           || responseHeader.find("no-store") != string::npos) {
                    doCache = false;
                } else {
                    doCache = false;
                }
            }
            if (doCache) {
                session->cacheManager->addResponse(response);
            }
        }
    }
}

#ifndef QTNG_NO_HTTP3
void Http3Protocol::exchange(HttpSessionPrivate *, HttpRequest &, HttpResponse &response, shared_ptr<SocketLike>,
                             unique_ptr<ScopedLock<Semaphore>> &)
{
    response.setError(new UnsupportedVersion());
}
#endif

}  // namespace qtng
