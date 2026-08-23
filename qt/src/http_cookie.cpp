#include <QtCore/qdebug.h>

#include "bridge/core_access.h"
#include "bridge/http_access.h"
#include "bridge/stream_bridge.h"
#include "http_cookie.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class HttpCookiePrivate : public QSharedData
{
public:
    qtng_core::HttpCookie core;

    static qtng_core::HttpCookie toCore(const HttpCookie &cookie)
    {
        return cookie.d->core;
    }

    static HttpCookie fromCore(const qtng_core::HttpCookie &cookie)
    {
        HttpCookie result;
        result.d->core = cookie;
        return result;
    }
};

HttpCookie::HttpCookie(const QByteArray &name, const QByteArray &value)
    : d(new HttpCookiePrivate)
{
    d->core.setName(toStdString(name));
    d->core.setValue(toStdString(value));
}

HttpCookie::HttpCookie(const HttpCookie &other)
    : d(other.d)
{
}

HttpCookie::~HttpCookie() = default;

HttpCookie &HttpCookie::operator=(const HttpCookie &other)
{
    d = other.d;
    return *this;
}

bool HttpCookie::operator==(const HttpCookie &other) const
{
    return d->core == other.d->core;
}

bool HttpCookie::isSecure() const
{
    return d->core.isSecure();
}

void HttpCookie::setSecure(bool enable)
{
    d->core.setSecure(enable);
}

bool HttpCookie::isHttpOnly() const
{
    return d->core.isHttpOnly();
}

void HttpCookie::setHttpOnly(bool enable)
{
    d->core.setHttpOnly(enable);
}

HttpCookie::SameSite HttpCookie::sameSitePolicy() const
{
    return static_cast<SameSite>(d->core.sameSitePolicy());
}

void HttpCookie::setSameSitePolicy(SameSite sameSite)
{
    d->core.setSameSitePolicy(static_cast<qtng_core::HttpCookie::SameSite>(sameSite));
}

bool HttpCookie::isSessionCookie() const
{
    return d->core.isSessionCookie();
}

QDateTime HttpCookie::expirationDate() const
{
    return toQDateTime(d->core.expirationDate());
}

void HttpCookie::setExpirationDate(const QDateTime &date)
{
    d->core.setExpirationDate(toCoreDateTime(date));
}

QString HttpCookie::domain() const
{
    return toQString(d->core.domain());
}

void HttpCookie::setDomain(const QString &domain)
{
    d->core.setDomain(toStdString(domain));
}

QString HttpCookie::path() const
{
    return toQString(d->core.path());
}

void HttpCookie::setPath(const QString &path)
{
    d->core.setPath(toStdString(path));
}

QByteArray HttpCookie::name() const
{
    return toQByteArray(d->core.name());
}

void HttpCookie::setName(const QByteArray &cookieName)
{
    d->core.setName(toStdString(cookieName));
}

QByteArray HttpCookie::value() const
{
    return toQByteArray(d->core.value());
}

void HttpCookie::setValue(const QByteArray &value)
{
    d->core.setValue(toStdString(value));
}

QByteArray HttpCookie::toRawForm(RawForm form) const
{
    return toQByteArray(d->core.toRawForm(static_cast<qtng_core::HttpCookie::RawForm>(form)));
}

bool HttpCookie::hasSameIdentifier(const HttpCookie &other) const
{
    return d->core.hasSameIdentifier(other.d->core);
}

void HttpCookie::normalize(const QUrl &url)
{
    d->core.normalize(toCoreUrl(url).toString());
}

QList<HttpCookie> HttpCookie::parseCookies(const QByteArray &cookieString)
{
    const vector<qtng_core::HttpCookie> cookies = qtng_core::HttpCookie::parseCookies(toStdString(cookieString));
    QList<HttpCookie> result;
    for (const qtng_core::HttpCookie &cookie : cookies) {
        result.append(HttpCookiePrivate::fromCore(cookie));
    }
    return result;
}

