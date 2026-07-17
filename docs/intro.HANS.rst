.. qtng documentation master file, created by
   sphinx-quickstart on Fri Nov 10 11:50:39 2017.
   You can adapt this file completely to your liking, but it should at least
   contain the root `toctree` directive.

qtng简介
===========================

qtng是基于协程的网络编程工具包，类似 boost::asio，并借鉴 Python gevent 的协程风格 API。


为何选择协程
--------------

协程并非新事物，Python、Go和C#多年前就使用协程简化网络编程。

传统网络编程采用线程机制，``send()/recv()`` 会阻塞线程，操作系统将当前线程切换到就绪线程直至数据到达。这种方式直观易用，但线程资源消耗大，数千连接会占用大量内存。更严重的是，线程可能导致数据竞争、数据损坏甚至程序崩溃。

另一种选择是回调范式。在调用 ``send() / recv()`` 前使用 ``select()/poll()/epoll()`` 检测数据到达。``select()`` 会阻塞，但多个连接可在单线程中处理。回调范式被视作"新时代的goto语句"，代码难以理解和维护，但因boost::asio等框架的流行而在C++中广泛使用。

协程范式是网络编程的现在与未来。协程是轻量级线程，拥有独立栈空间，由qtng而非操作系统管理。类似线程范式，``send() / recv()`` 会阻塞，但会在数据到达时切换到同一线程内的其他协程。可低成本创建大量协程。由于单线程运行，天然避免数据竞争问题。API保持线程范式的直观性，同时规避了线程的复杂性。


跨平台支持
---------------

qtng已在Linux、Android、Windows、MacOS和OpenBSD平台测试通过，支持gcc、clang、mingw32、msvc编译器。

构建qtng需要 C++11 编译器、zlib，以及 TLS/加密支持（可通过 ``libressl/`` 子目录使用内置 LibreSSL，或使用系统 OpenSSL/LibreSSL）。内置单元测试需要 C++17 以及本机安装的 Catch2 v3（或更高版本），且默认不编译。Catch2 不会被自动下载，安装方式见下文"构建并运行测试"。

协程实现采用boost::context汇编代码，同时支持原生posix ``ucontext``和windows ``fiber`` API，已在ARM、ARM64、x86、amd64架构成功运行测试。

库在 Unix 上使用 libev 事件循环，并通过 LibreSSL 或 OpenSSL 提供 SSL/加密功能。


在CMake项目中使用qtng
----------------------------

从 GitHub 克隆 qtng 并将其作为子目录加入项目：

.. code-block:: bash
    :caption: 获取qtng

    git clone https://github.com/hgoldfish/qtng.git

示例 ``CMakeLists.txt``：

