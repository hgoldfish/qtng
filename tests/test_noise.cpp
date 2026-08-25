#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qtng/coroutine.h"
#include "qtng/noise.h"
#include "qtng/socket.h"

using namespace std;
using namespace qtng;

namespace {

struct ConnectedPair {
    shared_ptr<Socket> serverSide;
    shared_ptr<Socket> clientSide;
};

ConnectedPair makeConnectedPair()
{
    unique_ptr<Socket> listener(Socket::createServer(HostAddress::LocalHost, 0, 1));
    REQUIRE(listener);
    const uint16_t port = listener->localPort();
    REQUIRE(port != 0);

    shared_ptr<Socket> client;
    shared_ptr<Socket> accepted;
    shared_ptr<Coroutine> acceptor(Coroutine::spawn([&] {
        accepted.reset(listener->accept());
    }));
    client.reset(Socket::createConnection(HostAddress::LocalHost, port));
    REQUIRE(client);
    acceptor->join();
    REQUIRE(accepted);

    ConnectedPair pair;
    pair.serverSide = accepted;
    pair.clientSide = client;
    return pair;
}

void runHandshake(NoiseHandshakeState *initiator, NoiseHandshakeState *responder, const string &initPayload,
                  const string &respPayload)
{
    string msg;
    string payload;

    REQUIRE(initiator->writeMessage(initPayload, &msg));
    REQUIRE(responder->readMessage(msg, &payload));
    REQUIRE(payload == initPayload);

    REQUIRE(responder->writeMessage(respPayload, &msg));
    REQUIRE(initiator->readMessage(msg, &payload));
    REQUIRE(payload == respPayload);

    if (!initiator->isComplete()) {
        REQUIRE(initiator->writeMessage(string(), &msg));
        REQUIRE(responder->readMessage(msg, &payload));
        REQUIRE(payload.empty());
    }

    REQUIRE(initiator->isComplete());
    REQUIRE(responder->isComplete());
}

NoiseConfig makeConfig(NoisePattern pattern, NoiseRole role, const NoiseKey &local,
                       const string &remoteStatic = string())
{
    NoiseConfig cfg(local);
    cfg.setPattern(pattern);
    cfg.setRole(role);
    cfg.setRemoteStaticPublic(remoteStatic);
    return cfg;
}

void checkTransport(NoiseCipherState *sendA, NoiseCipherState *recvA, NoiseCipherState *sendB, NoiseCipherState *recvB)
{
    const string plain = "hello noise";
    const string ct = sendA->encryptWithAd(string(), plain);
    REQUIRE(ct.size() >= 16);
    const string pt = recvB->decryptWithAd(string(), ct);
    REQUIRE(recvB->lastDecryptOk());
    REQUIRE(pt == plain);

    const string ct2 = sendB->encryptWithAd(string(), "reply");
    const string pt2 = recvA->decryptWithAd(string(), ct2);
    REQUIRE(recvA->lastDecryptOk());
    REQUIRE(pt2 == "reply");
}

void checkHashHandshake(NoiseHash hash, size_t expectedHashLen)
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    NoiseConfig initCfg = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    initCfg.setHash(hash);
    const bool ok = initiator.initialize(initCfg);
    if (!ok) {
        REQUIRE(initiator.errorString().find("unavailable") != string::npos);
        NoiseConfig respCfg = makeConfig(NoisePattern::XX, NoiseRole::Responder, bob);
        respCfg.setHash(hash);
        REQUIRE_FALSE(responder.initialize(respCfg));
        REQUIRE(responder.errorString().find("unavailable") != string::npos);
        return;
    }
    NoiseConfig respCfg = makeConfig(NoisePattern::XX, NoiseRole::Responder, bob);
    respCfg.setHash(hash);
    REQUIRE(responder.initialize(respCfg));
    runHandshake(&initiator, &responder, "hi", "hello");
    REQUIRE(initiator.handshakeHash() == responder.handshakeHash());
    REQUIRE(initiator.handshakeHash().size() == expectedHashLen);

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

void runDatagramHandshake(NoiseDatagram *initiator, NoiseDatagram *responder, const string &initPayload,
                          const string &respPayload)
{
    string msg;
    string payload;

    REQUIRE(initiator->writeHandshake(initPayload, &msg));
    REQUIRE(responder->readHandshake(msg, &payload));
    REQUIRE(payload == initPayload);

    REQUIRE(responder->writeHandshake(respPayload, &msg));
    REQUIRE(initiator->readHandshake(msg, &payload));
    REQUIRE(payload == respPayload);

    if (!initiator->isHandshakeComplete()) {
        REQUIRE(initiator->writeHandshake(string(), &msg));
        REQUIRE(responder->readHandshake(msg, &payload));
        REQUIRE(payload.empty());
    }

    REQUIRE(initiator->isHandshakeComplete());
    REQUIRE(responder->isHandshakeComplete());
}

}  // namespace

