#include <algorithm>
#include <cstdio>

#include "joker_client.h"
#include "joker_config.h"

#ifdef NG_OS_UNIX
#include <sys/resource.h>
#endif

using namespace std;
using namespace qtng;
using namespace qtng::utils;

const string usage = "Usage: joker-client [config-file-path]";

enum ParserResult
{
    Success,
    Failed,
    Help,
    Version,
};

static ParserResult parseArguments(const string &configFilePath, JokerClientConfigure *configure,
                                   shared_ptr<JokerServerConnection> *server, string *errorMessage)
{
    if (configFilePath == "-h") {
        *errorMessage = usage;
        return Help;
    }
    if (configFilePath == "-v") {
        *errorMessage = "joker-client 1.2";
        return Version;
    }

    PosixPath configPath(configFilePath);
    if (!configPath.isReadable()) {
        if (isHostAddress(configFilePath)) {
            *server = make_shared<JokerServerConnection>();
            (*server)->remoteAddress = configFilePath;
            (*server)->name = "kcp";
            return Success;
        }
        *errorMessage = "configure file `" + configFilePath + "` is not readable.";
        return Failed;
    }

    IniConfig settings;
    if (!IniConfig::load(configFilePath, &settings, errorMessage)) {
        return Failed;
    }

    const string password = settings.value("general", "password");
    if (!password.empty()) {
        const string salt("3.14159265358979323846");
        shared_ptr<Cipher> cipher(new Cipher(Cipher::Chacha20, Cipher::CBC, Cipher::Encrypt));
        if (!cipher->setPassword(password, salt, MessageDigest::Sha256, 100000)) {
            *errorMessage = "password `" + password + "` is a bad password.";
            return Failed;
        }
        configure->templateCipher = cipher;
    }

    const string timeoutStr = settings.value("general", "timeout");
    if (!timeoutStr.empty()) {
        bool ok = false;
        configure->timeout = parseFloat(timeoutStr, &ok);
        if (!ok) {
            *errorMessage = "timeout `" + timeoutStr + "` is invalid number.";
            return Failed;
        }
    }

    const string localSocks5AddressStr = settings.value("local/socks5", "address");
    if (!localSocks5AddressStr.empty()) {
        HostAddress address(localSocks5AddressStr);
        if (address.isNull()) {
            *errorMessage = "the local socks5 address `" + localSocks5AddressStr + "` is invalid.";
            return Failed;
        }
        configure->localSocks5Address = address;
    }

    const string localSocks5PortStr = settings.value("local/socks5", "port");
    if (!localSocks5PortStr.empty()) {
        if (!parseUInt16(localSocks5PortStr, &configure->localSocks5Port)) {
            *errorMessage = "the local socks5 port `" + localSocks5PortStr + "` is invalid.";
            return Failed;
        }
    }

    const string localHttpAddressStr = settings.value("local/http", "address");
    if (!localHttpAddressStr.empty()) {
        HostAddress address(localHttpAddressStr);
        if (address.isNull()) {
            *errorMessage = "the local http address `" + localHttpAddressStr + "` is invalid.";
            return Failed;
        }
        configure->localHttpAddress = address;
    }

    const string localHttpPortStr = settings.value("local/http", "port");
    if (!localHttpPortStr.empty()) {
        if (!parseUInt16(localHttpPortStr, &configure->localHttpPort)) {
            *errorMessage = "the local http port `" + localHttpPortStr + "` is invalid.";
            return Failed;
        }
    }

    const string addressStr = settings.value("remote", "address");
    if (addressStr.empty()) {
        *errorMessage = "remote address must be specified.";
        return Failed;
    }

    *server = make_shared<JokerServerConnection>();
    (*server)->remoteAddress = addressStr;

    const string typeStr = toLower(settings.value("remote", "type"));
    if (typeStr.empty()) {
        *errorMessage = "remote has no type. choices are `kcp` and `http`.";
        return Failed;
    }
    if (typeStr == "http") {
        (*server)->type = JokerServerConnection::Http;
        (*server)->name = "http";
    } else if (typeStr == "kcp") {
        (*server)->type = JokerServerConnection::Kcp;
        (*server)->name = "kcp";
    } else {
        *errorMessage = "remote has an unknown type `" + typeStr + "`.";
        return Failed;
    }

    const string portStr = settings.value("remote", "port");
    if (!portStr.empty()) {
        if (!parseUInt16(portStr, &(*server)->remotePort)) {
            *errorMessage = "the remote port `" + portStr + "` is invalid number.";
            return Failed;
        }
    }

    const string mtuStr = settings.value("remote", "mtu");
    if (!mtuStr.empty()) {
        if (!parseUInt16(mtuStr, &(*server)->mtu)) {
            *errorMessage = "the remote MTU `" + mtuStr + "` is invalid number.";
            return Failed;
        }
    }

    if ((*server)->type == JokerServerConnection::Kcp) {
        const string modeStr = settings.value("remote", "mode");
        if (!parseKcpMode(modeStr, &(*server)->mode, errorMessage)) {
            *errorMessage = "the remote kcp mode `" + modeStr + "` is unknown.";
            return Failed;
        }
    }

    return Success;
}

int main(int argc, char **argv)
{
    JokerClientConfigure configure;
    shared_ptr<JokerServerConnection> server;
    string errorMessage;

    if (argc > 1) {
        const string configFilePath = argv[1];
        const ParserResult result = parseArguments(configFilePath, &configure, &server, &errorMessage);
        if (result == Help || result == Version) {
            printf("%s\n", errorMessage.c_str());
            return 0;
        }
        if (result != Success) {
            printf("%s\n", errorMessage.c_str());
            return 1;
        }
    } else {
        printf("%s\n", usage.c_str());
        return 1;
    }

#ifdef NG_OS_UNIX
    struct rlimit r;
    if (getrlimit(RLIMIT_NOFILE, &r) == 0) {
        const rlim_t oldLimit = r.rlim_cur;
        if (oldLimit < 1024 * 8) {
            r.rlim_cur = min<rlim_t>(1024 * 8, r.rlim_max);
            if (setrlimit(RLIMIT_NOFILE, &r) != 0) {
                printf("warning: the limit of open files is too small.");
            }
        }
    }
#endif

    JokerClient client(configure, server);
    if (!client.start()) {
        errorMessage = formatMessage("can not start server. there may be some application use the local port: %1:%2 or %3:%4",
                                     {configure.localSocks5Address.toString(),
                                      number(configure.localSocks5Port),
                                      configure.localHttpAddress.toString(),
                                      number(configure.localHttpPort)});
        printf("%s\n", errorMessage.c_str());
        return 2;
    }

    return 0;
}
