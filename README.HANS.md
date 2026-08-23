qtng
===========

[English](README.md)

简介
----

qtng 是基于协程的网络工具包。与 boost::asio 等传统异步框架相比，它提供了受 Python gevent 启发的更简洁 API。**2.0 版本**核心库目标 **C++11**，公开 API 使用 C++ 标准库类型；**核心库不依赖 Qt**。

Qt 应用可选用 [`qt/`](qt/) 兼容层，以旧版 [qtnetworkng](https://github.com/hgoldfish/qtnetworkng) API（`QString`、`startQtLoop()`、`qAwait()` 等）接入，且不会暴露核心头文件。详见 [Qt 集成](docs/qt_integration.HANS.rst) 与 `qt/examples/fetch_web_content/`。

更多文档见：

[qtng 介绍](https://qtng.org/intro.HANS.html)


文档
----

访问 https://qtng.org/


特性
----

* 栈式协程，API 类似轻量级线程。
* `Socket` 支持 UDP 与 TCP。
* `SSLSocket` 与 `Socket` API 类似。
* `KcpSocket` 在 UDP 上实现 KCP。
* `UtpSocket` 在 UDP 上实现 µTP（BEP-29），自研 LEDBAT，运行时不依赖 libutp。
* `QuicConnection` / `QuicStream` 实现 QUICv1 **传输层 MVP**（不含 HTTP/3）。
* `SocketLike` 统一上述类型，作为其他组件的基础。
* `SocketServer` 提供网络服务器框架（HTTP 代理、SOCKS5 代理等）。
* `HttpSession` 实现 HTTP/1.1 与 HTTP/2 客户端，支持 SOCKS5/HTTP 代理。
* `SimpleHttpServer` / `TcpServer<SimpleHttpRequestHandler>` 用于静态 HTTP/1.1 文件服务。
* `NetworkInterface` 与 `HostAddress` 用于网络配置。
* `WebSocketConnection` 实现 WebSocket 客户端/服务端。
* `MqttClient` 实现 MQTT 3.1.1 客户端（QoS 0/1/2、Will、TLS），见 `qtng/mqtt.h`。
* `MsgPackStream` 为 MessagePack 实现。
* `BencodeStream` / `Bencode` 编解码 BitTorrent bencode（种子、tracker、DHT）。
* `DhtNode` 实现 BitTorrent DHT（BEP-5），存储可插拔（`MemoryDhtStore` / `LmdbDhtStore`）。
* `TorrentSession` 实现 BitTorrent 核心下载栈（peer wire、HTTP/UDP tracker、DHT + µTP/TCP），见 ``qtng/bt.h``。
* `Cipher`、`MessageDigest`、`PublicKey`、`PrivateKey` 封装 OpenSSL/LibreSSL API。

示例
----

获取网页：

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

建立 IPv4 TCP 连接：

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

创建 IPv4 TCP 服务器：

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

创建 HTTP 服务器：

```cpp
qtng::TcpServer<qtng::SimpleHttpRequestHandler> httpd(qtng::HostAddress::LocalHost, 8000);
httpd.serveForever();
```


许可证
------

qtng 以 LGPL 3.0 许可证发布。

可在 https://www.gnu.org/licenses/lgpl-3.0.zh.html 获取 LGPL 3.0 副本。


依赖
----

* **C++11** 编译器即可构建库本身（GCC、Clang、MSVC）；仅编译内置单元测试时需要 C++17，测试默认关闭（`-DQTNG_BUILD_TESTS=ON` 启用）
* **zlib**（系统库，用于 gzip）
* **OpenSSL 1.1.1+ 或 LibreSSL**（TLS/加密，可选）：
  * 初始化 `3rdparty/libressl/` 子模块可内嵌编译（`git submodule update --init 3rdparty/libressl`），或
  * 安装系统 OpenSSL 开发包（如 Debian/Ubuntu 的 `libssl-dev`）
  * 若两者皆无（或 `-DQTNG_DISABLE_CRYPTO=ON`），构建仍可继续并定义 `QTNG_NO_CRYPTO`：不编译 TLS/SSL、Noise、AEAD、QUIC；`MessageDigest`（MD5/SHA-1/SHA-224/SHA-256）改用软件实现

可选协议开关（默认 **OFF**；需要时再打开不完善/可选协议栈）：

* `-DQTNG_WITH_HTTP2=ON` — 编译 HTTP/2 与 HPACK
* `-DQTNG_WITH_QUIC=ON` — 编译 QUICv1 传输层 MVP（无加密时也会强制关闭）
* `-DQTNG_WITH_HTTP3=ON` — 编译 HTTP/3 占位实现（无 QUIC 时也会强制关闭）
* `-DQTNG_WITH_BT=ON` — 编译 BitTorrent 下载栈（`TorrentSession` / `qtng/bt.h`；bencode 与 DHT 仍默认可用）



支持平台
--------

已在 Linux、Android、macOS、Windows、OpenBSD 上测试。

gzip 压缩需要 zlib。

在 arm、arm64、x86、amd64 上使用 boost::context 风格汇编；其他架构回退到 ucontext 或 Windows fiber。


2.0 路线图
----------

- [x] 核心库移除 QtCore 依赖（Qt 应用可选 ``qt/`` 兼容层）
- [x] 支持 HTTP/2
- [ ] 支持 HTTP/3
- [x] 支持 QUIC（传输层 MVP：`QuicConnection` / `QuicStream`；尚无 HTTP/3）
- [x] 支持 Kademlia
- [x] 支持 BitTorrent 协议（核心下载栈 + magnet/BEP-9 + ``examples/btclient``；MSE/PEX 后续）
- [x] 支持 MQTT


3.0 路线图
----------

- [ ] Windows 上使用 IOCP 提升性能
- [ ] Windows 11 上使用 I/O Rings
- [ ] 较新 Linux 内核上使用 io_uring


构建
----

```bash
git clone --recurse-submodules https://github.com/hgoldfish/qtng.git
cd qtng
# 若已用普通 clone：
# git submodule update --init --recursive
mkdir build && cd build
cmake ..
cmake --build .
```

第三方源码位于 `3rdparty/` 下的 git 子模块（`libressl`、`libutp`）。默认构建不强制初始化子模块：无 LibreSSL 时回退到系统 OpenSSL，若 OpenSSL 也没有则进入 `QTNG_NO_CRYPTO`；无 libutp 时跳过 µTP 互通测试。

将应用程序与 `qtng` 静态库（`libqtng.a`，MSVC 上为 `qtng.lib`）链接。头文件位于 `include/qtng/`，公开包含路径为 `include/`，因此使用：

```cpp
#include <qtng.h>              // 总头文件：包含全部公开头文件
#include <qtng/coroutine.h>    // 或按需包含单个模块头文件
// 等价写法：#include <qtng/qtng.h>
```

### 安装到系统

```bash
cd build
sudo cmake --install . --prefix /usr/local
```

安装结果：

* `${prefix}/include/qtng.h` — 总头文件（`#include <qtng.h>`）
* `${prefix}/include/qtng/` — 各模块头文件（`coroutine.h`、`socket.h`、`utils/`、`private/` 等）
* `${prefix}/lib/libqtng.a` — 静态库（部分 Linux 发行版可能在 `lib64/`；MSVC 上为 `qtng.lib`）


如何贡献
--------

在 github.com 上提交 patch 并发起 pull request。