TEST_CASE("NoiseKey generate and DH", "[noise]")
{
    const NoiseKey a = NoiseKey::generate();
    const NoiseKey b = NoiseKey::generate();
    REQUIRE(a.isValid());
    REQUIRE(b.isValid());
    const string ab = NoiseKey::dh(a.privateKey(), b.publicKey());
    const string ba = NoiseKey::dh(b.privateKey(), a.publicKey());
    REQUIRE(ab.size() == 32);
    REQUIRE(ab == ba);

    const NoiseKey restored = NoiseKey::fromPrivateKey(a.privateKey());
    REQUIRE(restored.isValid());
    REQUIRE(restored.publicKey() == a.publicKey());
}

TEST_CASE("NoiseConfig generates local static when private key is empty", "[noise]")
{
    NoiseConfig aliceCfg;
    NoiseConfig bobCfg;
    aliceCfg.setRole(NoiseRole::Initiator);
    bobCfg.setRole(NoiseRole::Responder);
    REQUIRE(aliceCfg.localStatic().isValid());
    REQUIRE(bobCfg.localStatic().isValid());
    REQUIRE(aliceCfg.localStatic().publicKey() != bobCfg.localStatic().publicKey());

    NoiseConfig copied(aliceCfg.localStatic());
    REQUIRE(copied.localStatic().publicKey() == aliceCfg.localStatic().publicKey());

    NoiseConfig emptyKey{NoiseKey{}};
    REQUIRE_FALSE(emptyKey.localStatic().isValid());

    NoiseConfig fromBad(string(16, 'x'));
    REQUIRE_FALSE(fromBad.localStatic().isValid());

    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(aliceCfg));
    REQUIRE(responder.initialize(bobCfg));
    runHandshake(&initiator, &responder, "hi", "hello");
    REQUIRE(initiator.remoteStaticPublic() == bobCfg.localStatic().publicKey());
    REQUIRE(responder.remoteStaticPublic() == aliceCfg.localStatic().publicKey());

    NoiseHandshakeState bad;
    fromBad.setRole(NoiseRole::Initiator);
    REQUIRE_FALSE(bad.initialize(fromBad));
    REQUIRE(bad.errorString().find("invalid") != string::npos);
}

TEST_CASE("parseNoiseProtocolName is the inverse of noiseProtocolName", "[noise]")
{
    NoisePattern pattern = NoisePattern::XX;
    NoisePskModifier mod = NoisePskModifier::None;
    Aead::Algorithm cipher = Aead::ChaCha20Poly1305;
    NoiseHash hash = NoiseHash::Sha256;
    string err;

    const string full = noiseProtocolName(NoisePattern::XX, NoisePskModifier::Psk0,
                                          Aead::ChaCha20Poly1305, NoiseHash::Sha256);
    REQUIRE(full == "Noise_XXpsk0_25519_ChaChaPoly_SHA256");
    REQUIRE(parseNoiseProtocolName(full, &pattern, &mod, &cipher, &hash, &err));
    REQUIRE(pattern == NoisePattern::XX);
    REQUIRE(mod == NoisePskModifier::Psk0);
    REQUIRE(cipher == Aead::ChaCha20Poly1305);
    REQUIRE(hash == NoiseHash::Sha256);

    REQUIRE(parseNoiseProtocolName("noise_ikpsk2_25519_aesgcm_sha512", &pattern, &mod, &cipher, &hash,
                                   &err));
    REQUIRE(pattern == NoisePattern::IK);
    REQUIRE(mod == NoisePskModifier::Psk2);
    REQUIRE(cipher == Aead::Aes256Gcm);
    REQUIRE(hash == NoiseHash::Sha512);

    NoiseConfig cfg;
    REQUIRE(applyNoiseProtocolName("Noise_XXpsk0_25519_ChaChaPoly_SHA256", &cfg, &err));
    REQUIRE(cfg.pattern() == NoisePattern::XX);
    REQUIRE(cfg.pskModifier() == NoisePskModifier::Psk0);
    REQUIRE(cfg.cipher() == Aead::ChaCha20Poly1305);
    REQUIRE(cfg.hash() == NoiseHash::Sha256);

    REQUIRE_FALSE(parseNoiseProtocolName("XX", &pattern, &mod, &cipher, &hash, &err));
    REQUIRE_FALSE(parseNoiseProtocolName("Noise_IKpsk3_25519_ChaChaPoly_SHA256", &pattern, &mod,
                                         &cipher, &hash, &err));
    REQUIRE_FALSE(parseNoiseProtocolName("Noise_XX_25519_ChaChaPoly_MD5", &pattern, &mod, &cipher,
                                         &hash, &err));
}

