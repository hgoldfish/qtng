#ifndef QTNG_QT_BRIDGE_SOCKET_ACCESS_H
#define QTNG_QT_BRIDGE_SOCKET_ACCESS_H

#include <memory>

#include "config.h"

namespace qtng_core {
class Socket;
class SocketDnsCache;
}

QTNETWORKNG_NAMESPACE_BEGIN
class Socket;
class SocketDnsCache;
QTNETWORKNG_NAMESPACE_END

namespace qtng_bridge {

std::shared_ptr<qtng_core::Socket> socketCoreOf(QTNETWORKNG_NAMESPACE::Socket *socket);
void assignSocketCore(QTNETWORKNG_NAMESPACE::Socket *socket, std::shared_ptr<qtng_core::Socket> core);
std::shared_ptr<qtng_core::SocketDnsCache> dnsCacheCoreOf(QTNETWORKNG_NAMESPACE::SocketDnsCache *cache);
QSharedPointer<QTNETWORKNG_NAMESPACE::SocketDnsCache>
dnsCacheFromCore(const std::shared_ptr<qtng_core::SocketDnsCache> &core);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_SOCKET_ACCESS_H
