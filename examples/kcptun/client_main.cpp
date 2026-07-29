#include <cstdio>
#include <memory>
#include <string>

#include "common.h"
#include "qtng/locks.h"
#include "qtng/multi_stream.h"
#include "qtng/qtng.h"
#include "qtng/socket_server.h"
#include "qtng/socket_utils.h"
#include "qtng/udp.h"

using namespace std;
using namespace qtng;

namespace {

const char *clientUsage =
    "Usage:\n"
    "  kcptun-client -l \":12948\" -r \"HOST:29900\" [-mode fast|normal]\n"
    "\n"
    "Options:\n"
    "  -l, --localaddr   local TCP listen address (default: \":12948\")\n"
    "  -r, --remoteaddr  kcp server address (required)\n"
    "  -mode             kcp profile: fast, normal (default: fast)\n"
    "  -h, --help        show help\n"
    "  -v, --version     print version\n";

struct ClientConfig {
    Endpoint local;
    Endpoint remote;
    KcpSocket::Mode mode = KcpSocket::FastInternet;
};

struct ClientContext {
    MultiStreamMaster *master = nullptr;
    Lock lock;
};

class LocalTcpHandler : public BaseRequestHandler
{
protected:
    void handle() override
    {
        ClientContext *ctx = userData<ClientContext>();
        if (!ctx || !ctx->master) {
            return;
        }

        shared_ptr<MultiStreamSlave> slave;
        {
            ScopedLock<Lock> guard(ctx->lock);
            if (!guard.isSuccess()) {
                return;
            }
            slave = ctx->master->makeSlave();
        }
        if (!slave || slave->isBroken()) {
            return;
        }

        Exchanger exchanger(request, asSocketLike(slave), 1024 * 1024 * 8);
        exchanger.exchange();
    }
};

enum ParserResult {
    Success,
    Failed,
    Help,
    Version,
};

ParserResult parseArguments(int argc, char **argv, ClientConfig *config, string *errorMessage)
{
    config->local.host.clear();
    config->local.address = HostAddress(HostAddress::Any);
    config->local.port = 12948;
    config->remote = Endpoint();
    config->mode = KcpSocket::FastInternet;

    bool hasRemote = false;

    for (int i = 1; i < argc; ++i) {
        const string arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            *errorMessage = clientUsage;
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

        if (arg == "-l" || arg == "--localaddr") {
            if (!takeValue(arg)) {
                return Failed;
            }
            if (!parseEndpoint(value, &config->local, errorMessage)) {
                return Failed;
            }
            if (config->local.address.isNull()) {
                *errorMessage = "localaddr `" + value + "` must be an IP or `:port` for listening.";
                return Failed;
            }
        } else if (arg == "-r" || arg == "--remoteaddr") {
            if (!takeValue(arg)) {
                return Failed;
            }
            if (!parseEndpoint(value, &config->remote, errorMessage)) {
                return Failed;
            }
            if (config->remote.host.empty() && config->remote.address.isNull()) {
                *errorMessage = "remoteaddr `" + value + "` is invalid.";
                return Failed;
            }
            hasRemote = true;
        } else if (arg == "-mode" || arg == "--mode") {
            if (!takeValue(arg)) {
                return Failed;
            }
            if (!parseKcpMode(value, &config->mode, errorMessage)) {
                return Failed;
            }
        } else {
            *errorMessage = "unknown argument `" + arg + "`.\n" + clientUsage;
            return Failed;
        }
    }

    if (!hasRemote) {
        *errorMessage = "remoteaddr (-r) is required.\n" + string(clientUsage);
        return Failed;
    }
    return Success;
}

shared_ptr<KcpSocket> connectRemote(const ClientConfig &config, string *errorMessage)
{
    shared_ptr<KcpSocket> kcp;
    if (!config.remote.host.empty()) {
        kcp.reset(KcpSocket::createConnection(config.remote.host, config.remote.port));
    } else {
        kcp.reset(KcpSocket::createConnection(config.remote.address, config.remote.port));
    }
    if (!kcp) {
        *errorMessage = "failed to connect kcp server.";
        return shared_ptr<KcpSocket>();
    }
    kcp->setMode(config.mode);
    return kcp;
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

    shared_ptr<KcpSocket> kcp = connectRemote(config, &message);
    if (!kcp) {
        fprintf(stderr, "%s\n", message.c_str());
        return 1;
    }

    MultiStreamMaster master(kcp, MultiStreamPositivePole);
    master.setKeepaliveTimeout(30.0f);
    master.setPayloadSizeHint(kcp->payloadSizeHint());

    ClientContext ctx;
    ctx.master = &master;

    typedef TcpServer<LocalTcpHandler> LocalTcpServer;
    LocalTcpServer server(config.local.address, config.local.port);
    server.setUserData(&ctx);

    const string remoteDesc =
        !config.remote.host.empty() ? config.remote.host : config.remote.address.toString();
    printf("%s\n", kcptunVersion());
    printf("listening on %s:%u, remote %s:%u\n",
           config.local.address.toString().c_str(),
           static_cast<unsigned>(config.local.port),
           remoteDesc.c_str(),
           static_cast<unsigned>(config.remote.port));

    if (!server.serveForever()) {
        fprintf(stderr, "failed to start local tcp server.\n");
        return 1;
    }
    return 0;
}
