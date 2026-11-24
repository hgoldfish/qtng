#ifndef QTNG_QT_BRIDGE_EVENTLOOP_ACCESS_H
#define QTNG_QT_BRIDGE_EVENTLOOP_ACCESS_H

#include <QtCore/qsharedpointer.h>

#include "config.h"

namespace QTNG_NAMESPACE {
class EventLoopCoroutine;
}

QTNETWORKNG_NAMESPACE_BEGIN
class EventLoopCoroutine;

// internal: create the qt EventLoopCoroutine view wrapping the given core loop.
QSharedPointer<EventLoopCoroutine> wrapCoreLoop(QTNG_NAMESPACE::EventLoopCoroutine *core);
QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_QT_BRIDGE_EVENTLOOP_ACCESS_H
