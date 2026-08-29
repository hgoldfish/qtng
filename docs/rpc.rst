The qtng/rpc Sublibrary
========================

``qtng/rpc`` is an optional sublibrary of qtng that implements remote procedure
calls over ``qtng::SocketLike``. It is wire-compatible with the legacy
``lafrpc/cpp`` protocol (MessagePack framing), so an existing lafrpc client can
talk to a ``qtng::rpc`` server and vice versa.

.. note::

   ``qtng/rpc`` requires C++17. It is built only when the top-level
   ``-DQTNG_BUILD_RPC=ON`` option is given, or when a consumer does
   ``add_subdirectory(qtng/rpc)`` directly. Add it to your ``CMakeLists.txt``::

       add_subdirectory(qtng)          # provides the qtng target
       add_subdirectory(qtng/rpc)      # provides the qtng_rpc target

       target_link_libraries(foo PRIVATE qtng_rpc)

   The umbrella header is ``qtng/rpc.h``.

Design
------

``qtng::rpc::Rpc`` does **not** manage connections itself. It only works with
ready-made ``std::shared_ptr<qtng::SocketLike>`` connections:

* inbound: the application accepts a socket (TCP/SSL/KCP, or an HTTP
  ``Upgrade: lafrpc`` stream) and hands it to ``Rpc::handleRequest(socket)``;
* outbound: the application connects a socket and hands it to
  ``Rpc::connect(socket, name)``.

How connections are made and how listeners are run is entirely up to the
application. The rpc layer only speaks the lafrpc MessagePack frame protocol
over the socket.

A minimal server/client pair over a TCP connection:

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

    // server side (inside a coroutine that accepted a connection):
    std::shared_ptr<rpc::Rpc> server = rpc::Rpc::builder().create();
    std::shared_ptr<EchoService> service = std::make_shared<EchoService>();
    service->bindAll();
    server->registerInstance(service, "demo");
    server->handleRequest(acceptedSocket);           // blocks while serving

    // client side (inside a coroutine):
    std::shared_ptr<rpc::Rpc> client = rpc::Rpc::builder().create();
    std::shared_ptr<rpc::Peer> peer = client->connect(connectedSocket, "");
    rpc::Value result = peer->call("demo.echo", rpc::Value::str("hello"));
    // result.asStr() == "hello"

Values
------

``rpc::Value`` is the wire value type. It distinguishes msgpack strings and
binary buffers explicitly (``Value::str()`` vs ``Value::bin()``), and supports
nil, bool, integers, doubles, arrays, maps (``std::map``, key-sorted),
datetimes (timestamp ext), arbitrary ext data and registered serializable
objects.

Registered classes use the ``__laf_sid__`` map key, byte-compatible with the
legacy protocol. Derive from ``rpc::Serializable`` and register with
``rpc::detail::registerClass<T>()`` (the built-in ``RpcRemoteException``,
``RpcFile`` and ``RpcDir`` are registered automatically when an ``Rpc`` is
created).

Method registration
-------------------

Reflection-based dispatch (QObject slots) does not exist in the std version.
Bind methods explicitly with ``rpc::bindMethod(obj, &T::method)``; the template
converts ``rpc::Value`` arguments to the typed parameters at compile time. A
``rpc::Service`` holds a name -> ``RpcFunction`` dispatch table:

.. code-block:: c++

    service->bind("add", rpc::bindMethod(service, &CalcService::add));
    serverRpc->registerInstance(service, "calc");
    peer->call("calc.add", rpc::Value(2), rpc::Value(3));   // 5

Errors
------

Calls throw on failure. ``rpc::RpcRemoteException`` (and registered subclasses)
are rebuilt on the calling side from the wire payload, so a remote exception
arrives as its original type. ``rpc::RpcDisconnectedException`` is thrown when
the connection is lost while waiting for a response.

File and directory transfer
---------------------------

``rpc::RpcFile`` and ``rpc::RpcDir`` stream file/directory contents over a
``qtng::VirtualChannel`` (or a raw socket when ``preferRawSocket`` is set).
They are passed as ordinary arguments/results:

.. code-block:: c++

    // server downloads a file:
    std::shared_ptr<rpc::RpcFile> download()
    {
        std::shared_ptr<rpc::RpcFile> f = rpc::RpcFile::prepareToSend(size);
        Coroutine::spawn([f] { f->sendall(content); });   // stream in another coroutine
        return f;
    }

    // client receives it:
    std::shared_ptr<rpc::RpcFile> f = peer->call("file.download").asShared<rpc::RpcFile>();
    std::string data;
    f->recvall(data);

Note the streaming contract: while ``Peer::call()`` blocks waiting for the
response, the file content must be written (``sendall()`` / ``readFrom()``) in
another coroutine, exactly like the legacy lafrpc.

Events
------

Instead of Qt signals, ``Rpc`` exposes ``newPeer`` (a
``rpc::EventDispatcher<std::shared_ptr<Peer>>``) and ``Peer`` exposes
``disconnected`` (``rpc::EventDispatcher<Peer *>``). Multi-cast callbacks can
be registered with ``connect(cb)`` and auto-unbound with
``connectWeak(shared_ptr<obj>, cb)``.

Threading
---------

qtng is a coroutine-based single-threaded toolkit: call ``Peer::call()``,
``Rpc::handleRequest()``, ``Rpc::connect()``, ``RpcFile``/``RpcDir``
send/receive functions from coroutines only. ``callInThread()``-style helpers
are available for heavy file hashing/directory population.
