// A complete NoiseDatagram example: a Noise session built on a caller-owned UDP socket.
// NoiseDatagram only does the cryptography and touches no I/O; UDP send/receive is entirely up to you.
// Driving writeHandshake / readHandshake back and forth by hand is tedious, so here "send handshake
// message" and "receive handshake message" are wrapped into an interactive loop, performNoiseHandshake().
// It alternates send/receive by message number (the initiator sends the first message with a payload,
// the responder replies immediately, an empty payload ends the exchange), and works for both 2-message
// (IK/KK) and 3-message (XX/XK) patterns.
//
// This example is written in qtng's coroutine style: no threads are created and no callbacks are used.
// The client and server logic are each encapsulated in runClient() / runServer(), started together by
// CoroutineGroup workers, and main sequentially joins them to drive both ends. Each end generates its
// keys, initializes the session (NoiseDatagram), and creates/binds its UDP socket inside the function;
// the socket's blocking I/O (sendto / recvfrom) is automatically registered with the event loop and
// yields in coroutine context, so both ends alternate on the same event loop without true concurrency.
// The server's bind port is reported to the client via a ValueEvent; after the sequential joins finish,
// main performs the session verification.
//
// Convention: an empty string returned by recv means a failed receive. Neither Noise handshake messages
// nor transport packets (at least [8-byte nonce][ciphertext||tag] = 24 bytes) are empty, so an empty
// string can only mean a recvfrom error or a 0-byte UDP packet from the peer -- both are treated as
// failures.

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "qtng/coroutine_utils.h"
#include "qtng/noise.h"
#include "qtng/socket.h"

using namespace std;
using namespace qtng;

namespace {

// Run one full handshake until isHandshakeComplete() is true on this end.
// The UDP socket is called blocking directly: qtng's recvfrom / sendto are automatically registered with
// the event loop and yield in coroutine context, with no callbacks or threads needed.
//
// Ordering: the initiator first sends a message with a payload, then both sides strictly alternate
// "receive one message -> send one message". Every message carries this end's payload (the initiator's
// closing message is empty). For both 2-message (IK/KK) and 3-message (XX/XK) patterns this loop sends
// and receives exactly all handshake messages.
//
// The meaning of the peer address (peerAddr/peerPort) depends on the role:
//   - Initiator: it is an input holding the fixed server address; the function only reads it.
//   - Responder: it is an output; the first recvfrom brings out the peer address and fills it in, and all
//     subsequent replies are sent to that address.
// Both roles must pass non-null pointers.
bool performNoiseHandshake(NoiseDatagram *session, NoiseRole role, const string &payload, Socket *sock,
                           HostAddress *peerAddr, uint16_t *peerPort)
{
    string packet;
    bool sentPayload = (role == NoiseRole::Initiator);
    if (sentPayload) {
        if (!session->writeHandshake(payload, &packet)
            || sock->sendto(packet, *peerAddr, *peerPort) != int32_t(packet.size())) {
            return false;
        }
    }
    while (!session->isHandshakeComplete()) {
        HostAddress from;
        uint16_t fromPort = 0;
        packet = sock->recvfrom(65535, &from, &fromPort);
        if (packet.empty()) {
            return false;
        }
        if (role == NoiseRole::Responder) {
            *peerAddr = from;  // the first recvfrom brings out the peer address; used for replies
            *peerPort = fromPort;
        }
        if (!session->readHandshake(packet, nullptr)) {
            return false;
        }
        if (session->isHandshakeComplete()) {
            return true;
        }
        if (!session->writeHandshake(sentPayload ? string() : payload, &packet)
            || sock->sendto(packet, *peerAddr, *peerPort) != int32_t(packet.size())) {
            return false;
        }
        sentPayload = true;
    }
    return true;
}

// The output of one end (client or server): the session and static key, used by main for session
// verification after both ends finish. NoiseDatagram is not copyable, so it is held via shared_ptr.
// Returns null on failure.
struct EndpointState
{
    shared_ptr<NoiseDatagram> session;
    NoiseKey key;
};

// Server: generate keys, initialize the session, create and bind a UDP socket, run the responder
// handshake, then actually receive one encrypted message over UDP and decrypt it. The bind port is
// reported through *port (0 means initialization/bind failure) for the client to connect; returns the
// session and key on success, null on failure.
shared_ptr<EndpointState> runServer(ValueEvent<uint16_t> *port)
{
    const shared_ptr<EndpointState> out = make_shared<EndpointState>();
    out->session = make_shared<NoiseDatagram>();
    out->key = NoiseKey::generate();
    NoiseConfig cfg(out->key.privateKey());
    cfg.setRole(NoiseRole::Responder);
    if (!out->session->initialize(cfg)) {
        cerr << "[server] initialize failed: " << out->session->errorString() << endl;
        port->send(0);  // notify the client on failure too (port 0), so it does not wait forever
        return nullptr;
    }

    Socket sock(HostAddress::IPv4Protocol, Socket::UdpSocket);
    if (!sock.bind(HostAddress(HostAddress::LocalHost), 0)) {
        cerr << "[server] bind failed" << endl;
        port->send(0);
        return nullptr;
    }
    port->send(sock.localPort());  // notify the client that it can connect

    HostAddress peer;
    uint16_t peerPort = 0;
    if (!performNoiseHandshake(out->session.get(), NoiseRole::Responder, "server-hello", &sock, &peer, &peerPort)) {
        cerr << "[server] handshake failed: " << out->session->errorString() << endl;
        return nullptr;
    }

    const string packet = sock.recvfrom(65535, &peer, &peerPort);
    if (packet.empty()) {
        cerr << "[server] recvfrom failed" << endl;
        return nullptr;
    }
    const string text = out->session->decrypt(packet);
    if (!out->session->lastDecryptOk()) {
        cerr << "[server] decrypt failed: " << out->session->errorString() << endl;
        return nullptr;
    }
    cout << "[server] got over UDP: " << text << endl;
    return out;
}

// Client: generate keys, initialize the session, create and bind a UDP socket, run the initiator
// handshake, then send one encrypted message to the server. Returns the session and key on success,
// null on failure. Inputs: serverAddr/serverPort are the server address.
shared_ptr<EndpointState> runClient(const HostAddress &serverAddr, uint16_t serverPort)
{
    const shared_ptr<EndpointState> out = make_shared<EndpointState>();
    out->session = make_shared<NoiseDatagram>();
    out->key = NoiseKey::generate();
    NoiseConfig cfg(out->key.privateKey());
    if (!out->session->initialize(cfg)) {
        cerr << "[client] initialize failed: " << out->session->errorString() << endl;
        return nullptr;
    }

    Socket sock(HostAddress::IPv4Protocol, Socket::UdpSocket);
    if (!sock.bind(HostAddress(HostAddress::LocalHost), 0)) {
        cerr << "[client] bind failed" << endl;
        return nullptr;
    }

    // peerAddr/peerPort are inputs for the initiator (the server address); read-only inside the function.
    HostAddress peerAddr = serverAddr;
    uint16_t peerPort = serverPort;
    if (!performNoiseHandshake(out->session.get(), NoiseRole::Initiator, "client-hello", &sock, &peerAddr, &peerPort)) {
        cerr << "[client] handshake failed: " << out->session->errorString() << endl;
        return nullptr;
    }
    const string encrypted = out->session->encrypt("hello-over-udp");
    if (encrypted.empty()) {
        cerr << "[client] encrypt failed: " << out->session->errorString() << endl;
        return nullptr;
    }
    if (sock.sendto(encrypted, serverAddr, serverPort) != int32_t(encrypted.size())) {
        cerr << "[client] sendto failed" << endl;
        return nullptr;
    }
    return out;
}

// Session verification: channel binding, reordering, replay and forgery. Called by main when both end
// sessions are ready -- after the sequential joins, client/server hold their final values. Verification
// advances the nonce state on both ends, but happens after all transport is done, so it affects nothing.
void verifySessions(const EndpointState &client, const EndpointState &server)
{
    cout << "[ok] handshake hash matches: "
         << (client.session->handshakeHash() == server.session->handshakeHash()) << endl;
    cout << "[ok] peer statics: " << (client.session->remoteStaticPublic() == server.key.publicKey()) << "/"
         << (server.session->remoteStaticPublic() == client.key.publicKey()) << endl;

    const string a = client.session->encrypt("ping-1");
    const string b = client.session->encrypt("ping-2");
    cout << "[ok] reordered decrypt: "
         << (server.session->decrypt(b) == "ping-2" && server.session->decrypt(a) == "ping-1") << endl;
    cout << "[ok] replay rejected: "
         << (server.session->decrypt(a).empty() && !server.session->lastDecryptOk()) << endl;
    cout << "[ok] forged rejected: "
         << (server.session->decrypt(string(a.size(), 'x')).empty() && !server.session->lastDecryptOk()) << endl;
    cout << "[ok] server->client: "
         << (client.session->decrypt(server.session->encrypt("pong")) == "pong") << endl;
}

}  // namespace

