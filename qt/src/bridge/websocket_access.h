#ifndef QTNG_QT_BRIDGE_WEBSOCKET_ACCESS_H
#define QTNG_QT_BRIDGE_WEBSOCKET_ACCESS_H

// Internal bridge header: only included from qt/src/*.cpp translation units.

#include <memory>

#include "bridge/core_access.h"
#include "websocket.h"

namespace qtng_bridge {

// Wrap a core WebSocketConnection (already handshaken by core::HttpSession::ws())
// into a Qt binding WebSocketConnection. Returns null for a null core.
QSharedPointer<QTNETWORKNG_NAMESPACE::WebSocketConnection>
webSocketConnectionFromCore(std::shared_ptr<qtng_core::WebSocketConnection> core);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_WEBSOCKET_ACCESS_H
