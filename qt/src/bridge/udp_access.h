#ifndef QTNG_QT_BRIDGE_UDP_ACCESS_H
#define QTNG_QT_BRIDGE_UDP_ACCESS_H

#include <cstdint>
#include <memory>
#include <vector>

#include "bridge/core_access.h"
#include "bridge/hostaddress_access.h"
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

// Batched Datagram conversion. The Qt binding owns its QByteArray buffers, the
// core borrows char* into them: toCoreDatagrams() points the core views at the
// Qt buffers (detaching them if shared), and applyCoreDatagrams() mirrors the
// receive result (length, sender address) back and shrinks the Qt list to the
// number received. The Qt list must not be modified between the two calls.

inline std::vector<qtng_core::Datagram> toCoreDatagrams(QList<QTNETWORKNG_NAMESPACE::Datagram> &qtDatagrams)
{
    std::vector<qtng_core::Datagram> coreDatagrams;
    coreDatagrams.reserve(static_cast<size_t>(qtDatagrams.size()));
    for (int i = 0; i < qtDatagrams.size(); ++i) {
        qtng_core::Datagram datagram;
        datagram.data = qtDatagrams[i].data.data();
        datagram.len = static_cast<std::int32_t>(qtDatagrams[i].data.size());
        datagram.addr = toCoreAddress(qtDatagrams[i].addr);
        datagram.port = qtDatagrams[i].port;
        coreDatagrams.push_back(datagram);
    }
    return coreDatagrams;
}

inline std::vector<qtng_core::Datagram> toCoreDatagrams(const QList<QTNETWORKNG_NAMESPACE::Datagram> &qtDatagrams)
{
    std::vector<qtng_core::Datagram> coreDatagrams;
    coreDatagrams.reserve(static_cast<size_t>(qtDatagrams.size()));
    for (int i = 0; i < qtDatagrams.size(); ++i) {
        qtng_core::Datagram datagram;
        // safe! const_cast is ok here because the Qt binding owns its QByteArray buffers,
        // the core borrows char* into them.
        datagram.data = const_cast<char *>(qtDatagrams[i].data.constData());
        datagram.len = static_cast<std::int32_t>(qtDatagrams[i].data.size());
        datagram.addr = toCoreAddress(qtDatagrams[i].addr);
        datagram.port = qtDatagrams[i].port;
        coreDatagrams.push_back(datagram);
    }
    return coreDatagrams;
}

inline void applyCoreDatagrams(const std::vector<qtng_core::Datagram> &coreDatagrams,
                               QList<QTNETWORKNG_NAMESPACE::Datagram> &qtDatagrams)
{
    for (size_t i = 0; i < coreDatagrams.size(); ++i) {
        qtDatagrams[static_cast<int>(i)].data.resize(coreDatagrams[i].len);
        qtDatagrams[static_cast<int>(i)].addr = toQtAddress(coreDatagrams[i].addr);
        qtDatagrams[static_cast<int>(i)].port = coreDatagrams[i].port;
    }
    if (coreDatagrams.size() < static_cast<size_t>(qtDatagrams.size())) {
        qtDatagrams.erase(qtDatagrams.begin() + static_cast<int>(coreDatagrams.size()), qtDatagrams.end());
    }
}

std::shared_ptr<qtng_core::DatagramLink>
toCoreDatagramLink(const QSharedPointer<QTNETWORKNG_NAMESPACE::DatagramLink> &link);
QSharedPointer<QTNETWORKNG_NAMESPACE::DatagramLink>
toQtDatagramLink(const std::shared_ptr<qtng_core::DatagramLink> &core);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_UDP_ACCESS_H
