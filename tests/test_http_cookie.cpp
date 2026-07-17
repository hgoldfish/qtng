using namespace std;

#include <catch2/catch_test_macros.hpp>

#include "qtng/http_cookie.h"
#include "qtng/utils/datetime.h"

using namespace qtng;
using namespace qtng::utils;


TEST_CASE("HttpCookie basic properties", "[http_cookie]")
{
    HttpCookie cookie("session", "abc123");
    REQUIRE(cookie.name() == "session");
    REQUIRE(cookie.value() == "abc123");
    REQUIRE_FALSE(cookie.isSecure());
    REQUIRE_FALSE(cookie.isHttpOnly());
    REQUIRE(cookie.isSessionCookie());
    REQUIRE(cookie.sameSitePolicy() == HttpCookie::Default);
}

TEST_CASE("HttpCookie setters", "[http_cookie]")
{
    HttpCookie cookie("a", "b");
    cookie.setSecure(true);
    cookie.setHttpOnly(true);
    cookie.setSameSitePolicy(HttpCookie::Strict);
    cookie.setDomain(".example.com");
    cookie.setPath("/app");
    cookie.setExpirationDate(utils::DateTime::fromSecsSinceEpoch(1700000000));

    REQUIRE(cookie.isSecure());
    REQUIRE(cookie.isHttpOnly());
    REQUIRE(cookie.sameSitePolicy() == HttpCookie::Strict);
    REQUIRE(cookie.domain() == ".example.com");
    REQUIRE(cookie.path() == "/app");
    REQUIRE_FALSE(cookie.isSessionCookie());
    REQUIRE(cookie.expirationDate().toSecsSinceEpoch() == 1700000000);
}

TEST_CASE("HttpCookie toRawForm", "[http_cookie]")
{
    HttpCookie cookie("name", "value");
    REQUIRE(cookie.toRawForm(HttpCookie::NameAndValueOnly) == "name=value");

    cookie.setSecure(true);
    cookie.setHttpOnly(true);
    cookie.setPath("/");
    string full = cookie.toRawForm(HttpCookie::Full);
    REQUIRE(full.find("name=value") != string::npos);
    REQUIRE(full.find("secure") != string::npos);
    REQUIRE(full.find("HttpOnly") != string::npos);
    REQUIRE(full.find("path=/") != string::npos);
}

TEST_CASE("HttpCookie parseCookies simple", "[http_cookie]")
{
    auto cookies = HttpCookie::parseCookies("session=abc");
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].name() == "session");
    REQUIRE(cookies[0].value() == "abc");
}

TEST_CASE("HttpCookie parseCookies with attributes", "[http_cookie]")
{
    const string header =
            "id=token; Path=/; Domain=example.com; Secure; HttpOnly; SameSite=Lax; Max-Age=3600";
    auto cookies = HttpCookie::parseCookies(header);
    REQUIRE(cookies.size() == 1);

    const HttpCookie &cookie = cookies[0];
    REQUIRE(cookie.name() == "id");
    REQUIRE(cookie.value() == "token");
    REQUIRE(cookie.path() == "/");
    REQUIRE(cookie.domain() == "example.com");
    REQUIRE(cookie.isSecure());
    REQUIRE(cookie.isHttpOnly());
    REQUIRE(cookie.sameSitePolicy() == HttpCookie::Lax);
    REQUIRE_FALSE(cookie.isSessionCookie());
}

TEST_CASE("HttpCookie parseCookies expires date", "[http_cookie]")
{
    const string header = "c=v; expires=Wed, 09 Jun 2021 10:18:14 GMT";
    auto cookies = HttpCookie::parseCookies(header);
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].expirationDate().isValid());
    REQUIRE(cookies[0].expirationDate().toString("%Y") == "2021");
    REQUIRE(cookies[0].expirationDate().toString("%H:%M:%S") == "10:18:14");
    REQUIRE(cookies[0].expirationDate().toHttpDate() == "Wed, 09 Jun 2021 10:18:14 GMT");
}

TEST_CASE("HttpCookie parseCookies expires date without time", "[http_cookie]")
{
    auto cookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 GMT");
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].expirationDate().isValid());
    REQUIRE(cookies[0].expirationDate().toString("%Y") == "2021");
    REQUIRE(cookies[0].expirationDate().toString("%H:%M:%S") == "00:00:00");
}

TEST_CASE("HttpCookie parseCookies expires with pm suffix", "[http_cookie]")
{
    auto cookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 10:18:14 PM GMT");
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].expirationDate().isValid());
    REQUIRE(cookies[0].expirationDate().toString("%H:%M:%S") == "22:18:14");

    auto amCookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 10:18:14 AM GMT");
    REQUIRE(amCookies[0].expirationDate().toString("%H:%M:%S") == "10:18:14");
}

TEST_CASE("HttpCookie parseCookies expires single-digit hour", "[http_cookie]")
{
    auto cookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 9:18:14 GMT");
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].expirationDate().isValid());
    REQUIRE(cookies[0].expirationDate().toString("%H:%M:%S") == "09:18:14");
}

TEST_CASE("HttpCookie parseCookies expires with milliseconds", "[http_cookie]")
{
    auto gmtCookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 10:18:14 GMT");
    auto msCookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 10:18:14.500 GMT");
    REQUIRE(msCookies.size() == 1);
    REQUIRE(msCookies[0].expirationDate().isValid());
    REQUIRE(msCookies[0].expirationDate().toMSecsSinceEpoch()
            == gmtCookies[0].expirationDate().toMSecsSinceEpoch() + 500);
    // milliseconds are below the second precision of the HTTP date format
    REQUIRE(msCookies[0].expirationDate().toHttpDate() == "Wed, 09 Jun 2021 10:18:14 GMT");
}

