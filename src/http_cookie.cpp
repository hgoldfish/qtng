#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "qtng/hostaddress.h"
#include "qtng/http_cookie.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/url.h"

using namespace std;

namespace qtng {

class HttpCookiePrivate
{
public:
    HttpCookiePrivate();
    static vector<HttpCookie> parseSetCookieHeaderLine(const string &cookieString);
public:
    utils::DateTime expirationDate;
    string domain;
    string path;
    string comment;
    string name;
    string value;
    HttpCookie::SameSite sameSite;
    bool secure;
    bool httpOnly;
};

class HttpCookieJarPrivate
{
public:
    vector<HttpCookie> allCookies;
};

HttpCookiePrivate::HttpCookiePrivate()
    : sameSite(HttpCookie::SameSite::Default)
    , secure(false)
    , httpOnly(false)
{
}

static inline bool isLWS(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int nextNonWhitespace(const string &text, int from)
{
    // RFC 2616 defines linear whitespace as:
    //  LWS = [CRLF] 1*( SP | HT )
    // We ignore the fact that CRLF must come as a pair at this point
    // It's an invalid HTTP header if that happens.
    while (from < text.size()) {
        if (isLWS(text[from])) {
            ++from;
        } else {
            return from;  // non-whitespace
        }
    }

    // reached the end
    return text.size();
}

HttpCookie::HttpCookie(const string &name, const string &value)
    : d(new HttpCookiePrivate())
{
    

    d->name = name;
    d->value = value;
}

HttpCookie::HttpCookie(const HttpCookie &other)
    : d(other.d)
{
}

HttpCookie::~HttpCookie()
{
    d = nullptr;
}

HttpCookie &HttpCookie::operator=(const HttpCookie &other)
{
    d = other.d;
    return *this;
}

bool HttpCookie::operator==(const HttpCookie &other) const
{
    if (d == other.d) {
        return true;
    }
    return d->name == other.d->name && d->value == other.d->value
            && d->expirationDate == other.d->expirationDate && d->domain == other.d->domain
            && d->path == other.d->path && d->secure == other.d->secure && d->comment == other.d->comment
            && d->sameSite == other.d->sameSite;
}

bool HttpCookie::hasSameIdentifier(const HttpCookie &other) const
{
    return d->name == other.d->name && d->domain == other.d->domain && d->path == other.d->path;
}

bool HttpCookie::isSecure() const
{
    return d->secure;
}

void HttpCookie::setSecure(bool enable)
{
    d->secure = enable;
}

HttpCookie::SameSite HttpCookie::sameSitePolicy() const
{
    return d->sameSite;
}

void HttpCookie::setSameSitePolicy(HttpCookie::SameSite sameSite)
{
    d->sameSite = sameSite;
}

bool HttpCookie::isHttpOnly() const
{
    return d->httpOnly;
}

void HttpCookie::setHttpOnly(bool enable)
{
    d->httpOnly = enable;
}

bool HttpCookie::isSessionCookie() const
{
    return !d->expirationDate.isValid();
}

utils::DateTime HttpCookie::expirationDate() const
{
    return d->expirationDate;
}

void HttpCookie::setExpirationDate(const utils::DateTime &date)
{
    d->expirationDate = date;
}

string HttpCookie::domain() const
{
    return d->domain;
}

void HttpCookie::setDomain(const string &domain)
{
    d->domain = domain;
}

string HttpCookie::path() const
{
    return d->path;
}

void HttpCookie::setPath(const string &path)
{
    d->path = path;
}

string HttpCookie::name() const
{
    return d->name;
}

void HttpCookie::setName(const string &cookieName)
{
    d->name = cookieName;
}

string HttpCookie::value() const
{
    return d->value;
}

void HttpCookie::setValue(const string &value)
{
    d->value = value;
}

static pair<string, string> nextField(const string &text, int &position, bool isNameValue)
{
    // format is one of:
    //    (1)  token
    //    (2)  token = token
    //    (3)  token = quoted-string
    const int length = text.size();
    position = nextNonWhitespace(text, position);

    int semiColonPosition = text.find(';', position);
    if (semiColonPosition == string::npos)
        semiColonPosition = length;  // no ';' means take everything to end of string

    int equalsPosition = text.find('=', position);
    if (equalsPosition == string::npos || equalsPosition > semiColonPosition) {
        if (isNameValue)
            return make_pair(string(),
                             string());  //'=' is required for name-value-pair (RFC6265 section 5.2, rule 2)
        equalsPosition = semiColonPosition;  // no '=' means there is an attribute-name but no attribute-value
    }

    string first = utils::trimmed(text.substr(position, equalsPosition - position));
    string second;
    int secondLength = semiColonPosition - equalsPosition - 1;
    if (secondLength > 0)
        second = utils::trimmed(text.substr(equalsPosition + 1, secondLength));

    position = semiColonPosition;
    return make_pair(first, second);
}

namespace {
string sameSiteToRawString(HttpCookie::SameSite samesite)
{
    switch (samesite) {
    case HttpCookie::SameSite::None:
        return "None";
    case HttpCookie::SameSite::Lax:
        return "Lax";
    case HttpCookie::SameSite::Strict:
        return "Strict";
    case HttpCookie::SameSite::Default:
        break;
    }
    return string();
}

HttpCookie::SameSite sameSiteFromRawString(string str)
{
    str = utils::toLower(str);
    if (str == "none")
        return HttpCookie::SameSite::None;
    if (str == "lax")
        return HttpCookie::SameSite::Lax;
    if (str == "strict")
        return HttpCookie::SameSite::Strict;
    return HttpCookie::SameSite::Default;
}
}  // namespace

string HttpCookie::toRawForm(RawForm form) const
{
    string result;
    if (d->name.empty())
        return result;  // not a valid cookie

    result = d->name;
    result += '=';
    result += d->value;

    if (form == Full) {
        // same as above, but encoding everything back
        if (isSecure())
            result += "; secure";
        if (isHttpOnly())
            result += "; HttpOnly";
        if (d->sameSite != SameSite::Default) {
            result += "; SameSite=";
            result += sameSiteToRawString(d->sameSite);
        }
        if (!isSessionCookie()) {
            result += "; expires=";
            result += d->expirationDate.toHttpDate();
        }
        if (!d->domain.empty()) {
            result += "; domain=";
            if (utils::startsWith(d->domain, ".")) {
                result += '.';
                result += utils::toAce(d->domain.substr(1));
            } else {
                HostAddress hostAddr(d->domain);
                if (hostAddr.protocol() == HostAddress::IPv6Protocol) {
                    result += '[';
                    result += d->domain;
                    result += ']';
                } else {
                    result += utils::toAce(d->domain);
                }
            }
        }
        if (!d->path.empty()) {
            result += "; path=";
            result += d->path;
        }
    }
    return result;
}

static const char zones[] = "pst\0"  // -8
                            "pdt\0"
                            "mst\0"  // -7
                            "mdt\0"
                            "cst\0"  // -6
                            "cdt\0"
                            "est\0"  // -5
                            "edt\0"
                            "ast\0"  // -4
                            "nst\0"  // -3
                            "gmt\0"  // 0
                            "utc\0"
                            "bst\0"
                            "met\0"  // 1
                            "eet\0"  // 2
                            "jst\0"  // 9
                            "\0";
static const int zoneOffsets[] = { -8, -8, -7, -7, -6, -6, -5, -5, -4, -3, 0, 0, 0, 1, 2, 9 };

static const char months[] = "jan\0"
                             "feb\0"
                             "mar\0"
                             "apr\0"
                             "may\0"
                             "jun\0"
                             "jul\0"
                             "aug\0"
                             "sep\0"
                             "oct\0"
                             "nov\0"
                             "dec\0"
                             "\0";

static inline bool isNumber(char s)
{
    return s >= '0' && s <= '9';
}

static inline bool isTerminator(char c)
{
    return c == '\n' || c == '\r';
}

static inline bool isValueSeparator(char c)
{
    return isTerminator(c) || c == ';';
}

static inline bool isWhitespace(char c)
{
    return c == ' ' || c == '\t';
}

static bool checkStaticArray(int &val, const string &dateString, int at, const char *array, int size)
{
    if (dateString[at] < 'a' || dateString[at] > 'z')
        return false;
    if (val == -1 && dateString.size() >= at + 3) {
        int j = 0;
        int i = 0;
        while (i <= size) {
            const char *str = array + i;
            if (str[0] == dateString[at] && str[1] == dateString[at + 1] && str[2] == dateString[at + 2]) {
                val = j;
                return true;
            }
            i += int(strlen(str)) + 1;
            ++j;
        }
    }
    return false;
}

//#define PARSEDATESTRINGDEBUG

#define ADAY 1
#define AMONTH 2
#define AYEAR 4

/*
    Parse all the date formats that Firefox can.

    The official format is:
    expires=ddd(d)?, dd-MMM-yyyy hh:mm:ss GMT

    But browsers have been supporting a very wide range of date
    strings. To work on many sites we need to support more then
    just the official date format.

    For reference see Firefox's PR_ParseTimeStringToExplodedTime in
    prtime.c. The Firefox date parser is coded in a very complex way
    and is slightly over ~700 lines long.  While this implementation
    will be slightly slower for the non standard dates it is smaller,
    more readable, and maintainable.

    Or in their own words:
        "} // else what the hell is this."
*/
// Parse a time component of the form H:M[:S][.ms][ am|pm] beginning at
// dateString[at] (which must be a digit). On success fills hour/minute/second/
// msec and the pm flag, and returns the number of characters consumed. Returns
// 0 if no valid time starts at `at`. Mirrors qtnetworkng's QRegularExpression
//    (\d{1,2}):(\d{1,2})(:(\d{1,2})|)(\.(\d{1,3})|)((\s{0,}(am|pm))|)
static int parseTimeAt(const string &dateString, int at, int &hour, int &minute, int &second, int &msec, bool &pm)
{
    const int length = static_cast<int>(dateString.size());
    const int start = at;

    int h = 0;
    int hLen = 0;
    while (hLen < 2 && at + hLen < length && isNumber(dateString[at + hLen])) {
        h = h * 10 + (dateString[at + hLen] - '0');
        ++hLen;
    }
    if (hLen == 0)
        return 0;
    int p = at + hLen;
    if (p >= length || dateString[p] != ':')
        return 0;
    ++p;

    int m = 0;
    int mLen = 0;
    while (mLen < 2 && p + mLen < length && isNumber(dateString[p + mLen])) {
        m = m * 10 + (dateString[p + mLen] - '0');
        ++mLen;
    }
    if (mLen == 0)
        return 0;
    p += mLen;

    int s = 0;
    if (p < length && dateString[p] == ':') {
        ++p;
        int sLen = 0;
        while (sLen < 2 && p + sLen < length && isNumber(dateString[p + sLen])) {
            s = s * 10 + (dateString[p + sLen] - '0');
            ++sLen;
        }
        if (sLen == 0)
            return 0;
        p += sLen;
    }

    int ms = 0;
    if (p < length && dateString[p] == '.') {
        ++p;
        int msLen = 0;
        while (msLen < 3 && p + msLen < length && isNumber(dateString[p + msLen])) {
            ms = ms * 10 + (dateString[p + msLen] - '0');
            ++msLen;
        }
        if (msLen == 0)
            return 0;
        p += msLen;
    }

    int q = p;
    while (q < length && isWhitespace(dateString[q]))
        ++q;
    bool isPm = false;
    if (q + 1 < length
            && ((dateString[q] == 'a' && dateString[q + 1] == 'm')
                || (dateString[q] == 'p' && dateString[q + 1] == 'm'))) {
        isPm = (dateString[q] == 'p');
        p = q + 2;
    }

    hour = h;
    minute = m;
    second = s;
    msec = ms;
    pm = isPm;
    return p - start;
}

static utils::DateTime parseDateString(const string &dateString)
{
    int month = -1, day = -1, year = -1, zoneOffset = -1;
    int hour = 0, minute = 0, second = 0, msec = 0;
    bool pm = false;
    bool hasTime = false;
    int at = 0;
    const int length = static_cast<int>(dateString.size());
    while (at < length) {
        if (checkStaticArray(month, dateString, at, months, sizeof(months) - 1)) {
            ++month;
            at += 3;
            continue;
        }
        if (zoneOffset == -1 && checkStaticArray(zoneOffset, dateString, at, zones, sizeof(zones) - 1)) {
            int sign = (at >= 1 && dateString[at - 1] == '-') ? -1 : 1;
            zoneOffset = sign * zoneOffsets[zoneOffset] * 60 * 60;
            at += 3;
            continue;
        }
        // Numeric zone offset such as +0800, -08, +8. May appear after 'gmt'
        // (which sets zoneOffset to 0) to refine it, hence the -1 || 0 guard.
        if ((dateString[at] == '+' || dateString[at] == '-')
                && (zoneOffset == -1 || zoneOffset == 0)
                && (at == 0 || isWhitespace(dateString[at - 1]) || dateString[at - 1] == ','
                    || (at >= 3 && dateString[at - 3] == 'g' && dateString[at - 2] == 'm'
                        && dateString[at - 1] == 't'))) {
            int end = 1;
            while (end < 5 && at + end < length && isNumber(dateString[at + end]))
                ++end;
            const int digits = end - 1;
            int hours = 0;
            int minutes = 0;
            switch (digits) {
            case 4:
                minutes = atoi(dateString.substr(at + 3, 2).c_str());
                hours = atoi(dateString.substr(at + 1, 2).c_str());
                break;
            case 2:
                hours = atoi(dateString.substr(at + 1, 2).c_str());
                break;
            case 1:
                hours = atoi(dateString.substr(at + 1, 1).c_str());
                break;
            default:
                at += end;
                continue;
            }
            const int sign = dateString[at] == '-' ? -1 : 1;
            zoneOffset = sign * (minutes * 60 + hours * 60 * 60);
            at += end;
            continue;
        }
        // Time: H:M[:S][.ms][ am|pm] (1-2 digit components)
        if (isNumber(dateString[at]) && !hasTime && at + 1 < length
                && (dateString[at + 1] == ':' || isNumber(dateString[at + 1]))) {
            const int consumed = parseTimeAt(dateString, at, hour, minute, second, msec, pm);
            if (consumed > 0) {
                hasTime = true;
                at += consumed;
                continue;
            }
        }
        if (isNumber(dateString[at]) && year == -1 && at + 3 < length
                && isNumber(dateString[at + 1]) && isNumber(dateString[at + 2]) && isNumber(dateString[at + 3])) {
            year = atoi(dateString.substr(at, 4).c_str());
            at += 4;
            continue;
        }
        if (isNumber(dateString[at]) && day == -1) {
            day = atoi(dateString.substr(at, 2).c_str());
            at += 2;
            continue;
        }
        ++at;
    }
    if (month < 0 || day < 0 || year < 0) {
        return utils::DateTime();
    }
    if (pm && hour < 12)
        hour += 12;
    int y2k = year < 70 ? 2000 : (year < 100 ? 1900 : 0);
    utils::DateTime dateTime = utils::DateTime::fromUtc(year + y2k, month, day, hour, minute, second, msec);
    if (zoneOffset != -1) {
        dateTime = dateTime.addSecs(zoneOffset);
    }
    return dateTime;
}

vector<HttpCookie> HttpCookie::parseCookies(const string &cookieString)
{
    // cookieString can be a number of set-cookie header strings joined together
    // by \n, parse each line separately.
    vector<HttpCookie> cookies;
    vector<string> list = utils::split(cookieString, '\n');
    for (int a = 0; a < list.size(); a++) {
        auto lineCookies = HttpCookiePrivate::parseSetCookieHeaderLine(list[a]);
        cookies.insert(cookies.end(), lineCookies.begin(), lineCookies.end());
    }
    return cookies;
}

vector<HttpCookie> HttpCookiePrivate::parseSetCookieHeaderLine(const string &cookieString)
{
    // According to http://wp.netscape.com/newsref/std/cookie_spec.html,<
    // the Set-Cookie response header is of the format:
    //
    //   Set-Cookie: NAME=VALUE; expires=DATE; path=PATH; domain=DOMAIN_NAME; secure
    //
    // where only the NAME=VALUE part is mandatory
    //
    // We do not support RFC 2965 Set-Cookie2-style cookies

    vector<HttpCookie> result;
    const utils::DateTime now = utils::DateTime::currentDateTimeUtc();

    int position = 0;
    const int length = cookieString.size();
    while (position < length) {
        HttpCookie cookie;

        // The first part is always the "NAME=VALUE" part
        pair<string, string> field = nextField(cookieString, position, true);
        if (field.first.empty())
            // parsing error
            break;
        cookie.setName(field.first);
        cookie.setValue(field.second);

        position = nextNonWhitespace(cookieString, position);
        while (position < length) {
            switch (cookieString[position++]) {
            case ';':
                // new field in the cookie
                field = nextField(cookieString, position, false);
                field.first = utils::toLower(field.first);  // everything but the NAME=VALUE is case-insensitive

                if (field.first == "expires") {
                    position -= field.second.size();
                    int end;
                    for (end = position; end < length; ++end)
                        if (isValueSeparator(cookieString[end]))
                            break;

                    string dateString = utils::trimmed(cookieString.substr(position, end - position));
                    position = end;
                    utils::DateTime dt = parseDateString(utils::toLower(dateString));
                    if (dt.isValid())
                        cookie.setExpirationDate(dt);
                    // if unparsed, ignore the attribute but not the whole cookie (RFC6265 section 5.2.1)
                } else if (field.first == "domain") {
                    string rawDomain = field.second;
                    // empty domain should be ignored (RFC6265 section 5.2.3)
                    if (!rawDomain.empty()) {
                        string maybeLeadingDot;
                        if (utils::startsWith(rawDomain, ".")) {
                            maybeLeadingDot = ".";;
                            rawDomain = rawDomain.substr(1);
                        }

                        // IDN domains are required by RFC6265, accepting utf8 as well doesn't break any test cases.
                        string normalizedDomain = utils::fromAce(utils::toAce(rawDomain));
                        if (!normalizedDomain.empty()) {
                            cookie.setDomain(maybeLeadingDot + normalizedDomain);
                        } else {
                            // Normalization fails for malformed domains, e.g. "..example.org", reject the cookie now
                            // rather than accepting it but never sending it due to domain match failure, as the
                            // strict reading of RFC6265 would indicate.
                            return result;
                        }
                    }
                } else if (field.first == "max-age") {
                    bool ok = false;
                    int secs = utils::parseInt(field.second, &ok);
                    if (ok) {
                        if (secs <= 0) {
                            // earliest representable time (RFC6265 section 5.2.2)
                            cookie.setExpirationDate(utils::DateTime::fromMSecsSinceEpoch(0));
                        } else {
                            cookie.setExpirationDate(now.addSecs(secs));
                        }
                    }
                    // if unparsed, ignore the attribute but not the whole cookie (RFC6265 section 5.2.2)
                } else if (field.first == "path") {
                    if (utils::startsWith(field.second, "/")) {
                        // ### we should treat cookie paths as an octet sequence internally
                        // However RFC6265 says we should assume UTF-8 for presentation as a string
                        cookie.setPath(field.second);
                    } else {
                        // if the path doesn't start with '/' then set the default path (RFC6265 section 5.2.4)
                        // and also IETF test case path0030 which has valid and empty path in the same cookie
                        cookie.setPath(string());
                    }
                } else if (field.first == "secure") {
                    cookie.setSecure(true);
                } else if (field.first == "httponly") {
                    cookie.setHttpOnly(true);
                } else if (field.first == "samesite") {
                    cookie.setSameSitePolicy(sameSiteFromRawString(field.second));
                } else {
                    // ignore unknown fields in the cookie (RFC6265 section 5.2, rule 6)
                }

                position = nextNonWhitespace(cookieString, position);
            }
        }

        if (!cookie.name().empty())
            result.push_back(cookie);
    }

    return result;
}

void HttpCookie::normalize(const string &url)
{
    const utils::Url parsedUrl(url);
    if (d->path.empty()) {
        string pathAndFileName = parsedUrl.path();
        string defaultPath = pathAndFileName.substr(0, pathAndFileName.rfind('/') + 1);
        if (defaultPath.empty()) {
            defaultPath = "/";
        }
        d->path = defaultPath;
    }

    if (d->domain.empty()) {
        d->domain = parsedUrl.host();
    } else {
        HostAddress hostAddress(d->domain);
        if (hostAddress.protocol() != HostAddress::IPv4Protocol && hostAddress.protocol() != HostAddress::IPv6Protocol
            && !utils::startsWith(d->domain, ".")) {
            d->domain.insert(0, 1, '.');
        }
    }
}

HttpCookieJar::HttpCookieJar()
    : d_ptr(new HttpCookieJarPrivate())
{
}

HttpCookieJar::~HttpCookieJar()
{
    delete d_ptr;
}

vector<HttpCookie> HttpCookieJar::allCookies() const
{
    return d_func()->allCookies;
}

void HttpCookieJar::setAllCookies(const vector<HttpCookie> &cookieList)
{
    NG_D(HttpCookieJar);
    d->allCookies = cookieList;
}

static inline bool isParentPath(const string &path, const string &reference)
{
    if ((path.empty() && reference == string("/")) || utils::startsWith(path, reference)) {
        // The cookie-path and the request-path are identical.
        if (path.size() == reference.size())
            return true;
        // The cookie-path is a prefix of the request-path, and the last
        // character of the cookie-path is %x2F ("/").
        if (utils::endsWith(reference, "/"))
            return true;
        // The cookie-path is a prefix of the request-path, and the first
        // character of the request-path that is not included in the cookie-
        // path is a %x2F ("/") character.
        if (path.at(reference.size()) == '/')
            return true;
    }
    return false;
}

static inline bool isParentDomain(const string &domain, const string &reference)
{
    if (!utils::startsWith(reference, "."))
        return domain == reference;

    return utils::endsWith(domain, reference) || domain == reference.substr(1);
}

bool HttpCookieJar::setCookiesFromUrl(const vector<HttpCookie> &cookieList, const string &url)
{
    bool added = false;
    for (HttpCookie cookie : cookieList) {
        cookie.normalize(url);
        if (validateCookie(cookie, url) && insertCookie(cookie))
            added = true;
    }
    return added;
}

static bool qIsEffectiveTLD(const string &domain)
{
    // provide minimal checking by not accepting cookies on real TLDs
    return domain.find('.') == string::npos;
}

vector<HttpCookie> HttpCookieJar::cookiesForUrl(const string &url) const
{
    //     \b Warning! This is only a dumb implementation!
    //     It does NOT follow all of the recommendations from
    //     http://wp.netscape.com/newsref/std/cookie_spec.html
    //     It does not implement a very good cross-domain verification yet.

    NG_D(const HttpCookieJar);
    const utils::DateTime now = utils::DateTime::currentDateTimeUtc();
    vector<HttpCookie> result;
    bool isEncrypted = utils::Url(url).scheme() == string("https");

    // scan our cookies for something that matches
    for (vector<HttpCookie>::const_iterator it = d->allCookies.begin(), end = d->allCookies.end(); it != end; ++it) {
        if (!isParentDomain(utils::Url(url).host(), it->domain()))
            continue;
        if (!isParentPath(utils::Url(url).path(), it->path()))
            continue;
        if (!(*it).isSessionCookie() && (*it).expirationDate() < now)
            continue;
        if ((*it).isSecure() && !isEncrypted)
            continue;

        string domain = it->domain();
        if (utils::startsWith(domain, "."))
            domain = domain.substr(1);
        if (domain.find('.') == string::npos && utils::Url(url).host() != domain)
            continue;

        // insert this cookie into result, sorted by path
        vector<HttpCookie>::iterator insertIt = result.begin();
        while (insertIt != result.end()) {
            if (insertIt->path().size() < it->path().size()) {
                // insert here
                insertIt = result.insert(insertIt, *it);
                break;
            } else {
                ++insertIt;
            }
        }

        // this is the shortest path yet, just append
        if (insertIt == result.end())
            result.push_back(*it);
    }

    return result;
}

bool HttpCookieJar::insertCookie(const HttpCookie &cookie)
{
    NG_D(HttpCookieJar);
    const utils::DateTime now = utils::DateTime::currentDateTimeUtc();
    bool isDeletion = !cookie.isSessionCookie() && cookie.expirationDate() < now;

    deleteCookie(cookie);

    if (!isDeletion) {
        d->allCookies.push_back(cookie);
        return true;
    }
    return false;
}

bool HttpCookieJar::updateCookie(const HttpCookie &cookie)
{
    if (deleteCookie(cookie))
        return insertCookie(cookie);
    return false;
}

bool HttpCookieJar::deleteCookie(const HttpCookie &cookie)
{
    NG_D(HttpCookieJar);
    vector<HttpCookie>::iterator it;
    for (it = d->allCookies.begin(); it != d->allCookies.end(); ++it) {
        if (it->hasSameIdentifier(cookie)) {
            d->allCookies.erase(it);
            return true;
        }
    }
    return false;
}

bool HttpCookieJar::validateCookie(const HttpCookie &cookie, const string &url) const
{
    string domain = cookie.domain();
    const string host = utils::Url(url).host();
    if (!isParentDomain(domain, host) && !isParentDomain(host, domain))
        return false;  // not accepted

    if (utils::startsWith(domain, "."))
        domain = domain.substr(1);

    // We shouldn't reject if:
    // "[...] the domain-attribute is identical to the canonicalized request-host"
    // https://tools.ietf.org/html/rfc6265#section-5.3 step 5
    if (host == domain)
        return true;
    // the check for effective TLDs makes the "embedded dot" rule from RFC 2109 section 4.3.2
    // redundant; the "leading dot" rule has been relaxed anyway, see HttpCookie::normalize()
    // we remove the leading dot for this check if it's present
    // Normally defined in qtldurl_p.h, but uses fall-back in this file when topleveldomain isn't
    // configured:
    return !qIsEffectiveTLD(domain);
}

}  // namespace qtng

