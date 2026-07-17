#include <catch2/catch_test_macros.hpp>

#include "qtng/utils/string_utils.h"

using namespace std;

using namespace qtng::utils;

TEST_CASE("split", "[string_utils]")
{
    SECTION("single char separator")
    {
        auto parts = split("a,b,,c", ',');
        REQUIRE(parts.size() == 4);
        REQUIRE(parts[0] == "a");
        REQUIRE(parts[1] == "b");
        REQUIRE(parts[2].empty());
        REQUIRE(parts[3] == "c");
    }

    SECTION("string separator")
    {
        auto parts = split("one::two::three", "::");
        REQUIRE(parts.size() == 3);
        REQUIRE(parts[1] == "two");
    }

    SECTION("empty separator returns whole text")
    {
        auto parts = split("abc", "");
        REQUIRE(parts.size() == 1);
        REQUIRE(parts[0] == "abc");
    }
}

TEST_CASE("trimmed and case conversion", "[string_utils]")
{
    REQUIRE(trimmed("  \t hello \n ") == "hello");
    REQUIRE(toLower("AbC123") == "abc123");
    REQUIRE(toUpper("AbC123") == "ABC123");
}

TEST_CASE("startsWith and endsWith", "[string_utils]")
{
    REQUIRE(startsWith("prefix-suffix", "prefix"));
    REQUIRE_FALSE(startsWith("prefix", "prefix-suffix"));
    REQUIRE(endsWith("prefix-suffix", "suffix"));
    REQUIRE_FALSE(endsWith("suffix", "prefix-suffix"));
}

TEST_CASE("number and parseInt", "[string_utils]")
{
    REQUIRE(number(42) == "42");
    REQUIRE(number(9876543210LL) == "9876543210");
    REQUIRE(number(1.2345, 2) == "1.23");

    bool ok = false;
    REQUIRE(parseInt("123", &ok) == 123);
    REQUIRE(ok);
    ok = true;
    REQUIRE(parseInt("12a", &ok) == 0);
    REQUIRE_FALSE(ok);

    long long llValue = parseLongLong("-9223372036854775808", &ok);
    REQUIRE(ok);
    REQUIRE(llValue == -9223372036854775807LL - 1);
}

TEST_CASE("parseFloat and parseDouble", "[string_utils]")
{
    bool ok = false;
    REQUIRE(parseFloat("1.5", &ok) == 1.5f);
    REQUIRE(ok);
    ok = true;
    REQUIRE(parseFloat("1.5x", &ok) == 0.0f);
    REQUIRE_FALSE(ok);

    REQUIRE(parseDouble("-2.5", &ok) == -2.5);
    REQUIRE(ok);
    ok = true;
    REQUIRE(parseDouble("bad", &ok) == 0.0);
    REQUIRE_FALSE(ok);
}

TEST_CASE("join", "[string_utils]")
{
    vector<string> parts{"a", "b", "c"};
    REQUIRE(join(parts, ",") == "a,b,c");
    REQUIRE(join(parts, '-') == "a-b-c");
    REQUIRE(join(vector<string>{}, ",").empty());
}

TEST_CASE("equalsIgnoreCase", "[string_utils]")
{
    REQUIRE(equalsIgnoreCase("Hello", "hello"));
    REQUIRE_FALSE(equalsIgnoreCase("Hello", "hell"));
}

TEST_CASE("htmlEscape", "[string_utils]")
{
    REQUIRE(htmlEscape("a<b>&\"c") == "a&lt;b&gt;&amp;&quot;c");
    REQUIRE(htmlEscape("plain").empty() == false);
}

TEST_CASE("formatMessage", "[string_utils]")
{
    vector<string> args{"world", "42"};
    REQUIRE(formatMessage("hello %1", args) == "hello world");
    REQUIRE(formatMessage("n=%2 v=%1", args) == "n=42 v=world");
    REQUIRE(formatMessage("percent %% ok", args) == "percent % ok");
}

