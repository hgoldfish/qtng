#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "3rdparty/libutp/utp.h"
#include "qtng/coroutine.h"
#include "qtng/hostaddress.h"
#include "qtng/socket.h"
#include "qtng/udp.h"

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace std;
using namespace qtng;

namespace {

struct LibutpPeer
{
    utp_context *ctx = nullptr;
    int udpFd = -1;
    atomic<bool> running{true};
    thread pump;
    utp_socket *accepted = nullptr;
    string recvData;
    mutex recvMutex;

    static uint64 utpCallback(utp_callback_arguments *args)
    {
        auto *self = static_cast<LibutpPeer *>(utp_context_get_userdata(args->context));
        switch (args->callback_type) {
        case UTP_SENDTO:
            return static_cast<uint64>(sendto(self->udpFd, args->buf, args->len, 0, args->address, args->address_len));
        case UTP_ON_READ: {
            lock_guard<mutex> lock(self->recvMutex);
            self->recvData.append(reinterpret_cast<const char *>(args->buf), args->len);
            utp_read_drained(args->socket);
            return 0;
        }
        case UTP_ON_STATE_CHANGE:
            if (args->state == UTP_STATE_EOF) {
                utp_close(args->socket);
            }
            return 0;
        case UTP_ON_ACCEPT:
            self->accepted = args->socket;
            utp_set_userdata(args->socket, self);
            return 0;
        case UTP_ON_ERROR:
            return 0;
        case UTP_ON_FIREWALL:
            return 0;
        case UTP_GET_READ_BUFFER_SIZE:
            return 1024 * 1024;
        default:
            return 0;
        }
    }

    void start(uint16_t bindPort)
    {
        udpFd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        REQUIRE(udpFd >= 0);
        sockaddr_in sin {};
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sin.sin_port = htons(bindPort);
        REQUIRE(bind(udpFd, reinterpret_cast<sockaddr *>(&sin), sizeof(sin)) == 0);

        ctx = utp_init(2);
        utp_context_set_userdata(ctx, this);
        utp_set_callback(ctx, UTP_SENDTO, &utpCallback);
        utp_set_callback(ctx, UTP_ON_READ, &utpCallback);
        utp_set_callback(ctx, UTP_ON_STATE_CHANGE, &utpCallback);
        utp_set_callback(ctx, UTP_ON_ACCEPT, &utpCallback);
        utp_set_callback(ctx, UTP_ON_ERROR, &utpCallback);
        utp_set_callback(ctx, UTP_ON_FIREWALL, &utpCallback);
        utp_set_callback(ctx, UTP_GET_READ_BUFFER_SIZE, &utpCallback);

        pump = thread([this] {
            char buf[65536];
            while (running.load()) {
                utp_check_timeouts(ctx);
                sockaddr_in from {};
                socklen_t fromlen = sizeof(from);
                const ssize_t n = recvfrom(udpFd, buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&from),
                                           &fromlen);
                if (n > 0) {
                    utp_process_udp(ctx, reinterpret_cast<const unsigned char *>(buf), static_cast<size_t>(n),
                                    reinterpret_cast<sockaddr *>(&from), fromlen);
                    utp_issue_deferred_acks(ctx);
                } else {
                    this_thread::sleep_for(chrono::milliseconds(5));
                }
            }
        });
    }

    void stop()
    {
        running.store(false);
        if (pump.joinable()) {
            pump.join();
        }
        if (ctx) {
            utp_destroy(ctx);
            ctx = nullptr;
        }
        if (udpFd >= 0) {
            close(udpFd);
            udpFd = -1;
        }
    }
};

}  // namespace

TEST_CASE("UtpSocket interoperates with libutp server", "[utp][libutp]")
{
    shared_ptr<Coroutine> job(Coroutine::spawn([] {
        LibutpPeer peer;
        peer.start(0);
        sockaddr_in bound {};
        socklen_t blen = sizeof(bound);
        getsockname(peer.udpFd, reinterpret_cast<sockaddr *>(&bound), &blen);
        const uint16_t port = ntohs(bound.sin_port);

        unique_ptr<UtpSocket> client(new UtpSocket());
        REQUIRE(client->connect(HostAddress::LocalHost, port));

        const char payload[] = "libutp interoperability";
        REQUIRE(client->sendall(payload, static_cast<int32_t>(strlen(payload)))
                == static_cast<int32_t>(strlen(payload)));


        string got;
        for (int i = 0; i < 500; ++i) {
            {
                lock_guard<mutex> lock(peer.recvMutex);
                got = peer.recvData;
            }
            if (!got.empty()) {
                break;
            }
            this_thread::sleep_for(chrono::milliseconds(10));
        }
        REQUIRE(got == payload);

        client->close();
        peer.stop();
    }));
    job->join();
}

TEST_CASE("libutp client interoperates with UtpSocket server", "[utp][libutp]")
{
    unique_ptr<Socket> probe(new Socket(HostAddress::IPv4Protocol, Socket::UdpSocket));
    if (!probe->bind(HostAddress::Any, 0)) {
        SKIP("UDP bind is not available in this environment");
    }
    probe.reset();

    shared_ptr<Coroutine> job(Coroutine::spawn([] {
        unique_ptr<UtpSocket> server(new UtpSocket());
        REQUIRE(server->bind(HostAddress::Any, 0));
        const uint16_t port = server->localPort();
        REQUIRE(server->listen(4));

        shared_ptr<UtpSocket> accepted;

        LibutpPeer peer;
        peer.start(0);
        utp_socket *sock = utp_create_socket(peer.ctx);
        sockaddr_in remote {};
        remote.sin_family = AF_INET;
        remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        remote.sin_port = htons(port);
        utp_connect(sock, reinterpret_cast<sockaddr *>(&remote), sizeof(remote));

        for (int i = 0; i < 1000 && !accepted; ++i) {
            accepted.reset(server->accept());
            if (!accepted) {
                this_thread::sleep_for(chrono::milliseconds(10));
            }
        }
        REQUIRE(accepted);

        const char payload[] = "libutp client to qtng";
        utp_write(sock, const_cast<char *>(payload), strlen(payload));

        char buf[128] = {};
        const int32_t n = accepted->recvall(buf, static_cast<int32_t>(strlen(payload)));
        REQUIRE(n == static_cast<int32_t>(strlen(payload)));
        REQUIRE(string(buf, static_cast<size_t>(n)) == payload);

        utp_shutdown(sock, SHUT_WR);
        accepted->close();
        peer.stop();
    }));
    job->join();
}
