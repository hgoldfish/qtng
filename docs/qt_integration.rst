Qt5/Qt6 optional binding
========================

The **qtng core** library (``libqtng``) has no Qt dependency. For Qt applications that want the
legacy **qtnetworkng** API (``QString``, ``QByteArray``, ``startQtLoop()``, ``qAwait()``, etc.),
use the optional binding under ``qt/``.

Build integration
-----------------

Include ``qt/CMakeLists.txt`` from your project and link ``qtnetworkng``::

    include(/path/to/qtng/qt/CMakeLists.txt)
    target_link_libraries(myapp PRIVATE qtnetworkng)

The binding target:

* **PUBLIC**: ``qt/include`` and Qt5/6::Core
* **PRIVATE**: ``qtng`` core (headers are **not** propagated to consumers)

The binding supports Qt **5.6 or newer** on the Qt5 line, as well as Qt 6.

When pulled in via ``add_subdirectory``, the binding exports the chosen Qt major
version as ``QTNG_QT_VERSION_MAJOR`` (``5`` or ``6``) into the parent scope, so parent
projects can select their own Qt components without re-detecting::

    find_package(Qt${QTNG_QT_VERSION_MAJOR}Core CONFIG REQUIRED)

When both Qt5 and Qt6 are installed, CMake cache option ``QTNG_QT_VERSION`` selects which one
to use:

* ``auto`` (default) — prefer Qt5 if found, otherwise Qt6
* ``5`` — require Qt5
* ``6`` — require Qt6

Example (force Qt6)::

    cmake -S /path/to/qtng/qt/examples/fetch_web_content -B build-fetch -DQTNG_QT_VERSION=6

Toolchain requirements
~~~~~~~~~~~~~~~~~~~~~~

The binding compiles Qt5 targets with C++11 and Qt6 targets with C++17
(``CMAKE_CXX_STANDARD`` is set automatically by ``qt/CMakeLists.txt``). Qt6 headers use
``<filesystem>``, so building the Qt6 variant needs a C++17 compiler (GCC 8+, Clang 7+ or
equivalent); a C++11 compiler such as GCC 7 is only sufficient for the Qt5 variant.

Minimal example
---------------

.. code-block:: c++

    #include <qtnetworkng.h>
    #include <QCoreApplication>

    int main(int argc, char **argv)
    {
        QCoreApplication app(argc, argv);
        qtng::HttpSession session;
        qtng::HttpResponse r = session.get(QStringLiteral("http://example.com/"));
        return qtng::startQtLoop();
    }

See ``qt/examples/fetch_web_content/`` for a complete CMake project. Build it manually::

    cmake -S /path/to/qtng/qt/examples/fetch_web_content -B build-fetch
    cmake --build build-fetch

The ``qt/examples/include_isolation/`` target must **fail** to compile when including ``<qtng/socket.h>`` (core headers are not propagated).

Qt event loop
-------------

On the GUI thread (the thread owning ``QCoreApplication``), coroutines use a **Qt-backed** event loop
by default: I/O readiness is watched with ``QSocketNotifier`` and timers with ``QObject::startTimer()``,
so ``QCoreApplication::exec()`` drives all spawned coroutines. This mirrors the qtnetworkng 1.0 design
instead of running a separate libev/Win loop.

The Qt-backed loop is created lazily on first coroutine use via a factory registered by the
binding; no ``startQtLoop()`` call is needed for coroutines to run. Calling
``qtng::startQtLoop()`` then runs ``QCoreApplication::exec()`` on top of it::

    int main(int argc, char **argv)
    {
        QCoreApplication app(argc, argv);
        qtng::Coroutine::spawn([]() { /* coroutine work, driven by the Qt event loop */ });
        return qtng::startQtLoop();
    }

``startQtLoop()`` returns ``-1`` if the current core loop is **not** the Qt backend — e.g. when
coroutines were first used on a non-GUI thread (that thread keeps its default libev/Win loop),
or when ``useEventloop(EventLoopType::Ev)`` forced libev on the GUI thread.

Selecting the event loop backend
--------------------------------

qtng decides which event loop to create on each thread; two public APIs control this:

* ``qtng::useEventloop(qtng::EventLoopType)`` — select the backend explicitly. Call it once at the
  very beginning of ``main()``, before any coroutine/network API. Built-in types are
  ``EventLoopType::Ev`` (1) and ``EventLoopType::Qt`` (2); third-party backends (io_uring, gtk,
  kqueue, ...) register under values >= 100 via ``qtng_core::registerEventLoop()``.
* ``qtng::useQtEventloop()`` — shortcut for ``useEventloop(EventLoopType::Qt)``.

Without an explicit call the default backend is used: the Qt backend on the GUI thread,
libev/Win elsewhere. ``useEventloop(EventLoopType::Ev)`` forces libev even on the GUI thread
(the former qtnetworkng 1.0 ``preferLibev()``).

``waitThread(QThread *)`` and ``waitProcess(QProcess *)`` wait for a Qt object from a coroutine without blocking the event loop:

* If the current loop is the Qt backend, they watch ``QThread::finished`` / ``QProcess::finished`` and yield the caller on an event.
* Otherwise (libev/Win backend, or the Qt event loop is not running) they join on a worker thread via ``QThread::wait()`` or ``waitpid`` / ``WaitForSingleObject``. ``QProcess`` does not update its state without a Qt event loop, so connecting to the signal alone is not enough.

Include isolation
-----------------

Applications linked against ``qtnetworkng`` must **not** include core headers such as
``#include <qtng/socket.h>``. Only headers under ``qt/include/`` (e.g. ``qtnetworkng.h``,
``qtng.h``) are supported for Qt users.

API coverage
------------

The binding exposes the full qtng 2.0 public surface with Qt types, plus 1.0 compatibility
(``qtnetworkng.h``, ``kcp.h`` aliases, ``pool.h``, etc.). New 2.0 modules include
``bencode``, ``kademlia``, ``noise``, ``multi_stream``, and ``udp``.
