#ifndef BANDWIDTH_COMMON_H
#define BANDWIDTH_COMMON_H

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "qtng/hostaddress.h"
#include "qtng/utils/string_utils.h"

struct Endpoint {
    std::string host;
    qtng::HostAddress address;
    std::uint16_t port = 0;
};

inline const char *bandwidthVersion()
{
    return "bandwidth 0.1.0";
}

// Parse `host:port`, `[ipv6]:port` or `:port` (empty host -> any address).
inline bool parseEndpoint(const std::string &text, Endpoint *endpoint, std::string *errorMessage)
{
    if (!endpoint) {
        return false;
    }
    endpoint->host.clear();
    endpoint->address = qtng::HostAddress();
    endpoint->port = 0;

    if (text.empty()) {
        if (errorMessage) {
            *errorMessage = "address is empty.";
        }
        return false;
    }

    // The port is always after the last ':'. Bracketed IPv6 works the same way:
    // "[2001:db8::1]:8080" -> host "[2001:db8::1]", port "8080".
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos) {
        if (errorMessage) {
            *errorMessage = "address `" + text + "` must be host:port.";
        }
        return false;
    }

    std::string hostPart = text.substr(0, colon);
    const std::string portPart = text.substr(colon + 1);

    if (portPart.empty()) {
        if (errorMessage) {
            *errorMessage = "invalid port in `" + text + "`.";
        }
        return false;
    }
    char *end = nullptr;
    const unsigned long parsedPort = std::strtoul(portPart.c_str(), &end, 10);
    if (end == portPart.c_str() || *end != '\0' || parsedPort == 0UL || parsedPort > 65535UL) {
        if (errorMessage) {
            *errorMessage = "invalid port in `" + text + "`.";
        }
        return false;
    }
    endpoint->port = static_cast<std::uint16_t>(parsedPort);

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
        endpoint->address = qtng::HostAddress(qtng::HostAddress::Any);
        return true;
    }

    qtng::HostAddress address(hostPart);
    if (!address.isNull()) {
        endpoint->address = address;
        endpoint->host = hostPart;
        return true;
    }

    endpoint->host = hostPart;
    return true;
}

// Bytes as integer, optionally with K/M/G suffix (1024-based), e.g. 32768, 32K, 8M.
// The result must fit in int (Socket::setOption contract).
inline bool parseByteSize(const std::string &text, int *value, std::string *errorMessage = nullptr)
{
    if (text.empty()) {
        if (errorMessage) {
            *errorMessage = "byte size is empty.";
        }
        return false;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str()) {
        if (errorMessage) {
            *errorMessage = "byte size `" + text + "` is invalid.";
        }
        return false;
    }
    unsigned long long multiplier = 1;
    const std::string suffix = qtng::utils::toUpper(qtng::utils::trimmed(std::string(end)));
    if (suffix == "K" || suffix == "KB") {
        multiplier = 1024ULL;
    } else if (suffix == "M" || suffix == "MB") {
        multiplier = 1024ULL * 1024ULL;
    } else if (suffix == "G" || suffix == "GB") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (!suffix.empty()) {
        if (errorMessage) {
            *errorMessage = "byte size `" + text + "` has unknown suffix.";
        }
        return false;
    }
    if (parsed == 0ULL) {
        if (errorMessage) {
            *errorMessage = "byte size `" + text + "` must be greater than zero.";
        }
        return false;
    }
    if (parsed > (static_cast<unsigned long long>(INT_MAX) / multiplier)) {
        if (errorMessage) {
            *errorMessage = "byte size `" + text + "` is too large.";
        }
        return false;
    }
    *value = static_cast<int>(parsed * multiplier);
    return true;
}

#endif  // BANDWIDTH_COMMON_H
