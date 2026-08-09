#ifndef QTNG_QT_BRIDGE_SSL_ACCESS_H
#define QTNG_QT_BRIDGE_SSL_ACCESS_H

#include <memory>

#include "config.h"

namespace qtng_core {
class SslSocket;
}

QTNETWORKNG_NAMESPACE_BEGIN
class SslSocket;
QTNETWORKNG_NAMESPACE_END

namespace qtng_bridge {

std::shared_ptr<qtng_core::SslSocket> sslSocketCoreOf(QTNETWORKNG_NAMESPACE::SslSocket *socket);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_SSL_ACCESS_H
