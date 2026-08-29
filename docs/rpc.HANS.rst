qtng/rpc 子库
================

``qtng/rpc`` 是 qtng 的可选子库，基于 ``qtng::SocketLike`` 实现远程过程调用。它与旧的
``lafrpc/cpp`` 协议（MessagePack 分帧）wire 兼容，旧 lafrpc 客户端可以直接连接
``qtng::rpc`` 服务端，反之亦然。

.. note::

   ``qtng/rpc`` 需要 C++17。仅当顶层 ``-DQTNG_BUILD_RPC=ON`` 或消费者直接
   ``add_subdirectory(qtng/rpc)`` 时构建。在 ``CMakeLists.txt`` 中加入::

       add_subdirectory(qtng)          # 提供 qtng 目标
       add_subdirectory(qtng/rpc)      # 提供 qtng_rpc 目标

       target_link_libraries(foo PRIVATE qtng_rpc)

   聚合头为 ``qtng/rpc.h``。

设计
----

``qtng::rpc::Rpc`` 本身**不管理连接**，只处理现成的 ``std::shared_ptr<qtng::SocketLike>``:

* 入站：应用 accept 出 socket（TCP/SSL/KCP，或 HTTP ``Upgrade: lafrpc`` 升级流），交给
  ``Rpc::handleRequest(socket)``；
* 出站：应用连好 socket，交给 ``Rpc::connect(socket, name)``。

连接如何建立、监听如何启动，完全由应用负责。rpc 层只在 socket 上使用 lafrpc 的
MessagePack 分帧协议。

TCP 连接上的最小服务端/客户端对：

.. code-block:: c++

    #include <qtng/rpc.h>
    #include <qtng/socket.h>

    using namespace qtng;
    namespace rpc = qtng::rpc;

    class EchoService : public rpc::Service, public std::enable_shared_from_this<EchoService>
    {
    public:
        void bindAll()
        {
            bind("echo", rpc::bindMethod(shared_from_this(), &EchoService::echo));
        }
        std::string echo(const std::string &s) { return s; }
    };

    // 服务端（在 accept 出连接的协程里）：
    std::shared_ptr<rpc::Rpc> server = rpc::Rpc::builder().create();
    std::shared_ptr<EchoService> service = std::make_shared<EchoService>();
    service->bindAll();
    server->registerInstance(service, "demo");
    server->handleRequest(acceptedSocket);           // 阻塞，服务期间持有

    // 客户端（在协程里）：
    std::shared_ptr<rpc::Rpc> client = rpc::Rpc::builder().create();
    std::shared_ptr<rpc::Peer> peer = client->connect(connectedSocket, "");
    rpc::Value result = peer->call("demo.echo", rpc::Value::str("hello"));
    // result.asStr() == "hello"

值类型
------

``rpc::Value`` 是 wire 值类型。它显式区分 msgpack 字符串与二进制缓冲
（``Value::str()`` 与 ``Value::bin()``），支持 nil、bool、整数、double、数组、
map（``std::map``，按键排序）、日期时间（timestamp ext）、任意 ext 数据以及已注册的可序列化对象。

注册类使用 ``__laf_sid__`` map 键，与旧协议字节兼容。派生 ``rpc::Serializable`` 并用
``rpc::detail::registerClass<T>()`` 注册（内置 ``RpcRemoteException``/``RpcFile``/``RpcDir``
在创建 ``Rpc`` 时自动注册）。

方法注册
--------

std 版没有基于反射的分派（QObject slots）。用 ``rpc::bindMethod(obj, &T::method)``
显式绑定方法，模板在编译期把 ``rpc::Value`` 参数转换为 typed 参数。
``rpc::Service`` 持有一张 方法名 -> ``RpcFunction`` 的分发表：

.. code-block:: c++

    service->bind("add", rpc::bindMethod(service, &CalcService::add));
    serverRpc->registerInstance(service, "calc");
    peer->call("calc.add", rpc::Value(2), rpc::Value(3));   // 5

错误
----

调用失败会抛异常。``rpc::RpcRemoteException``（及其注册的派生类）在调用端从 wire
载荷重建后抛出，远端异常以原始类型到达。连接在等待响应期间丢失时抛
``rpc::RpcDisconnectedException``。

文件与目录传输
--------------

``rpc::RpcFile`` 与 ``rpc::RpcDir`` 通过 ``qtng::VirtualChannel``（或
``preferRawSocket`` 时的 raw socket）流式传输文件/目录内容，可作为普通参数/返回值：

.. code-block:: c++

    // 服务端下载文件：
    std::shared_ptr<rpc::RpcFile> download()
    {
        std::shared_ptr<rpc::RpcFile> f = rpc::RpcFile::prepareToSend(size);
        Coroutine::spawn([f] { f->sendall(content); });   // 在另一个协程里流式发送
        return f;
    }

    // 客户端接收：
    std::shared_ptr<rpc::RpcFile> f = peer->call("file.download").asShared<rpc::RpcFile>();
    std::string data;
    f->recvall(data);

注意流式约定：``Peer::call()`` 阻塞等待响应期间，文件内容必须在**另一个协程**中写出
（``sendall()`` / ``readFrom()``），与旧 lafrpc 完全一致。

事件
----

取代 Qt 信号：``Rpc`` 提供 ``newPeer``（``rpc::EventDispatcher<std::shared_ptr<Peer>>``），
``Peer`` 提供 ``disconnected``（``rpc::EventDispatcher<Peer *>``）。多播回调用
``connect(cb)`` 注册，用 ``connectWeak(shared_ptr<obj>, cb)`` 在对象销毁时自动解绑。

线程模型
--------

qtng 是协程式单线程库：只在协程里调用 ``Peer::call()``、``Rpc::handleRequest()``、
``Rpc::connect()``、``RpcFile``/``RpcDir`` 的收发函数。重活（文件哈希、目录收集）可用
``callInThread()`` 系列工具。
