#ifndef QTNG_QT_BRIDGE_STREAM_BRIDGE_H
#define QTNG_QT_BRIDGE_STREAM_BRIDGE_H

#include <memory>

#include <QtCore/qdatetime.h>

#include "bridge/core_access.h"
#include "bridge/hostaddress_access.h"
#include "hostaddress.h"
#include "socket_utils.h"
#include "io_utils.h"

namespace qtng_bridge {

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
