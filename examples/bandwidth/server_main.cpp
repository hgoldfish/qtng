#include <cstdint>
#include <cstdio>
#include <string>

#include "common.h"
#include "qtng/coroutine.h"
#include "qtng/random.h"
#include "qtng/socket.h"
#include "qtng/socket_server.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/string_utils.h"

using namespace std;
using namespace qtng;
using namespace qtng::utils;

namespace {

const char *serverUsage =
    "Usage:\n"
    "  bandwidth-server [-l :8000]\n"
    "\n"
    "Options:\n"
    "  -l, --listen   TCP listen address (default: \":8000\")\n"
    "  -h, --help     show help\n"
    "  -v, --version  print version\n";

struct ServerConfig {
    Endpoint listen;
};

enum ParserResult {
    Success,
    Failed,
    Help,
    Version,
};

// One shared random block is enough: the payload is meaningless by design, so
// there is no point in burning CPU on fresh entropy per connection.
const int kPayloadSize = 1024 * 1024;
const int kSendBufferSize = 1024 * 1024;

class BandwidthHandler : public BaseRequestHandler
{
protected:
    void handle() override
    {
        request->setOption(Socket::LowDelayOption, 1);
        request->setOption(Socket::SendBufferSizeSocketOption, kSendBufferSize);

        const string payload = randomBytes(kPayloadSize);
        const string peer = request->peerAddress().toString() + ":" + number(static_cast<int>(request->peerPort()));

        ElapsedTimer interval;
        interval.restart();
        ElapsedTimer total;
        total.restart();
        uint64_t intervalBytes = 0;
        uint64_t totalBytes = 0;

        while (true) {
            const int32_t n = request->sendall(payload.data(), static_cast<int32_t>(payload.size()));
            if (n <= 0) {
                // The client went away or the socket broke; end this connection.
                break;
            }
            intervalBytes += static_cast<uint64_t>(n);
            totalBytes += static_cast<uint64_t>(n);

            const int64_t elapsed = interval.elapsed();
            if (elapsed >= 1000) {
                const double secs = static_cast<double>(elapsed) / 1000.0;
                printf("%s  %.2f MB/s  total %.2f MB\n", peer.c_str(),
                       static_cast<double>(intervalBytes) / secs / (1024.0 * 1024.0),
                       static_cast<double>(totalBytes) / (1024.0 * 1024.0));
                fflush(stdout);
                interval.restart();
                intervalBytes = 0;
            }
        }

        const double secs = static_cast<double>(total.elapsed()) / 1000.0;
        printf("%s  disconnected, sent %.2f MB in %.1fs (avg %.2f MB/s)\n", peer.c_str(),
               static_cast<double>(totalBytes) / (1024.0 * 1024.0), secs,
               secs > 0.0 ? static_cast<double>(totalBytes) / secs / (1024.0 * 1024.0) : 0.0);
        fflush(stdout);
    }
};

ParserResult parseArguments(int argc, char **argv, ServerConfig *config, string *errorMessage)
{
    config->listen.host.clear();
    config->listen.address = HostAddress(HostAddress::Any);
    config->listen.port = 8000;

    for (int i = 1; i < argc; ++i) {
        const string arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            *errorMessage = serverUsage;
            return Help;
        }
        if (arg == "-v" || arg == "--version") {
            *errorMessage = bandwidthVersion();
            return Version;
        }
        if (arg == "-l" || arg == "--listen") {
            if (i + 1 >= argc) {
                *errorMessage = arg + " requires a value.";
                return Failed;
            }
            const string value(argv[++i]);
            if (!parseEndpoint(value, &config->listen, errorMessage)) {
                return Failed;
            }
            if (config->listen.address.isNull()) {
                *errorMessage = "listen `" + value + "` must be an IP or `:port`.";
                return Failed;
            }
        } else {
            *errorMessage = "unknown argument `" + arg + "`.\n" + string(serverUsage);
            return Failed;
        }
    }
    return Success;
}

}  // namespace

int main(int argc, char **argv)
{
    ServerConfig config;
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

    // BaseStreamServer runs every accepted connection in its own coroutine, so
    // multiple clients are served concurrently without extra bookkeeping here.
    typedef TcpServer<BandwidthHandler> BandwidthTcpServer;
    BandwidthTcpServer server(config.listen.address, config.listen.port);

    printf("%s\n", bandwidthVersion());
    printf("listening on %s:%u, streaming random data\n", config.listen.address.toString().c_str(),
           static_cast<unsigned>(config.listen.port));
    fflush(stdout);

    if (!server.serveForever()) {
        fprintf(stderr, "failed to start tcp server.\n");
        return 1;
    }
    return 0;
}
