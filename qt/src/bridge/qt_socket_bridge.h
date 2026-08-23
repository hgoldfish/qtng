#ifndef QTNG_QT_BRIDGE_SOCKET_BRIDGE_H
#define QTNG_QT_BRIDGE_SOCKET_BRIDGE_H

#include "bridge/stream_bridge.h"
#include "network_interface.h"

namespace qtng_bridge {

// Implemented in qt/src/network_interface.cpp, where the binding NetworkInterface keeps the
// core instance directly, so conversion is a shallow copy.

qtng_core::NetworkInterface toCoreInterface(const QTNETWORKNG_NAMESPACE::NetworkInterface &iface);
QTNETWORKNG_NAMESPACE::NetworkInterface toQtInterface(const qtng_core::NetworkInterface &iface);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_SOCKET_BRIDGE_H
