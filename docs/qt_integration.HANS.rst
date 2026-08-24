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

兼容层支持 **Qt 5.6 及以上**（Qt 5.x 系列）与 Qt 6。

通过 ``add_subdirectory`` 引入时，兼容层会把选定的 Qt 主版本号 ``QTNG_QT_VERSION_MAJOR``
（值为 ``5`` 或 ``6``）经 ``PARENT_SCOPE`` 导出给父工程；父工程可直接据此选择自己的 Qt 组件，
无需重复探测::

    find_package(Qt${QTNG_QT_VERSION_MAJOR}Core CONFIG REQUIRED)

当系统同时安装了 Qt5 与 Qt6 时，可用 CMake 缓存选项 ``QTNG_QT_VERSION`` 选择使用哪一个：

* ``auto``（默认）—— 找到 Qt5 则优先使用 Qt5，否则用 Qt6
* ``5`` —— 强制 Qt5
* ``6`` —— 强制 Qt6

示例（强制 Qt6）::

    cmake -S /path/to/qtng/qt/examples/fetch_web_content -B build-fetch -DQTNG_QT_VERSION=6

编译器要求
----------

Qt5 目标以 C++11 编译，Qt6 目标以 C++17 编译（``qt/CMakeLists.txt`` 会自动设置
``CMAKE_CXX_STANDARD``）。Qt6 头文件使用了 ``<filesystem>``，因此编译 Qt6 版本需要
C++17 编译器（GCC 8+、Clang 7+ 或等效）；仅编译 Qt5 版本时 C++11 编译器（如 GCC 7）
即可。

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

在 GUI 线程（拥有 ``QCoreApplication`` 的线程）上，协程默认使用 **Qt 后端**事件循环：I/O 就绪用
``QSocketNotifier`` 监视，定时器用 ``QObject::startTimer()``，因此 ``QCoreApplication::exec()`` 驱动
所有已派生的协程。该设计复刻自 qtnetworkng 1.0，不再在 Qt 事件循环之外单独跑 libev/Win 循环。

Qt 后端事件循环由 binding 注册的工厂在首次使用协程时懒创建，无需先调用
``startQtLoop()`` 协程即可运行。调用 ``qtng::startQtLoop()`` 在它之上运行
``QCoreApplication::exec()``::

    int main(int argc, char **argv)
    {
        QCoreApplication app(argc, argv);
        qtng::Coroutine::spawn([]() { /* 协程工作，由 Qt 事件循环驱动 */ });
        return qtng::startQtLoop();
    }

若当前 core 循环**不是** Qt 后端——例如协程先在非 GUI 线程被使用（该线程保持默认的
libev/Win 循环），或调用了 ``useEventloop(EventLoopType::Ev)`` 在 GUI 线程强制 libev——
``startQtLoop()`` 返回 ``-1``。

选择事件循环后端
----------------

每个线程创建何种事件循环由 qtng 决定，可用两个公开 API 控制：

* ``qtng::useEventloop(qtng::EventLoopType)`` —— 显式选择后端。在 ``main()`` 的最前面、
  任何协程/网络 API 之前调用一次。内置类型为 ``EventLoopType::Ev``（1）与
  ``EventLoopType::Qt``（2）；第三方后端（io_uring、gtk、kqueue 等）经
  ``qtng_core::registerEventLoop()`` 以 ≥100 的值注册。
* ``qtng::useQtEventloop()`` —— 等价于 ``useEventloop(EventLoopType::Qt)`` 的简写。

不显式调用时使用默认后端：GUI 线程为 Qt 后端，其余线程为 libev/Win。
``useEventloop(EventLoopType::Ev)`` 即使在 GUI 线程也强制使用 libev（即旧版
qtnetworkng 1.0 的 ``preferLibev()``）。

``waitThread(QThread *)`` 与 ``waitProcess(QProcess *)`` 在协程中等待 Qt 对象结束，而不会阻塞事件循环：

* 若当前事件循环是 Qt 后端，则监视 ``QThread::finished`` / ``QProcess::finished`` 信号，调用方协程在事件上 yield。
* 否则（libev/Win 后端，或尚未启动 Qt 事件循环），改在工作线程上执行 ``QThread::wait()`` 或 ``waitpid`` / ``WaitForSingleObject``。没有 Qt 事件循环时 ``QProcess`` 状态不会更新，因此不能只连接信号。

Include 隔离
------------

链接 ``qtnetworkng`` 的应用**不得** ``#include <qtng/socket.h>`` 等核心头文件；Qt 用户仅应使用
``qt/include/`` 下的公开头（如 ``qtnetworkng.h``、``qtng.h``）。

API 覆盖
--------

兼容层以 Qt 类型暴露 qtng 2.0 全部公开 API，并向后兼容 1.0（``qtnetworkng.h``、``kcp.h`` 别名、
``pool.h`` 等）。2.0 新增模块包括 ``bencode``、``kademlia``、``noise``、``multi_stream``、``udp``。
