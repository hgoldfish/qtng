#ifndef QTNG_QT_BRIDGE_COROUTINE_REGISTRY_H
#define QTNG_QT_BRIDGE_COROUTINE_REGISTRY_H

#include "config.h"

namespace QTNG_NAMESPACE {
class BaseCoroutine;
}

QTNETWORKNG_NAMESPACE_BEGIN
class BaseCoroutine;
QTNETWORKNG_NAMESPACE_END

namespace qtng_bridge {

void registerQtWrapper(QTNG_NAMESPACE::BaseCoroutine *core, QTNETWORKNG_NAMESPACE::BaseCoroutine *wrapper);
void unregisterQtWrapper(QTNG_NAMESPACE::BaseCoroutine *core);
QTNETWORKNG_NAMESPACE::BaseCoroutine *qtWrapperFor(QTNG_NAMESPACE::BaseCoroutine *core);
QTNG_NAMESPACE::BaseCoroutine *coreFor(QTNETWORKNG_NAMESPACE::BaseCoroutine *wrapper);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_COROUTINE_REGISTRY_H
