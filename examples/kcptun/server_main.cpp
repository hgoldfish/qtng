#include <cstdio>
#include <memory>
#include <string>

#include "common.h"
#include "qtng/coroutine_utils.h"
#include "qtng/httpd.h"
#include "qtng/io_utils.h"
#include "qtng/multi_stream.h"
#include "qtng/qtng.h"
#include "qtng/socket_server.h"
#include "qtng/socket_utils.h"
#include "qtng/kcp.h"

using namespace std;
using namespace qtng;

namespace {

const char *serverUsage =
    "Usage:\n"
    "  kcptun-server -t \"127.0.0.1:22\" [-mode fast|normal]\n"
    "  kcptun-server -r \"/path/to/webroot\" [-mode fast|normal]\n"
    "\n"
    "Options:\n"
    "  -l, --listen   kcp server listen address (default: \":8000\")\n"
    "  -t, --target   target tcp server address (default: \"127.0.0.1:22\")\n"
    "  -r, --webroot  serve static files from this directory instead of -t\n"
    "  -mode          kcp profile: fast, normal (default: fast)\n"
    "  -h, --help     show help\n"
    "  -v, --version  print version\n"
    "\n"
    "Modes:\n"
    "  -t forwards each multiplexed stream to a TCP target.\n"
    "  -r runs a built-in static httpd on each stream (mutually exclusive with -t).\n";

struct ServerConfig {
    Endpoint listen;
    Endpoint target;
    PosixPath webRoot;
    bool httpdMode = false;
    KcpSocket::Mode mode = KcpSocket::FastInternet;
};

struct ServerContext {
    ServerConfig config;
    CoroutineGroup operations;
};

void exchangeSlave(shared_ptr<MultiStreamSlave> slave, const Endpoint &target)
{
    shared_ptr<Socket> forward;
    if (!target.host.empty()) {
        forward.reset(Socket::createConnection(target.host, target.port));
    } else {
        forward.reset(Socket::createConnection(target.address, target.port));
    }
    if (!forward) {
        slave->abort();
        return;
    }

    Exchanger exchanger(asSocketLike(slave), asSocketLike(forward), 1024 * 1024 * 8);
    exchanger.exchange();
}

void serveHttpSlave(shared_ptr<MultiStreamSlave> slave, const PosixPath &webRoot)
{
    SimpleHttpRequestHandler handler;
    handler.setRootDir(webRoot);
    handler.request = asSocketLike(slave);
    handler.run();
}

class KcpSessionHandler : public BaseRequestHandler
{
protected:
    void handle() override
    {
        ServerContext *ctx = userData<ServerContext>();
        if (!ctx) {
            return;
        }

        shared_ptr<KcpSocket> kcp = convertSocketLikeToKcpSocket(request);
        if (!kcp) {
            return;
        }
        kcp->setMode(ctx->config.mode);

        MultiStreamMaster master(kcp, MultiStreamNegativePole);
        master.setKeepaliveTimeout(30.0f);
        master.setPayloadSizeHint(kcp->payloadSizeHint());

        while (true) {
            shared_ptr<MultiStreamSlave> slave = master.takeSlave();
            if (!slave) {
                return;
            }
            if (ctx->config.httpdMode) {
                const PosixPath webRoot = ctx->config.webRoot;
                ctx->operations.spawn([slave, webRoot] {
                    serveHttpSlave(slave, webRoot);
                });
            } else {
                const Endpoint target = ctx->config.target;
                ctx->operations.spawn([slave, target] {
                    exchangeSlave(slave, target);
                });
            }
        }
    }
};

enum ParserResult {
    Success,
    Failed,
    Help,
    Version,
};

ParserResult parseArguments(int argc, char **argv, ServerConfig *config, string *errorMessage)
{
    config->listen.host.clear();
    config->listen.address = HostAddress(HostAddress::Any);
    config->listen.port = 8000;
    config->target.host = "127.0.0.1";
    config->target.address = HostAddress("127.0.0.1");
    config->target.port = 22;
    config->webRoot = PosixPath();
    config->httpdMode = false;
    config->mode = KcpSocket::FastInternet;

    bool hasTarget = false;
    bool hasWebRoot = false;

    for (int i = 1; i < argc; ++i) {
        const string arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            *errorMessage = serverUsage;
            return Help;
        }
        if (arg == "-v" || arg == "--version") {
            *errorMessage = kcptunVersion();
            return Version;
        }

        string value;
        auto takeValue = [&](const string &name) -> bool {
            if (i + 1 >= argc) {
                *errorMessage = name + " requires a value.";
                return false;
            }
            value = argv[++i];
            return true;
        };

        if (arg == "-l" || arg == "--listen") {
            if (!takeValue(arg)) {
                return Failed;
            }
            if (!parseEndpoint(value, &config->listen, errorMessage)) {
                return Failed;
            }
            if (config->listen.address.isNull()) {
                *errorMessage = "listen `" + value + "` must be an IP or `:port`.";
                return Failed;
            }
        } else if (arg == "-t" || arg == "--target") {
            if (!takeValue(arg)) {
                return Failed;
            }
            if (!parseEndpoint(value, &config->target, errorMessage)) {
                return Failed;
            }
            if (config->target.host.empty() && config->target.address.isNull()) {
                *errorMessage = "target `" + value + "` is invalid.";
                return Failed;
            }
            hasTarget = true;
        } else if (arg == "-r" || arg == "--webroot") {
            if (!takeValue(arg)) {
                return Failed;
            }
            PosixPath root(value);
            if (!root.isDir() || !root.isReadable()) {
                *errorMessage = "webroot `" + value + "` is not a readable directory.";
                return Failed;
            }
            config->webRoot = root;
            hasWebRoot = true;
        } else if (arg == "-mode" || arg == "--mode") {
            if (!takeValue(arg)) {
                return Failed;
            }
            if (!parseKcpMode(value, &config->mode, errorMessage)) {
                return Failed;
            }
        } else {
            *errorMessage = "unknown argument `" + arg + "`.\n" + serverUsage;
            return Failed;
        }
    }

    if (hasTarget && hasWebRoot) {
        *errorMessage = "-t/--target and -r/--webroot are mutually exclusive.\n" + string(serverUsage);
        return Failed;
    }
    config->httpdMode = hasWebRoot;
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

    ServerContext ctx;
    ctx.config = config;

    typedef KcpServer<KcpSessionHandler> KcptunKcpServer;
    KcptunKcpServer server(config.listen.address, config.listen.port);
    server.setUserData(&ctx);

    printf("%s\n", kcptunVersion());
    if (config.httpdMode) {
        printf("listening on %s:%u, webroot %s\n",
               config.listen.address.toString().c_str(),
               static_cast<unsigned>(config.listen.port),
               config.webRoot.path().c_str());
    } else {
        const string targetDesc =
            !config.target.host.empty() ? config.target.host : config.target.address.toString();
        printf("listening on %s:%u, target %s:%u\n",
               config.listen.address.toString().c_str(),
               static_cast<unsigned>(config.listen.port),
               targetDesc.c_str(),
               static_cast<unsigned>(config.target.port));
    }

    if (!server.serveForever()) {
        fprintf(stderr, "failed to start kcp server.\n");
        return 1;
    }
    return 0;
}