TEST_CASE("Noise XX handshake and transport", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice)));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::XX, NoiseRole::Responder, bob)));

    runHandshake(&initiator, &responder, "hi", "hello");
    REQUIRE(initiator.remoteStaticPublic() == bob.publicKey());
    REQUIRE(responder.remoteStaticPublic() == alice.publicKey());
    REQUIRE(initiator.handshakeHash() == responder.handshakeHash());

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    REQUIRE(sendI.algorithm() == Aead::ChaCha20Poly1305);
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

TEST_CASE("Noise XX AESGCM handshake and transport", "[noise][aead]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    NoiseConfig initCfg = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    initCfg.setCipher(Aead::Aes256Gcm);
    NoiseConfig respCfg = makeConfig(NoisePattern::XX, NoiseRole::Responder, bob);
    respCfg.setCipher(Aead::Aes256Gcm);
    REQUIRE(initiator.initialize(initCfg));
    REQUIRE(responder.initialize(respCfg));

    runHandshake(&initiator, &responder, "hi", "hello");
    REQUIRE(initiator.handshakeHash() == responder.handshakeHash());

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    REQUIRE(sendI.algorithm() == Aead::Aes256Gcm);
    REQUIRE(recvR.algorithm() == Aead::Aes256Gcm);
    checkTransport(&sendI, &recvI, &sendR, &recvR);

    const string emptyCt = sendI.encryptWithAd(string(), string());
    REQUIRE(emptyCt.size() == 16);
    REQUIRE(recvR.decryptWithAd(string(), emptyCt).empty());
    REQUIRE(recvR.lastDecryptOk());
}

TEST_CASE("Noise rejects AES-128-GCM and cipher mismatch", "[noise][aead]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    NoiseConfig badCfg = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    badCfg.setCipher(Aead::Aes128Gcm);
    REQUIRE_FALSE(initiator.initialize(badCfg));
    REQUIRE(initiator.errorString().find("AEAD") != string::npos);

    NoiseCipherState bad(Aead::Aes128Gcm);
    bad.initializeKey(string(32, 'k'));
    REQUIRE_FALSE(bad.hasKey());

    NoiseConfig gcmCfg = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    gcmCfg.setCipher(Aead::Aes256Gcm);
    REQUIRE(initiator.initialize(gcmCfg));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::XX, NoiseRole::Responder, bob)));
    REQUIRE(initiator.handshakeHash() != responder.handshakeHash());

    string msg;
    string payload;
    REQUIRE(initiator.writeMessage(string(), &msg));
    REQUIRE(responder.readMessage(msg, &payload));
    REQUIRE(responder.writeMessage(string(), &msg));
    REQUIRE_FALSE(initiator.readMessage(msg, &payload));
}

TEST_CASE("Noise BLAKE2s handshake", "[noise][blake2]")
{
    checkHashHandshake(NoiseHash::Blake2s, 32);
}

TEST_CASE("Noise BLAKE2b handshake", "[noise][blake2]")
{
    checkHashHandshake(NoiseHash::Blake2b, 64);
}

TEST_CASE("Noise SHA512 handshake", "[noise]")
{
    checkHashHandshake(NoiseHash::Sha512, 64);
}

