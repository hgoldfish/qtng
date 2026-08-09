Qt5/Qt6 可选兼容层
==================

**qtng 核心库**（``libqtng``）不依赖 Qt。若 Qt 应用需要旧版 **qtnetworkng** API（``QString``、
``QByteArray``、``startQtLoop()``、``qAwait()`` 等），请使用 ``qt/`` 下的可选兼容层。

构建集成
--------

在工程中 ``include(qt/CMakeLists.txt)`` 并链接 ``qtnetworkng``::

    include(/path/to/qtng/qt/CMakeLists.txt)
    target_link_libraries(myapp PRIVATE qtnetworkng)

该目标：

* **PUBLIC**：``qt/include`` 与 Qt5/6::Core
* **PRIVATE**：``qtng`` 核心（**不会**向消费者传播核心头文件路径）

最小示例
--------

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

完整 CMake 示例见 ``qt/examples/fetch_web_content/``。请手动配置并构建::

    cmake -S /path/to/qtng/qt/examples/fetch_web_content -B build-fetch
    cmake --build build-fetch

Include 隔离验证：``qt/examples/include_isolation/`` 尝试 ``#include <qtng/socket.h>`` 应编译失败（核心头未传播）。

Qt 事件循环
-----------

在 Qt GUI 或 ``QCoreApplication`` 进程中，请在协程/网络 API 之前调用 ``qtng::startQtLoop()``。
它通过 ``currentLoop()->set()`` 注入 Qt 版 ``EventLoopCoroutine`` 并运行 ``QCoreApplication::exec()``。

若明确要使用 libev/Win 后端而非 Qt 集成，可调用 ``qtng::preferLibev()``。

Include 隔离
------------

链接 ``qtnetworkng`` 的应用**不得** ``#include <qtng/socket.h>`` 等核心头文件；Qt 用户仅应使用
``qt/include/`` 下的公开头（如 ``qtnetworkng.h``、``qtng.h``）。

API 覆盖
--------

兼容层以 Qt 类型暴露 qtng 2.0 全部公开 API，并向后兼容 1.0（``qtnetworkng.h``、``kcp.h`` 别名、
``pool.h`` 等）。2.0 新增模块包括 ``bencode``、``kademlia``、``noise``、``multi_stream``、``udp``。
