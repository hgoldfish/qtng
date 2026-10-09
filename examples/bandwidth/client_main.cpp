#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "qtng/coroutine.h"
#include "qtng/socket.h"
#include "qtng/utils/datetime.h"

using namespace std;
using namespace qtng;
using namespace qtng::utils;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handleSignal(int)
{
    g_stop = 1;
}

const char *clientUsage =
    "Usage:\n"
    "  bandwidth-client -r HOST:PORT [-b 8M]\n"
    "\n"
    "Options:\n"
    "  -r, --remote   server address (required)\n"
    "  -b, --rate     target download rate, e.g. 8M (default: 8M)\n"
    "  -h, --help     show help\n"
    "  -v, --version  print version\n";

struct ClientConfig {
    Endpoint remote;
    int rate = 8 * 1024 * 1024;
};

enum ParserResult {
    Success,
    Failed,
    Help,
    Version,
};

ParserResult parseArguments(int argc, char **argv, ClientConfig *config, string *errorMessage)
{
    config->remote.host.clear();
    config->remote.address = HostAddress();
    config->remote.port = 0;

    bool hasRemote = false;
    for (int i = 1; i < argc; ++i) {
        const string arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            *errorMessage = clientUsage;
            return Help;
        }
        if (arg == "-v" || arg == "--version") {
            *errorMessage = bandwidthVersion();
            return Version;
        }
        if (arg == "-r" || arg == "--remote") {
            if (i + 1 >= argc) {
                *errorMessage = arg + " requires a value.";
                return Failed;
            }
            const string value(argv[++i]);
            if (!parseEndpoint(value, &config->remote, errorMessage)) {
                return Failed;
            }
            hasRemote = true;
        } else if (arg == "-b" || arg == "--rate") {
            if (i + 1 >= argc) {
                *errorMessage = arg + " requires a value.";
                return Failed;
            }
            const string value(argv[++i]);
            if (!parseByteSize(value, &config->rate, errorMessage)) {
                return Failed;
            }
        } else {
            *errorMessage = "unknown argument `" + arg + "`.\n" + string(clientUsage);
            return Failed;
        }
    }

    if (!hasRemote) {
        *errorMessage = "remote (-r) is required.\n" + string(clientUsage);
        return Failed;
    }
    return Success;
}

// Bound the connect so an unreachable server cannot park the client forever:
// the raw connect has no timeout, so a black-hole address would otherwise make
// Ctrl-C a no-op for the OS default (minutes).
const float kConnectTimeoutSecs = 5.0f;

shared_ptr<Socket> connectRemote(const ClientConfig &config, string *errorMessage)
{
    shared_ptr<Socket> socket;
    try {
        Timeout connectTimeout(kConnectTimeoutSecs);
        (void) connectTimeout;
        if (!config.remote.host.empty()) {
            socket.reset(Socket::createConnection(config.remote.host, config.remote.port));
        } else {
            socket.reset(Socket::createConnection(config.remote.address, config.remote.port));
        }
    } catch (TimeoutException &) {
        *errorMessage = "connecting to the bandwidth server timed out.";
        return shared_ptr<Socket>();
    }
    if (!socket) {
        *errorMessage = "failed to connect the bandwidth server.";
    }
    return socket;
}

}  // namespace

int main(int argc, char **argv)
{
    ClientConfig config;
    string message;
    const ParserResult result = parseArguments(argc, argv, &config, &message);
    if (result == Help || result == Version) {
        printf("%s\n", message.c_str());
        return 0;
    }
    if (result != Success) {
        fprintf(stderr, "%s\n", message.c_str());
        return 1;
    }

    std::signal(SIGINT, handleSignal);

    shared_ptr<Socket> socket = connectRemote(config, &message);
    if (!socket) {
        fprintf(stderr, "%s\n", message.c_str());
        return 1;
    }

    printf("%s\n", bandwidthVersion());
    printf("connected to %s, target rate %.2f MB/s\n", socket->peerAddressURI().c_str(),
           static_cast<double>(config.rate) / (1024.0 * 1024.0));
    fflush(stdout);

    const int32_t chunk = 256 * 1024;
    vector<char> buffer(chunk);

    ElapsedTimer interval;
    interval.restart();
    ElapsedTimer total;
    total.restart();
    uint64_t intervalBytes = 0;
    uint64_t totalBytes = 0;

    // Read as fast as the kernel lets us, then sleep until the cumulative
    // schedule `totalBytes / rate` says we are back on pace. This self-corrects
    // instead of accumulating error, and once our receive buffer fills the TCP
    // window closes and the server's sendall() blocks -- the throttle lives in
    // back-pressure, not in the server.
    //
    // Both the read and the sleep are bounded so Ctrl-C is honored even if the
    // peer stalls or the rate is very low: recv() would otherwise park forever
    // on a peer that stops sending without closing.
    while (!g_stop) {
        int32_t n = 0;
        try {
            Timeout wake(1.0f);
            (void) wake;
            n = socket->recv(buffer.data(), chunk);
        } catch (TimeoutException &) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        totalBytes += static_cast<uint64_t>(n);
        intervalBytes += static_cast<uint64_t>(n);

        // Throttle in short, interruptible naps until cumulative progress
        // `totalBytes` is back on the `totalBytes / rate` schedule. Slicing the
        // wait keeps Ctrl-C responsive even at very low rates; the schedule
        // stays exact because no read happens between naps.
        while (!g_stop) {
            const double expectedSecs = static_cast<double>(totalBytes) / static_cast<double>(config.rate);
            const double elapsedSecs = static_cast<double>(total.elapsedMicroseconds()) / 1000000.0;
            const double remainingSecs = expectedSecs - elapsedSecs;
            if (remainingSecs <= 0.0) {
                break;
            }
            const uint32_t napMs = remainingSecs > 0.2 ? 200 : static_cast<uint32_t>(remainingSecs * 1000.0);
            if (napMs == 0) {
                break;
            }
            Coroutine::msleep(napMs);
        }

        const int64_t elapsed = interval.elapsed();
        if (elapsed >= 1000) {
            const double secs = static_cast<double>(elapsed) / 1000.0;
            printf("%.2f MB/s  total %.2f MB\n", static_cast<double>(intervalBytes) / secs / (1024.0 * 1024.0),
                   static_cast<double>(totalBytes) / (1024.0 * 1024.0));
            fflush(stdout);
            interval.restart();
            intervalBytes = 0;
        }
    }

    const double secs = static_cast<double>(total.elapsed()) / 1000.0;
    printf("downloaded %.2f MB in %.1fs (avg %.2f MB/s)\n", static_cast<double>(totalBytes) / (1024.0 * 1024.0),
           secs, secs > 0.0 ? static_cast<double>(totalBytes) / secs / (1024.0 * 1024.0) : 0.0);
    return 0;
}
