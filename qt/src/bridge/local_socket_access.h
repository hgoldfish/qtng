#ifndef QTNG_QT_BRIDGE_LOCAL_SOCKET_ACCESS_H
#define QTNG_QT_BRIDGE_LOCAL_SOCKET_ACCESS_H

#include <memory>

#include "config.h"

namespace qtng_core {
class LocalSocket;
}

QTNETWORKNG_NAMESPACE_BEGIN
class LocalSocket;
QTNETWORKNG_NAMESPACE_END

namespace qtng_bridge {

std::shared_ptr<qtng_core::LocalSocket> localSocketCoreOf(QTNETWORKNG_NAMESPACE::LocalSocket *socket);
void assignLocalSocketCore(QTNETWORKNG_NAMESPACE::LocalSocket *socket,
                           std::shared_ptr<qtng_core::LocalSocket> core);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_LOCAL_SOCKET_ACCESS_H