TEST_CASE("Noise rejects hash mismatch", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    NoiseConfig initCfg = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    initCfg.setHash(NoiseHash::Sha512);
    REQUIRE(initiator.initialize(initCfg));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::XX, NoiseRole::Responder, bob)));
    REQUIRE(initiator.handshakeHash() != responder.handshakeHash());
    REQUIRE(initiator.handshakeHash().size() == 64);
    REQUIRE(responder.handshakeHash().size() == 32);

    string msg;
    string payload;
    REQUIRE(initiator.writeMessage(string(), &msg));
    REQUIRE(responder.readMessage(msg, &payload));
    REQUIRE(responder.writeMessage(string(), &msg));
    REQUIRE_FALSE(initiator.readMessage(msg, &payload));
}

TEST_CASE("Noise IKpsk2 BLAKE2s handshake", "[noise][blake2]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    const string psk(32, 'w');
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    NoiseConfig initCfg = makeConfig(NoisePattern::IK, NoiseRole::Initiator, alice, bob.publicKey());
    initCfg.setPsk(psk);
    initCfg.setPskModifier(NoisePskModifier::Psk2);
    initCfg.setHash(NoiseHash::Blake2s);
    const bool ok = initiator.initialize(initCfg);
    if (!ok) {
        REQUIRE(initiator.errorString().find("unavailable") != string::npos);
        return;
    }
    NoiseConfig respCfg = makeConfig(NoisePattern::IK, NoiseRole::Responder, bob);
    respCfg.setPsk(psk);
    respCfg.setPskModifier(NoisePskModifier::Psk2);
    respCfg.setHash(NoiseHash::Blake2s);
    REQUIRE(responder.initialize(respCfg));
    runHandshake(&initiator, &responder, "init", "resp");
    REQUIRE(initiator.handshakeHash().size() == 32);

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

TEST_CASE("Noise XXpsk0 handshake", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    const string psk(32, 'p');
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    NoiseConfig initCfg = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    initCfg.setPsk(psk);
    initCfg.setPskModifier(NoisePskModifier::Psk0);
    NoiseConfig respCfg = makeConfig(NoisePattern::XX, NoiseRole::Responder, bob);
    respCfg.setPsk(psk);
    respCfg.setPskModifier(NoisePskModifier::Psk0);
    REQUIRE(initiator.initialize(initCfg));
    REQUIRE(responder.initialize(respCfg));

    runHandshake(&initiator, &responder, "a", "b");

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

TEST_CASE("Noise PSK modifier validation", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const string psk(32, 'p');
    NoiseHandshakeState hs;
    NoiseConfig pskNoMod = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    pskNoMod.setPsk(psk);
    REQUIRE_FALSE(hs.initialize(pskNoMod));
    REQUIRE(hs.errorString().find("modifier") != string::npos);

    NoiseConfig modNoPsk = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    modNoPsk.setPskModifier(NoisePskModifier::Psk0);
    REQUIRE_FALSE(hs.initialize(modNoPsk));
    REQUIRE(hs.errorString().find("32") != string::npos);

    NoiseConfig badSlot = makeConfig(NoisePattern::IK, NoiseRole::Initiator, alice, alice.publicKey());
    badSlot.setPsk(psk);
    badSlot.setPskModifier(NoisePskModifier::Psk3);
    REQUIRE_FALSE(hs.initialize(badSlot));
    REQUIRE(hs.errorString().find("modifier") != string::npos);
}

TEST_CASE("Noise IK handshake", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::IK, NoiseRole::Initiator, alice, bob.publicKey())));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::IK, NoiseRole::Responder, bob)));

    runHandshake(&initiator, &responder, "init", "resp");
    REQUIRE(responder.remoteStaticPublic() == alice.publicKey());

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

TEST_CASE("Noise XK handshake", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::XK, NoiseRole::Initiator, alice, bob.publicKey())));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::XK, NoiseRole::Responder, bob)));

    runHandshake(&initiator, &responder, "init", "resp");
    REQUIRE(responder.remoteStaticPublic() == alice.publicKey());
    REQUIRE(initiator.handshakeHash() == responder.handshakeHash());

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

TEST_CASE("Noise KK handshake", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::KK, NoiseRole::Initiator, alice, bob.publicKey())));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::KK, NoiseRole::Responder, bob, alice.publicKey())));

    runHandshake(&initiator, &responder, "init", "resp");
    REQUIRE(initiator.remoteStaticPublic() == bob.publicKey());
    REQUIRE(responder.remoteStaticPublic() == alice.publicKey());

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

