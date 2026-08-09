#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

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

void runHandshake(NoiseHandshakeState *initiator, NoiseHandshakeState *responder, NoisePattern pattern,
                  const string &initPayload, const string &respPayload)
{
    string msg;
    string payload;

    REQUIRE(initiator->writeMessage(initPayload, &msg));
    REQUIRE(responder->readMessage(msg, &payload));
    REQUIRE(payload == initPayload);

    REQUIRE(responder->writeMessage(respPayload, &msg));
    REQUIRE(initiator->readMessage(msg, &payload));
    REQUIRE(payload == respPayload);

    if (pattern != NoisePattern::IK) {
        REQUIRE(initiator->writeMessage(string(), &msg));
        REQUIRE(responder->readMessage(msg, &payload));
        REQUIRE(payload.empty());
    }

    REQUIRE(initiator->isComplete());
    REQUIRE(responder->isComplete());
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

}  // namespace

TEST_CASE("NoiseKey generate and DH", "[noise]")
{
    const NoiseKey a = NoiseKey::generate();
    const NoiseKey b = NoiseKey::generate();
    REQUIRE(a.isValid());
    REQUIRE(b.isValid());
    const string ab = NoiseKey::dh(a.privateKey, b.publicKey);
    const string ba = NoiseKey::dh(b.privateKey, a.publicKey);
    REQUIRE(ab.size() == 32);
    REQUIRE(ab == ba);

    const NoiseKey restored = NoiseKey::fromPrivateKey(a.privateKey);
    REQUIRE(restored.isValid());
    REQUIRE(restored.publicKey == a.publicKey);
}

TEST_CASE("Noise XX handshake and transport", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(NoisePattern::XX, NoiseRole::Initiator, alice));
    REQUIRE(responder.initialize(NoisePattern::XX, NoiseRole::Responder, bob));

    runHandshake(&initiator, &responder, NoisePattern::XX, "hi", "hello");
    REQUIRE(initiator.remoteStaticPublic() == bob.publicKey);
    REQUIRE(responder.remoteStaticPublic() == alice.publicKey);
    REQUIRE(initiator.handshakeHash() == responder.handshakeHash());

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

TEST_CASE("Noise PSK_XX handshake", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    const string psk = "shared-secret-psk-bytes!!";
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(NoisePattern::PSK_XX, NoiseRole::Initiator, alice, string(), psk));
    REQUIRE(responder.initialize(NoisePattern::PSK_XX, NoiseRole::Responder, bob, string(), psk));

    runHandshake(&initiator, &responder, NoisePattern::PSK_XX, "a", "b");

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

TEST_CASE("Noise IK handshake", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(NoisePattern::IK, NoiseRole::Initiator, alice, bob.publicKey));
    REQUIRE(responder.initialize(NoisePattern::IK, NoiseRole::Responder, bob));

    runHandshake(&initiator, &responder, NoisePattern::IK, "init", "resp");
    REQUIRE(responder.remoteStaticPublic() == alice.publicKey);

    NoiseCipherState sendI, recvI, sendR, recvR;
    REQUIRE(initiator.split(&sendI, &recvI));
    REQUIRE(responder.split(&sendR, &recvR));
    checkTransport(&sendI, &recvI, &sendR, &recvR);
}

TEST_CASE("Noise rejects remote static mismatch", "[noise]")
{
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();
    const NoiseKey impostor = NoiseKey::generate();
    NoiseHandshakeState initiator;
    NoiseHandshakeState responder;
    REQUIRE(initiator.initialize(NoisePattern::XX, NoiseRole::Initiator, alice, impostor.publicKey));
    REQUIRE(responder.initialize(NoisePattern::XX, NoiseRole::Responder, bob));

    string msg;
    string payload;
    REQUIRE(initiator.writeMessage(string(), &msg));
    REQUIRE(responder.readMessage(msg, &payload));
    REQUIRE(responder.writeMessage(string(), &msg));
    REQUIRE_FALSE(initiator.readMessage(msg, &payload));
    REQUIRE(initiator.errorString().find("mismatch") != string::npos);
}

TEST_CASE("NoiseCipherState rekey and replay window", "[noise]")
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
    REQUIRE_FALSE(b.acceptIncomingNonce(0));

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
}

TEST_CASE("NoiseStream XX over TCP", "[noise]")
{
    ConnectedPair sockets = makeConnectedPair();
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();

    shared_ptr<NoiseStream> client(new NoiseStream(asSocketLike(sockets.clientSide)));
    shared_ptr<NoiseStream> server(new NoiseStream(asSocketLike(sockets.serverSide)));
    REQUIRE(client->initialize(NoisePattern::XX, NoiseRole::Initiator, alice));
    REQUIRE(server->initialize(NoisePattern::XX, NoiseRole::Responder, bob));

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

    REQUIRE(client->sendMessage("ping"));
    REQUIRE(server->recvMessage() == "ping");
    REQUIRE(server->sendall("pong") == 4);
    REQUIRE(client->recv(4) == "pong");
}

TEST_CASE("NoiseStream IK over TCP", "[noise]")
{
    ConnectedPair sockets = makeConnectedPair();
    const NoiseKey alice = NoiseKey::generate();
    const NoiseKey bob = NoiseKey::generate();

    shared_ptr<NoiseStream> client(new NoiseStream(asSocketLike(sockets.clientSide)));
    shared_ptr<NoiseStream> server(new NoiseStream(asSocketLike(sockets.serverSide)));
    REQUIRE(client->initialize(NoisePattern::IK, NoiseRole::Initiator, alice, bob.publicKey));
    REQUIRE(server->initialize(NoisePattern::IK, NoiseRole::Responder, bob));

    bool serverOk = false;
    shared_ptr<Coroutine> serverHs(Coroutine::spawn([&] {
        serverOk = server->handshake();
    }));
    REQUIRE(client->handshake());
    serverHs->join();
    REQUIRE(serverOk);
    REQUIRE(client->sendMessage("ik-data"));
    REQUIRE(server->recvMessage() == "ik-data");
}
