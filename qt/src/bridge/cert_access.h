#ifndef QTNG_QT_BRIDGE_CERT_ACCESS_H
#define QTNG_QT_BRIDGE_CERT_ACCESS_H

#include "config.h"

namespace QTNG_NAMESPACE {
class Certificate;
}

QTNETWORKNG_NAMESPACE_BEGIN
class Certificate;
QTNETWORKNG_NAMESPACE_END

namespace qtng_bridge {

QTNETWORKNG_NAMESPACE::Certificate toQtCertificate(const QTNG_NAMESPACE::Certificate &cert);
const QTNG_NAMESPACE::Certificate &certificateCoreOf(const QTNETWORKNG_NAMESPACE::Certificate &cert);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_CERT_ACCESS_H
