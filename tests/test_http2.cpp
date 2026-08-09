#include <catch2/catch_test_macros.hpp>

#include "qtng/private/hpack_p.h"
#include "qtng/http.h"
#include "qtng/eventloop.h"
#include "qtng/coroutine.h"

using namespace std;
using namespace qtng;

TEST_CASE("HPACK encode/decode roundtrip", "[http2][hpack]")
{
    HpackEncoder encoder;
    HpackDecoder decoder;
    vector<HpackHeader> input;
    input.push_back(HpackHeader{":method", "GET"});
    input.push_back(HpackHeader{":path", "/"});
    input.push_back(HpackHeader{":scheme", "https"});
    input.push_back(HpackHeader{":authority", "example.com"});
    input.push_back(HpackHeader{"user-agent", "qtng-test"});
    input.push_back(HpackHeader{"accept", "*/*"});

    const string encoded = encoder.encode(input);
    REQUIRE_FALSE(encoded.empty());

    vector<HpackHeader> output;
    REQUIRE(decoder.decode(encoded, &output));
    REQUIRE(output.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        REQUIRE(output[i].name == input[i].name);
        REQUIRE(output[i].value == input[i].value);
    }
}

TEST_CASE("HPACK static indexed :method GET", "[http2][hpack]")
{
    HpackEncoder encoder;
    vector<HpackHeader> input;
    input.push_back(HpackHeader{":method", "GET"});
    const string encoded = encoder.encode(input);
    // Indexed header field for static table index 2 is a single byte 0x82.
    REQUIRE(encoded.size() == 1);
    REQUIRE(static_cast<unsigned char>(encoded[0]) == 0x82);
}

TEST_CASE("HPACK dynamic table reuse across requests", "[http2][hpack]")
{
    HpackEncoder encoder;
    HpackDecoder decoder;

    vector<HpackHeader> first;
    first.push_back(HpackHeader{":method", "GET"});
    first.push_back(HpackHeader{":path", "/index.html"});
    first.push_back(HpackHeader{":scheme", "https"});
    first.push_back(HpackHeader{":authority", "example.com"});
    first.push_back(HpackHeader{"custom-header", "custom-value"});

    string enc1 = encoder.encode(first);
    vector<HpackHeader> out1;
    REQUIRE(decoder.decode(enc1, &out1));
    REQUIRE(out1.size() == first.size());

    // Second request reuses dynamic entries where possible.
    vector<HpackHeader> second = first;
    second[1].value = "/other.html";
    string enc2 = encoder.encode(second);
    vector<HpackHeader> out2;
    REQUIRE(decoder.decode(enc2, &out2));
    REQUIRE(out2.size() == second.size());
    REQUIRE(out2[1].value == "/other.html");
    REQUIRE(out2[4].name == "custom-header");
    REQUIRE(out2[4].value == "custom-value");
}

TEST_CASE("HPACK static indexed :status 200", "[http2][hpack]")
{
    // Static table index 8 (:status 200) encodes as a single byte 0x88.
    HpackEncoder encoder;
    vector<HpackHeader> input;
    input.push_back(HpackHeader{":status", "200"});
    const string encoded = encoder.encode(input);
    REQUIRE(encoded.size() == 1);
    REQUIRE(static_cast<unsigned char>(encoded[0]) == 0x88);

    HpackDecoder decoder;
    vector<HpackHeader> output;
    REQUIRE(decoder.decode(encoded, &output));
    REQUIRE(output.size() == 1);
    REQUIRE(output[0].name == ":status");
    REQUIRE(output[0].value == "200");
}

TEST_CASE("HttpSession HTTP/2 against public host", "[http2][network]")
{
    // Optional smoke test — skipped when offline / blocked / not h2.
    bool ran = false;
    bool networkError = false;
    int status = 0;
    HttpVersion version = Unknown;
    size_t bodySize = 0;
    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        HttpSession session;
        HttpRequest req;
        req.setUrl("https://www.cloudflare.com/");
        req.setMethod("GET");
        req.setMaxRedirects(2);
        req.setTimeout(15.0f);
        HttpResponse resp = session.send(req);
        ran = true;
        if (resp.hasNetworkError()) {
            networkError = true;
            return;
        }
        status = resp.statusCode();
        version = resp.version();
        bodySize = resp.body().size();
    }));
    job->join();
    REQUIRE(ran);
    if (networkError) {
        SKIP("network unavailable");
    }
    if (version != Http2_0) {
        SKIP("HTTP/2 not negotiated");
    }
    REQUIRE(status >= 200);
    REQUIRE(status < 500);
    REQUIRE(bodySize > 0);
}

TEST_CASE("HttpSession concurrent HTTP/2 requests share session", "[http2][network]")
{
    bool ran = false;
    bool networkError = false;
    int okCount = 0;
    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        HttpSession session;
        vector<shared_ptr<Coroutine>> workers;
        Lock countLock;
        for (int i = 0; i < 3; ++i) {
            workers.emplace_back(Coroutine::spawn([&session, &okCount, &countLock, &networkError] {
                HttpResponse resp = session.get("https://www.cloudflare.com/cdn-cgi/trace");
                if (resp.hasNetworkError()) {
                    networkError = true;
                    return;
                }
                if (resp.version() == Http2_0 && resp.statusCode() >= 200 && resp.statusCode() < 500) {
                    ScopedLock<Lock> locker(countLock);
                    if (locker.isSuccess()) {
                        ++okCount;
                    }
                }
            }));
        }
        for (auto &w : workers) {
            w->join();
        }
        ran = true;
    }));
    job->join();
    REQUIRE(ran);
    if (okCount == 0) {
        if (networkError) {
            SKIP("network unavailable");
        }
        SKIP("HTTP/2 not negotiated");
    }
    REQUIRE(okCount == 3);
}
