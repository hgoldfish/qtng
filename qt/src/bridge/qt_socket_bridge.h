#ifndef QTNG_QT_BRIDGE_SOCKET_BRIDGE_H
#define QTNG_QT_BRIDGE_SOCKET_BRIDGE_H

#include "bridge/stream_bridge.h"
#include "network_interface.h"

namespace qtng_bridge {

inline qtng_core::NetworkInterface toCoreInterface(const QTNETWORKNG_NAMESPACE::NetworkInterface &iface)
{
    if (!iface.isValid()) {
        return qtng_core::NetworkInterface();
    }
    return qtng_core::NetworkInterface::interfaceFromIndex(iface.index());
}

inline QTNETWORKNG_NAMESPACE::NetworkInterface toQtInterface(const qtng_core::NetworkInterface &iface)
{
    if (!iface.isValid()) {
        return QTNETWORKNG_NAMESPACE::NetworkInterface();
    }
    return QTNETWORKNG_NAMESPACE::NetworkInterface::interfaceFromIndex(iface.index());
}

QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> fromCoreSocketLike(const std::shared_ptr<qtng_core::SocketLike> &socket);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_SOCKET_BRIDGE_H
