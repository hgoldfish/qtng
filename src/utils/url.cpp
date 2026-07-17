#include <cctype>
#include <iomanip>
#include <sstream>

#include "qtng/utils/url.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {
namespace utils {

static bool isHex(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

static int hexValue(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    return 10 + ch - 'A';
}

string Url::fromEncodedComponent(const string &component)
{
    string result;
    for (size_t i = 0; i < component.size(); ++i) {
        if (component[i] == '%' && i + 2 < component.size() && isHex(component[i + 1])
                && isHex(component[i + 2])) {
            char decoded = static_cast<char>(hexValue(component[i + 1]) * 16 + hexValue(component[i + 2]));
            result.push_back(decoded);
            i += 2;
        } else if (component[i] == '+') {
            result.push_back(' ');
        } else {
            result.push_back(component[i]);
        }
    }
    return result;
}

string Url::toEncodedComponent(const string &component)
{
    ostringstream oss;
    oss.setf(ios::uppercase);
    for (unsigned char ch : component) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/') {
            oss << static_cast<char>(ch);
        } else {
            oss << '%' << hex << setw(2) << setfill('0') << static_cast<int>(ch) << dec;
        }
    }
    return oss.str();
}

UrlQuery::UrlQuery() { }

UrlQuery::UrlQuery(const string &query)
{
    setQuery(query);
}

void UrlQuery::setQuery(const string &query)
{
    queryItems.clear();
    string q = query;
    if (!q.empty() && q[0] == '?') {
        q.erase(0, 1);
    }
    for (const string &pair : split(q, '&')) {
        size_t pos = pair.find('=');
        if (pos == string::npos) {
            queryItems[Url::fromEncodedComponent(pair)] = string();
        } else {
            queryItems[Url::fromEncodedComponent(pair.substr(0, pos))] =
                    Url::fromEncodedComponent(pair.substr(pos + 1));
        }
    }
}

string UrlQuery::query() const
{
    ostringstream oss;
    bool first = true;
    for (const auto &item : queryItems) {
        if (!first) {
            oss << '&';
        }
        first = false;
        oss << Url::toEncodedComponent(item.first) << '=' << Url::toEncodedComponent(item.second);
    }
    return oss.str();
}

void UrlQuery::addQueryItem(const string &key, const string &value)
{
    queryItems[key] = value;
}

string UrlQuery::queryItemValue(const string &key) const
{
    auto it = queryItems.find(key);
    if (it == queryItems.end()) {
        return string();
    }
    return it->second;
}

bool UrlQuery::hasQueryItem(const string &key) const
{
    return queryItems.find(key) != queryItems.end();
}

Url::Url()
    : urlPort(-1)
    , valid(false)
{
}

Url::Url(const string &url)
    : urlPort(-1)
    , valid(false)
{
    parse(url);
}

void Url::setQuery(const string &query)
{
    urlQuery = query;
    if (!urlQuery.empty() && urlQuery[0] == '?') {
        urlQuery.erase(0, 1);
    }
    valid = true;
}

void Url::setUrl(const string &url)
{
    parse(url);
}

string Url::query() const
{
    return urlQuery;
}

void Url::parse(const string &url)
{
    *this = Url();
    if (url.empty()) {
        return;
    }
    size_t pos = 0;
    size_t schemeEnd = url.find("://");
    if (schemeEnd != string::npos) {
        urlScheme = url.substr(0, schemeEnd);
        pos = schemeEnd + 3;
    }
    size_t fragmentPos = url.find('#', pos);
    size_t queryPos = url.find('?', pos);
    size_t pathStart = pos;
    size_t authorityEnd = url.size();
    if (fragmentPos != string::npos) {
        urlFragment = url.substr(fragmentPos + 1);
        authorityEnd = fragmentPos;
    }
    if (queryPos != string::npos && queryPos < authorityEnd) {
        urlQuery = url.substr(queryPos + 1, authorityEnd - queryPos - 1);
        authorityEnd = queryPos;
    }
    size_t slashPos = url.find('/', pos);
    if (slashPos != string::npos && slashPos < authorityEnd) {
        pathStart = slashPos;
    } else {
        pathStart = authorityEnd;
    }
    string authority = url.substr(pos, pathStart - pos);
    urlPath = url.substr(pathStart, authorityEnd - pathStart);
    if (urlPath.empty()) {
        urlPath = "/";
    }
    size_t atPos = authority.find('@');
    if (atPos != string::npos) {
        string userInfo = authority.substr(0, atPos);
        size_t colon = userInfo.find(':');
        if (colon == string::npos) {
            urlUserName = userInfo;
        } else {
            urlUserName = userInfo.substr(0, colon);
            urlPassword = userInfo.substr(colon + 1);
        }
        authority = authority.substr(atPos + 1);
    }
    if (authority.empty()) {
        valid = !urlScheme.empty() || !urlPath.empty();
        return;
    }
    if (authority[0] == '[') {
        size_t endBracket = authority.find(']');
        if (endBracket != string::npos) {
            urlHost = authority.substr(1, endBracket - 1);
            if (endBracket + 1 < authority.size() && authority[endBracket + 1] == ':') {
                urlPort = stoi(authority.substr(endBracket + 2));
            }
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon != string::npos) {
            urlHost = authority.substr(0, colon);
            urlPort = stoi(authority.substr(colon + 1));
        } else {
            urlHost = authority;
        }
    }
    valid = true;
}

string Url::toString() const
{
    if (!valid) {
        return string();
    }
    ostringstream oss;
    if (!urlScheme.empty()) {
        oss << urlScheme << "://";
    }
    if (!urlUserName.empty() || !urlPassword.empty()) {
        oss << urlUserName;
        if (!urlPassword.empty()) {
            oss << ':' << urlPassword;
        }
        oss << '@';
    }
    if (!urlHost.empty()) {
        if (urlHost.find(':') != string::npos) {
            oss << '[' << urlHost << ']';
        } else {
            oss << urlHost;
        }
    }
    if (urlPort >= 0) {
        oss << ':' << urlPort;
    }
    if (!urlPath.empty()) {
        oss << urlPath;
    } else if (!urlScheme.empty() && !urlHost.empty()) {
        oss << '/';
    }
    if (!urlQuery.empty()) {
        oss << '?' << urlQuery;
    }
    if (!urlFragment.empty()) {
        oss << '#' << urlFragment;
    }
    return oss.str();
}

}  // namespace utils
}  // namespace qtng
