#ifndef QTNG_QT_BRIDGE_HOSTADDRESS_ACCESS_H
#define QTNG_QT_BRIDGE_HOSTADDRESS_ACCESS_H

#include "bridge/core_access.h"
#include "hostaddress.h"

namespace qtng_bridge {

// Convert between the Qt binding and core HostAddress. Implemented in qt/src/hostaddress.cpp,
// where the binding type stores the core instance directly, so conversion is a shallow copy.

qtng_core::HostAddress toCoreAddress(const QTNETWORKNG_NAMESPACE::HostAddress &addr);
QTNETWORKNG_NAMESPACE::HostAddress toQtAddress(const qtng_core::HostAddress &addr);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_HOSTADDRESS_ACCESS_H
