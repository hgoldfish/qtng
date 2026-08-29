#ifndef QTNG_QT_BRIDGE_CORE_ACCESS_H
#define QTNG_QT_BRIDGE_CORE_ACCESS_H

// Internal bridge header: only included from qt/src/*.cpp translation units.
// Provides access to qtng core implementation without exposing headers to consumers.
//
// Core types are remapped into namespace qtng_core so they do not collide with the
// Qt binding API (also namespace qtng) or share include guards with qt/include/.

#define qtng qtng_core

#include "qtng/coroutine.h"
#include "qtng/deferred.h"
#include "qtng/eventloop.h"
#include "qtng/locks.h"
#include "qtng/coroutine_utils.h"
#include "qtng/hostaddress.h"
#include "qtng/network_interface.h"
#include "qtng/socket.h"
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
#include "qtng/private/http_p.h"
#include "qtng/private/hostaddress_p.h"
#include "qtng/private/network_interface_p.h"
#include "qtng/private/kademlia_p.h"

// The Qt-backed core event loop backend. Declared here with the qtng->qtng_core macro active so
// qt/src translation units can name it (e.g. for dynamic_cast-based backend checks) without
// re-including core headers later.
namespace qtng_core {
class QtEventLoopCoroutine : public EventLoopCoroutine
{
public:
    QtEventLoopCoroutine();
};
}  // namespace qtng_core

#ifndef QTNG_NO_CRYPTO
#  include "qtng/ssl.h"
#  include "qtng/md.h"
#  include "qtng/cipher.h"
#  include "qtng/pkey.h"
#  include "qtng/certificate.h"
#  include "qtng/noise.h"
#  include "qtng/private/crypto_p.h"
#endif

#undef qtng

#include "bridge/convert.h"

// Drop core include guards so qt/include counterparts can be included next.
#undef QTNG_COROUTINE_H
#undef QTNG_DEFERRED_H
#undef QTNG_EVENTLOOP_H
#undef QTNG_EVENTLOOP_P_H
#undef QTNG_LOCKS_H
#undef QTNG_COROUTINE_UTILS_H
#undef QTNG_COROUTINE_UTILS_P_H
#undef QTNG_COROUTINE_P_H
#undef QTNG_HOSTADDRESS_H
#undef QTNG_HOSTADDRESS_P_H
#undef QTNG_NETWORK_INTERFACE_H
#undef QTNG_NETWORK_INTERFACE_P_H
#undef QTNG_SOCKET_H
#undef QTNG_SOCKET_P_H
#undef QTNG_SOCKET_UTILS_H
#undef QTNG_IO_UTILS_H
#undef QTNG_HTTP_H
#undef QTNG_HTTP_P_H
#undef QTNG_HTTP_UTILS_H
#undef QTNG_HTTP_PROXY_H
#undef QTNG_HTTP_COOKIE_H
#undef QTNG_SOCKS5PROXY_H
#undef QTNG_MSGPACK_H
#undef QTNG_BENCODE_H
#undef QTNG_KADEMLIA_H
#undef QTNG_KADEMLIA_P_H
#undef QTNG_DATA_CHANNEL_H
#undef QTNG_MULTI_STREAM_H
#undef QTNG_UDP_H
#undef QTNG_HTTPD_H
#undef QTNG_SOCKET_SERVER_H
#undef QTNG_WEBSOCKET_H
#undef QTNG_LMDB_H
#undef QTNG_GZIP_H
#undef QTNG_SSL_H
#undef QTNG_RANDOM_H
#undef QTNG_MD_H
#undef QTNG_CIPHER_H
#undef QTNG_PKEY_H
#undef QTNG_CERTIFICATE_H
#undef QTNG_NOISE_H
#undef QTNG_CRYPTO_P_H
#undef QTNG_UTILS_URL_H
#undef QTNG_UTILS_PLATFORM_H
#undef QTNG_UTILS_LOGGING_H
#undef QTNG_UTILS_DATETIME_H
#undef QTNG_UTILS_STRING_UTILS_H
#undef QTNG_UTILS_THREAD_LOCAL_H
#undef QTNG_UTILS_SHARED_MUTEX_COMPAT_H
#undef QTNG_UTILS_PUNYCODE_H
#undef QTNG_UTILS_MIME_H

#include "bridge/coroutine_registry.h"

#endif  // QTNG_QT_BRIDGE_CORE_ACCESS_H