int main()
{
    // The server's bind port is reported to the client via a ValueEvent (0 means the server failed to
    // initialize/bind). Each end creates its own session, key and socket inside runServer/runClient,
    // with the results assigned to the two shared variables below (null on failure).
    ValueEvent<uint16_t> serverPort;
    shared_ptr<EndpointState> client;
    shared_ptr<EndpointState> server;

    // Start one coroutine per end, managed by a single CoroutineGroup. The main coroutine drives the
    // event loop through join(); during the handshake both ends alternate yielding. Session verification
    // runs in main after both ends finish -- the sequential joins guarantee client/server hold their
    // final values when join returns.
    CoroutineGroup workers;
    const shared_ptr<Coroutine> serverCoro = workers.spawn([&] { server = runServer(&serverPort); });
    const shared_ptr<Coroutine> clientCoro = workers.spawn([&] {
        const uint16_t port = serverPort.tryWait();  // wait for the server bind (0 on failure)
        client = (port == 0) ? nullptr : runClient(HostAddress(HostAddress::LocalHost), port);
    });

    // Drive both ends with sequential joins. The client only depends on the server's port (the server is
    // already spawned and binds on its own), so joining the client first always finishes -- on success
    // the session is stored back, on failure it is null (every failure point returns nullptr).
    clientCoro->join();
    if (!client) {
        // Client failure -> the server is stuck in recvfrom waiting for a message that never comes (or
        // already failed on its own). kill() throws CoroutineExitException to interrupt it; join then
        // returns immediately.
        serverCoro->kill();
    }
    // Client success -> the server is finishing normally (waiting for the last message); join waits for
    // it to complete.
    serverCoro->join();
    if (client && server) {
        verifySessions(*client, *server);
    }
    return (client && server) ? 0 : 1;
}
