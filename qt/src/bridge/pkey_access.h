#ifndef QTNG_QT_BRIDGE_PKEY_ACCESS_H
#define QTNG_QT_BRIDGE_PKEY_ACCESS_H

#include "config.h"

namespace qtng_core {
class PrivateKey;
}

QTNETWORKNG_NAMESPACE_BEGIN
class PrivateKey;
QTNETWORKNG_NAMESPACE_END

namespace qtng_bridge {

QTNETWORKNG_NAMESPACE::PrivateKey toQtPrivateKey(const qtng_core::PrivateKey &key);
const qtng_core::PrivateKey &privateKeyCoreOf(const QTNETWORKNG_NAMESPACE::PrivateKey &key);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_PKEY_ACCESS_H
