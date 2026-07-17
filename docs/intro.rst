Introduction to qtng
===========================

qtng is a coroutine-based networking programming toolkit, like boost::asio but inspired by Python gevent. It offers a simpler, coroutine-first API for network programming.


Why Coroutines
--------------

The Coroutine is not a new thing, Python, Go, and C# was using coroutines to simplify network programming many years ago. 

The traditional network programming use threads. ``send()/recv()`` are blocked, and then the Operating System switch current thread to another ready thread until data arrived. This is very straightforward, and easy for network programming. But threads use heavy resources, thousands of connections may consume many memory. More worse, threads cause data races, data currupt, even crashes.

Another choice is using callback-based paradigm. Before calling ``send()/recv()``, use ``select()/poll()/epoll()`` to determine data arriving. ``select()`` is blocked, but many connections are handled in one thread. Callback-based paradigm is considered "the new-age goto", hard to understand and read/write code. But it is used widely by C++ programmer for the popularity of boost::asio and other traditional C++ networking programming frameworks.

Coroutine-based paradigm is the now and the future of network programming. Coroutines are light-weight threads which have their own stack, not managed by the Operating System but qtng. Like thread-based paradigm, ``send()/recv()`` are blocked, but switch to another coroutine in the same thread unitl data arrived. Many coroutines can be created at low cost. Because there is only one thread, there is no data race. The API is straightforward like thread-based paradigm, but avoid the complexities of using threads.


Cross platforms
----------------

qtng is tested in Linux, Android, Windows, MacOS, and OpenBSD. And support gcc, clang, mingw32, msvc.

Building qtng requires a C++11 compiler, zlib, and TLS/crypto support via bundled LibreSSL (when a ``libressl/`` subdirectory is present) or system OpenSSL/LibreSSL. The bundled unit tests require C++17 and a system-installed Catch2 v3 (or newer); they are not built by default. Catch2 is never downloaded automatically — see "Build and run tests" below for how to install it.

The coroutine is implemented using boost::context asm code, and support native posix `ucontext` and windows `fiber` API. Running tests is success in ARM, ARM64, x86, amd64.

The library uses libev for its event loop on Unix and provides SSL/cipher functions through LibreSSL or OpenSSL.


Use qtng in CMake projects
---------------------------------

Clone qtng from GitHub and add it as a subdirectory of your project:

.. code-block:: bash
    :caption: get qtng

    git clone https://github.com/hgoldfish/qtng.git

An example ``CMakeLists.txt``:

