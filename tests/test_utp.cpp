#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <string>

#include "qtng/coroutine.h"
#include "qtng/coroutine_utils.h"
#include "qtng/hostaddress.h"
#include "qtng/private/kcp.h"
#include "qtng/private/utp.h"
#include "qtng/socket.h"
#include "qtng/udp.h"

using namespace std;
using namespace qtng;

namespace {

const char kPayload[] = "qtng utp loopback payload for testing";

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

bool udpBindWorks()
{
    unique_ptr<Socket> udp(new Socket(HostAddress::IPv4Protocol, Socket::UdpSocket));
    if (!udp->bind(HostAddress::LocalHost, 0)) {
        return false;
    }
    return udp->localPort() != 0;
}

}  // namespace

TEST_CASE("UtpStream paired DatagramLink connect", "[utp]")
{
    shared_ptr<Coroutine> job(Coroutine::spawn([] {
        PairedLink linkA("server");
        PairedLink linkB("client");
        linkA.peer = &linkB;
        linkB.peer = &linkA;

        shared_ptr<DatagramLink> serverLink(&linkA, [](PairedLink *) {});
        shared_ptr<DatagramLink> clientLink(&linkB, [](PairedLink *) {});

        unique_ptr<UtpStream> server(new UtpStream(serverLink));
        unique_ptr<UtpStream> client(new UtpStream(clientLink));
        REQUIRE(server->markBound());
        REQUIRE(server->listen(4));
        Coroutine::msleep(0);
        REQUIRE(client->connect(DatagramPath("server")));
        REQUIRE(client->state() == Socket::ConnectedState);
        if (UtpStream *pending = server->accept()) {
            delete pending;
        }
        client->close();
        server->close();
    }));
    job->join();
}

TEST_CASE("UtpStream paired DatagramLink loopback", "[utp]")
{
    shared_ptr<Coroutine> testJob(Coroutine::spawn([] {
        PairedLink linkA("server");
        PairedLink linkB("client");
        linkA.peer = &linkB;
        linkB.peer = &linkA;

        shared_ptr<DatagramLink> serverLink(&linkA, [](PairedLink *) {});
        shared_ptr<DatagramLink> clientLink(&linkB, [](PairedLink *) {});

        unique_ptr<UtpStream> server(new UtpStream(serverLink));
        unique_ptr<UtpStream> client(new UtpStream(clientLink));
        REQUIRE(server->markBound());
        REQUIRE(server->listen(4));

        REQUIRE(client->connect(DatagramPath("server")));

        UtpStream *accepted = nullptr;
        for (int i = 0; i < 10000 && !accepted; ++i) {
            accepted = server->accept();
            if (!accepted) {
                Coroutine::msleep(0);
            }
        }
        REQUIRE(accepted);

        const int32_t sent = client->sendall(kPayload, static_cast<int32_t>(strlen(kPayload)));
        REQUIRE(sent == static_cast<int32_t>(strlen(kPayload)));

        char buf[128] = {};
        const int32_t n = accepted->recv(buf, static_cast<int32_t>(strlen(kPayload)));
        REQUIRE(n == static_cast<int32_t>(strlen(kPayload)));
        REQUIRE(string(buf, static_cast<size_t>(n)) == kPayload);
        accepted->close();
        delete accepted;
        client->close();
        server->close();
    }));
    testJob->join();
}

TEST_CASE("UtpSocket UDP loopback send recv", "[utp]")
{
    if (!udpBindWorks()) {
        SKIP("UDP bind is not available in this environment");
    }

    unique_ptr<UtpSocket> listener(new UtpSocket());
    REQUIRE(listener->bind(HostAddress::LocalHost, 0));
    const uint16_t port = listener->localPort();
    REQUIRE(port != 0);
    REQUIRE(listener->listen(8));

    shared_ptr<UtpSocket> accepted;
    shared_ptr<Coroutine> acceptor(Coroutine::spawn([&] {
        accepted.reset(listener->accept());
    }));

    shared_ptr<Coroutine> clientJob(Coroutine::spawn([&] {
        unique_ptr<UtpSocket> client(new UtpSocket());
        REQUIRE(client->connect(HostAddress::LocalHost, port));
        const int32_t sent = client->sendall(kPayload, static_cast<int32_t>(strlen(kPayload)));
        REQUIRE(sent == static_cast<int32_t>(strlen(kPayload)));
        client->close();
    }));

    clientJob->join();
    acceptor->join();
    REQUIRE(accepted);

    char buf[256] = {};
    const int32_t received = accepted->recvall(buf, static_cast<int32_t>(strlen(kPayload)));
    REQUIRE(received == static_cast<int32_t>(strlen(kPayload)));
    REQUIRE(string(buf, static_cast<size_t>(received)) == string(kPayload, static_cast<size_t>(strlen(kPayload))));

    accepted->close();
}
