#include <QtCore/qmutex.h>
#include <QtCore/qhash.h>

#include "bridge/coroutine_registry.h"
#include "config.h"

using CoreCoroutine = qtng_core::BaseCoroutine;

namespace qtng_bridge {

static QMutex registryMutex;
static QHash<CoreCoroutine *, QTNETWORKNG_NAMESPACE::BaseCoroutine *> coreToQt;
static QHash<QTNETWORKNG_NAMESPACE::BaseCoroutine *, CoreCoroutine *> qtToCore;

void registerQtWrapper(CoreCoroutine *core, QTNETWORKNG_NAMESPACE::BaseCoroutine *wrapper)
{
    QMutexLocker lock(&registryMutex);
    coreToQt.insert(core, wrapper);
    qtToCore.insert(wrapper, core);
}

void unregisterQtWrapper(CoreCoroutine *core)
{
    QMutexLocker lock(&registryMutex);
    QTNETWORKNG_NAMESPACE::BaseCoroutine *wrapper = coreToQt.take(core);
    if (wrapper) {
        qtToCore.remove(wrapper);
    }
}

QTNETWORKNG_NAMESPACE::BaseCoroutine *qtWrapperFor(CoreCoroutine *core)
{
    QMutexLocker lock(&registryMutex);
    return coreToQt.value(core, nullptr);
}

CoreCoroutine *coreFor(QTNETWORKNG_NAMESPACE::BaseCoroutine *wrapper)
{
    QMutexLocker lock(&registryMutex);
    return qtToCore.value(wrapper, nullptr);
}

}  // namespace qtng_bridge