TEST_CASE("fromLatin1", "[string_utils]")
{
    const char data[] = {'\x00', 'A', '\x7f'};
    const string result = fromLatin1(data, 3);
    REQUIRE(result.size() == 3);
    REQUIRE(static_cast<unsigned char>(result[0]) == 0x00);
    REQUIRE(result[1] == 'A');
    REQUIRE(static_cast<unsigned char>(result[2]) == 0x7f);
    REQUIRE(fromLatin1(nullptr, 10).empty());
    REQUIRE(fromLatin1(data, 0).empty());
}

TEST_CASE("bytesToHex and bytesToBase64", "[string_utils]")
{
    const string raw = string("\x00\xff\x10", 3);
    REQUIRE(bytesToHex(raw) == "00ff10");

    REQUIRE(bytesToBase64("") == "");
    REQUIRE(bytesToBase64("f") == "Zg==");
    REQUIRE(bytesToBase64("fo") == "Zm8=");
    REQUIRE(bytesToBase64("foo") == "Zm9v");
    REQUIRE(bytesToBase64("foob") == "Zm9vYg==");
}

TEST_CASE("toAce and fromAce handle IDN via Punycode", "[string_utils]")
{
    SECTION("ASCII domains pass through unchanged")
    {
        REQUIRE(toAce("example.com") == "example.com");
        REQUIRE(fromAce("example.com") == "example.com");
        REQUIRE(toAce("sub.example.com") == "sub.example.com");
        // trailing dot preserved
        REQUIRE(toAce("example.com.") == "example.com.");
        REQUIRE(fromAce("example.com.") == "example.com.");
        REQUIRE(toAce("").empty());
        REQUIRE(fromAce("").empty());
    }

    SECTION("known Punycode values")
    {
        // RFC 3492 / Wikipedia classic example: bücher -> bcher-kva
        REQUIRE(toAce("bücher.com") == "xn--bcher-kva.com");
        REQUIRE(fromAce("xn--bcher-kva.com") == "bücher.com");
        // single non-ASCII character label
        REQUIRE(toAce("ü") == "xn--tda");
        REQUIRE(fromAce("xn--tda") == "ü");
    }

    SECTION("round-trip for various IDN labels")
    {
        const char *cases[] = {
            "中文.com",
            "例え.jp",
            "bücher.de",
            "αβγ.com",
            "München-Ost.de",
            "почта.рф",
            "マイクロソフト.com",
        };
        for (const char *raw : cases) {
            string ace = toAce(raw);
            REQUIRE_FALSE(ace.empty());
            REQUIRE(ace.find("xn--") != string::npos);
            REQUIRE(fromAce(ace) == string(raw));
        }
    }

    SECTION("ACE labels are not double-encoded")
    {
        // ACE labels should pass through toAce unchanged, not re-encoded
        REQUIRE(toAce("xn--bcher-kva.com") == "xn--bcher-kva.com");
        // xn-- prefix is recognized case-insensitively (body stays lowercase, decodes back to original)
        REQUIRE(fromAce("XN--bcher-kva.com") == "bücher.com");
        // Punycode basic code points are case-sensitive: Bcher-Kva decodes to Bücher (uppercase B preserved)
        REQUIRE(fromAce("xn--Bcher-Kva.com") == "Bücher.com");
    }

    SECTION("invalid input yields empty result")
    {
        // invalid UTF-8 (truncated multibyte sequence: 0x80 is a continuation byte with no leading byte)
        REQUIRE(toAce(string("中文\x80.com")).empty());
        // invalid ACE body
        REQUIRE(fromAce("xn--!!!").empty());
    }
}

TEST_CASE("split edge cases", "[string_utils]")
{
    SECTION("empty input")
    {
        auto parts = split("", ',');
        REQUIRE(parts.size() == 1);
        REQUIRE(parts[0].empty());
    }

    SECTION("trailing separator")
    {
        auto parts = split("a,b,", ',');
        REQUIRE(parts.size() == 3);
        REQUIRE(parts[2].empty());
    }

    SECTION("leading separator")
    {
        auto parts = split(",a", ',');
        REQUIRE(parts.size() == 2);
        REQUIRE(parts[0].empty());
        REQUIRE(parts[1] == "a");
    }

    SECTION("multi-char separator at boundaries")
    {
        auto parts = split("::a::", "::");
        REQUIRE(parts.size() == 3);
        REQUIRE(parts[0].empty());
        REQUIRE(parts[1] == "a");
        REQUIRE(parts[2].empty());
    }
}

