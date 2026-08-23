#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "qtng/coroutine.h"
#include "qtng/local_socket.h"
#include "qtng/socket.h"
#include "qtng/utils/platform.h"

#ifdef NG_OS_UNIX
#  include <unistd.h>
#endif
#ifdef NG_OS_WIN
#  include <process.h>
#endif

using namespace std;
using namespace qtng;

namespace {

string testSocketName(const char *tag)
{
    char buf[128];
#ifdef NG_OS_WIN
    snprintf(buf, sizeof(buf), "qtng_local_socket_%d_%s", static_cast<int>(_getpid()), tag);
#else
    snprintf(buf, sizeof(buf), "/tmp/qtng_local_socket_%d_%s.sock", static_cast<int>(getpid()), tag);
#endif
    return string(buf);
}

void removeSocketFile(const string &name)
{
#ifdef NG_OS_UNIX
    unlink(name.c_str());
#else
    (void) name;
#endif
}

const char kEchoMessage[] = "hello local socket";

}  // namespace

TEST_CASE("LocalSocket stream echo", "[local_socket]")
{
    const string path = testSocketName("echo");
    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        unique_ptr<LocalSocket> server(new LocalSocket());
        REQUIRE(server->bind(path));
        REQUIRE(server->listen(16));
        REQUIRE(server->state() == LocalSocket::ListeningState);

        shared_ptr<Coroutine> clientJob(Coroutine::spawn([&] {
            unique_ptr<LocalSocket> client(new LocalSocket());
            REQUIRE(client->connect(path));
            REQUIRE(client->state() == LocalSocket::ConnectedState);
            const int32_t sent = client->sendall(kEchoMessage, static_cast<int32_t>(strlen(kEchoMessage)));
            REQUIRE(sent == static_cast<int32_t>(strlen(kEchoMessage)));
            char buf[128] = {};
            const int32_t n = client->recvall(buf, static_cast<int32_t>(strlen(kEchoMessage)));
            REQUIRE(n == static_cast<int32_t>(strlen(kEchoMessage)));
            REQUIRE(string(buf, static_cast<size_t>(n)) == kEchoMessage);
            client->close();
        }));

        unique_ptr<LocalSocket> conn(server->accept());
        REQUIRE(conn);
        REQUIRE(conn->state() == LocalSocket::ConnectedState);
        char buf[128] = {};
        const int32_t n = conn->recvall(buf, static_cast<int32_t>(strlen(kEchoMessage)));
        REQUIRE(n == static_cast<int32_t>(strlen(kEchoMessage)));
        REQUIRE(string(buf, static_cast<size_t>(n)) == kEchoMessage);
        const int32_t echoed = conn->sendall(buf, n);
        REQUIRE(echoed == n);
        conn->close();
        clientJob->join();
        server->close();
    }));
    job->join();
    removeSocketFile(path);
}

TEST_CASE("LocalSocket large sendall recvall", "[local_socket]")
{
    const string path = testSocketName("large");
    const int32_t payloadSize = 64 * 1024;
    const string payload(static_cast<size_t>(payloadSize), 'z');

    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        unique_ptr<LocalSocket> server(new LocalSocket());
        REQUIRE(server->bind(path));
        REQUIRE(server->listen(16));

        shared_ptr<Coroutine> clientJob(Coroutine::spawn([&] {
            unique_ptr<LocalSocket> client(new LocalSocket());
            REQUIRE(client->connect(path));
            REQUIRE(client->sendall(payload) == payloadSize);
            client->close();
        }));

        unique_ptr<LocalSocket> conn(server->accept());
        REQUIRE(conn);
        string received = conn->recvall(payloadSize);
        REQUIRE(received.size() == payload.size());
        REQUIRE(received == payload);
        conn->close();
        clientJob->join();
        server->close();
    }));
    job->join();
    removeSocketFile(path);
}

TEST_CASE("LocalSocket connect refused", "[local_socket]")
{
    const string path = testSocketName("refused");
    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        unique_ptr<LocalSocket> client(new LocalSocket());
        REQUIRE_FALSE(client->connect(path));
        REQUIRE(client->error() == Socket::ConnectionRefusedError);
    }));
    job->join();
    removeSocketFile(path);
}

