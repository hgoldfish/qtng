#include <catch2/catch_test_macros.hpp>

#include "qtng/utils/url.h"

using namespace std;

using namespace qtng::utils;

TEST_CASE("Url encoded component round-trip", "[url]")
{
    const string raw = "hello world/%+";
    string encoded = Url::toEncodedComponent(raw);
    REQUIRE(encoded == "hello%20world/%25%2B");
    REQUIRE(Url::fromEncodedComponent(encoded) == raw);
    REQUIRE(Url::fromEncodedComponent("a+b") == "a b");
}

TEST_CASE("UrlQuery parsing and serialization", "[url]")
{
    UrlQuery query("?foo=bar&empty=&spaced=hello%20world");
    REQUIRE(query.hasQueryItem("foo"));
    REQUIRE(query.queryItemValue("foo") == "bar");
    REQUIRE(query.hasQueryItem("empty"));
    REQUIRE(query.queryItemValue("empty").empty());
    REQUIRE(query.queryItemValue("spaced") == "hello world");
    REQUIRE_FALSE(query.hasQueryItem("missing"));

    query.addQueryItem("new", "value=with&chars");
    string serialized = query.query();
    REQUIRE(serialized.find("new=") != string::npos);

    UrlQuery restored(serialized);
    REQUIRE(restored.queryItemValue("foo") == "bar");
    REQUIRE(restored.queryItemValue("new") == "value=with&chars");

    query.clear();
    REQUIRE(query.query().empty());
}

TEST_CASE("Url parse http URLs", "[url]")
{
    SECTION("basic https")
    {
        Url url("https://example.com/path/to/page?x=1#frag");
        REQUIRE(url.isValid());
        REQUIRE(url.scheme() == "https");
        REQUIRE(url.host() == "example.com");
        REQUIRE(url.port() == -1);
        REQUIRE(url.path() == "/path/to/page");
        REQUIRE(url.query() == "x=1");
        REQUIRE(url.fragment() == "frag");
    }

    SECTION("with port and credentials")
    {
        Url url("http://user:pass@host.example:8080/api");
        REQUIRE(url.isValid());
        REQUIRE(url.userName() == "user");
        REQUIRE(url.password() == "pass");
        REQUIRE(url.host() == "host.example");
        REQUIRE(url.port() == 8080);
        REQUIRE(url.path() == "/api");
    }

    SECTION("IPv6 literal")
    {
        Url url("http://[2001:db8::1]:443/index");
        REQUIRE(url.isValid());
        REQUIRE(url.host() == "2001:db8::1");
        REQUIRE(url.port() == 443);
        REQUIRE(url.path() == "/index");
    }

    SECTION("path-only")
    {
        Url url("/relative/path");
        REQUIRE(url.isValid());
        REQUIRE(url.scheme().empty());
        REQUIRE(url.host().empty());
        REQUIRE(url.path() == "/relative/path");
    }

    SECTION("empty URL is invalid")
    {
        Url url("");
        REQUIRE_FALSE(url.isValid());
    }
}

TEST_CASE("Url toString round-trip", "[url]")
{
    const string original = "https://user:secret@[::1]:8443/dir/?q=1#top";
    Url url(original);
    REQUIRE(url.isValid());
    string rebuilt = url.toString();
    Url again(rebuilt);
    REQUIRE(again.isValid());
    REQUIRE(again.scheme() == url.scheme());
    REQUIRE(again.host() == url.host());
    REQUIRE(again.port() == url.port());
    REQUIRE(again.path() == url.path());
    REQUIRE(again.query() == url.query());
    REQUIRE(again.fragment() == url.fragment());
    REQUIRE(again.userName() == url.userName());
    REQUIRE(again.password() == url.password());
}

TEST_CASE("Url setters and query items", "[url]")
{
    Url url;
    url.setScheme("ftp");
    url.setHost("files.example");
    url.setPort(21);
    url.setPath("/pub");
    url.setQuery("mode=binary");
    url.setFragment("readme");
    REQUIRE(url.isValid());
    REQUIRE(url.toString().find("ftp://files.example:21/pub?mode=binary#readme") != string::npos);

    UrlQuery items = url.queryItems();
    REQUIRE(items.queryItemValue("mode") == "binary");
}

TEST_CASE("Url operator less-than", "[url]")
{
    Url a("http://a.example");
    Url b("http://b.example");
    REQUIRE(a < b);
}

TEST_CASE("Url encoded component edge cases", "[url]")
{
    REQUIRE(Url::toEncodedComponent("") == "");
    REQUIRE(Url::toEncodedComponent("abc-._~") == "abc-._~");
    REQUIRE(Url::fromEncodedComponent("100%") == "100%");
    REQUIRE(Url::fromEncodedComponent("%ZZ") == "%ZZ");
    REQUIRE(Url::fromEncodedComponent("%41") == "A");
}

TEST_CASE("UrlQuery key without value", "[url]")
{
    UrlQuery query("flag&key=value");
    REQUIRE(query.hasQueryItem("flag"));
    REQUIRE(query.queryItemValue("flag").empty());
    REQUIRE(query.queryItemValue("key") == "value");
}

TEST_CASE("UrlQuery duplicate keys last wins", "[url]")
{
    UrlQuery query("a=1&a=2");
    REQUIRE(query.queryItemValue("a") == "2");
}

TEST_CASE("Url setUrl reparses", "[url]")
{
    Url url("http://old.example/");
    url.setUrl("https://new.example:9000/path?q=1#frag");
    REQUIRE(url.scheme() == "https");
    REQUIRE(url.host() == "new.example");
    REQUIRE(url.port() == 9000);
    REQUIRE(url.path() == "/path");
    REQUIRE(url.query() == "q=1");
    REQUIRE(url.fragment() == "frag");
}

TEST_CASE("Url scheme-only is valid", "[url]")
{
    Url url("file:///tmp/data");
    REQUIRE(url.isValid());
    REQUIRE(url.scheme() == "file");
    REQUIRE(url.path() == "/tmp/data");
}

TEST_CASE("Url username without password", "[url]")
{
    Url url("http://user@host/");
    REQUIRE(url.userName() == "user");
    REQUIRE(url.password().empty());
}
