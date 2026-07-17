#ifndef QTNG_HTTP_COOKIE_H
#define QTNG_HTTP_COOKIE_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <utility>
#include "qtng/utils/platform.h"
#include "qtng/utils/datetime.h"

namespace qtng {

class HttpCookiePrivate;
class HttpCookie
{
public:
    enum RawForm { NameAndValueOnly, Full };
    enum SameSite { Default, None, Lax, Strict };
public:
    explicit HttpCookie(const std::string &name = std::string(), const std::string &value = std::string());
    HttpCookie(const HttpCookie &other);
    ~HttpCookie();
#ifdef Q_COMPILER_RVALUE_REFS
    HttpCookie &operator=(HttpCookie &&other) noexcept
    {
        swap(other);
        return *this;
    }
#endif
    HttpCookie &operator=(const HttpCookie &other);
    void swap(HttpCookie &other) noexcept { std::swap(d, other.d); }
    bool operator==(const HttpCookie &other) const;
    inline bool operator!=(const HttpCookie &other) const { return !(*this == other); }
public:
    bool isSecure() const;
    void setSecure(bool enable);
    bool isHttpOnly() const;
    void setHttpOnly(bool enable);
    SameSite sameSitePolicy() const;
    void setSameSitePolicy(SameSite sameSite);
    bool isSessionCookie() const;
    qtng::utils::DateTime expirationDate() const;
    void setExpirationDate(const qtng::utils::DateTime &date);
    std::string domain() const;
    void setDomain(const std::string &domain);
    std::string path() const;
    void setPath(const std::string &path);
    std::string name() const;
    void setName(const std::string &cookieName);
    std::string value() const;
    void setValue(const std::string &value);
public:
    std::string toRawForm(RawForm form = Full) const;
    bool hasSameIdentifier(const HttpCookie &other) const;
    void normalize(const std::string &url);
public:
    static std::vector<HttpCookie> parseCookies(const std::string &cookieString);
private:
    std::shared_ptr<HttpCookiePrivate> d;
    friend class HttpCookiePrivate;
};

class HttpCookieJarPrivate;
class HttpCookieJar
{
public:
    HttpCookieJar();
    virtual ~HttpCookieJar();

    virtual std::vector<HttpCookie> cookiesForUrl(const std::string &url) const;
    virtual bool setCookiesFromUrl(const std::vector<HttpCookie> &cookieList, const std::string &url);

    virtual bool insertCookie(const HttpCookie &cookie);
    virtual bool updateCookie(const HttpCookie &cookie);
    virtual bool deleteCookie(const HttpCookie &cookie);
protected:
    std::vector<HttpCookie> allCookies() const;
    void setAllCookies(const std::vector<HttpCookie> &cookieList);
    virtual bool validateCookie(const HttpCookie &cookie, const std::string &url) const;
private:
    NG_DECLARE_PRIVATE(HttpCookieJar)
    NG_DISABLE_COPY(HttpCookieJar)
    HttpCookieJarPrivate * const d_ptr;
};

}  // namespace qtng

#ifndef NG_NO_DEBUG_STREAM
#include <ostream>
#include "qtng/utils/platform.h"
std::ostream &operator<<(std::ostream &out, const qtng::HttpCookie &cookie);
#endif

#endif  // QTNG_HTTP_COOKIE_H
