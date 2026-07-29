#include "common.h"

#include <cstdlib>

#include "qtng/utils/string_utils.h"

using namespace std;
using namespace qtng;
using namespace qtng::utils;

const char *kcptunVersion()
{
    return "kcptun 0.1.0";
}

static bool parsePort(const string &text, uint16_t *port)
{
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed == 0UL || parsed > 65535UL) {
        return false;
    }
    *port = static_cast<uint16_t>(parsed);
    return true;
}

bool parseEndpoint(const string &text, Endpoint *endpoint, string *errorMessage)
{
    if (!endpoint) {
        return false;
    }
    endpoint->host.clear();
    endpoint->address = HostAddress();
    endpoint->port = 0;

    if (text.empty()) {
        if (errorMessage) {
            *errorMessage = "address is empty.";
        }
        return false;
    }

    // Port is always after the last ':'. Bracketed IPv6 works the same way:
    // "[2001:db8::1]:8080" -> host "[2001:db8::1]", port "8080".
    const size_t colon = text.rfind(':');
    if (colon == string::npos) {
        if (errorMessage) {
            *errorMessage = "address `" + text + "` must be host:port.";
        }
        return false;
    }

    string hostPart = text.substr(0, colon);
    const string portPart = text.substr(colon + 1);

    if (!parsePort(portPart, &endpoint->port)) {
        if (errorMessage) {
            *errorMessage = "invalid port in `" + text + "`.";
        }
        return false;
    }

    if (!hostPart.empty() && hostPart[0] == '[') {
        if (hostPart.size() < 2 || hostPart[hostPart.size() - 1] != ']') {
            if (errorMessage) {
                *errorMessage = "invalid IPv6 address `" + text + "`.";
            }
            return false;
        }
        hostPart = hostPart.substr(1, hostPart.size() - 2);
        if (hostPart.empty()) {
            if (errorMessage) {
                *errorMessage = "invalid IPv6 address `" + text + "`.";
            }
            return false;
        }
    }

    if (hostPart.empty()) {
        endpoint->address = HostAddress(HostAddress::Any);
        return true;
    }

    HostAddress address(hostPart);
    if (!address.isNull()) {
        endpoint->address = address;
        endpoint->host = hostPart;
        return true;
    }

    endpoint->host = hostPart;
    return true;
}

bool parseKcpMode(const string &modeStr, KcpSocket::Mode *mode, string *errorMessage)
{
    const string normalized = toLower(modeStr);
    if (normalized == "fast") {
        *mode = KcpSocket::FastInternet;
        return true;
    }
    if (normalized == "normal") {
        *mode = KcpSocket::Internet;
        return true;
    }
    if (normalized == "asymmetric") {
        *mode = KcpSocket::AsymmetricInternet;
        return true;
    }
    if (errorMessage) {
        *errorMessage = "mode `" + modeStr + "` is unknown. choices are `fast`, `normal` and `asymmetric`.";
    }
    return false;
}