.. code-block:: cmake

    cmake_minimum_required(VERSION 3.14)
    project(foo LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    add_subdirectory(qtng)

    add_executable(foo main.cpp)
    target_link_libraries(foo PRIVATE qtng)

Example ``main.cpp``:

.. code-block:: c++
    :caption: get web page

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

To build:

.. code-block:: bash
    :caption: build project

    mkdir build
    cd build
    cmake ..
    cmake --build .

Unit tests are off by default and require a C++17 compiler and a system-installed Catch2 v3 (or newer). Catch2 is intentionally never fetched from the network; install it yourself first, for example:

.. code-block:: bash
    :caption: install Catch2 v3 (pick one)

    sudo apt install catch2          # Debian/Ubuntu bookworm+
    sudo dnf install catch2-devel    # Fedora
    sudo pacman -S catch2            # Arch Linux
    brew install catch2              # macOS (Homebrew)
    vcpkg install catch2             # vcpkg

Then enable and run the tests:

.. code-block:: bash
    :caption: build and run tests

    cmake -DQTNG_BUILD_TESTS=ON ..
    cmake --build .
    ctest

If Catch2 is installed to a non-default prefix, pass it to CMake with ``-DCMAKE_PREFIX_PATH=...`` (or ``-DCatch2_ROOT=...``). Without Catch2, configuring with ``QTNG_BUILD_TESTS=ON`` fails with an error; leaving ``QTNG_BUILD_TESTS=OFF`` builds the qtng library without any Catch2 dependency.

Install to a system prefix (optional):

.. code-block:: bash
    :caption: install qtng

    sudo cmake --install . --prefix /usr/local

The umbrella header is installed as ``include/qtng.h``; module headers under ``include/qtng/``; the static library is ``libqtng.a`` under ``lib/`` (or ``lib64/`` on some platforms; ``qtng.lib`` on MSVC). When installed, use ``#include <qtng.h>`` or ``#include <qtng/coroutine.h>`` (or ``#include <qtng/qtng.h>``).


The Coroutine 
-------------

qtng is created base on the ``Coroutine``. Make sure qtng's network operations is running in ``Coroutine``. Be convenient, the main thread is converted to Coroutine implicitly. There are two ways to create Coroutine. I strong recommend using ``CoroutineGroup``, as it use ``std::shared_ptr`` to manage coroutines instead of raw pointer, and considers many corner cases.

.. code-block:: c++
    :caption: start coroutine
    
    void coroutine_entry()
    {
        Coroutine::sleep(1.0); // sleep 1s
        ngDebug() << "I am coroutine: " << Coroutine::current().id();
    }
    // I strong recommend using CoroutineGroup.
    CoroutineGroup operations;
    std::shared_ptr<Coroutine> coroutine = operations.spawn(coroutine_entry);
    
    // Or manage coroutine yourself.
    std::shared_ptr<Coroutine> coroutine = Coroutine::spawn(coroutine_entry);
    
Call ``Coroutine::start()`` schedule coroutine to start. And ``Coroutine::kill()`` to send exception to coroutine. Two function return immediately, while coroutine will start or be killed later.

The CoroutineGroup can spawn coroutines, and kill or get coroutines by name.

.. code-block:: c++
    :caption: manage many coroutines
    
    CoroutineGroup operations;
    operations.spawnWithName("coroutine1", coroutine_entry);
    operations.kill("coroutine1");
    operations.killall();

Killing coroutine safely is a big advanced feature of coroutine compare to thread and process. If coroutine is killed by other coroutine, it will throw a ``CoroutineExit`` exception. At your will, any exception based on ``CoroutineException`` can be thrown. Coroutine is killed and joined before deleted.

.. code-block:: c++
    :caption: how to kill coroutine
    
    coroutine.kill(new MyCoroutineException());

    void coroutine_entry()
    {
        try {
            communicate_with_remote_host();
        } catch (MyCoroutineException const &e) {
            // deal with exception.
        }
    }
    
The ``CoroutineExit`` exception is handled by qtng silently.


Note on Qt GUI integration (removed in 2.0)
-------------------------------------------

qtng 2.0 no longer integrates with Qt Widgets or the Qt event loop. Use a standard ``main()`` entry point; the library manages its own event loop internally. Functions such as ``startQtLoop()`` and ``qAwait()`` have been removed.


The Socket and SslSocket
------------------------

The main purpose to create qtng is to simplify C++ network programming. There are many great networking programming toolkits already, like boost::asio, libco, libgo, poco, QtNetowrk and others. Many of them has complex callback-style API, or just simple coroutine implementations without Object Oriented socket API. 

The ``Socket`` class is a straightforward transliteration of the bsd socket interface to object-oriented interface. 

``SslSocket`` has the same interface as ``Socket``, but do ssl handshake after connection established.

``Socket`` and ``SslSocket`` objects can be converted to ``SocketLike`` objects, which are useful for functions accept both ``Socket`` and ``SslSocket`` parameter.

There is a ``KcpSocket`` implementing KCP over UDP. It has a simpliar API like ``Socket``, and support converting to ``SocketLike`` too.


Create Socket client
^^^^^^^^^^^^^^^^^^^^

``Socket`` class has two constructors. One accpets plain unix socket descriptor and another accpets protocol family and socket type.

.. code-block:: c++
    :caption: connect to remote host
    
    // only for ipv4
    Socket s(Socket::IPv4Protocol, Socket::TcpSocket);
    bool ok = s.connect(remoteHost, 80);
    
    // auto detect ipv4/ipv6 host.
    std::unique_ptr<Socket> s(Socket::createConnection(remoteHost, 80));
    bool ok = static_cast<bool>(s);
    
    Socket s(socketDescriptor); // socketDescriptor is set to nonblocking.
    bool ok = s.connect(remoteHost, 80);
    
The ``SslSocket`` has similar constructors which accpet an extra ``SslConfiguration``
    
.. code-block:: c++
    :caption: connect to remote ssl server.
    
    // only for ipv4
    SslConfiguration config;
    SslSocket s(Socket::IPv4Protocol, config);
    bool ok = s.connect(remoteHost, 443);
    
    // auto detect ipv4/ipv6 host
    SslConfiguration config;
    std::unique_ptr<SslSocket> s(SslSocket::createConnection(remoteHost, 443, config));
    bool ok = static_cast<bool>(s);
    
    SslSocket s(socketDescriptor, config);
    bool ok = s.connect(remoteHost, 443);
    
    
Create socket server
^^^^^^^^^^^^^^^^^^^^

Combine ``Socket`` and ``Coroutine``, you can create socket server in few lines of code.

.. code-block:: c++
    :caption: tcp server
    
    std::unique_ptr<Socket> s(Socket::createServer(HostAddress::AnyIPv4, 8000, 100));
    CoroutineGroup operations;
    while (true) {
        std::shared_ptr<Socket> request(s->accept());
        if (!request) {
            break;
        }
        operations.spawn([request] {
            request->sendall("hello!");
        });
    }
    
    
Http Client
-----------

qtng provides a HTTP client support http 1.1 and https, can handle socks5 proxies, cookies, redirection and many data types such as form-data, etc..

HTTP 2.0 is planned.

The API are inspired by *requests* module of Python.


Get url from HTTP server
^^^^^^^^^^^^^^^^^^^^^^^^

qtng implement HTTP client in ``HttpSession`` class. To fetch data from or send data to HTTP server, you should create ``HttpSession`` object first.

.. code-block:: c++
    :caption: get web page
    
    qtng::HttpSession session;
    HttpResponse resp = session.get(url);
    
The ``HttpSession`` accept and store cookies from response, so sessions is persisted among HTTP requests. 


Send data to HTTP server
^^^^^^^^^^^^^^^^^^^^^^^^

The most common method to send data to HTTP server is making HTTP POST form data request.

.. code-block:: c++
    :caption: post query
    
    FormData data;
    data.addQuery("name", "fish");
    data.addFile("file", "filename.txt", std::string("file content"));
    HttpResponse resp = session.post(url, data.toByteArray());
    
Or send a JSON payload as a raw string (you must set Content-Type yourself):

.. code-block:: c++
    :caption: post json string
    
    HttpRequest request("POST", url);
    request.setHeader("Content-Type", "application/json");
    request.setBody(R"({"name":"fish"})");
    HttpResponse resp = session.send(request);
    

With headers:

.. code-block:: c++
    :caption: post headers
    
    HttpRequest request("POST", url);
    request.setHeader("Content-Type", "application/json");
    request.setBody(R"({"username":"somebody","password":"secret"})");
    std::map<std::string, std::string> headers;
    headers.insert({"X-My-Header", "test"});
    request.setHeaders(headers);
    HttpResponse resp = session.send(request);

Get data from ``HttpResponse``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``HttpResponse`` contains all the data from HTTP server, such as headers, content, and status code.

.. code-block:: c++
    :caption: get response information

    HttpResponse resp = session.get(url);
    ngDebug() << resp.isOk();  // return true if there is no error
    ngDebug() << resp.getContentType();  // the content type of response.
    ngDebug() << resp.statusCode();  // the status code of response: 200
    ngDebug() << resp.statusText();  // the status text of response: OK
    
``HttpResponse`` can handle many data types.

.. code-block:: c++
    :caption: get response content

    ngDebug() << resp.text();  // as UTF8 std::string
    ngDebug() << resp.html();  // as UTF8 std::string
    ngDebug() << resp.body();  // as std::string
    ngDebug() << resp.bodyAsFile() // as a FileLike which can be read or write.


Cryptography
------------

qtng use LibreSSL or OpenSSL to provide many cryptography routines.


Message Digest
^^^^^^^^^^^^^^

qtng support most OpenSSL Message Digest.

.. code-block:: c++
    :caption: hash message using sha512

    MessageDigest m(MessageDigest::SHA512);
    m.update("data");
    ngDebug() << m.hexDigest();
    
    
Symmetrical encryption and decryption
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

QtNetworNg support many ciphers, such as AES, Blowfish, and ChaCha20.


.. code-block:: c++
    :caption: encrypt message using aes256_cbf
    
    Cipher ciph(Cihper::AES256, Cipher::CBF, Cipher::Encrypt);
    ciph.setPassword("thepassword", MessageDigest::Sha256, "salt");
    std::string encrypted = ciph.update("fish");
    encrypted.push_back(ciph.final();

``Cipher::setPassword()`` generate initial vector using PBKDF2 method. You should save ``Cipher::saltHeader()`` before saving the final data.


Public Key Algorithm
^^^^^^^^^^^^^^^^^^^^

qtng can generate and manipulate RSA/DSA keys.

.. code-block:: c++
    :caption: generate rsa key

    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    ngDebug() << key.sign("fish is here.", MessageDigest::SHA256);
    ngDebug() << key.save();
    PrivateKey clonedKey = PrivateKey::load(key.save());

    
Certificate and CertificateRequest
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

qtng can manipulate Certificate from ssl socket, or new-generated certificates.

.. code-block:: c++
    :caption: get ssl connection certificate.

    Certificate cert = sslSocket.peerCertificate();
    ngDebug() << cert.subjectInfo(Certificate::CommonName);
    Certificate clonedCert = Certificate::load(cert.save());
    