class HttpCookieJarPrivate
{
public:
    qtng_core::HttpCookieJar core;
    qtng_core::HttpCookieJar *externalCore = nullptr;

    qtng_core::HttpCookieJar &coreRef() { return externalCore ? *externalCore : core; }
    const qtng_core::HttpCookieJar &coreRef() const { return externalCore ? *externalCore : core; }

    static void bindToCore(HttpCookieJar *jar, qtng_core::HttpCookieJar *core)
    {
        if (jar) {
            jar->d_ptr->externalCore = core;
        }
    }
};

HttpCookieJar::HttpCookieJar()
    : d_ptr(new HttpCookieJarPrivate)
{
}

HttpCookieJar::~HttpCookieJar()
{
    delete d_ptr;
}

QList<HttpCookie> HttpCookieJar::cookiesForUrl(const QUrl &url) const
{
    const vector<qtng_core::HttpCookie> cookies = d_ptr->coreRef().cookiesForUrl(toCoreUrl(url).toString());
    QList<HttpCookie> result;
    for (const qtng_core::HttpCookie &cookie : cookies) {
        result.append(HttpCookiePrivate::fromCore(cookie));
    }
    return result;
}

bool HttpCookieJar::setCookiesFromUrl(const QList<HttpCookie> &cookieList, const QUrl &url)
{
    vector<qtng_core::HttpCookie> cookies;
    cookies.reserve(static_cast<size_t>(cookieList.size()));
    for (const HttpCookie &cookie : cookieList) {
        cookies.push_back(HttpCookiePrivate::toCore(cookie));
    }
    return d_ptr->coreRef().setCookiesFromUrl(cookies, toCoreUrl(url).toString());
}

bool HttpCookieJar::insertCookie(const HttpCookie &cookie)
{
    return d_ptr->coreRef().insertCookie(HttpCookiePrivate::toCore(cookie));
}

bool HttpCookieJar::updateCookie(const HttpCookie &cookie)
{
    return d_ptr->coreRef().updateCookie(HttpCookiePrivate::toCore(cookie));
}

bool HttpCookieJar::deleteCookie(const HttpCookie &cookie)
{
    return d_ptr->coreRef().deleteCookie(HttpCookiePrivate::toCore(cookie));
}

QList<HttpCookie> HttpCookieJar::allCookies() const
{
    const vector<qtng_core::HttpCookie> cookies = d_ptr->coreRef().allCookies();
    QList<HttpCookie> result;
    for (const qtng_core::HttpCookie &cookie : cookies) {
        result.append(HttpCookiePrivate::fromCore(cookie));
    }
    return result;
}

void HttpCookieJar::setAllCookies(const QList<HttpCookie> &cookieList)
{
    vector<qtng_core::HttpCookie> cookies;
    cookies.reserve(static_cast<size_t>(cookieList.size()));
    for (const HttpCookie &cookie : cookieList) {
        cookies.push_back(HttpCookiePrivate::toCore(cookie));
    }
    d_ptr->coreRef().setAllCookies(cookies);
}

bool HttpCookieJar::validateCookie(const HttpCookie &cookie, const QUrl &url) const
{
    const qtng_core::HttpCookie coreCookie = HttpCookiePrivate::toCore(cookie);
    return d_ptr->coreRef().validateCookie(coreCookie, toCoreUrl(url).toString());
}

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug debug, const HttpCookie &cookie)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "HttpCookie(" << cookie.name() << '=' << cookie.value() << ')';
    return debug;
}
#endif

}  // namespace QTNETWORKNG_NAMESPACE

namespace qtng_bridge {

void bindHttpCookieJarToCore(QTNETWORKNG_NAMESPACE::HttpCookieJar *jar, qtng_core::HttpCookieJar *core)
{
    QTNETWORKNG_NAMESPACE::HttpCookieJarPrivate::bindToCore(jar, core);
}

}  // namespace qtng_bridge