.. code-block:: cmake

    cmake_minimum_required(VERSION 3.14)
    project(foo LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    add_subdirectory(qtng)

    add_executable(foo main.cpp)
    target_link_libraries(foo PRIVATE qtng)

示例 ``main.cpp``：

.. code-block:: c++
    :caption: 获取网页

    #include "qtng.h"

    using namespace qtng;
    int main()
    {
        HttpSession session;
        HttpResponse resp = session.get("http://www.example.com/");
        if (resp.isOk()) {
            ngDebug() << resp.html();
        } else {
            ngDebug() << "failed.";
        }
        return 0;
    }

构建命令：

.. code-block:: bash
    :caption: 构建项目

    mkdir build
    cd build
    cmake ..
    cmake --build .

单元测试默认不编译，且需要 C++17 编译器与本机安装的 Catch2 v3（或更高版本）。出于供应链安全考虑，Catch2 不会被自动下载，请先自行安装，例如：

.. code-block:: bash
    :caption: 安装 Catch2 v3（任选其一）

    sudo apt install catch2          # Debian/Ubuntu bookworm+
    sudo dnf install catch2-devel    # Fedora
    sudo pacman -S catch2            # Arch Linux
    brew install catch2              # macOS (Homebrew)
    vcpkg install catch2             # vcpkg

随后启用并运行测试：

.. code-block:: bash
    :caption: 构建并运行测试

    cmake -DQTNG_BUILD_TESTS=ON ..
    cmake --build .
    ctest

若 Catch2 安装在非默认前缀，请通过 ``-DCMAKE_PREFIX_PATH=...``（或 ``-DCatch2_ROOT=...``）告知 CMake。未安装 Catch2 时，以 ``QTNG_BUILD_TESTS=ON`` 配置会直接报错；保持 ``QTNG_BUILD_TESTS=OFF`` 则构建 qtng 库时完全不依赖 Catch2。

可选：安装到系统前缀：

.. code-block:: bash
    :caption: 安装 qtng

    sudo cmake --install . --prefix /usr/local

总头文件安装为 ``include/qtng.h``；各模块头文件在 ``include/qtng/``；静态库为 ``lib/``（或部分平台为 ``lib64/``）下的 ``libqtng.a``（MSVC 上为 ``qtng.lib``）。安装后请使用 ``#include <qtng.h>`` 或 ``#include <qtng/coroutine.h>``（也可 ``#include <qtng/qtng.h>``）。


协程机制
--------

qtng基于``Coroutine``实现。确保所有网络操作运行在协程环境中，主线程已隐式转换为协程。推荐使用``CoroutineGroup``管理协程，其采用``std::shared_ptr``智能指针处理协程生命周期及边界情况。

.. code-block:: c++
    :caption: 启动协程
    
    void coroutine_entry()
    {
        Coroutine::sleep(1.0); // 休眠1秒
        ngDebug() << "当前协程ID: " << Coroutine::current().id();
    }
    
    // 推荐使用CoroutineGroup
    CoroutineGroup operations;
    std::shared_ptr<Coroutine> coroutine = operations.spawn(coroutine_entry);
    
    // 或手动管理协程
    std::shared_ptr<Coroutine> coroutine = Coroutine::spawn(coroutine_entry);
    
通过 ``Coroutine::start()`` 调度协程启动， ``Coroutine::kill()`` 发送终止异常。两个函数立即返回，实际操作异步执行。

``CoroutineGroup`` 支持命名协程管理：

.. code-block:: c++
    :caption: 管理多个协程
    
    CoroutineGroup operations;
    operations.spawnWithName("coroutine1", coroutine_entry);
    operations.kill("coroutine1");
    operations.killall();

协程终止时抛出``CoroutineExit``异常，可捕获处理。协程被删除前会自动等待结束。

.. code-block:: c++
    :caption: 终止协程示例
    
    coroutine.kill(new MyCoroutineException());

    void coroutine_entry()
    {
        try {
            与远程主机通信();
        } catch (MyCoroutineException const &e) {
            // 异常处理
        }
    }
    
``CoroutineExit`` 异常由qtng静默处理。


Qt GUI 集成说明（2.0 已移除）
------------------------------

qtng 2.0 不再与 Qt Widgets 或 Qt 事件循环集成。请使用标准 ``main()`` 入口，库会在内部管理自己的事件循环。``startQtLoop()`` 和 ``qAwait()`` 等函数已移除。


Socket与SslSocket
-----------------

qtng旨在简化C++网络编程。 ``Socket`` 类是对BSD socket接口的面向对象封装。

``SslSocket`` 接口与``Socket``一致，在建立连接后执行SSL握手。

``Socket`` 和 ``SslSocket`` 可转换为``SocketLike``接口，便于统一处理。

``KcpSocket`` 实现基于UDP的KCP协议，提供类似``Socket``的API，同样支持``SocketLike``转换。


创建Socket客户端
^^^^^^^^^^^^^^^^

``Socket`` 提供两种构造函数：接受原生socket描述符或协议族/类型组合。

.. code-block:: c++
    :caption: 连接远程主机
    
    // 仅IPv4
    Socket s(Socket::IPv4Protocol, Socket::TcpSocket);
    bool ok = s.connect(remoteHost, 80);
    
    // 自动检测IPv4/IPv6
    std::unique_ptr<Socket> s(Socket::createConnection(remoteHost, 80));
    bool ok = static_cast<bool>(s);
    
    Socket s(socketDescriptor); // socketDescriptor需设为非阻塞
    bool ok = s.connect(remoteHost, 80);
    
``SslSocket``构造函数需额外接受``SslConfiguration``：

.. code-block:: c++
    :caption: 连接SSL服务器
    
    // 仅IPv4
    SslConfiguration config;
    SslSocket s(Socket::IPv4Protocol, config);
    bool ok = s.connect(remoteHost, 443);
    
    // 自动检测
    SslConfiguration config;
    std::unique_ptr<SslSocket> s(SslSocket::createConnection(remoteHost, 443, config));
    bool ok = static_cast<bool>(s);
    
    SslSocket s(socketDescriptor, config);
    bool ok = s.connect(remoteHost, 443);
    

创建Socket服务器
^^^^^^^^^^^^^^^^

结合协程可快速搭建服务器：

.. code-block:: c++
    :caption: TCP服务器
    
    std::unique_ptr<Socket> s(Socket::createServer(HostAddress::AnyIPv4, 8000, 100));
    CoroutineGroup operations;
    while (true) {
        std::shared_ptr<Socket> request(s->accept());
        if (!request) {
            break;
        }
        operations.spawn([request] {
            request->sendall("你好！");
        });
    }
    

HTTP客户端
-----------

qtng提供支持HTTP1.1/HTTPS的客户端，支持SOCKS5代理、Cookie、重定向及 form-data 等数据类型。

HTTP 2.0支持正在规划中。

API设计灵感源自Python的*requests*模块。


获取HTTP资源
^^^^^^^^^^^^^^^^^^^^^^^^

使用``HttpSession``类进行HTTP通信：

.. code-block:: c++
    :caption: 获取网页
    
    qtng::HttpSession session;
    HttpResponse resp = session.get(url);
    
``HttpSession`` 会自动存储响应中的Cookie，保持会话状态。


提交数据到HTTP服务器
^^^^^^^^^^^^^^^^^^^^

常用方式为POST表单提交：

.. code-block:: c++
    :caption: 提交表单
    
    FormData data;
    data.addQuery("name", "fish");
    data.addFile("file", "filename.txt", std::string("文件内容"));
    HttpResponse resp = session.post(url, data.toByteArray());
    
或提交 JSON 等自定义请求体（需自行序列化并设置 Content-Type）：

.. code-block:: c++
    :caption: 提交 JSON 字符串
    
    HttpRequest request("POST", url);
    request.setHeader("Content-Type", "application/json");
    request.setBody(R"({"name":"fish"})");
    HttpResponse resp = session.send(request);
    
添加请求头：

.. code-block:: c++
    :caption: 带请求头提交
    
    HttpRequest request("POST", url);
    request.setHeader("Content-Type", "application/json");
    request.setBody(R"({"username":"somebody","password":"secret"})");
    std::map<std::string, std::string> headers;
    headers.insert({"X-My-Header", "test"});
    request.setHeaders(headers);
    HttpResponse resp = session.send(request);


处理HTTP响应
^^^^^^^^^^^^

``HttpResponse`` 包含服务器返回的所有信息：

.. code-block:: c++
    :caption: 获取响应信息

    HttpResponse resp = session.get(url);
    ngDebug() << resp.isOk();       // 无错误返回true
    ngDebug() << resp.getContentType();  // 响应内容类型
    ngDebug() << resp.statusCode();      // 状态码如200
    ngDebug() << resp.statusText();      // 状态文本如OK
    
支持多种数据类型解析：

.. code-block:: c++
    :caption: 获取响应内容

    ngDebug() << resp.text();        // UTF8字符串
    ngDebug() << resp.html();        // UTF8字符串
    ngDebug() << resp.body();        // 原始字节数据
    ngDebug() << resp.bodyAsFile()   // 可读写的文件类对象


加密技术
--------

qtng使用LibreSSL或OpenSSL提供加密功能。


消息摘要
^^^^^^^^^^^^^^

支持主流摘要算法：

.. code-block:: c++
    :caption: SHA512哈希计算

    MessageDigest m(MessageDigest::SHA512);
    m.update("data");
    ngDebug() << m.hexDigest();
    

对称加密解密
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

支持AES、Blowfish、ChaCha20等算法：

.. code-block:: c++
    :caption: AES256_CBF加密
    
    Cipher ciph(Cihper::AES256, Cipher::CBF, Cipher::Encrypt);
    ciph.setPassword("密码", MessageDigest::Sha256, "盐值");
    std::string encrypted = ciph.update("fish");
    encrypted.push_back(ciph.final();

``Cipher::setPassword()``使用PBKDF2方法生成初始向量，需保存``Cipher::saltHeader()``。


非对称加密算法
^^^^^^^^^^^^^^

支持RSA/DSA密钥生成与管理：

.. code-block:: c++
    :caption: 生成RSA密钥

    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    ngDebug() << key.sign("fish is here.", MessageDigest::SHA256);
    ngDebug() << key.save();
    PrivateKey clonedKey = PrivateKey::load(key.save());

    
证书与证书请求
^^^^^^^^^^^^^^

支持SSL证书操作：

.. code-block:: c++
    :caption: 获取SSL证书信息

    Certificate cert = sslSocket.peerCertificate();
    ngDebug() << cert.subjectInfo(Certificate::CommonName);
    Certificate clonedCert = Certificate::load(cert.save());