#ifndef QTNG_QT_BRIDGE_STREAM_BRIDGE_H
#define QTNG_QT_BRIDGE_STREAM_BRIDGE_H

#include <memory>

#include <QtCore/qdatetime.h>

#include "bridge/core_access.h"
#include "hostaddress.h"
#include "socket_utils.h"
#include "io_utils.h"

namespace qtng_bridge {

inline qtng_core::HostAddress toCoreAddress(const QTNETWORKNG_NAMESPACE::HostAddress &addr)
{
    if (addr.isNull()) {
        return qtng_core::HostAddress();
    }
    if (addr.protocol() == QTNETWORKNG_NAMESPACE::HostAddress::IPv4Protocol) {
        return qtng_core::HostAddress(addr.toIPv4Address());
    }
    const QTNETWORKNG_NAMESPACE::IPv6Address v6 = addr.toIPv6Address();
    qtng_core::HostAddress result(v6.c);
    result.setScopeId(toStdString(addr.scopeId()));
    return result;
}

inline QTNETWORKNG_NAMESPACE::HostAddress toQtAddress(const qtng_core::HostAddress &addr)
{
    if (addr.isNull()) {
        return QTNETWORKNG_NAMESPACE::HostAddress();
    }
    if (addr.protocol() == qtng_core::HostAddress::IPv4Protocol) {
        return QTNETWORKNG_NAMESPACE::HostAddress(addr.toIPv4Address());
    }
    const qtng_core::IPv6Address v6 = addr.toIPv6Address();
    QTNETWORKNG_NAMESPACE::IPv6Address qv6;
    for (int i = 0; i < 16; ++i) {
        qv6.c[i] = v6.c[i];
    }
    QTNETWORKNG_NAMESPACE::HostAddress result(qv6);
    result.setScopeId(toQString(addr.scopeId()));
    return result;
}

class CoreSocketLikeAdapter;
class QtBackedCoreSocketLike;

std::shared_ptr<qtng_core::SocketLike> kcpOrUtpToCoreSocketLike(
        const QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> &socket);

std::shared_ptr<qtng_core::SocketLike> toCoreSocketLike(const QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> &socket);
QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike>
toQtSocketLike(const std::shared_ptr<qtng_core::SocketLike> &core);

std::shared_ptr<qtng_core::FileLike> toCoreFileLike(const QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> &file);
QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> toQtFileLike(const std::shared_ptr<qtng_core::FileLike> &core);

inline QDateTime toQDateTime(const qtng_core::utils::DateTime &dt)
{
    if (!dt.isValid()) {
        return QDateTime();
    }
    return QDateTime::fromMSecsSinceEpoch(dt.toMSecsSinceEpoch(), Qt::UTC);
}

inline qtng_core::utils::DateTime toCoreDateTime(const QDateTime &dt)
{
    if (!dt.isValid()) {
        return qtng_core::utils::DateTime();
    }
    return qtng_core::utils::DateTime::fromMSecsSinceEpoch(dt.toMSecsSinceEpoch());
}

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_STREAM_BRIDGE_H
