#ifndef QTNG_QT_BRIDGE_STREAM_BRIDGE_H
#define QTNG_QT_BRIDGE_STREAM_BRIDGE_H

#include <memory>

#include <QtCore/qdatetime.h>
#include <QtCore/qtimezone.h>

#include "bridge/core_access.h"
#include "bridge/hostaddress_access.h"
#include "hostaddress.h"
#include "socket_utils.h"
#include "io_utils.h"

namespace qtng_bridge {

class CoreSocketLikeAdapter;
class QtBackedCoreSocketLike;

std::shared_ptr<QTNG_NAMESPACE::SocketLike> kcpOrUtpToCoreSocketLike(
        const QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> &socket);
// Unwraps a Qt LocalSocket-backed SocketLike into the core LocalSocket's
// SocketLike, and wraps a core LocalSocket-like back into Qt. Both return null
// when the argument is not a local socket, so callers can chain them.
std::shared_ptr<QTNG_NAMESPACE::SocketLike> localSocketToCoreSocketLike(
        const QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> &socket);
QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike>
localSocketFromCoreSocketLike(const std::shared_ptr<QTNG_NAMESPACE::SocketLike> &core);

std::shared_ptr<QTNG_NAMESPACE::SocketLike> toCoreSocketLike(const QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> &socket);
QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike>
toQtSocketLike(const std::shared_ptr<QTNG_NAMESPACE::SocketLike> &core);

std::shared_ptr<QTNG_NAMESPACE::FileLike> toCoreFileLike(const QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> &file);
QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> toQtFileLike(const std::shared_ptr<QTNG_NAMESPACE::FileLike> &core);

inline QDateTime toQDateTime(const QTNG_NAMESPACE::utils::DateTime &dt)
{
    if (!dt.isValid()) {
        return QDateTime();
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QDateTime::fromMSecsSinceEpoch(dt.toMSecsSinceEpoch(), QTimeZone::UTC);
#else
    // QTimeZone::UTC is a constant since Qt 6.5; older Qt only offers QTimeZone::utc().
    return QDateTime::fromMSecsSinceEpoch(dt.toMSecsSinceEpoch(), QTimeZone::utc());
#endif
}

inline QTNG_NAMESPACE::utils::DateTime toCoreDateTime(const QDateTime &dt)
{
    if (!dt.isValid()) {
        return QTNG_NAMESPACE::utils::DateTime();
    }
    return QTNG_NAMESPACE::utils::DateTime::fromMSecsSinceEpoch(dt.toMSecsSinceEpoch());
}

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_STREAM_BRIDGE_H
