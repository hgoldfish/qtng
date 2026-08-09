#ifndef QTNG_QT_BRIDGE_COROUTINE_REGISTRY_H
#define QTNG_QT_BRIDGE_COROUTINE_REGISTRY_H

#include "config.h"

namespace qtng_core {
class BaseCoroutine;
}

QTNETWORKNG_NAMESPACE_BEGIN
class BaseCoroutine;
QTNETWORKNG_NAMESPACE_END

namespace qtng_bridge {

void registerQtWrapper(qtng_core::BaseCoroutine *core, QTNETWORKNG_NAMESPACE::BaseCoroutine *wrapper);
void unregisterQtWrapper(qtng_core::BaseCoroutine *core);
QTNETWORKNG_NAMESPACE::BaseCoroutine *qtWrapperFor(qtng_core::BaseCoroutine *core);
qtng_core::BaseCoroutine *coreFor(QTNETWORKNG_NAMESPACE::BaseCoroutine *wrapper);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_COROUTINE_REGISTRY_H
