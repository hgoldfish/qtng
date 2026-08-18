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

When both Qt5 and Qt6 are installed, CMake cache option ``QTNG_QT_VERSION`` selects which one
to use:

* ``auto`` (default) — prefer Qt5 if found, otherwise Qt6
* ``5`` — require Qt5
* ``6`` — require Qt6

Example (force Qt6)::

    cmake -S /path/to/qtng/qt/examples/fetch_web_content -B build-fetch -DQTNG_QT_VERSION=6

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

Call ``qtng::startQtLoop()`` **before** using coroutine/network APIs in a Qt GUI or ``QCoreApplication``
process. It injects a Qt-backed ``EventLoopCoroutine`` via ``currentLoop()->set()`` and runs
``QCoreApplication::exec()``.

Use ``qtng::preferLibev()`` if you explicitly want the libev/Win backend instead of Qt integration.

``waitThread(QThread *)`` and ``waitProcess(QProcess *)`` wait for a Qt object from a coroutine without blocking the event loop:

* If the current loop is the Qt backend (``startQtLoop()`` was called), they watch ``QThread::finished`` / ``QProcess::finished`` and yield the caller on an event.
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
