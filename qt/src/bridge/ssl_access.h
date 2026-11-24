#ifndef QTNG_QT_BRIDGE_SSL_ACCESS_H
#define QTNG_QT_BRIDGE_SSL_ACCESS_H

#include <memory>

#include "config.h"

namespace QTNG_NAMESPACE {
class SslConfiguration;
class SslSocket;
}

QTNETWORKNG_NAMESPACE_BEGIN
class SslConfiguration;
class SslSocket;
QTNETWORKNG_NAMESPACE_END

namespace qtng_bridge {

std::shared_ptr<QTNG_NAMESPACE::SslSocket> sslSocketCoreOf(QTNETWORKNG_NAMESPACE::SslSocket *socket);
std::shared_ptr<QTNETWORKNG_NAMESPACE::SslConfiguration>
sslConfigurationFromCore(std::shared_ptr<QTNG_NAMESPACE::SslConfiguration> core);
std::shared_ptr<QTNG_NAMESPACE::SslConfiguration>
sslConfigurationToCore(const std::shared_ptr<QTNETWORKNG_NAMESPACE::SslConfiguration> &config);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_SSL_ACCESS_H