TEST_CASE("Noise IKpsk2 and XXpsk3 handshake", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    const string psk(32, 'k');

    NoiseHandshakeState ikI, ikR;
    NoiseConfig ikICfg = makeConfig(NoisePattern::IK, NoiseRole::Initiator, alice, bob.publicKey());
    ikICfg.setPsk(psk);
    ikICfg.setPskModifier(NoisePskModifier::Psk2);
    NoiseConfig ikRCfg = makeConfig(NoisePattern::IK, NoiseRole::Responder, bob);
    ikRCfg.setPsk(psk);
    ikRCfg.setPskModifier(NoisePskModifier::Psk2);
    REQUIRE(ikI.initialize(ikICfg));
    REQUIRE(ikR.initialize(ikRCfg));
    runHandshake(&ikI, &ikR, "i", "r");
    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(ikI.split(&sendI, &recvI));
    REQUIRE(ikR.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);

    NoiseHandshakeState xxI, xxR;
    NoiseConfig xxICfg = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    xxICfg.setPsk(psk);
    xxICfg.setPskModifier(NoisePskModifier::Psk3);
    NoiseConfig xxRCfg = makeConfig(NoisePattern::XX, NoiseRole::Responder, bob);
    xxRCfg.setPsk(psk);
    xxRCfg.setPskModifier(NoisePskModifier::Psk3);
    REQUIRE(xxI.initialize(xxICfg));
    REQUIRE(xxR.initialize(xxRCfg));
    runHandshake(&xxI, &xxR, "a", "b");
    REQUIRE(xxI.handshakeHash() == xxR.handshakeHash());

    NoiseHandshakeState badI, badR;
    NoiseConfig badICfg = makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice);
    badICfg.setPsk(psk);
    badICfg.setPskModifier(NoisePskModifier::Psk0);
    NoiseConfig badRCfg = makeConfig(NoisePattern::XX, NoiseRole::Responder, bob);
    badRCfg.setPsk(string(32, 'x'));
    badRCfg.setPskModifier(NoisePskModifier::Psk0);
    REQUIRE(badI.initialize(badICfg));
    REQUIRE(badR.initialize(badRCfg));
    string msg;
    string payload;
    REQUIRE(badI.writeMessage("x", &msg));
    REQUIRE_FALSE(badR.readMessage(msg, &payload));
}

TEST_CASE("Noise rejects remote static mismatch", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    const NoiseKey impostor = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice, impostor.publicKey())));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::XX, NoiseRole::Responder, bob)));

    string msg;
    string payload;
    REQUIRE(initiator.writeMessage(string(), &msg));
    REQUIRE(responder.readMessage(msg, &payload));
    REQUIRE(responder.writeMessage(string(), &msg));
    REQUIRE_FALSE(initiator.readMessage(msg, &payload));
    REQUIRE(initiator.errorString().find("mismatch") != string::npos);
}

