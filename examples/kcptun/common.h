#ifndef KCPTUN_COMMON_H
#define KCPTUN_COMMON_H

#include <cstdint>
#include <string>

#include "qtng/hostaddress.h"
#include "qtng/kcp.h"

struct Endpoint {
    std::string host;
    qtng::HostAddress address;
    std::uint16_t port = 0;
};

bool parseEndpoint(const std::string &text, Endpoint *endpoint, std::string *errorMessage);
bool parseKcpMode(const std::string &modeStr, qtng::KcpSocket::Mode *mode, std::string *errorMessage);

const char *kcptunVersion();

#endif  // KCPTUN_COMMON_H
