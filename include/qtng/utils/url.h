#ifndef QTNG_UTILS_URL_H
#define QTNG_UTILS_URL_H

#include <map>
#include <string>
#include <vector>

namespace qtng {
namespace utils {

class UrlQuery
{
public:
    UrlQuery();
    explicit UrlQuery(const std::string &query);
    void setQuery(const std::string &query);
    std::string query() const;
    void addQueryItem(const std::string &key, const std::string &value);
    std::string queryItemValue(const std::string &key) const;
    bool hasQueryItem(const std::string &key) const;
    void clear() { queryItems.clear(); }
    std::string toString() const { return query(); }
    std::map<std::string, std::string> items() const { return queryItems; }
private:
    std::map<std::string, std::string> queryItems;
};

class Url
{
public:
    Url();
    explicit Url(const std::string &url);

    bool isValid() const { return valid; }
    std::string toString() const;

    std::string scheme() const { return urlScheme; }
    std::string host() const { return urlHost; }
    int port() const { return urlPort; }
    std::string path() const { return urlPath; }
    std::string fragment() const { return urlFragment; }
    std::string userName() const { return urlUserName; }
    std::string password() const { return urlPassword; }
    std::string query() const;

    void setScheme(const std::string &scheme) { urlScheme = scheme; valid = true; }
    void setHost(const std::string &host) { urlHost = host; valid = true; }
    void setPort(int port) { urlPort = port; valid = true; }
    void setPath(const std::string &path) { urlPath = path; valid = true; }
    void setFragment(const std::string &fragment) { urlFragment = fragment; valid = true; }
    void setUserName(const std::string &userName) { urlUserName = userName; valid = true; }
    void setPassword(const std::string &password) { urlPassword = password; valid = true; }
    void setQuery(const std::string &query);
    void setUrl(const std::string &url);

    UrlQuery queryItems() const { return UrlQuery(query()); }
    static std::string fromEncodedComponent(const std::string &component);
    static std::string toEncodedComponent(const std::string &component);

private:
    void parse(const std::string &url);
    std::string urlScheme;
    std::string urlHost;
    int urlPort;
    std::string urlPath;
    std::string urlQuery;
    std::string urlFragment;
    std::string urlUserName;
    std::string urlPassword;
    bool valid;
};

inline bool operator<(const Url &a, const Url &b)
{
    return a.toString() < b.toString();
}

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_URL_H