TEST_CASE("NoiseCipherState rekey and nonce exhaustion", "[noise]")
{
    const string key(32, 'k');

    NoiseCipherState a;
    NoiseCipherState b;
    a.initializeKey(key);
    b.initializeKey(key);
    uint64_t n0 = 0;
    const string ct0 = a.encryptWithAd("ad", "p0", &n0);
    REQUIRE(n0 == 0);
    REQUIRE(b.decryptWithAd("ad", ct0, 0) == "p0");
    REQUIRE(b.lastDecryptOk());
    REQUIRE(b.decryptWithAd("ad", ct0, 0) == "p0");
    REQUIRE(b.nonce() == 0);

    NoiseCipherState send;
    NoiseCipherState recv;
    send.initializeKey(key);
    recv.initializeKey(key);
    const string c1 = send.encryptWithAd(string(), "one");
    REQUIRE(recv.decryptWithAd(string(), c1) == "one");
    REQUIRE(send.rekey());
    REQUIRE(recv.rekey());
    const string c2 = send.encryptWithAd(string(), "after-rekey");
    REQUIRE(recv.decryptWithAd(string(), c2) == "after-rekey");
    REQUIRE(recv.lastDecryptOk());

    NoiseCipherState sendGcm(Aead::Aes256Gcm);
    NoiseCipherState recvGcm(Aead::Aes256Gcm);
    sendGcm.initializeKey(key);
    recvGcm.initializeKey(key);
    REQUIRE(sendGcm.rekey());
    REQUIRE(recvGcm.rekey());
    const string c3 = sendGcm.encryptWithAd(string(), "gcm-rekey");
    REQUIRE(recvGcm.decryptWithAd(string(), c3) == "gcm-rekey");
    REQUIRE(recvGcm.lastDecryptOk());

    NoiseCipherState limited;
    limited.initializeKey(key);
    limited.setNonce(NoiseCipherState::MaxNonce);
    uint64_t nMax = 0;
    const string last = limited.encryptWithAd(string(), "last", &nMax);
    REQUIRE(nMax == NoiseCipherState::MaxNonce);
    REQUIRE(limited.nonce() == NoiseCipherState::MaxNonce + 1);
    REQUIRE(limited.encryptWithAd(string(), "overflow").empty());

    NoiseCipherState limitedRecv;
    limitedRecv.initializeKey(key);
    limitedRecv.setNonce(NoiseCipherState::MaxNonce);
    REQUIRE(limitedRecv.decryptWithAd(string(), last) == "last");
    REQUIRE(limitedRecv.nonce() == NoiseCipherState::MaxNonce + 1);
    REQUIRE(limitedRecv.decryptWithAd(string(), last).empty());
    REQUIRE_FALSE(limitedRecv.lastDecryptOk());
    REQUIRE(limitedRecv.decryptWithAd(string(), last, NoiseCipherState::MaxNonce + 1).empty());

    REQUIRE(limited.rekey());
    REQUIRE(limited.nonce() == 0);
    REQUIRE_FALSE(limited.encryptWithAd(string(), "after-rekey").empty());
}

TEST_CASE("NoiseSocket XX over TCP", "[noise]")
{
    ConnectedPair sockets = makeConnectedPair();
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();

    shared_ptr<NoiseSocket> client(new NoiseSocket(asSocketLike(sockets.clientSide)));
    shared_ptr<NoiseSocket> server(new NoiseSocket(asSocketLike(sockets.serverSide)));
    REQUIRE(client->initialize(makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice)));
    REQUIRE(server->initialize(makeConfig(NoisePattern::XX, NoiseRole::Responder, bob)));

    bool serverOk = false;
    shared_ptr<Coroutine> serverHs(Coroutine::spawn([&] {
        serverOk = server->handshake("server-hello");
    }));
    REQUIRE(client->handshake("client-hello"));
    serverHs->join();
    REQUIRE(serverOk);
    REQUIRE(client->isHandshakeComplete());
    REQUIRE(server->isHandshakeComplete());
    REQUIRE(client->peerHandshakePayload() == "server-hello");
    REQUIRE(server->peerHandshakePayload() == "client-hello");
    REQUIRE(client->handshakeHash() == server->handshakeHash());

    REQUIRE(client->sendall("ping") == 4);
    REQUIRE(server->recv(4) == "ping");
    REQUIRE(server->sendall("pong") == 4);
    REQUIRE(client->recv(4) == "pong");

    shared_ptr<SocketLike> clientLike = asSocketLike(client);
    shared_ptr<SocketLike> serverLike = asSocketLike(server);
    REQUIRE(clientLike->sendall("via-like") == 8);
    REQUIRE(serverLike->recvall(8) == "via-like");
}

TEST_CASE("NoiseSocket IK over TCP", "[noise]")
{
    ConnectedPair sockets = makeConnectedPair();
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();

    shared_ptr<NoiseSocket> client(new NoiseSocket(asSocketLike(sockets.clientSide)));
    shared_ptr<NoiseSocket> server(new NoiseSocket(asSocketLike(sockets.serverSide)));
    REQUIRE(client->initialize(makeConfig(NoisePattern::IK, NoiseRole::Initiator, alice, bob.publicKey())));
    REQUIRE(server->initialize(makeConfig(NoisePattern::IK, NoiseRole::Responder, bob)));

    bool serverOk = false;
    shared_ptr<Coroutine> serverHs(Coroutine::spawn([&] {
        serverOk = server->handshake();
    }));
    REQUIRE(client->handshake());
    serverHs->join();
    REQUIRE(serverOk);
    REQUIRE(client->sendall("ik-data") == 7);
    REQUIRE(server->recvall(7) == "ik-data");
}

