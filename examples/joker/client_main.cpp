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
                                   vector<shared_ptr<JokerServerConnection>> *servers, string *errorMessage)
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
            shared_ptr<JokerServerConnection> kcpServer(new JokerServerConnection());
            kcpServer->remoteAddress = configFilePath;
            kcpServer->name = "kcp";
            servers->push_back(kcpServer);
            shared_ptr<JokerServerConnection> httpServer(new JokerServerConnection());
            httpServer->type = JokerServerConnection::Http;
            httpServer->remoteAddress = configFilePath;
            httpServer->name = "http";
            servers->push_back(httpServer);
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

    const string maxWeightStr = settings.value("general", "maxweight");
    if (!maxWeightStr.empty()) {
        bool ok = false;
        int weight = parseInt(maxWeightStr, &ok);
        if (!ok) {
            *errorMessage = "max weight `" + maxWeightStr + "` is invalid number.";
            return Failed;
        }
        if (weight < 1) {
            *errorMessage = "the max weight should ge greater than 1.";
            return Failed;
        }
        configure->maxWeight = weight;
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

    for (const string &childGroup : settings.childGroups("remote")) {
        const string section = "remote/" + childGroup;
        shared_ptr<JokerServerConnection> server(new JokerServerConnection());
        server->name = childGroup;

        const string addressStr = settings.value(section, "address");
        if (addressStr.empty()) {
            continue;
        }
        server->remoteAddress = addressStr;

        const string typeStr = toLower(settings.value(section, "type"));
        if (typeStr.empty()) {
            *errorMessage = "remote `" + childGroup + "` has an no type. choices are `kcp` and `http`.";
            return Failed;
        }
        if (typeStr == "http") {
            server->type = JokerServerConnection::Http;
        } else if (typeStr == "kcp") {
            server->type = JokerServerConnection::Kcp;
        } else {
            *errorMessage = "remote `" + childGroup + "` has an unknown type `" + typeStr + "`.";
            return Failed;
        }

        const string portStr = settings.value(section, "port");
        if (!portStr.empty()) {
            if (!parseUInt16(portStr, &server->remotePort)) {
                *errorMessage = "the port `" + portStr + "` of remote `" + childGroup + "` is invalid number.";
                return Failed;
            }
        }

        const string mtuStr = settings.value(section, "mtu");
        if (!mtuStr.empty()) {
            if (!parseUInt16(mtuStr, &server->mtu)) {
                *errorMessage = "the MTU `" + mtuStr + "` of remote `" + childGroup + "` is invalid number.";
                return Failed;
            }
        }

        const string weightStr = settings.value(section, "weight");
        if (!weightStr.empty()) {
            bool ok = false;
            int weight = parseInt(weightStr, &ok);
            if (!ok) {
                *errorMessage = "the weight `" + weightStr + "` of remote `" + childGroup + "` is invalid number.";
                return Failed;
            }
            if (weight < 1) {
                *errorMessage = "the weight of remote `" + childGroup + "` should ge greater than 1.";
                return Failed;
            }
            if (weight > configure->maxWeight) {
                *errorMessage = "the weight of remote `" + childGroup + "` should not greater than "
                                + number(configure->maxWeight) + ".";
                return Failed;
            }
            server->weight = weight;
        }

        if (server->type == JokerServerConnection::Kcp) {
            const string modeStr = settings.value(section, "mode");
            if (!parseKcpMode(modeStr, &server->mode, errorMessage)) {
                *errorMessage = "the kcp mode `" + modeStr + "` of remote `" + childGroup + "` is unknown.";
                return Failed;
            }
        }

        servers->push_back(server);
    }

    if (servers->empty()) {
        *errorMessage = "either remote http or remote kcp address must be specified.";
        return Failed;
    }

    return Success;
}

int main(int argc, char **argv)
{
    JokerClientConfigure configure;
    vector<shared_ptr<JokerServerConnection>> servers;
    string errorMessage;

    if (argc > 1) {
        const string configFilePath = argv[1];
        const ParserResult result = parseArguments(configFilePath, &configure, &servers, &errorMessage);
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

    JokerClient client(configure, servers);
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
