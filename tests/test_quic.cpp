#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <string>

#include "qtng/aead.h"
#include "qtng/certificate.h"
#include "qtng/coroutine.h"
#include "qtng/md.h"
#include "qtng/pkey.h"
#include "qtng/private/kcp.h"
#include "qtng/private/quic_p.h"
#include "qtng/quic.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/string_utils.h"

using namespace std;
using namespace qtng;

namespace {

string unhex(const char *hex)
{
    string out;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        while (hex[i] == ' ' || hex[i] == '\n') {
            ++i;
        }
        if (!hex[i] || !hex[i + 1]) {
            break;
        }
        int hi = val(hex[i]);
        int lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            --i;
            continue;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

struct PairedLink : DatagramLink
{
    Queue<string> inbox;
    string localName;
    PairedLink *peer = nullptr;

    explicit PairedLink(string name)
        : localName(std::move(name))
    {
    }

    int32_t recvfrom(char *data, int32_t size, DatagramPath *who) override
    {
        if (inbox.isEmpty()) {
            return 0;
        }
        string packet = inbox.get();
        if (packet.empty()) {
            return 0;
        }
        if (who) {
            *who = DatagramPath(peer ? peer->localName : "");
        }
        const int32_t len = min<int32_t>(size, static_cast<int32_t>(packet.size()));
        memcpy(data, packet.data(), static_cast<size_t>(len));
        return len;
    }

    int32_t sendto(const char *data, int32_t size, const DatagramPath &who) override
    {
        (void) who;
        if (!peer) {
            return -1;
        }
        peer->inbox.put(string(data, static_cast<size_t>(size)));
        return size;
    }

    void close() override {}
    void abort() override {}
    bool isValid() const override { return true; }
};

QuicConfiguration makeServerConfig()
{
    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    const utils::DateTime now = utils::DateTime::currentDateTimeUtc();
    Certificate cert = Certificate::selfSign(key, MessageDigest::Sha256, 1, now, now.addSecs(3650 * 24 * 3600),
                                             {{Certificate::CommonName, "quic.test"}});
    QuicConfiguration cfg;
    cfg.setPrivateKey(key);
    cfg.setLocalCertificate(cert);
    cfg.setVerifyPeer(false);
    cfg.setAlpnProtocols({"hq-interop"});
    return cfg;
}

}  // namespace

TEST_CASE("quic varint roundtrip", "[quic]")
{
    for (uint64_t v : {0ull, 63ull, 64ull, 16383ull, 16384ull, 1073741823ull, 1073741824ull}) {
        string enc;
        REQUIRE(quicEncodeVarint(v, &enc));
        size_t consumed = 0;
        uint64_t out = 0;
        REQUIRE(quicDecodeVarint(enc.data(), enc.size(), &consumed, &out));
        REQUIRE(consumed == enc.size());
        REQUIRE(out == v);
    }
}

TEST_CASE("quic initial keys RFC9001 A.1", "[quic]")
{
    // DCID 8394c8f03e515708 from RFC 9001 Appendix A
    QuicConnectionId dcid;
    dcid.bytes = unhex("8394c8f03e515708");
    string clientSecret = quicDeriveInitialSecret(dcid, true);
    REQUIRE(clientSecret.size() == 32);
    QuicTrafficKeys keys = quicDeriveTrafficKeys(clientSecret);
    REQUIRE(keys.valid());
    // Compare key against RFC 9001 A.1 client:
    // key = 1f369613dd76d5467730efcbe3b1a22d
    REQUIRE(utils::bytesToHex(keys.key) == "1f369613dd76d5467730efcbe3b1a22d");
    REQUIRE(utils::bytesToHex(keys.iv) == "fa044b2f42a3fd3b46fb255c");
    REQUIRE(utils::bytesToHex(keys.hp) == "9f50449e04a0e810283a1e9933adedd2");
}

TEST_CASE("aead aes-gcm roundtrip", "[quic][aead]")
{
    Aead aead(Aead::Aes128Gcm);
    REQUIRE(aead.isValid());
    string key(16, 'k');
    REQUIRE(aead.setKey(key));
    string nonce(12, 'n');
    string aad = "aad";
    string plain = "hello quic";
    string sealed;
    REQUIRE(aead.seal(nonce, aad, plain, &sealed));
    string out;
    REQUIRE(aead.open(nonce, aad, sealed, &out));
    REQUIRE(out == plain);
}

TEST_CASE("quic loopback handshake and stream", "[quic]")
{
    shared_ptr<Coroutine> job(Coroutine::spawn([] {
        auto linkServer = make_shared<PairedLink>("server");
        auto linkClient = make_shared<PairedLink>("client");
        linkServer->peer = linkClient.get();
        linkClient->peer = linkServer.get();

        QuicConnection server(linkServer);
        server.setConfiguration(makeServerConfig());

        QuicConnection client(linkClient);
        QuicConfiguration clientCfg;
        clientCfg.setVerifyPeer(false);
        clientCfg.setAlpnProtocols({"hq-interop"});
        client.setConfiguration(clientCfg);

        shared_ptr<Coroutine> serverJob(Coroutine::spawn([&] {
            REQUIRE(server.serve());
        }));

        Coroutine::sleep(0.05f);
        REQUIRE(client.connect(DatagramPath("server"), "quic.test"));
        REQUIRE(client.state() == QuicConnection::ConnectedState);
        REQUIRE(server.state() == QuicConnection::ConnectedState);

        shared_ptr<QuicStream> cs = client.openStream();
        REQUIRE(cs);
        const char payload[] = "hello-quic-mvp";
        REQUIRE(cs->sendall(payload) == static_cast<int32_t>(sizeof(payload) - 1));
        cs->close();

        shared_ptr<QuicStream> ss = server.acceptStream();
        REQUIRE(ss);
        string got = ss->recvall(static_cast<int32_t>(sizeof(payload) - 1));
        REQUIRE(got == payload);

        serverJob->join();
        client.close();
        server.close();
    }));
    job->join();
}

TEST_CASE("quic frame ack decode and recovery", "[quic]")
{
    string enc;
    QuicFrame ack;
    ack.type = QuicFrame::Ack;
    ack.largestAcknowledged = 5;
    ack.ackDelay = 0;
    ack.firstAckRange = 5;
    REQUIRE(quicEncodeFrame(ack, &enc));
    vector<QuicFrame> frames;
    REQUIRE(quicDecodeFrames(enc.data(), enc.size(), &frames));
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].type == QuicFrame::Ack);
    REQUIRE(frames[0].largestAcknowledged == 5);

    QuicLossRecovery recovery;
    QuicSentPacket sent;
    sent.pn = 1;
    sent.space = QuicPnApplication;
    sent.raw = string(20, 'x');
    sent.inFlight = true;
    recovery.onPacketSent(sent);
    sent.pn = 2;
    recovery.onPacketSent(sent);
    sent.pn = 5;
    recovery.onPacketSent(sent);
    REQUIRE(recovery.bytesInFlight() == 60);
    vector<string> lost;
    recovery.onAckReceived(QuicPnApplication, frames[0], &lost);
    // pn 1 and 2 should be marked lost (largest-2 thresh), 5 acked
    REQUIRE(recovery.bytesInFlight() == 0);
}
