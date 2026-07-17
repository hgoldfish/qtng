#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "qtng/http_utils.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.http")

namespace qtng {

bool toMessage(HttpStatus status, string *shortMessage, string *longMessage)
{
    switch (status) {
    case Continue:
        *shortMessage = "Continue";
        if (longMessage)
            *longMessage = "Request received, please continue";
        return true;
    case SwitchProtocol:
        *shortMessage = "Switching Protocols";
        if (longMessage)
            *longMessage = "Switching to new protocol; obey Upgrade header";
        return true;
    case Processing:
        *shortMessage = "Processing";
        if (longMessage)
            *longMessage = "Processing";
        return true;
    case OK:
        *shortMessage = "OK";
        if (longMessage)
            *longMessage = "Request fulfilled, document follows";
        return true;
    case Created:
        *shortMessage = "Created";
        if (longMessage)
            *longMessage = "Document created, URL follows";
        return true;
    case Accepted:
        *shortMessage = "Accepted";
        if (longMessage)
            *longMessage = "Request accepted, processing continues off-line";
        return true;
    case NonAuthoritative:
        *shortMessage = "Non-Authoritative Information";
        if (longMessage)
            *longMessage = "Request fulfilled from cache";
        return true;
    case NoContent:
        *shortMessage = "No Content";
        if (longMessage)
            *longMessage = "Request fulfilled, nothing follows";
        return true;
    case ResetContent:
        *shortMessage = "Reset Content";
        if (longMessage)
            *longMessage = "Clear input form for further input";
        return true;
    case PartialContent:
        *shortMessage = "Partial Content";
        if (longMessage)
            *longMessage = "Partial content follows";
        return true;
    case MultiStatus:
        *shortMessage = "Multi-Status";
        if (longMessage)
            *longMessage = "Multi-Status";
        return true;
    case AlreadyReported:
        *shortMessage = "Already Reported";
        if (longMessage)
            *longMessage = "Already Reported";
        return true;
    case IMUsed:
        *shortMessage = "IM Used";
        if (longMessage)
            *longMessage = "IM Used";
        return true;
    case MultipleChoices:
        *shortMessage = "Multiple Choices";
        if (longMessage)
            *longMessage = "Object has several resources -- see URI list";
        return true;
    case MovedPermanently:
        *shortMessage = "Moved Permanently";
        if (longMessage)
            *longMessage = "Object moved permanently -- see URI list";
        return true;
    case Found:
        *shortMessage = "Found";
        if (longMessage)
            *longMessage = "Object moved temporarily -- see URI list";
        return true;
    case SeeOther:
        *shortMessage = "See Other";
        if (longMessage)
            *longMessage = "Object moved -- see Method and URL list";
        return true;
    case NotModified:
        *shortMessage = "Not Modified";
        if (longMessage)
            *longMessage = "Document has not changed since given time";
        return true;
    case UseProxy:
        *shortMessage = "Use Proxy";
        if (longMessage)
            *longMessage = "You must use proxy specified in Location to access this resource";
        return true;
    case TemporaryRedirect:
        *shortMessage = "Temporary Redirect";
        if (longMessage)
            *longMessage = "Object moved temporarily -- see URI list";
        return true;
    case PermanentRedirect:
        *shortMessage = "Permanent Redirect";
        if (longMessage)
            *longMessage = "Object moved temporarily -- see URI list";
        return true;
    case BadRequest:
        *shortMessage = "Bad Request";
        if (longMessage)
            *longMessage = "Bad request syntax or unsupported method";
        return true;
    case Unauthorized:
        *shortMessage = "Unauthorized";
        if (longMessage)
            *longMessage = "No permission -- see authorization schemes";
        return true;
    case PaymentRequired:
        *shortMessage = "Payment Required";
        if (longMessage)
            *longMessage = "No payment -- see charging schemes";
        return true;
    case Forbidden:
        *shortMessage = "Forbidden";
        if (longMessage)
            *longMessage = "Request forbidden -- authorization will not help";
        return true;
    case NotFound:
        *shortMessage = "Not Found";
        if (longMessage)
            *longMessage = "Nothing matches the given URI";
        return true;
    case MethodNotAllowed:
        *shortMessage = "Method Not Allowed";
        if (longMessage)
            *longMessage = "Specified method is invalid for this resource";
        return true;
    case NotAcceptable:
        *shortMessage = "Not Acceptable";
        if (longMessage)
            *longMessage = "URI not available in preferred format";
        return true;
    case ProxyAuthenticationRequired:
        *shortMessage = "Proxy Authentication Required";
        if (longMessage)
            *longMessage = "You must authenticate with this proxy before proceeding";
        return true;
    case RequestTimeout:
        *shortMessage = "Request Timeout";
        if (longMessage)
            *longMessage = "Request timed out; try again later";
        return true;
    case Conflict:
        *shortMessage = "Conflict";
        if (longMessage)
            *longMessage = "Request conflict";
        return true;
    case Gone:
        *shortMessage = "Gone";
        if (longMessage)
            *longMessage = "URI no longer exists and has been permanently removed";
        return true;
    case LengthRequired:
        *shortMessage = "Length Required";
        if (longMessage)
            *longMessage = "Client must specify Content-Length";
        return true;
    case PreconditionFailed:
        *shortMessage = "Precondition Failed";
        if (longMessage)
            *longMessage = "Precondition in headers is false";
        return true;
    case RequestEntityTooLarge:
        *shortMessage = "Request Entity Too Large";
        if (longMessage)
            *longMessage = "Entity is too large";
        return true;
    case RequestURITooLong:
        *shortMessage = "Request-URI Too Long";
        if (longMessage)
            *longMessage = "URI is too long";
        return true;
    case UnsupportedMediaType:
        *shortMessage = "Unsupported Media Type";
        if (longMessage)
            *longMessage = "Entity body in unsupported format";
        return true;
    case RequestedRangeNotSatisfiable:
        *shortMessage = "Requested Range Not Satisfiable";
        if (longMessage)
            *longMessage = "Cannot satisfy request range";
        return true;
    case ExpectationFailed:
        *shortMessage = "Expectation Failed";
        if (longMessage)
            *longMessage = "Expect condition could not be satisfied";
        return true;
    case ImaTeapot:
        *shortMessage = "I'm A Teapot";
        if (longMessage)
            *longMessage = "Maybe be short and stout";
        return true;
    case UnprocessableEntity:
        *shortMessage = "Unprocessable Entity";
        if (longMessage)
            *longMessage = "Unprocessable Entity";
        return true;
    case Locked:
        *shortMessage = "Locked";
        if (longMessage)
            *longMessage = "Locked";
        return true;
    case FailedDependency:
        *shortMessage = "Failed Dependency";
        if (longMessage)
            *longMessage = "Failed Dependency";
        return true;
    case UpgradeRequired:
        *shortMessage = "Upgrade Required";
        if (longMessage)
            *longMessage = "Upgrade Required";
        return true;
    case PreconditionRequired:
        *shortMessage = "Precondition Required";
        if (longMessage)
            *longMessage = "The origin server requires the request to be conditional";
        return true;
    case TooManyRequests:
        *shortMessage = "Too Many Requests";
        if (longMessage)
            *longMessage = 
                    "The user has sent too many requests in a given amount of time (\"rate limiting\"";
        return true;
    case RequestHeaderFieldsTooLarge:
        *shortMessage = "Request Header Fields Too Large";
        if (longMessage)
            *longMessage = 
                    "The server is unwilling to process the request because its header fields are too large";
        return true;
    case InternalServerError:
        *shortMessage = "Internal Server Error";
        if (longMessage)
            *longMessage = "Server got itself in trouble";
        return true;
    case NotImplemented:
        *shortMessage = "Not Implemented";
        if (longMessage)
            *longMessage = "Server does not support this operation";
        return true;
    case BadGateway:
        *shortMessage = "Bad Gateway";
        if (longMessage)
            *longMessage = "Invalid responses from another server/proxy";
        return true;
    case ServiceUnavailable:
        *shortMessage = "Service Unavailable";
        if (longMessage)
            *longMessage = "The server cannot process the request due to a high load";
        return true;
    case GatewayTimeout:
        *shortMessage = "Gateway Timeout";
        if (longMessage)
            *longMessage = "The gateway server did not receive a timely response";
        return true;
    case HTTPVersionNotSupported:
        *shortMessage = "HTTP Version Not Supported";
        if (longMessage)
            *longMessage = "Cannot fulfill request";
        return true;
    case VariantAlsoNegotiates:
        *shortMessage = "Variant Also Negotiates";
        if (longMessage)
            *longMessage = "Variant Also Negotiates";
        return true;
    case InsufficientStorage:
        *shortMessage = "Insufficient Storage";
        if (longMessage)
            *longMessage = "Insufficient Storage";
        return true;
    case LoopDetected:
        *shortMessage = "Loop Detected";
        if (longMessage)
            *longMessage = "Loop Detected";
        return true;
    case NotExtended:
        *shortMessage = "Not Extended";
        if (longMessage)
            *longMessage = "Not Extended";
        return true;
    case NetworkAuthenticationRequired:
        *shortMessage = "Network Authentication Required";
        if (longMessage)
            *longMessage = "The client needs to authenticate to gain network access";
        return true;
    }
    return false;
}

// Fast month string to int conversion. This code
// assumes that the Month name is correct and that
// the string is at least three chars long.
static int name_to_month(const char *month_str)
{
    switch (month_str[0]) {
    case 'J':
        switch (month_str[1]) {
        case 'a':
            return 1;
        case 'u':
            switch (month_str[2]) {
            case 'n':
                return 6;
            case 'l':
                return 7;
            }
        }
        break;
    case 'F':
        return 2;
    case 'M':
        switch (month_str[2]) {
        case 'r':
            return 3;
        case 'y':
            return 5;
        }
        break;
    case 'A':
        switch (month_str[1]) {
        case 'p':
            return 4;
        case 'u':
            return 8;
        }
        break;
    case 'O':
        return 10;
    case 'S':
        return 9;
    case 'N':
        return 11;
    case 'D':
        return 12;
    }

    return 0;
}

utils::DateTime fromHttpDate(const string &value)
{
    const int pos = static_cast<int>(value.find(','));
    if (pos == 3) {
        char month_name[4] = {};
        int day = 0, year = 0, hour = 0, minute = 0, second = 0;
        if (sscanf(value.c_str(), "%*3s, %d %3s %d %d:%d:%d 'GMT'", &day, month_name, &year, &hour, &minute, &second) == 6) {
            return utils::DateTime::fromUtc(year, name_to_month(month_name), day, hour, minute, second);
        }
    } else if (pos > 3) {
        const string sansWeekday = value.substr(static_cast<size_t>(pos + 2));
        int day = 0, year = 0, hour = 0, minute = 0, second = 0;
        char month_name[4] = {};
        if (sscanf(sansWeekday.c_str(), "%d-%3s-%d %d:%d:%d 'GMT'", &day, month_name, &year, &hour, &minute, &second) == 6) {
            if (year < 100) {
                year += (year < 70) ? 2000 : 1900;
            }
            return utils::DateTime::fromUtc(year, name_to_month(month_name), day, hour, minute, second);
        }
    } else if (pos == -1) {
        int month = 0, day = 0, hour = 0, minute = 0, second = 0, year = 0;
        char month_name[4] = {};
        if (sscanf(value.c_str(), "%*3s %3s %d %d:%d:%d %d", month_name, &day, &hour, &minute, &second, &year) == 6) {
            return utils::DateTime::fromUtc(year, name_to_month(month_name), day, hour, minute, second);
        }
    }
    return utils::DateTime();
}

string toHttpDate(const utils::DateTime &dt)
{
    return dt.toHttpDate();
}

static vector<string> knownHeaders = {
    "Content-Type",
    "Content-Length",
    "Content-Encoding",
    "Transfer-Encoding",
    "Location",
    "Last-Modified",
    "Cookie",
    "Set-Cookie",
    "Content-Disposition",
    "Server",
    "User-Agent",
    "Accept",
    "Accept-Language",
    "Accept-Encoding",
    "DNT",
    "Connection",
    "Pragma",
    "Cache-Control",
    "Date",
    "Allow",
    "Vary",
    "X-Frame-Options",
    "MIME-Version",
    "Host",
};

string normalizeHeaderName(const string &headerName)
{
    for (const string &goodName : knownHeaders) {
        if (utils::equalsIgnoreCase(headerName, goodName)) {
            return goodName;
        }
    }
    return headerName;
}

string toString(KnownHeader knownHeader)
{
    switch (knownHeader) {
    case ContentTypeHeader:
        return "Content-Type";
    case ContentLengthHeader:
        return "Content-Length";
    case ContentEncodingHeader:
        return "Content-Encoding";
    case TransferEncodingHeader:
        return "Transfer-Encoding";
    case LocationHeader:
        return "Location";
    case LastModifiedHeader:
        return "Last-Modified";
    case CookieHeader:
        return "Cookie";
    case SetCookieHeader:
        return "Set-Cookie";
    case ContentDispositionHeader:
        return "Content-Disposition";
    case UserAgentHeader:
        return "User-Agent";
    case AcceptHeader:
        return "Accept";
    case AcceptLanguageHeader:
        return "Accept-Language";
    case AcceptEncodingHeader:
        return "Accept-Encoding";
    case PragmaHeader:
        return "Pragma";
    case CacheControlHeader:
        return "Cache-Control";
    case DateHeader:
        return "Date";
    case AllowHeader:
        return "Allow";
    case VaryHeader:
        return "Vary";
    case FrameOptionsHeader:
        return "X-Frame-Options";
    case MIMEVersionHeader:
        return "MIME-Version";
    case ServerHeader:
        return "Server";
    case ConnectionHeader:
        return "Connection";
    case UpgradeHeader:
        return "Upgrade";
    case HostHeader:
        return "Host";
    }
    return string();
}

string HeaderSplitter::nextLine(HeaderSplitter::Error *error)
{
    const int MaxLineLength = 1024 * 64;
    string line;
    bool expectingLineBreak = false;

    while (true) {
        if (buf.empty()) {
            buf = connection->recv(1024);
            if (buf.empty()) {
                *error = HeaderSplitter::ConnectionError;
                return string();
            }
        }
        int j = 0;
        for (; j < buf.size(); ++j) {
            char c = buf[j];
            if (c == '\n') {
                buf.erase(0, j + 1);
                *error = HeaderSplitter::NoError;
                return line;
            } else if (c == '\r') {
                if (expectingLineBreak) {
                    *error = HeaderSplitter::EncodingError;
                    return string();
                }
                expectingLineBreak = true;
            } else {
                if (expectingLineBreak) {
                    *error = HeaderSplitter::EncodingError;
                    return string();
                }
                line.push_back(c);
                if (line.size() > MaxLineLength) {
                    *error = HeaderSplitter::LineTooLong;
                    return string();
                }
            }
        }
        buf.clear();
    }
    *error = HeaderSplitter::ExhausedMaxLine;
    return string();
}

HttpHeader HeaderSplitter::nextHeader(Error *error)
{
    const string &line = nextLine(error);
    if (*error != HeaderSplitter::NoError) {
        return HttpHeader();
    }
    if (line.empty()) {
        *error = HeaderSplitter::NoError;
        return HttpHeader();
    }
    if (debugLevel > 2) {
        ngDebug() << "receiving data:" << line;
    }
    vector<string> headerParts = splitBytes(line, ':', 1);
    if (headerParts.size() != 2) {
        *error = HeaderSplitter::EncodingError;
        return HttpHeader();
    }
    string headerName = utils::trimmed(headerParts[0]);
    string headerValue = utils::trimmed(headerParts[1]);
    *error = HeaderSplitter::NoError;
    return HttpHeader(headerName, headerValue);
}

vector<HttpHeader> HeaderSplitter::headers(int maxHeaders, Error *error)
{
    vector<HttpHeader> headers;
    for (int i = 0; i < maxHeaders; ++i) {
        const HttpHeader &header = nextHeader(error);
        if (header.isValid()) {
            headers.push_back(header);
        } else {
            if (*error != HeaderSplitter::NoError) {
                return vector<HttpHeader>();
            } else {
                return headers;
            }
        }
    }
    *error = HeaderSplitter::ExhausedMaxLine;
    return vector<HttpHeader>();
}

vector<string> splitBytes(const string &bs, char sep, int maxSplit)
{
    vector<string> tokens;
    string token;
    for (int i = 0; i < bs.size(); ++i) {
        char c = bs[i];
        if (c == sep && (maxSplit < 0 || tokens.size() < maxSplit)) {
            tokens.push_back(token);
            token.clear();
        } else {
            token.push_back(c);
        }
    }
    if (!token.empty()) {
        tokens.push_back(token);
    }
    return tokens;
}

string ChunkedBlockReader::nextBlock(int64_t leftBytes, ChunkedBlockReader::Error *error)
{
    const int MaxLineLength = 6;  // ffff\r\n
    string numBytes;
    bool expectingLineBreak = false;
    while (buf.size() < MaxLineLength && buf.find('\n') == string::npos) {
        string buff(1024 * 64, '\0');
        int32_t readed = connection->read(&buff[0], buff.size());
        if (readed < 0) {
            *error = ChunkedBlockReader::ConnectionError;
            return string();
        }
        if (readed == 0) {
            break;
        }
        buf.append(buff.data(), readed);  // most server send the header at one tcp block.
    }
    if (buf.size() < 3) {  // 0\r\n
        *error = ChunkedBlockReader::ChunkedEncodingError;
        return string();
    }

    bool ok = false;
    for (int i = 0; i < buf.size() && i < MaxLineLength; ++i) {
        char c = buf[i];
        if (expectingLineBreak) {
            if (c == '\n') {
                buf.erase(0, i + 1);
                ok = true;
                break;
            } else {
                *error = ChunkedBlockReader::ChunkedEncodingError;
                return string();
            }
        } else {
            if (c == '\n') {
                *error = ChunkedBlockReader::ChunkedEncodingError;
                return string();
            } else if (c == '\r') {
                expectingLineBreak = true;
            } else {
                numBytes.push_back(c);
            }
        }
    }
    if (!ok) {
        *error = ChunkedBlockReader::ChunkedEncodingError;
        return string();
    }
    int32_t bytesToRead = 0;
    try {
        bytesToRead = static_cast<int32_t>(stoul(numBytes, nullptr, 16));
    } catch (...) {
        if (debugLevel > 0) {
            ngDebug() << "got invalid chunked bytes:" << numBytes;
        }
        *error = ChunkedBlockReader::ChunkedEncodingError;
        return string();
    }

    if (bytesToRead > leftBytes || bytesToRead < 0) {
        *error = ChunkedBlockReader::UnrewindableBodyError;
        return string();
    }

    while (buf.size() < bytesToRead + 2) {
        string buff(1024 * 64, '\0');
        int32_t readed = connection->read(&buff[0], buff.size());
        if (readed <= 0) {
            *error = ChunkedBlockReader::ConnectionError;
            return string();
        }
        buf.append(buff.data(), readed);
    }

    const string &result = buf.substr(0, bytesToRead);
    buf.erase(0, bytesToRead + 2);

    if (bytesToRead == 0 && !buf.empty() && debugLevel > 0) {
        ngDebug() << "bytesToRead == 0 but some bytes left.";
    }

    *error = ChunkedBlockReader::NoError;
    return result;
}

PlainBodyFile::PlainBodyFile(int64_t contentLength, const string &partialBody, shared_ptr<SocketLike> stream)
    : contentLength(contentLength)
    , stream(stream)
    , partialBody(partialBody)
    , count(0)
{
}

int32_t PlainBodyFile::read(char *data, int32_t size)
{
    if (!partialBody.empty()) {
        int32_t t = min<int32_t>(size, static_cast<int32_t>(partialBody.size()));
        memcpy(data, partialBody.data(), t);
        partialBody.erase(0, t);
        count += t;
        return t;
    }
    if (contentLength >= 0) {
        if (count >= contentLength) {
            return 0;
        }
        int64_t leftBytes = contentLength - count;
        int32_t bs = stream->recv(data, min<int64_t>(size, leftBytes));
        if (bs > 0) {
            count += bs;
        }
        return bs;
    } else {
        return stream->recv(data, size);
    }
}

ChunkedBodyFile::ChunkedBodyFile(int64_t maxBodySize, const string &partialBody, shared_ptr<FileLike> stream)
    : reader(stream, partialBody)
    , error(ChunkedBlockReader::NoError)
    , maxBodySize(maxBodySize)
    , count(0)
    , eof(false)
{
}

int32_t ChunkedBodyFile::read(char *data, int32_t size)
{
    while (buf.size() < size && !eof) {
        int64_t leftBytes;
        if (maxBodySize >= 0) {
            leftBytes = maxBodySize - count;
            if (leftBytes <= 0) {
                break;
            }
        } else {
            leftBytes = INT_MAX;
        }
        const string &block = reader.nextBlock(leftBytes, &error);
        if (error != ChunkedBlockReader::NoError) {
            return -1;
        }
        if (block.empty()) {
            eof = true;
            break;
        }
        count += block.size();
        buf.append(block);
    }
    int32_t bytesToRead = min<int32_t>(static_cast<int32_t>(buf.size()), size);
    memcpy(data, buf.data(), bytesToRead);
    buf.erase(0, bytesToRead);
    return bytesToRead;
}

ChunkedWriter::~ChunkedWriter()
{
    close();
}

int32_t ChunkedWriter::write(const char *data, int32_t size)
{
    if (!data) {
        return -1;
    }
    // the chunked block can not greater than 0xffff!
    int64_t sent = 0;
    while (sent < size) {
        int32_t blockSize = min<int32_t>(0xffff, size - sent);
        string buf;
        buf.reserve(blockSize + 8);
        buf.append(utils::number(blockSize, 16));
        buf.append("\r\n", 2);
        buf.append(data + sent, blockSize);
        buf.append("\r\n", 2);

        int32_t writtenBytes = stream->write(buf);
        if (writtenBytes != buf.size()) {
            return -1;
        }
        sent += blockSize;
    }
    return size;
}

void ChunkedWriter::close()
{
    stream->write("0\r\n\r\n", 5);
}

}  // namespace qtng