TEST_CASE("NoiseSocket XK over TCP", "[noise]")
{
    ConnectedPair sockets = makeConnectedPair();
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();

    shared_ptr<NoiseSocket> client(new NoiseSocket(asSocketLike(sockets.clientSide)));
    shared_ptr<NoiseSocket> server(new NoiseSocket(asSocketLike(sockets.serverSide)));
    REQUIRE(client->initialize(makeConfig(NoisePattern::XK, NoiseRole::Initiator, alice, bob.publicKey())));
    REQUIRE(server->initialize(makeConfig(NoisePattern::XK, NoiseRole::Responder, bob)));

    bool serverOk = false;
    shared_ptr<Coroutine> serverHs(Coroutine::spawn([&] {
        serverOk = server->handshake();
    }));
    REQUIRE(client->handshake());
    serverHs->join();
    REQUIRE(serverOk);
    REQUIRE(server->remoteStaticPublic() == alice.publicKey());
    REQUIRE(client->sendall("xk-data") == 7);
    REQUIRE(server->recvall(7) == "xk-data");
}

TEST_CASE("NoiseDatagram XX handshake and transport", "[noise][datagram]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseDatagram initiator;
    NoiseDatagram responder;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice)));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::XX, NoiseRole::Responder, bob)));

    runDatagramHandshake(&initiator, &responder, "client-hello", "server-hello");
    REQUIRE(initiator.peerHandshakePayload() == "server-hello");
    REQUIRE(responder.peerHandshakePayload() == "client-hello");
    REQUIRE(initiator.handshakeHash() == responder.handshakeHash());
    REQUIRE(initiator.remoteStaticPublic() == bob.publicKey());
    REQUIRE(responder.remoteStaticPublic() == alice.publicKey());

    const string ping = initiator.encrypt("ping");
    REQUIRE_FALSE(ping.empty());
    REQUIRE(responder.decrypt(ping) == "ping");
    REQUIRE(responder.lastDecryptOk());
    const string pong = responder.encrypt("pong");
    REQUIRE(initiator.decrypt(pong) == "pong");

    const string emptyWire = initiator.encrypt(string());
    REQUIRE(emptyWire.size() == 8 + 16);
    REQUIRE(responder.decrypt(emptyWire).empty());
    REQUIRE(responder.lastDecryptOk());
}

TEST_CASE("NoiseDatagram move hands off transport keys", "[noise][datagram]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseDatagram initiator;
    NoiseDatagram handshake;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice)));
    REQUIRE(handshake.initialize(makeConfig(NoisePattern::XX, NoiseRole::Responder, bob)));
    runDatagramHandshake(&initiator, &handshake, string(), string());

    NoiseDatagram session = std::move(handshake);
    REQUIRE(session.isHandshakeComplete());
    REQUIRE_FALSE(handshake.isHandshakeComplete());
    const string ping = initiator.encrypt("ping");
    REQUIRE(session.decrypt(ping) == "ping");
    REQUIRE(initiator.decrypt(session.encrypt("pong")) == "pong");
}

TEST_CASE("NoiseDatagram IK and reordered transport", "[noise][datagram]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseDatagram initiator;
    NoiseDatagram responder;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::IK, NoiseRole::Initiator, alice, bob.publicKey())));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::IK, NoiseRole::Responder, bob)));
    runDatagramHandshake(&initiator, &responder, "init", "resp");

    const string first = initiator.encrypt("first");
    const string second = initiator.encrypt("second");
    REQUIRE(responder.decrypt(second) == "second");
    REQUIRE(responder.decrypt(first) == "first");
    REQUIRE(responder.decrypt(first).empty());
    REQUIRE_FALSE(responder.lastDecryptOk());
    REQUIRE(responder.decrypt("forged").empty());
    REQUIRE_FALSE(responder.lastDecryptOk());
    REQUIRE(initiator.decrypt(responder.encrypt("ack")) == "ack");
}

TEST_CASE("NoiseDatagram KK handshake", "[noise][datagram]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseDatagram initiator;
    NoiseDatagram responder;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::KK, NoiseRole::Initiator, alice, bob.publicKey())));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::KK, NoiseRole::Responder, bob, alice.publicKey())));
    runDatagramHandshake(&initiator, &responder, "init", "resp");
    REQUIRE(initiator.encrypt("k").size() >= 8 + 16);
    REQUIRE(responder.decrypt(initiator.encrypt("kk")) == "kk");
}

