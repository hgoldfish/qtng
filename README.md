qtng
===========

[中文](README.HANS.md)

Introduction
------------

qtng is a coroutine-based network toolkit. Compared to boost::asio and traditional async frameworks, qtng offers a simpler API inspired by Python gevent. **Version 2.0** targets **C++11** and uses standard library types in its public API; the core library has **no Qt dependency**.

For **Qt5/Qt6** applications, an optional binding under [`qt/`](qt/) provides the legacy [qtnetworkng](https://github.com/hgoldfish/qtnetworkng) API (`QString`, `startQtLoop()`, `qAwait()`, etc.) without exposing core headers. See [Qt integration](docs/qt_integration.rst) and `qt/examples/fetch_web_content/`.

For more detail visit:

[Introduction to qtng](https://qtng.org/intro.html)


Documents
---------

Visit https://qtng.org/


Features
--------

* Stackful coroutines with an API similar to lightweight threads.
* `Socket` supports UDP and TCP.
* `SSLSocket` with similar API to `Socket`.
* `KcpSocket` implements KCP over UDP.
* `UtpSocket` implements µTP (BEP-29) over UDP with a self-contained LEDBAT stack (no runtime libutp dependency).
* `QuicConnection` / `QuicStream` implement a QUICv1 **transport MVP** .
* `SocketLike` unifies these classes as the base of other components.
* `SocketServer` provides a framework for network servers (HTTP proxy, SOCKS5 proxy, etc.).
* `HttpSession` implements HTTP/1.1 and HTTP/2 clients, with SOCKS5/HTTP proxy support.
* `SimpleHttpServer` / `TcpServer<SimpleHttpRequestHandler>` for static HTTP/1.1 file serving.
* `NetworkInterface` and `HostAddress` for network configuration.
* `WebSocketConnection` implements WebSocket client/server.
* `MqttClient` implements an MQTT 3.1.1 client (QoS 0/1/2, Will, TLS) in `qtng/mqtt.h`.
* `MsgPackStream` is a MessagePack implementation.
* `BencodeStream` / `Bencode` encode and decode BitTorrent bencode (torrents, trackers, DHT).
* `DhtNode` implements BitTorrent DHT (BEP-5) with pluggable `DhtStore` (memory / LMDB).
* `TorrentSession` implements a BitTorrent core download stack (peer wire, HTTP/UDP trackers, DHT + µTP/TCP) in `qtng/bt.h`.
* `Cipher`, `MessageDigest`, `PublicKey`, `PrivateKey` wrap OpenSSL/LibreSSL APIs.

Examples
--------

Fetch a web page:

```cpp
#include "qtng.h"
#include <iostream>

int main()
{
    qtng::HttpSession session;
    qtng::HttpResponse r = session.get("http://example.com/");
    if (r.isOk()) {
        std::cout << r.html() << std::endl;
    }
    return 0;
}
```

Make an IPv4 TCP connection:

```cpp
#include "qtng.h"
#include <iostream>

int main()
{
    qtng::Socket conn;
    conn.connect("example.com", 80);
    conn.sendall("GET / HTTP/1.0\r\n\r\n");
    std::cout << conn.recv(1024 * 8) << std::endl;
    return 0;
}
```

Create an IPv4 TCP server:

```cpp
qtng::Socket s;
qtng::CoroutineGroup workers;
s.bind(qtng::HostAddress::AnyIPv4, 8000);
s.listen(100);
while (true) {
    std::shared_ptr<qtng::Socket> request(s.accept());
    if (!request) {
        break;
    }
    workers.spawn([request] {
        request->sendall("hello!");
        request->close();
    });
}
```

Create an HTTP server:

```cpp
qtng::TcpServer<qtng::SimpleHttpRequestHandler> httpd(qtng::HostAddress::LocalHost, 8000);
httpd.serveForever();
```


License
-------

The qtng is distributed under LGPL 3.0 license.

You can obtain a copy of LGPL 3.0 license at: https://www.gnu.org/licenses/lgpl-3.0.en.html


Dependencies
------------

* **C++11** compiler for the library (GCC, Clang, MSVC); C++17 is required only to build the bundled unit tests, which are off by default (`-DQTNG_BUILD_TESTS=ON`)
* **zlib** (system library, for gzip support)
* **OpenSSL 1.1.1+ or LibreSSL** for TLS/crypto (optional):
  * Initialize the LibreSSL submodule under `3rdparty/libressl/` to build a bundled copy (`git submodule update --init 3rdparty/libressl`), or
  * Install system OpenSSL development packages (e.g. `libssl-dev` on Debian/Ubuntu)
  * If neither is available (or `-DQTNG_DISABLE_CRYPTO=ON`), the build continues with `QTNG_NO_CRYPTO`: TLS/SSL, Noise, AEAD, and QUIC are omitted; `MessageDigest` (MD5/SHA-1/SHA-256) still works via a software fallback

Optional protocol toggles (default **ON**; turn off incomplete stacks when needed):

* `-DQTNG_WITH_HTTP2=OFF` — skip HTTP/2 + HPACK
* `-DQTNG_WITH_QUIC=OFF` — skip QUICv1 transport MVP (also forced off without crypto)
* `-DQTNG_WITH_HTTP3=OFF` — skip HTTP/3 stub (also forced off without QUIC)

Optional test-only dependency:

* **libutp** (µTP interoperability tests): `git submodule update --init 3rdparty/libutp`. When the submodule is missing, those tests are skipped and the rest of the build is unchanged.


Supported Platforms
-----------------

Linux, Android, macOS, Windows, and OpenBSD are supported.

GZip compression requires zlib on all platforms.

qtng uses boost::context-style asm on arm, arm64, x86, and amd64; other architectures fall back to ucontext or Windows fibers.


Towards 2.0
-------------

- [x] Remove QtCore from the **core** library (optional `qt/` binding for Qt apps)
- [x] Support HTTP/2
- [ ] Support HTTP/3
- [x] Support QUIC (transport MVP: `QuicConnection` / `QuicStream`; no HTTP/3 yet)
- [x] Support Kademlia
- [x] Support BitTorrent protocol (core download stack + magnet/BEP-9 + ``examples/btclient``; MSE/PEX later)
- [x] Support MQTT


Towards 3.0
-------------

- [ ] use IOCP under Windows for ultimate performance.
- [ ] use I/O Rings under Windows 11
- [ ] use io_uring on recent Linux kernels.


Building
--------

```bash
git clone --recurse-submodules https://github.com/hgoldfish/qtng.git
cd qtng
# or after a plain clone:
# git submodule update --init --recursive
mkdir build && cd build
cmake ..
cmake --build .
```

Third-party sources live under `3rdparty/` as git submodules (`libressl`, `libutp`). Submodules are optional for a default build: without LibreSSL the build falls back to system OpenSSL, then to `QTNG_NO_CRYPTO` if OpenSSL is also missing; without libutp the µTP interoperability tests are omitted.

Link your application against the `qtng` static library (`libqtng.a`, or `qtng.lib` on MSVC). Headers live under `include/qtng/`; the public include path is `include/`, so use:

```cpp
#include <qtng.h>              // umbrella: all public headers
#include <qtng/coroutine.h>    // or a specific module header
// equivalent: #include <qtng/qtng.h>
```

### Install to system

```bash
cd build
sudo cmake --install . --prefix /usr/local
```

This installs:

* `${prefix}/include/qtng.h` — umbrella header (`#include <qtng.h>`)
* `${prefix}/include/qtng/` — module headers (`coroutine.h`, `socket.h`, `utils/`, `private/`, …)
* `${prefix}/lib/libqtng.a` — static library (may be `lib64/` on some Linux distros; `qtng.lib` on MSVC)


How to Contribute
-----------------

Create a pull request on github.com with your patch, then make a pull request to me.
