#ifndef QTNG_QT_BRIDGE_CORE_ACCESS_H
#define QTNG_QT_BRIDGE_CORE_ACCESS_H

// Internal bridge header: only included from qt/src/*.cpp translation units.
// Provides access to qtng core implementation without exposing headers to consumers.
//
// Core types live in QTNG_NAMESPACE (qt/CMakeLists defaults it to qtng_core) so they do
// not collide with the Qt binding API (QTNETWORKNG_NAMESPACE, default qtng) or share
// include guards with qt/include/.

#include "qtng/coroutine.h"
#include "qtng/deferred.h"
#include "qtng/eventloop.h"
#include "qtng/locks.h"
#include "qtng/coroutine_utils.h"
#include "qtng/hostaddress.h"
#include "qtng/network_interface.h"
#include "qtng/socket.h"
#include "qtng/local_socket.h"
#include "qtng/socket_utils.h"
#include "qtng/io_utils.h"
#include "qtng/http.h"
#include "qtng/http_utils.h"
#include "qtng/http_proxy.h"
#include "qtng/http_cookie.h"
#include "qtng/socks5_proxy.h"
#include "qtng/msgpack.h"
#include "qtng/bencode.h"
#include "qtng/kademlia.h"
#include "qtng/data_channel.h"
#include "qtng/multi_stream.h"
#include "qtng/udp.h"
#include "qtng/kcp.h"
#include "qtng/utp.h"
#include "qtng/httpd.h"
#include "qtng/socket_server.h"
#include "qtng/websocket.h"
#include "qtng/lmdb.h"
#include "qtng/random.h"
#include "qtng/gzip.h"
#include "qtng/private/eventloop_p.h"
#include "qtng/private/coroutine_p.h"
#include "qtng/private/coroutine_utils_p.h"
#include "qtng/private/socket_p.h"
#include "qtng/private/udp_p.h"
#include "qtng/private/http_p.h"
#include "qtng/private/hostaddress_p.h"
#include "qtng/private/network_interface_p.h"
#include "qtng/private/kademlia_p.h"

// The Qt-backed core event loop backend. Declared in QTNG_NAMESPACE so
// qt/src translation units can name it (e.g. for dynamic_cast-based backend checks)
// without re-including core headers later.
namespace QTNG_NAMESPACE {
class QtEventLoopCoroutine : public EventLoopCoroutine
{
public:
    QtEventLoopCoroutine();
};
}  // namespace QTNG_NAMESPACE

#ifndef QTNG_NO_CRYPTO
#  include "qtng/ssl.h"
#  include "qtng/md.h"
#  include "qtng/cipher.h"
#  include "qtng/pkey.h"
#  include "qtng/certificate.h"
#  include "qtng/noise.h"
#  include "qtng/private/crypto_p.h"
#endif

#include "bridge/convert.h"

#include "bridge/coroutine_registry.h"

#endif  // QTNG_QT_BRIDGE_CORE_ACCESS_H
