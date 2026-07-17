using namespace std;

#include <algorithm>
#include <cstdio>

#include "joker_config.h"
#include "joker_server.h"

using namespace qtng;
using namespace qtng::utils;

#ifdef NG_OS_UNIX
#include <sys/resource.h>
#endif


enum ParserResult
{
    Success,
    Failed,
    Help,
    Version,
};

static ParserResult parseArguments(const string &configFilePath, JokerServerConfigure *configure, string *errorMessage)
{
    if (configFilePath == "-h") {
        *errorMessage = "Usage: joker-server [config-file-path]";
        return Help;
    }
    if (configFilePath == "-v") {
        *errorMessage = "joker-server 1.2";
        return Version;
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

    const string kcpAddressStr = settings.value("kcp", "address");
    if (!kcpAddressStr.empty()) {
        HostAddress address(kcpAddressStr);
        if (address.isNull()) {
            *errorMessage = "the kcp address `" + kcpAddressStr + "` is invalid.";
            return Failed;
        }
        configure->kcpAddress = address;
    }

    const string kcpPortStr = settings.value("kcp", "port");
    if (!kcpPortStr.empty()) {
        if (!parseUInt16(kcpPortStr, &configure->kcpPort)) {
            *errorMessage = "the kcp port `" + kcpPortStr + "` is invalid.";
            return Failed;
        }
    }

    const string kcpModeStr = settings.value("kcp", "mode");
    if (!parseKcpMode(kcpModeStr, &configure->kcpMode, errorMessage)) {
        return Failed;
    }

    const string httpAddressStr = settings.value("http", "address");
    if (!httpAddressStr.empty()) {
        HostAddress address(httpAddressStr);
        if (address.isNull()) {
            *errorMessage = "the http address `" + httpAddressStr + "` is invalid.";
            return Failed;
        }
        configure->httpAddress = address;
    }

    const string httpPortStr = settings.value("http", "port");
    if (!httpPortStr.empty()) {
        if (!parseUInt16(httpPortStr, &configure->httpPort)) {
            *errorMessage = "the http port `" + httpPortStr + "` is invalid.";
            return Failed;
        }
    }

    const string httpRootDirStr = settings.value("http", "root");
    if (!httpRootDirStr.empty()) {
        PosixPath rootDir(httpRootDirStr);
        if (!rootDir.isReadable()) {
            *errorMessage = "the http root dir `" + httpRootDirStr + "` is not readable.";
            return Failed;
        }
        configure->httpRootDir = rootDir;
    }

    return Success;
}

int main(int argc, char **argv)
{
    JokerServerConfigure configure;
    string errorMessage;

    if (argc > 1) {
        const string configFilePath = argv[1];
        const ParserResult result = parseArguments(configFilePath, &configure, &errorMessage);
        if (result == Help || result == Version) {
            printf("%s\n", errorMessage.c_str());
            return 0;
        }
        if (result != Success) {
            printf("%s\n", errorMessage.c_str());
            return 1;
        }
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

    JokerServer server(configure);
    if (!server.start()) {
        errorMessage = formatMessage("can not start server. there may be some application use the local port: %1:%2 or %3:%4",
                                     {configure.kcpAddress.toString(),
                                      number(configure.kcpPort),
                                      configure.httpAddress.toString(),
                                      number(configure.httpPort)});
        printf("%s\n", errorMessage.c_str());
        return 2;
    }

    return 0;
}
