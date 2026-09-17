#ifndef QTNG_QT_BRIDGE_UDP_ACCESS_H
#define QTNG_QT_BRIDGE_UDP_ACCESS_H

#include <memory>

#include "bridge/core_access.h"
#include "udp.h"

namespace qtng_bridge {

inline qtng_core::DatagramPath toCoreDatagramPath(const QTNETWORKNG_NAMESPACE::DatagramPath &path)
{
    return qtng_core::DatagramPath(toStdString(path.key()));
}

inline QTNETWORKNG_NAMESPACE::DatagramPath toQtDatagramPath(const qtng_core::DatagramPath &path)
{
    return QTNETWORKNG_NAMESPACE::DatagramPath(toQString(path.key()));
}

std::shared_ptr<qtng_core::DatagramLink>
toCoreDatagramLink(const QSharedPointer<QTNETWORKNG_NAMESPACE::DatagramLink> &link);
QSharedPointer<QTNETWORKNG_NAMESPACE::DatagramLink>
toQtDatagramLink(const std::shared_ptr<qtng_core::DatagramLink> &core);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_UDP_ACCESS_H
