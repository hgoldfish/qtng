#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include "qtng/coroutine.h"
#include "qtng/hostaddress.h"
#include "qtng/quic.h"
#include "qtng/socket.h"

using namespace std;
using namespace qtng;

namespace {

string envOrEmpty(const char *name)
{
    const char *v = getenv(name);
    return v ? string(v) : string();
}

string picoquicDemoPath()
{
    string p = envOrEmpty("QTNG_PICOQUICDEMO");
    return p.empty() ? string("/tmp/picoquic-build/picoquicdemo") : p;
}

string picoquicCertDir()
{
    string p = envOrEmpty("QTNG_PICOQUIC_CERTDIR");
    return p.empty() ? string("/tmp/picoquic-certs") : p;
}

bool fileExists(const string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

bool picoquicAvailable()
{
    return fileExists(picoquicDemoPath()) && fileExists(picoquicCertDir() + "/cert.pem")
            && fileExists(picoquicCertDir() + "/key.pem");
}

uint16_t pickFreeUdpPort()
{
    unique_ptr<Socket> s(new Socket(HostAddress::IPv4Protocol, Socket::UdpSocket));
    // Must wrap SpecialAddress in HostAddress: bind(AnyIPv4, 0) otherwise resolves to
    // bind(uint16_t port=6, BindMode) via enum→integer conversion (privileged port).
    REQUIRE(s->bind(HostAddress(HostAddress::AnyIPv4), 0));
    const uint16_t port = s->localPort();
    REQUIRE(port != 0);
    return port;
}

#ifndef _WIN32
struct PicoquicServer
{
    pid_t pid = -1;
    uint16_t port = 0;

    bool start(const string &alpn)
    {
        port = pickFreeUdpPort();
        pid = fork();
        if (pid < 0) {
            return false;
        }
        if (pid == 0) {
            const string cert = picoquicCertDir() + "/cert.pem";
            const string key = picoquicCertDir() + "/key.pem";
            const string portStr = to_string(port);
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > STDERR_FILENO) {
                    close(fd);
                }
            }
            execl(picoquicDemoPath().c_str(), "picoquicdemo", "-c", cert.c_str(), "-k", key.c_str(), "-p",
                  portStr.c_str(), "-a", alpn.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        this_thread::sleep_for(chrono::milliseconds(400));
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            pid = -1;
            return false;
        }
        return r == 0;
    }

    ~PicoquicServer()
    {
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, 0);
            pid = -1;
        }
    }
};
#endif

}  // namespace

TEST_CASE("qtng client handshakes with picoquicdemo server", "[quic][interop][picoquic]")
{
#ifndef _WIN32
    if (!picoquicAvailable()) {
        SKIP("Build picoquicdemo and set QTNG_PICOQUICDEMO / QTNG_PICOQUIC_CERTDIR "
             "(defaults: /tmp/picoquic-build/picoquicdemo, /tmp/picoquic-certs)");
    }

    PicoquicServer server;
    REQUIRE(server.start("hq-interop"));

    shared_ptr<Coroutine> job(Coroutine::spawn([&] {
        QuicConnection client(HostAddress::IPv4Protocol);
        QuicConfiguration cfg;
        cfg.setVerifyPeer(false);
        cfg.setAlpnProtocols({"hq-interop"});
        client.setConfiguration(cfg);

        const bool ok = client.connect(HostAddress::LocalHost, server.port, "localhost");
        INFO("error=" << client.errorString());
        REQUIRE(ok);
        REQUIRE(client.state() == QuicConnection::ConnectedState);
        REQUIRE(client.error() == QuicConnection::NoError);
        client.close();
    }));
    job->join();
#else
    SKIP("picoquic interop test is Unix-only");
#endif
}
