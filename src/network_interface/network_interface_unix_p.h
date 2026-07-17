#ifndef QNETWORKINTERFACE_UNIX_P_H
#define QNETWORKINTERFACE_UNIX_P_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API. It exists purely as an
// implementation detail. This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include "qtng/private/network_interface_p.h"

#define IP_MULTICAST    // make AIX happy and define IFF_MULTICAST

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#ifdef Q_OS_SOLARIS
#  include <sys/sockio.h>
#endif
#ifdef Q_OS_HAIKU
#  include <sys/sockio.h>
#  define IFF_RUNNING 0x0001
#endif


// VxWorks' headers specify 'int' instead of '...' for the 3rd ioctl() parameter.
template <typename T>
static inline int qt_safe_ioctl(int sockfd, unsigned long request, T arg)
{
#ifdef Q_OS_VXWORKS
    return ::ioctl(sockfd, request, (int) arg);
#else
    return ::ioctl(sockfd, request, arg);
#endif
}

static inline int qt_safe_socket(int domain, int type, int protocol, int flags = 0)
{
    assert((flags & ~O_NONBLOCK) == 0);
    int fd;
#ifdef QT_THREADSAFE_CLOEXEC
    int newtype = type | SOCK_CLOEXEC;
    if (flags & O_NONBLOCK)
        newtype |= SOCK_NONBLOCK;
    fd = ::socket(domain, newtype, protocol);
    return fd;
#else
    fd = ::socket(domain, type, protocol);
    if (fd == -1)
        return -1;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    // set non-block too?
    if (flags & O_NONBLOCK)
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
#endif
}

#define EINTR_LOOP(var, cmd)                    \
    do {                                        \
        var = cmd;                              \
    } while (var == -1 && errno == EINTR)

static inline int qt_safe_close(int fd)
{
    int ret;
    EINTR_LOOP(ret, ::close(fd));
    return ret;
}


namespace qtng {

static NetworkInterface::InterfaceFlags convertint(uint rawint)
{
    NetworkInterface::InterfaceFlags flags;
    flags |= (rawint & IFF_UP) ? NetworkInterface::IsUp : NetworkInterface::InterfaceFlag(0);
    flags |= (rawint & IFF_RUNNING) ? NetworkInterface::IsRunning : NetworkInterface::InterfaceFlag(0);
    flags |= (rawint & IFF_BROADCAST) ? NetworkInterface::CanBroadcast : NetworkInterface::InterfaceFlag(0);
    flags |= (rawint & IFF_LOOPBACK) ? NetworkInterface::IsLoopBack : NetworkInterface::InterfaceFlag(0);
#ifdef IFF_POINTOPOINT //cygwin doesn't define IFF_POINTOPOINT
    flags |= (rawint & IFF_POINTOPOINT) ? NetworkInterface::IsPointToPoint : NetworkInterface::InterfaceFlag(0);
#endif

#ifdef IFF_MULTICAST
    flags |= (rawint & IFF_MULTICAST) ? NetworkInterface::CanMulticast : NetworkInterface::InterfaceFlag(0);
#endif
    return flags;
}

}  // namespace qtng

#endif // QNETWORKINTERFACE_UNIX_P_H