TEST_CASE("HttpCookie parseCookies expires numeric zone offset", "[http_cookie]")
{
    auto gmtCookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 10:18:14 GMT");

    // +0000 is equivalent to GMT (no shift)
    auto zeroCookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 10:18:14 +0000");
    REQUIRE(zeroCookies[0].expirationDate().toSecsSinceEpoch()
            == gmtCookies[0].expirationDate().toSecsSinceEpoch());

    // qtnetworkng applies numeric offsets by adding them to the UTC time, so
    // +0800 shifts the result forward by 8 hours relative to the GMT form.
    auto plusCookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 10:18:14 +0800");
    REQUIRE(plusCookies[0].expirationDate().toSecsSinceEpoch()
            == gmtCookies[0].expirationDate().toSecsSinceEpoch() + 8 * 3600);

    auto minusCookies = HttpCookie::parseCookies("c=v; expires=Wed, 09 Jun 2021 10:18:14 -0800");
    REQUIRE(minusCookies[0].expirationDate().toSecsSinceEpoch()
            == gmtCookies[0].expirationDate().toSecsSinceEpoch() - 8 * 3600);
}

TEST_CASE("HttpCookie parseCookies max-age zero", "[http_cookie]")
{
    auto cookies = HttpCookie::parseCookies("c=v; Max-Age=0");
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].expirationDate().toMSecsSinceEpoch() == 0);
}

TEST_CASE("HttpCookie parseCookies multiple lines", "[http_cookie]")
{
    auto cookies = HttpCookie::parseCookies("a=1\nb=2");
    REQUIRE(cookies.size() == 2);
    REQUIRE(cookies[0].name() == "a");
    REQUIRE(cookies[1].name() == "b");
}

TEST_CASE("HttpCookie hasSameIdentifier", "[http_cookie]")
{
    HttpCookie a("sid", "1");
    a.setDomain("example.com");
    a.setPath("/");

    HttpCookie b("sid", "2");
    b.setDomain("example.com");
    b.setPath("/");

    HttpCookie c("sid", "1");
    c.setDomain("other.com");
    c.setPath("/");

    REQUIRE(a.hasSameIdentifier(b));
    REQUIRE_FALSE(a.hasSameIdentifier(c));
}

TEST_CASE("HttpCookie equality", "[http_cookie]")
{
    HttpCookie a("n", "v");
    HttpCookie b("n", "v");
    HttpCookie c("n", "x");
    REQUIRE(a == b);
    REQUIRE(a != c);
}

TEST_CASE("HttpCookie normalize", "[http_cookie]")
{
    HttpCookie cookie("token", "x");
    cookie.normalize("https://www.example.com/app/page.html");
    REQUIRE(cookie.domain() == "www.example.com");
    REQUIRE(cookie.path() == "/app/");
}

TEST_CASE("HttpCookieJar insert and query", "[http_cookie]")
{
    HttpCookieJar jar;
    HttpCookie cookie("user", "alice");
    cookie.setPath("/");
    cookie.setDomain("example.com");
    REQUIRE(jar.insertCookie(cookie));

    auto cookies = jar.cookiesForUrl("http://example.com/home");
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].value() == "alice");
}

TEST_CASE("HttpCookieJar setCookiesFromUrl", "[http_cookie]")
{
    HttpCookieJar jar;
    vector<HttpCookie> list;
    HttpCookie cookie("lang", "en");
    list.push_back(cookie);
    REQUIRE(jar.setCookiesFromUrl(list, "http://example.com/"));

    auto cookies = jar.cookiesForUrl("http://example.com/page");
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].name() == "lang");
}

TEST_CASE("HttpCookieJar deleteCookie", "[http_cookie]")
{
    HttpCookieJar jar;
    HttpCookie cookie("x", "1");
    cookie.setDomain("example.com");
    cookie.setPath("/");
    jar.insertCookie(cookie);
    REQUIRE(jar.deleteCookie(cookie));
    REQUIRE(jar.cookiesForUrl("http://example.com/").empty());
}

TEST_CASE("HttpCookie empty name invalid raw form", "[http_cookie]")
{
    HttpCookie cookie("", "value");
    REQUIRE(cookie.toRawForm(HttpCookie::Full).empty());
}

TEST_CASE("HttpCookie SameSite serialization", "[http_cookie]")
{
    HttpCookie cookie("n", "v");
    cookie.setSameSitePolicy(HttpCookie::Strict);
    string raw = cookie.toRawForm(HttpCookie::Full);
    REQUIRE(raw.find("SameSite=Strict") != string::npos);
}

TEST_CASE("HttpCookie parseCookies ignores unknown attributes", "[http_cookie]")
{
    auto cookies = HttpCookie::parseCookies("a=1; unknown=1; Path=/");
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].path() == "/");
}

TEST_CASE("HttpCookieJar replace same identifier", "[http_cookie]")
{
    HttpCookieJar jar;
    HttpCookie first("sid", "old");
    first.setDomain("example.com");
    first.setPath("/");
    jar.insertCookie(first);

    HttpCookie second("sid", "new");
    second.setDomain("example.com");
    second.setPath("/");
    jar.insertCookie(second);

    auto cookies = jar.cookiesForUrl("http://example.com/");
    REQUIRE(cookies.size() == 1);
    REQUIRE(cookies[0].value() == "new");
}
