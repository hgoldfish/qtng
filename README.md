qtng
===========

[中文](README.HANS.md)

Introduction
------------

qtng is a coroutine-based network toolkit. Compared to boost::asio and traditional async frameworks, qtng offers a simpler API inspired by Python gevent. **Version 2.0** removes the Qt dependency: the library targets **C++11** and uses standard library types in its public API.

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
* `SocketLike` unifies these classes as the base of other components.
* `SocketServer` provides a framework for network servers (HTTP proxy, SOCKS5 proxy, etc.).
* `HttpSession` implements an HTTP/1.1 client with SOCKS5/HTTP proxy support.
* `SimpleHttpServer` / `TcpServer<SimpleHttpRequestHandler>` for static HTTP/1.1 file serving.
* `NetworkInterface` and `HostAddress` for network configuration.
* `WebSocketConnection` implements WebSocket client/server.
* `MsgPackStream` is a MessagePack implementation.
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
* **OpenSSL 1.1.0+ or LibreSSL** for TLS/crypto:
  * Place LibreSSL sources in `libressl/` to build a bundled copy, or
  * Install system OpenSSL development packages (e.g. `libssl-dev` on Debian/Ubuntu)


Supported Platforms
-----------------

Linux, Android, macOS, Windows, and OpenBSD are supported.

GZip compression requires zlib on all platforms.

qtng uses boost::context-style asm on arm, arm64, x86, and amd64; other architectures fall back to ucontext or Windows fibers.


Towards 2.0
-------------

- [x] Remove the QtCore dependence.
- [ ] Support HTTP/2
- [ ] Support HTTP/3
- [ ] Support QUIC
- [ ] Support Kademlia
- [ ] Support BitTorrent protocol
- [ ] Support MQTT


Towards 3.0
-------------

- [ ] use IOCP under Windows for ultimate performance.
- [ ] use I/O Rings under Windows 11
- [ ] use io_uring on recent Linux kernels.


Building
--------

```bash
git clone https://github.com/hgoldfish/qtng.git
cd qtng
mkdir build && cd build
cmake ..
cmake --build .
```

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
