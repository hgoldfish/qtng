最佳实践
=========

使用 CoroutineGroup 管理协程
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

应始终通过 ``CoroutineGroup`` 启动协程，并在释放其他资源之前先销毁协程组。这样可以保证协程正常退出，且 lambda 捕获的对象在协程运行期间始终有效。


send()和sendall()、recv()和recvall()的不同点
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``send()`` 和 ``recv()`` 单次调用可能只传输部分字节。当需要发送或接收完整数据，或协议规定了确切长度时，应使用 ``sendall()`` 和 ``recvall()``。


使用 std::shared_ptr<T> 管理协程捕获对象
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

若 lambda 捕获的对象必须在协程运行期间保持有效，应通过值捕获，或将可变状态放入 ``std::shared_ptr<T>``。除非能确保对象在协程结束前一直存在，否则不要捕获指向栈上或短生命周期对象的裸指针。


用 setLogHandler 接管 ngDebug / ngWarning
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

默认情况下 ``ngDebug()`` / ``ngWarning()`` / ``ngCritical()`` / ``ngFatal()`` 写到 stderr。
可在进程启动时调用一次 ``qtng::utils::setLogHandler(handler)``，把每次 ``LogStream`` 刷出
转到自定义接收端（例如 TUI 日志栏）。传入 ``nullptr`` 恢复默认行为。在其它线程正在刷日志时
更换 handler 不是线程安全的。