TEST_CASE("LocalSocket isValid lifecycle", "[local_socket]")
{
    const string path = testSocketName("valid");
    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        unique_ptr<LocalSocket> client(new LocalSocket());
        REQUIRE_FALSE(client->isValid());  // no descriptor yet

        unique_ptr<LocalSocket> server(new LocalSocket());
        REQUIRE(server->bind(path));
        REQUIRE(server->listen(16));
        REQUIRE(server->isValid());

        shared_ptr<Coroutine> clientJob(Coroutine::spawn([&] {
            unique_ptr<LocalSocket> c(new LocalSocket());
            REQUIRE(c->connect(path));
            REQUIRE(c->isValid());
            c->close();
            REQUIRE_FALSE(c->isValid());
        }));
        unique_ptr<LocalSocket> conn(server->accept());
        REQUIRE(conn);
        REQUIRE(conn->isValid());
        conn->close();
        REQUIRE_FALSE(conn->isValid());
        clientJob->join();
        server->close();
        REQUIRE_FALSE(server->isValid());
    }));
    job->join();
    removeSocketFile(path);
}

TEST_CASE("LocalSocket server file survives peer close", "[local_socket]")
{
    const string path = testSocketName("survive");
    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        unique_ptr<LocalSocket> server(new LocalSocket());
        REQUIRE(server->bind(path));
        REQUIRE(server->listen(16));

        // first client: connect, send, close
        shared_ptr<Coroutine> first(Coroutine::spawn([&] {
            unique_ptr<LocalSocket> client(new LocalSocket());
            REQUIRE(client->connect(path));
            REQUIRE(client->sendall(kEchoMessage, static_cast<int32_t>(strlen(kEchoMessage)))
                    == static_cast<int32_t>(strlen(kEchoMessage)));
            client->close();
        }));
        unique_ptr<LocalSocket> conn(server->accept());
        REQUIRE(conn);
        char buf[128] = {};
        const int32_t n = conn->recvall(buf, static_cast<int32_t>(strlen(kEchoMessage)));
        REQUIRE(n == static_cast<int32_t>(strlen(kEchoMessage)));
        conn->close();
        first->join();

        // second client must still be able to connect: the accept()ed socket's
        // close() must NOT have unlinked the server's socket file.
        shared_ptr<Coroutine> second(Coroutine::spawn([&] {
            unique_ptr<LocalSocket> client(new LocalSocket());
            REQUIRE(client->connect(path));
            client->close();
        }));
        unique_ptr<LocalSocket> conn2(server->accept());
        REQUIRE(conn2);
        conn2->close();
        second->join();
        server->close();
    }));
    job->join();
    removeSocketFile(path);
}

#ifdef NG_OS_UNIX
TEST_CASE("LocalSocket datagram loopback", "[local_socket]")
{
    const string serverPath = testSocketName("dgram_server");
    const string clientPath = testSocketName("dgram_client");
    const char kDatagram[] = "datagram payload";

    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        unique_ptr<LocalSocket> server(new LocalSocket(LocalSocket::DatagramSocket));
        REQUIRE(server->bind(serverPath));
        REQUIRE(server->state() == LocalSocket::BoundState);

        unique_ptr<LocalSocket> client(new LocalSocket(LocalSocket::DatagramSocket));
        REQUIRE(client->bind(clientPath));

        const int32_t sent = client->sendto(kDatagram, static_cast<int32_t>(strlen(kDatagram)), serverPath);
        REQUIRE(sent == static_cast<int32_t>(strlen(kDatagram)));

        string from;
        char buf[128] = {};
        const int32_t n = server->recvfrom(buf, static_cast<int32_t>(sizeof(buf)), &from);
        REQUIRE(n == static_cast<int32_t>(strlen(kDatagram)));
        REQUIRE(string(buf, static_cast<size_t>(n)) == kDatagram);
        REQUIRE(from == clientPath);

        server->close();
        client->close();
    }));
    job->join();
    removeSocketFile(serverPath);
    removeSocketFile(clientPath);
}

TEST_CASE("LocalSocket oversize path", "[local_socket]")
{
    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        string longPath(300, 'x');
        unique_ptr<LocalSocket> s(new LocalSocket());
        REQUIRE_FALSE(s->bind(longPath));
        REQUIRE(s->error() == Socket::SocketAddressNotAvailableError);
    }));
    job->join();
}
#endif  // NG_OS_UNIX