TEST_CASE("NoiseDatagram WireGuard replay window", "[noise][datagram]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseDatagram initiator;
    NoiseDatagram responder;
    REQUIRE(initiator.initialize(makeConfig(NoisePattern::IK, NoiseRole::Initiator, alice, bob.publicKey())));
    REQUIRE(responder.initialize(makeConfig(NoisePattern::IK, NoiseRole::Responder, bob)));
    runDatagramHandshake(&initiator, &responder, string(), string());

    const string nonce0 = initiator.encrypt("n0");
    string forged = nonce0;
    forged[forged.size() - 1] = static_cast<char>(static_cast<unsigned char>(forged.back()) ^ 0xff);
    REQUIRE(responder.decrypt(forged).empty());
    REQUIRE_FALSE(responder.lastDecryptOk());
    REQUIRE(responder.decrypt(nonce0) == "n0");
    REQUIRE(responder.lastDecryptOk());

    // WireGuard COUNTER_WINDOW_SIZE = 8192 - 64.
    const int window = 8128;
    vector<string> packets;
    packets.reserve(static_cast<size_t>(window + 2));
    for (int i = 0; i < window + 2; ++i) {
        packets.push_back(initiator.encrypt(string()));
    }
    REQUIRE(responder.decrypt(packets.back()).empty());
    REQUIRE(responder.lastDecryptOk());
    REQUIRE(responder.decrypt(packets.front()).empty());
    REQUIRE_FALSE(responder.lastDecryptOk());
    REQUIRE(responder.decrypt(packets[1]).empty());
    REQUIRE(responder.lastDecryptOk());
    REQUIRE(responder.decrypt(packets[1]).empty());
    REQUIRE_FALSE(responder.lastDecryptOk());
}

TEST_CASE("NoiseDatagram caller-owned UDP", "[noise][datagram]")
{
    shared_ptr<Socket> serverSock(new Socket(HostAddress::IPv4Protocol, Socket::UdpSocket));
    shared_ptr<Socket> clientSock(new Socket(HostAddress::IPv4Protocol, Socket::UdpSocket));
    REQUIRE(serverSock->bind(HostAddress(HostAddress::LocalHost), 0));
    REQUIRE(clientSock->bind(HostAddress(HostAddress::LocalHost), 0));
    const uint16_t serverPort = serverSock->localPort();
    const HostAddress loopback(HostAddress::LocalHost);

    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseDatagram client;
    NoiseDatagram server;
    REQUIRE(client.initialize(makeConfig(NoisePattern::XX, NoiseRole::Initiator, alice)));
    REQUIRE(server.initialize(makeConfig(NoisePattern::XX, NoiseRole::Responder, bob)));

    string msg;
    string payload;
    HostAddress who;
    uint16_t whoPort = 0;

    REQUIRE(client.writeHandshake("hi", &msg));
    REQUIRE(clientSock->sendto(msg, loopback, serverPort) == static_cast<int32_t>(msg.size()));
    msg = serverSock->recvfrom(65535, &who, &whoPort);
    REQUIRE(server.readHandshake(msg, &payload));
    REQUIRE(payload == "hi");

    REQUIRE(server.writeHandshake("hello", &msg));
    REQUIRE(serverSock->sendto(msg, who, whoPort) == static_cast<int32_t>(msg.size()));
    msg = clientSock->recvfrom(65535, &who, &whoPort);
    REQUIRE(client.readHandshake(msg, &payload));
    REQUIRE(payload == "hello");

    REQUIRE(client.writeHandshake(string(), &msg));
    REQUIRE(clientSock->sendto(msg, loopback, serverPort) == static_cast<int32_t>(msg.size()));
    msg = serverSock->recvfrom(65535, &who, &whoPort);
    REQUIRE(server.readHandshake(msg, &payload));
    REQUIRE(client.isHandshakeComplete());
    REQUIRE(server.isHandshakeComplete());

    const string wire = client.encrypt("ping");
    REQUIRE(clientSock->sendto(wire, loopback, serverPort) == static_cast<int32_t>(wire.size()));
    REQUIRE(server.decrypt(serverSock->recvfrom(65535, &who, &whoPort)) == "ping");
}