TEST_CASE("trimmed edge cases", "[string_utils]")
{
    REQUIRE(trimmed("").empty());
    REQUIRE(trimmed("   ").empty());
    REQUIRE(trimmed("x").size() == 1);
    REQUIRE(trimmed("\n\tx\n") == "x");
}

TEST_CASE("startsWith and endsWith edge cases", "[string_utils]")
{
    REQUIRE(startsWith("abc", ""));
    REQUIRE(endsWith("abc", ""));
    REQUIRE(startsWith("abc", "abc"));
    REQUIRE(endsWith("abc", "abc"));
    REQUIRE_FALSE(startsWith("", "a"));
    REQUIRE_FALSE(endsWith("", "a"));
}

TEST_CASE("parseInt and parseLongLong edge cases", "[string_utils]")
{
    SECTION("parseInt without ok pointer")
    {
        REQUIRE(parseInt("0") == 0);
        REQUIRE(parseInt("42") == 42);
        REQUIRE(parseInt("0x10") == 0);
        REQUIRE(parseInt("") == 0);
        REQUIRE(parseInt(" 1") == 1);
    }

    SECTION("parseLongLong full consumption")
    {
        bool ok = false;
        REQUIRE(parseLongLong("9223372036854775807", &ok) == 9223372036854775807LL);
        REQUIRE(ok);
        ok = true;
        REQUIRE(parseLongLong("9223372036854775807 ", &ok) == 0);
        REQUIRE_FALSE(ok);
    }

    SECTION("negative integers")
    {
        bool ok = false;
        REQUIRE(parseInt("-42", &ok) == -42);
        REQUIRE(ok);
    }
}

TEST_CASE("parseInt ok flag edge cases", "[string_utils]")
{
    bool ok = true;
    REQUIRE(parseInt("-99", &ok) == -99);
    REQUIRE(ok);
    ok = true;
    REQUIRE(parseInt("007", &ok) == 7);
    REQUIRE(ok);
    ok = true;
    REQUIRE(parseInt("12abc", &ok) == 0);
    REQUIRE_FALSE(ok);
    REQUIRE(parseInt("42") == 42);
}

TEST_CASE("join char separator", "[string_utils]")
{
    vector<string> parts{"x", "y"};
    REQUIRE(join(parts, '|') == "x|y");
    REQUIRE(join(parts, '/') == "x/y");
}

TEST_CASE("number formatting", "[string_utils]")
{
    REQUIRE(number(0) == "0");
    REQUIRE(number(-1) == "-1");
    REQUIRE(number(3.14159, 4) == "3.1416");
    REQUIRE(number(1.0, 0) == "1");
}

TEST_CASE("htmlEscape all special chars", "[string_utils]")
{
    REQUIRE(htmlEscape("<>&\"'") == "&lt;&gt;&amp;&quot;'");
    REQUIRE(htmlEscape("").empty());
}

TEST_CASE("formatMessage edge cases", "[string_utils]")
{
    vector<string> args{"a"};
    REQUIRE(formatMessage("no placeholders", args) == "no placeholders");
    REQUIRE(formatMessage("%9", args).empty());
    REQUIRE(formatMessage("%d", args) == "a");
    REQUIRE(formatMessage("%s", args) == "a");
    REQUIRE(formatMessage("trailing %", args) == "trailing %");
}

TEST_CASE("bytesToHex and bytesToBase64 edge cases", "[string_utils]")
{
    REQUIRE(bytesToHex("") == "");
    REQUIRE(bytesToHex(string("\x0a", 1)) == "0a");
    const string decoded = string("\xfb\xef", 2);
    const string encoded = bytesToBase64(decoded);
    REQUIRE(encoded == "++8=");
}
