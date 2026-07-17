qtng 参考文档
====================

1. 使用协程
-----------

1.1 基础与示例
^^^^^^^^^^^^^^

协程是轻量级线程。在其他编程语言中，也被称为 *fiber* 、 *goroutine* 、 *greenlet* 等。协程拥有独立的栈空间，可以手动切换（yield）到其他协程。

.. code-block:: c++
    :caption: 示例 1: 在两个协程间进行切换

    // 警告: yield() 通常不直接使用, 这里只是为了展示协程的切换
    #include <qtng.h>
    
    
    using namespace qtng;
    
    class MyCoroutine: public BaseCoroutine {
    public:
        MyCoroutine()
        :BaseCoroutine(nullptr) {
            // 保存协程上下文
            old = BaseCoroutine::current();
        }
        void run() {
            ngDebug() << "我的协程在这里";
            // 切换回主协程
            old->yield();
        }
    private:
        BaseCoroutine *old;
    };
    
    int main() {
        // 一旦创建了一个新的协程，主线程就会隐式地转换为主协程。
        MyCoroutine m;
        ngDebug() << "主协程在这里";
        // 切换到新的协程，yield（）函数返回直到切换回来。
        m.yield();
        ngDebug() << "返回主协程";
        return 0;
    }

上述示例中，我们首先定义继承自``BaseCoroutine``的``MyCoroutine``，并重写其``run()``成员函数。程序输出：

.. code-block:: text
    :caption: 示例1的输出

    主协程在这里
    我的协程在这里
    返回主协程
``BaseCoroutine::raise()`` 与 ``BaseCoroutine::yield()`` 类似，但会向目标协程发送``CoroutineException``异常。

实际开发中更常用的是``Coroutine::start()``和``Coroutine::kill()``。qtng 将协程功能分为``BaseCoroutine``和``Coroutine``两个类：

- ``BaseCoroutine``：提供基础切换功能
- ``Coroutine``：通过事件循环协程实现调度

示例2:展示两个协程交替执行

.. code-block:: c++
    :caption: 示例 2: 两个协程交替运行.
    
    #include "qtng.h"
    
    using namespace qtng;
    
    struct MyCoroutine: public Coroutine {
        MyCoroutine(const std::string &name)
            : name(name) {}
        void run() override {
            for (int i = 0; i < 3; ++i) {
                ngDebug() << name << i;
                // 进入事件循环，将在100 ms后切换回来。详情参见1.7.
                msleep(100); 
            }
        }
        std::string name;
    };
    
    int main() {
        MyCoroutine coroutine1("coroutine1");
        MyCoroutine coroutine2("coroutine2");
        coroutine1.start();
        coroutine2.start();
        // 切换回主协程
        coroutine1.join();
        // 切换到第二个协程来完成它
        coroutine2.join();
        return 0;
    }

输出结果：

.. code-block:: text
    :caption: 示例2的输出
    
    "coroutine1" 0
    "coroutine2" 0
    "coroutine1" 1
    "coroutine2" 1
    "coroutine1" 2
    "coroutine2" 2

1.2 启动协程
^^^^^^^^^^^^

.. note:: 

    使用 ``CoroutineGroup::spawn()`` 或 ``CoroutineGroup::spawnWithName()`` 来启动和管理新协程。

有多种方式可以启动新协程：

* 继承 ``Coroutine`` 并重写 ``Coroutine::run()`` 函数，该函数将在新协程中运行。
        
.. code-block:: c++
    :caption: 示例3: 启动协程的第一种方法
    
    class MyCoroutine: public Coroutine {
    public:
        virtual void run() override {
            // 在新协程中运行
        }
    };
    
    void start() {
        MyCoroutine coroutine;
        coroutine.join();
    }
    
* 将函数传递给 ``Coroutine::spawn()`` 函数，该函数会返回新协程。传递的函数将在新协程中被调用。

.. code-block:: c++
    :caption: 示例4: 启动协程的第二种方法
    
    void sendMessage() {
        // 在新协程中运行
    }
    Coroutine *coroutine = Corotuine::spawn(sendMessage);
    
* ``Coroutine::spawn()`` 接受 ``std::function<void()>`` 函数对象，因此也支持 C++11 lambda 表达式。

.. code-block:: c++
    :caption: 示例5: 启动协程的第三种方法
    
    std::shared_ptr<Event> event = std::make_shared<Event>();
    Coroutine *coroutine = Coroutine::spawn([event]{
        // 在新协程中运行
    });
    
.. note::

    捕获的对象必须在协程启动后继续存在。更多细节参考《最佳实践》。

.. method:: Deferred<BaseCoroutine*> BaseCoroutine::started`

和

.. method:: Deferred<BaseCoroutine*> BaseCoroutine::finished


1.3 操作协程
^^^^^^^^^^^^^^^^^^^^^^

最常用的函数位于 ``Coroutine`` 类中。

.. method:: bool Coroutine::isRunning() const

    检查协程是否正在运行，返回 true 或 false。

.. method:: bool Coroutine::isFinished() const

    检查协程是否已完成。若协程未启动或仍在运行则返回 false，否则返回 `true`。

.. method:: Coroutine *Coroutine::start(int msecs = 0);

    调度协程在当前协程阻塞时启动，并立即返回。参数 ``msecs`` 指定协程启动前的等待微秒数（从 ``start()`` 调用时开始计时）。返回 `this` 协程对象以支持链式调用。例如：

    .. code-block:: c++
        :caption: 示例7: 启动协程
        
        std::shared_ptr<Coroutine> coroutine(new MyCoroutine);
        coroutine->start()->join();

.. method:: void Coroutine::kill(CoroutineException *e = 0, int msecs = 0)

    调度协程在当前协程阻塞时抛出 ``CoroutineException`` 类型异常 ``e``，并立即返回。参数 ``msecs`` 指定操作执行前的等待微秒数（从 ``kill()`` 调用时开始计时）。

    若未指定参数 ``e``，将发送 ``CoroutineExitException``。

    若协程尚未启动，调用 ``kill()`` 可能导致协程启动后立即抛出异常。若需避免此行为，请改用 ``cancelStart()``。

.. method:: void Coroutine::cancelStart()

    若协程已被调度启动，本函数可取消该调度。若协程已启动，本函数将终止协程。最终协程状态会被设为 ``Stop``。

.. method:: bool Coroutine::join()

    阻塞当前协程直至目标协程停止。本函数将切换当前协程至事件循环协程，后者负责执行调度任务（如启动新协程、检查套接字可读/写状态）。

.. method:: virtual void Coroutine::run()

    重写本函数以定义协程逻辑。参考 *1.2 启动协程*。

.. method:: static Coroutine *Coroutine::current()

    静态函数返回当前协程对象指针。请勿保存该指针。

.. method:: static void Coroutine::msleep(int msecs)

    静态函数阻塞当前协程 ``msecs`` 微秒后唤醒。

.. method:: static void Coroutine::sleep(float secs)

    静态函数阻塞当前协程 ``secs`` 秒后唤醒。

.. method:: static Coroutine *Coroutine::spawn(std::function<void()> f)

    静态函数通过函数对象 ``f`` 启动新协程。参考 *1.2 启动协程*。

``BaseCoroutine`` 包含一些较少使用的函数，使用时需谨慎。

.. method:: State BaseCoroutine::state() const

    返回协程当前状态（``Initialized``, ``Started``, ``Stopped``, ``Joined``）。建议优先使用 `Coroutine::isRunning()` 或 ``Coroutine::isFinished()``。

.. method:: bool BaseCoroutine::raise(CoroutineException *exception = 0)

    立即切换至目标协程并抛出 ``CoroutineException`` 类型异常。若未指定 ``exception``，默认抛出 ``CoroutineExitException``。
    
    建议优先使用 ``Coroutine::kill()``。

.. method:: bool BaseCoroutine::yield()

    立即切换至目标协程。
    
    建议优先使用 ``Coroutine::start()``。

.. method:: std::uintptr_t BaseCoroutine::id() const

    返回协程唯一不可变 ID（通常为协程指针值）。

.. method:: BaseCoroutine *BaseCoroutine::previous() const

    返回本协程结束后将切换到的 ``BaseCoroutine`` 指针。

.. method:: void BaseCoroutine::setPrevious(BaseCoroutine *previous)

    设置本协程结束后将切换到的 ``BaseCoroutine`` 指针。

.. attribute:: Deferred<BaseCoroutine*> BaseCoroutine::started

    本属性为 ``Deferred`` 对象，可注册回调在协程启动后执行操作。

.. attribute:: Deferred<BaseCoroutine*> BaseCoroutine::finished

    本属性为 ``Deferred`` 对象，可注册回调在协程结束后执行操作。

1.4 使用 CoroutineGroup 管理多个协程
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

在 C++ 中创建和删除协程较为复杂，主要由于内存管理问题。通常需确保协程使用的资源在外部删除前协程已退出，并遵循以下规则：

• Lambda 捕获的不可变对象必须通过值传递（非指针或引用）
• 捕获可变对象时应使用智能指针（如 ``std::shared_ptr<>``）
• 若捕获 ``this`` 指针，需确保对象生命周期
• 在所有资源删除前删除协程

``CoroutineGroup`` 的使用模式遵循三条原则：

• 在类中声明 ``CoroutineGroup`` 指针（非值类型），避免隐式析构
• 在类析构函数中优先删除 ``CoroutineGroup``
• 始终通过 ``CoroutineGroup`` 启动协程

示例：

.. code-block:: c++
    :caption: 使用 CoroutineGroup
    
    class WebLoader {
    public:
        WebLoader();
        ~WebLoader();
        const std::string &lastHtml() const { return html; }
    private:
        void loadDataFromWeb();
        std::string html;
        CoroutineGroup *operations;  // 声明为指针
    };

    WebLoader::WebLoader()
        : operations(new CoroutineGroup)
    {
        operations->spawn([this] {
            loadDataFromWeb();
        });
    }

    WebLoader::~WebLoader()
    {
        delete operations;
    }

    void WebLoader::loadDataFromWeb()
    {
        HttpSession session;
        html = session.get("https://news.163.com/").html();
    }

``CoroutineGroup`` 方法列表：

.. method:: bool add(std::shared_ptr<Coroutine> coroutine, const std::string &name = std::string())

    通过智能指针添加协程到组。指定 ``name`` 后可后续通过 ``get()`` 获取
    
.. method:: bool add(Coroutine *coroutine, const std::string &name = std::string())

    通过裸指针添加协程到组。指定 ``name`` 后可后续通过 ``get()`` 获取
    
.. method:: bool start(Coroutine *coroutine, const std::string &name = std::string())

    启动协程并添加到组。指定 ``name`` 后可后续通过 ``get()`` 获取

.. method:: std::shared_ptr<Coroutine> get(const std::string &name)

    按名称获取协程。未找到返回空指针
    
.. method:: bool kill(const std::string &name, bool join = true)

    按名称终止协程。``join=true`` 时等待协程结束，``join=false`` 立即返回

.. method:: bool killall(bool join = true)

    终止组内所有协程。``join=true`` 时等待所有协程结束

.. method:: bool joinall()

    等待组内所有协程结束

.. method:: int size() const

    返回组内协程数量

.. method:: bool isEmpty() const

    判断组是否为空

.. method:: std::shared_ptr<Coroutine> spawnWithName(const std::string &name, const std::function<void()> &func, bool replace = false)

    启动名为 ``name`` 的协程执行 ``func``。``replace=false`` 时同名协程存在则不操作，返回旧协程；``replace=true`` 返回新协程

.. method:: std::shared_ptr<Coroutine> spawn(const std::function<void()> &func)

    启动新协程执行 ``func`` 并添加到组

.. method:: std::shared_ptr<Coroutine> spawnInThreadWithName(const std::string &name, const std::function<void()> &func, bool replace = false)

    在新线程执行 ``func``，创建等待线程完成的协程并命名。同名处理逻辑同 ``spawnWithName``

.. method:: std::shared_ptr<Coroutine> spawnInThread(const std::function<void()> &func)

    在新线程执行 ``func``，创建等待线程完成的协程并添加到组

.. method:: static std::vector<T> map(std::function<T(S)> func, const std::vector<S> &l)

    并行处理列表元素，返回结果列表：

    .. code-block:: c++
        :caption: map()

        #include "qtng.h"

        int pow2(int i)
        {
            return i * i;
        }

        int main()
        {
            std::vector<int> range10;
            for (int i = 0; i < 10; ++i)
                range10.push_back(i);
            
            std::vector<int> result = qtng::CoroutineGroup::map<int,int>(pow2, range10);
            for (int i = 0; i < 10; ++i)
                ngDebug() << result[i];
            
            return 0;
        }
    
.. method:: void each(std::function<void(S)> func, const std::vector<S> &l)

    并行处理列表元素无返回值：

    .. code-block:: c++
        :caption: each()

        #include "qtng.h"

        void output(int i)
        {
            ngDebug() << i;
        }

        int main()
        {
            std::vector<int> range10;
            for (int i = 0; i < 10; ++i)
                range10.push_back(i); 
            CoroutineGroup::each<int>(output, range10);
            return 0;
        }

1.5 协程间通信
^^^^^^^^^^^^^^

相较于 `boost::coroutine`，qtng 最显著的优势在于其完善的协程通信机制。

1.5.1 RLock
+++++++++++

`可重入锁` 是一种互斥（mutex）机制，允许同一协程多次加锁而不会引发死锁。

.. _可重入锁: https://en.wikipedia.org/wiki/Reentrant_mutex

``Lock``、``RLock``、``Semaphore`` 通常通过 ``ScopedLock<T>`` 在函数返回前自动释放锁：

.. code-block:: c++
    :caption: 使用 RLock
    
    #include "qtng.h"

    using namespace qtng;

    void output(std::shared_ptr<RLock> lock, const std::string &name)
    {
        ScopedLock<RLock> l(*lock);    // 立即获取锁，函数返回前自动释放。注释此行可观察不同效果
        ngDebug() << name << 1;
        Coroutine::sleep(1.0);
        ngDebug() << name << 2;
    }


    int main()
    {
        std::shared_ptr<RLock> lock = std::make_shared<RLock>();
        CoroutineGroup operations;
        operations.spawn([lock]{
            output(lock, "first");
        });
        operations.spawn([lock]{
            output(lock, "second");
        });
        operations.joinall();
        return 0;
    }
    
输出结果：

.. code-block:: text
    :caption: 带 RLock 的输出
    
    "first" 1
    "first" 2
    "second" 1
    "second" 2

若注释 ``ScopedLock l(lock);`` 行，输出变为：

.. code-block:: text
    :caption: 无 RLock 的输出
    
    "first" 1
    "second" 1
    "first" 2
    "second" 2

.. method:: bool acquire(bool blocking = true)

    获取锁。若锁被其他协程持有且 ``blocking=true``，则阻塞当前协程直至锁释放；否则立即返回。
    
    返回是否成功获取锁。
    
.. method:: void release()

    释放锁。等待此锁的协程将在当前协程切换至事件循环协程后恢复执行。
    
.. method:: bool isLocked() const

    检测是否有协程持有此锁。
    
.. method:: bool isOwned() const

1.5.2 Event
+++++++++++

`Event` (事件信号量)是用于通知等待协程特定条件已触发的同步机制。

.. _Event: https://en.wikipedia.org/wiki/Event_(synchronization_primitive)

.. method:: bool wait(bool blocking = true)

    等待事件。若事件未触发且 ``blocking=true``，阻塞当前协程直至事件触发；否则立即返回。
    
    返回事件是否已触发。
    
.. method:: void set()

    触发事件。等待此事件的协程将在当前协程切换至事件循环协程后恢复。
    
.. method:: void clear()

    重置事件状态。
    
.. method:: bool isSet() const

    检测事件是否已触发。
    
.. method:: int getting() const

    获取当前等待此事件的协程数量。
    
1.5.3 ValueEvent<>
++++++++++++++++++

``ValueEvent<>`` 继承自 ``Event``，支持协程间传递数据。

.. code-block:: c++
    :caption: 使用 ValueEvent<> 传递值
    
    
    #include "qtng.h"

    using namespace qtng;

    int main()
    {
        std::shared_ptr<ValueEvent<int>> event = std::make_shared<ValueEvent<int>>();
        
        CoroutineGroup operations;
        operations.spawn([event]{
            ngDebug() << event->wait();
        });
        operations.spawn([event]{
            event->send(3);
        });
        operations.joinall();
        return 0;
    }

输出结果：

.. code-block:: text

    3

.. method:: void send(const Value &value)
    
    发送数据并触发事件。等待协程将在当前协程切换至事件循环协程后恢复。
    
.. method:: Value wait(bool blocking = true)
    
    等待事件。若事件未触发且 ``blocking=true``，阻塞当前协程直至触发。返回发送的数据，失败时返回默认构造值。
    
.. method:: void set()

    触发事件（与 ``send()`` 等效）。
    
.. method:: void clear()

    重置事件状态。
    
.. method:: bool isSet() const

    检测事件是否已触发。
    
.. method:: int getting() const

1.5.4 Gate
++++++++++

``Gate`` 是 ``Event`` 的特殊接口，用于控制数据传输速率。

.. method:: bool goThrough(bool blocking = true)

    等效于 ``Event::wait()``。
    
.. method:: bool wait(bool blocking = true)

    等效于 ``Event::wait()``。
    
.. method:: void open();

    等效于 ``Event::set()``。
    
.. method:: void close();

    等效于 ``Event::clear()``。
    
.. method:: bool isOpen() const;

    等效于 ``Event::isSet()``。
    
1.5.5 Semaphore
+++++++++++++++

`信号量` 是用于控制多协程共享资源访问的变量或抽象数据类型。

.. _信号量: https://en.wikipedia.org/wiki/Semaphore_(programming)

.. code-block:: c++
    :caption: 使用 Semaphore 控制请求并发数
    
    #include "qtng.h"

    using namespace qtng;

    void send_request(std::shared_ptr<Semaphore> semaphore)
    {
        ScopedLock<Semaphore> l(semaphore);
        HttpSession session;
        ngDebug() << session.get("https://news.163.com").statusCode;
    }

    int main()
    {
        std::shared_ptr<Semaphore> semaphore = std::make_shared<Semaphore>(5);
        
        CoroutineGroup operations;
        for (int i = 0; i < 100; ++i) {
            operations.spawn([semaphore]{
                send_request(semaphore);
            });
        }
        return 0;
    }

该示例启动 100 个协程，但仅有 5 个协程同时向 HTTP 服务器发起请求。

.. method:: Semaphore(int value = 1)
    :no-index:

    构造函数指定最大资源数 ``value``。
    
.. method:: bool acquire(bool blocking = true)

    获取信号量。若资源耗尽且 ``blocking=true``，阻塞当前协程直至其他协程释放资源；否则立即返回。
    
    返回是否成功获取信号量。
    
.. method:: void release()

    释放信号量。等待此信号量的协程将在当前协程切换至事件循环协程后恢复。

.. method:: bool isLocked() const
    
    检测信号量是否被任一协程占用。

1.5.6 Queue
+++++++++++

协程间队列。

.. method:: Queue(int capacity)
    :no-index:

构造函数指定队列容量 ``capacity``。

.. method:: void setCapacity(int capacity)

设置队列最大容量。

.. method:: bool put(const T &e)

插入元素 ``e``。若队列已满，阻塞当前协程直至其他协程取出元素。

.. method:: T get()

取出元素。若队列为空，阻塞当前协程直至其他协程插入元素。

.. method:: bool isEmpty() const

检测队列是否为空。

.. method:: bool isFull() const

检测队列是否已满。

.. method:: int getCapacity() const

获取队列容量。

.. method:: int size() const

返回队列当前元素数量。

.. method:: int getting() const

返回当前等待元素的协程数量。

1.5.7 Lock
++++++++++

``Lock`` 类似 ``RLock``，但同一协程重复加锁会导致死锁。

1.5.8 Condition
+++++++++++++++

协程间变量值监控。

.. method:: bool wait()

阻塞当前协程直至被其他协程的 ``notify()`` 或 ``notifyAll()`` 唤醒。

.. method:: void notify(int value = 1)

唤醒指定数量（``value``）的等待协程。

.. method:: void notifyAll()

唤醒所有等待协程。

.. method:: int getting() const

返回当前等待此条件的协程数量。

1.6 实用工具
^^^^^^^^^^^^^

提供在内部事件循环或后台线程上运行任务的实用函数。

qtng 编程中**最严重的错误**是在事件循环协程中调用阻塞函数（如 ``Socket``、``RLock``、``Event`` 相关函数），这将导致未定义行为。若检测到此错误，qtng 会输出警告信息。

.. method:: T callInEventLoop(std::function<T ()> func)

    在库的事件循环中调度 ``func`` 并返回结果。当非协程代码需要在正确线程上与协程 API 交互时使用。

    .. code-block:: c++
    
        int value = callInEventLoop<int>([] {
            return 42;
        });

.. method:: void callInEventLoopAsync(std::function<void ()> func, std::uint32_t msecs = 0)

    ``callInEventLoop()`` 的异步版本，立即返回并在 ``msecs`` 毫秒后在事件循环上执行 ``func``。

    .. code-block:: c++
    
        callInEventLoopAsync([] {
            ngDebug() << "scheduled on event loop";
        });
    
    若不关心返回值，``callInEventLoopAsync()`` 通常比 ``callInEventLoop()`` 更轻量。
    
    
.. method:: T callInThread(std::function<T()> func)

    在新线程执行函数并返回结果。
    

1.7 内部机制：协程如何切换
^^^^^^^^^^^^^^^^^^^^^^^^^^
1.7.1 Iterator
+++++++++++++++
实现协程间的数据分块传输，支持批量处理数据（如分页读取文件或网络流）。

.. method:: bool next(T &result)

    从缓冲区获取下一个元素，若空则挂起调用方协程（callee->yield()），等待数据生成。

.. method:: void yield(const T &t)

    向缓冲区添加元素，达到batchSize时挂起当前协程，切换回调用方。

1.7.2 IteratorCoroutin
+++++++++++++++++++++++
继承自BaseCoroutine，实际执行用户传入的生成函数（func），通过yield()分批次返回数据。

.. method:: virtual void run()

   执行生成函数，填充数据到chunk，触发协程切换。 

2. 基础网络编程
----------------------------

qtng 支持 IPv4 和 IPv6，旨在提供类似 Python socket 模块的面向对象套接字接口。

除基础套接字接口外，qtng 还支持 Socks5 代理，并提供 ``SocketServer`` 相关类简化服务器开发。

2.1 Socket
^^^^^^^^^^

创建套接字非常简单，只需实例化 ``Socket`` 类或将平台特定的套接字描述符传递给构造函数。

.. code-block:: c++
    :caption: Socket 构造函数
    
    Socket(HostAddress::NetworkLayerProtocol protocol = AnyIPProtocol, SocketType type = TcpSocket);
    
    Socket(std::intptr_t socketDescriptor);

参数 ``protocol`` 可用于限制协议为 IPv4 或 IPv6。若省略此参数，``Socket`` 将自动选择首选协议（通常优先选择 IPv6）。TODO: 描述具体方法。

参数 ``type`` 指定套接字类型，目前仅支持 TCP 和 UDP。若省略此参数，默认使用 TCP。

第二种构造函数形式适用于将其他网络编程工具创建的套接字转换为 qtng 套接字。传入的套接字必须处于已连接状态。

以下是 ``Socket`` 类型的成员函数：

.. method:: Socket *accept()

    若套接字处于监听状态，``accept()`` 将阻塞当前协程，并在新客户端连接后返回新的 ``Socket`` 对象。该对象已与新客户端建立连接。若套接字被其他协程关闭，函数返回 ``0``。

.. method:: bool bind(HostAddress &address, std::uint16_t port = 0, BindMode mode = DefaultForPlatform)

    将套接字绑定到 ``address`` 和 ``port``。若省略 ``port`` 参数，操作系统将自动分配未使用的随机端口（可通过 ``port()`` 函数获取）。参数 ``mode`` 当前未使用。
    
    成功绑定端口时返回 true。

.. method:: bool bind(std::uint16_t port = 0, BindMode mode = DefaultForPlatform)

    将套接字绑定到任意地址和 ``port``。此函数为 ``bind(address, port)`` 的重载形式。

.. method:: bool connect(const HostAddress &host, std::uint16_t port)

    连接到 ``host`` 和 ``port`` 指定的远程主机。阻塞当前协程直至连接建立或失败。
    
    连接成功时返回 true。

.. method:: bool connect(const std::string &hostName, std::uint16_t port, HostAddress::NetworkLayerProtocol protocol = AnyIPProtocol)

    使用 ``protocol`` 连接到 ``hostName`` 和 ``port`` 指定的远程主机。若 ``hostName`` 非 IP 地址，qtng 将在连接前执行 DNS 查询。阻塞当前协程直至连接建立或失败。
    
    由于 DNS 查询耗时较长，建议对频繁连接的远程主机使用 ``setDnsCache()`` 缓存查询结果。
    
    若省略 ``protocol`` 或指定为 ``AnyIPProtocol``，qtng 将优先尝试 IPv6 连接，失败后尝试 IPv4。DNS 返回多个 IP 时按顺序尝试连接。
    
    连接成功时返回 true。

.. method:: bool close()

    关闭套接字。

.. method:: bool listen(int backlog)

    将套接字设为监听模式，后续可通过 ``accept()`` 获取新客户端请求。参数 ``backlog`` 的具体含义与平台相关，请参考 ``man listen`` 手册。

.. method:: bool setOption(SocketOption option, int value)

    将指定 ``option`` 设置为 ``value`` 描述的值。该函数用于配置套接字选项。

套接字选项可通过以下表格配置：

.. list-table:: Socket 选项说明
   :header-rows: 1
   :widths: 30 70

   * - 选项名称
     - 描述
   * - ``BroadcastSocketOption``
     - UDP套接字发送广播数据报
   * - ``AddressReusable``
     - 允许bind()调用重用本地地址
   * - ``ReceiveOutOfBandData``
     - 启用时将带外数据直接放入接收数据流
   * - ``ReceivePacketInformation``
     - 保留选项，暂不支持
   * - ``ReceiveHopLimit``
     - 保留选项，暂不支持
   * - ``LowDelayOption``
     - 禁用Nagle算法
   * - ``KeepAliveOption``
     - 在面向连接的套接字上启用保活报文发送
   * - ``MulticastTtlOption``
     - 设置/读取组播报文的生存时间(TTL)
   * - ``MulticastLoopbackOption``
     - 控制是否回环发送的组播报文
   * - ``TypeOfServiceOption``
     - 设置/读取IP报文的服务类型字段(TOS)
   * - ``SendBufferSizeSocketOption``
     - 设置/获取发送缓冲区最大字节数
   * - ``ReceiveBufferSizeSocketOption``
     - 设置/获取接收缓冲区最大字节数
   * - ``MaxStreamsSocketOption``
     - 保留选项，暂不支持STCP协议
   * - ``NonBlockingSocketOption``
     - 保留选项，Socket内部要求非阻塞模式
   * - ``BindExclusively``
     - 保留选项，暂不支持

注意：Windows Runtime中必须在连接前设置Socket::KeepAliveOption

.. method:: int option(SocketOption option) const

    返回指定选项的当前值
    
.. method:: std::int32_t recv(char *data, std::int32_t size)

    接收最多size字节数据，阻塞当前协程直至有数据到达。返回实际接收字节数（0表示连接关闭，-1表示错误）

.. method:: std::int32_t recvall(char *data, std::int32_t size)

    接收确切size字节数据，阻塞当前协程直至全部接收或连接关闭。建议在明确数据长度时使用

.. method:: std::int32_t send(const char *data, std::int32_t size)

    发送最多size字节数据，返回实际发送字节数（可能小于size）

.. method:: std::int32_t sendall(const char *data, std::int32_t size)

    发送全部size字节数据，阻塞直至完成或连接中断

.. method:: std::int32_t recvfrom(char *data, std::int32_t size, HostAddress *addr, std::uint16_t *port)

    (仅数据报套接字)接收数据并获取发送方地址

.. method:: std::int32_t sendto(const char *data, std::int32_t size, const HostAddress &addr, std::uint16_t port)

    (仅数据报套接字)向指定地址发送数据

.. method:: std::string recvall(std::int32_t size)

    std::string版本的全量接收方法

.. method:: std::string recv(std::int32_t size)

    std::string版本的接收方法

.. method:: std::int32_t send(const std::string &data)

    std::string版本的发送方法

.. method:: std::int32_t sendall(const std::string &data)

    std::string版本的全量发送方法

.. method:: std::string recvfrom(std::int32_t size, HostAddress *addr, std::uint16_t *port)

    std::string版本的数据报接收方法

.. method:: std::int32_t sendto(const std::string &data, const HostAddress &addr, std::uint16_t port)

    std::string版本的数据报发送方法

状态与信息查询
^^^^^^^^^^^^^^
.. method:: SocketError error() const

    返回最后一次错误类型
    
.. method:: std::string errorString() const

    返回最后一次错误描述
    
.. method:: bool isValid() const

    检测套接字是否有效
    
.. method:: HostAddress localAddress() const

    获取本地绑定地址
    
.. method:: std::uint16_t localPort() const

    获取本地绑定端口
    
.. method:: HostAddress peerAddress() const

    获取对端地址（仅连接状态有效）
    
.. method:: std::string peerName() const

    获取对端主机名
    
.. method:: std::uint16_t peerPort() const

    获取对端端口
    
.. method:: std::intptr_t fileno() const

    获取原生套接字描述符
    
协议与类型
^^^^^^^^^^
.. method:: SocketType type() const

    返回套接字类型(TCP/UDP)
    
.. method:: SocketState state() const

    返回当前状态
    
.. method:: NetworkLayerProtocol protocol() const

    返回网络层协议
    
DNS相关
^^^^^^^
.. method:: static std::vector<HostAddress> resolve(const std::string &hostName)

    执行DNS解析。若 ``hostName`` 为 IP 地址，则直接返回该 IP。

    国际化域名（IDN）会先经 ``utils::toAce()`` 转为 Punycode（ACE）再查询解析器，因此包含非 ASCII 字符的主机名（如 ``"bücher.com"``、``"中文.com"``）可用。该转换为最小 IDNA 外壳：纯 ASCII 标签原样通过，非 ASCII 标签以 ``xn--`` ACE 前缀编码。**不**做 Unicode 归一化（NFKC）、非 ASCII 大小写折叠及 Bidi/Joining 检查，需要 ASCII 小写归一化的调用方应自行处理。
    
.. method:: void setDnsCache(std::shared_ptr<SocketDnsCache> dnsCache)

    设置DNS缓存

2.2 SslSocket
^^^^^^^^^^^^^

``SslSocket`` 设计类似 ``Socket``，继承大部分函数如 ``connect()``、``recv()``、``send()``、``peerName()`` 等，但排除仅用于 UDP 套接字的 ``recvfrom()`` 和 ``sendto()``。

构造函数提供三种形式：

.. code-block:: c++
    :caption: SslSocket 构造函数
    
    SslSocket(HostAddress::NetworkLayerProtocol protocol = Socket::AnyIPProtocol,
            const SslConfiguration &config = SslConfiguration());
    
    SslSocket(std::intptr_t socketDescriptor, const SslConfiguration &config = SslConfiguration());
    
    SslSocket(std::shared_ptr<Socket> rawSocket, const SslConfiguration &config = SslConfiguration());

信息获取相关方法：

.. method:: bool handshake(bool asServer, const std::string &verificationPeerName = std::string())

    与对端进行握手协商。参数 ``asServer=true`` 时本端作为 SSL 服务器。仅当基于原生套接字创建时需手动调用此函数。
    
.. method:: Certificate localCertificate() const

    返回本地证书链的顶层证书，通常与 ``SslConfiguration::localCertificate()`` 一致。
    
.. method:: std::vector<Certificate> localCertificateChain() const

    返回本地完整证书链，包含 ``SslConfiguration::localCertificateChain()`` 及部分 ``SslConfiguration::caCertificates``。
    
.. method:: std::string nextNegotiatedProtocol() const

    返回 SSL 连接协商的下一层协议（如 HTTP/2 需 ALPN 扩展）。
    
    .. _The Application-Layer Protocol Negotiation: https://en.wikipedia.org/wiki/Application-Layer_Protocol_Negotiation

.. method:: NextProtocolNegotiationStatus nextProtocolNegotiationStatus() const

    返回协议协商状态。
    
.. method:: SslMode mode() const

    返回 SSL 连接模式（服务端/客户端）。
    
.. method:: Certificate peerCertificate() const

    返回对端证书链顶层证书。
    
.. method:: std::vector<Certificate> peerCertificateChain() const

    返回对端完整证书链。
    
.. method:: int peerVerifyDepth() const

    返回证书验证深度限制。若对端证书链层级超过此值则验证失败。
    
.. method:: Ssl::PeerVerifyMode peerVerifyMode() const

    返回对端验证模式：

 .. list-table:: SslSocket 对等验证模式说明
   :header-rows: 1
   :widths: 30 70

   * - PeerVerifyMode
     - 描述
   * - ``VerifyNone``
     - 不要求对端提供证书，连接仍加密但身份验证关闭
   * - ``QueryPeer``
     - 请求对端证书但不强制验证（服务端默认模式）
   * - ``VerifyPeer``
     - 强制验证对端证书有效性（客户端默认模式）
   * - ``AutoVerifyPeer``
     - 自动模式：服务端用 QueryPeer，客户端用 VerifyPeer


.. method:: std::string peerVerifyName() const

    返回对端验证名称
    
.. method:: PrivateKey privateKey() const

    返回本端私钥（与 ``SslConfiguration::privateKey()`` 一致）
    
.. method:: SslCipher cipher() const

    返回当前加密套件（握手完成后生效，无效时 ``Cipher::isNull()==true``）
    
.. method:: Ssl::SslProtocol sslProtocol() const

    返回使用的 SSL/TLS 协议版本
    
.. method:: SslConfiguration sslConfiguration() const

    返回当前 SSL 配置
    
.. method:: std::vector<SslError> sslErrors() const

    返回握手及通信期间发生的错误列表
    
.. method:: void setSslConfiguration(const SslConfiguration &configuration)

    设置 SSL 配置（必须在握手前调用）

2.3 Socks5 代理
^^^^^^^^^^^^^^^^

``Socks5Proxy`` 提供 SOCKS5 客户端支持，支持通过代理服务器连接远程主机。

构造函数：

.. code-block:: c++
    :caption: Socks5Proxy 构造函数
    
    Socks5Proxy();  // 创建空代理对象
    
    Socks5Proxy(const std::string &hostName, std::uint16_t port,
                 const std::string &user = std::string(), const std::string &password = std::string());  // 带认证信息的代理

核心方法：

.. method:: std::shared_ptr<Socket> connect(const std::string &remoteHost, std::uint16_t port)

    通过代理连接域名型目标（代理端执行DNS解析），阻塞协程直至连接成功/失败
    
.. method:: std::shared_ptr<Socket> connect(const HostAddress &remoteHost, std::uint16_t port)

    通过代理连接IP型目标，无DNS解析过程
    
.. method:: std::shared_ptr<SocketLike> listen(std::uint16_t port)

    请求代理服务器监听指定端口，返回监听对象
    
.. method:: bool isNull() const
    
    检测代理配置是否有效（hostName/port是否为空）
    
.. method:: Capabilities capabilities() const

    获取代理服务器支持的能力
    
属性访问器：

.. method:: std::string hostName() const

    代理服务器主机名
    
.. method:: std::uint16_t port() const

    代理服务器端口
    
.. method:: std::string user() const

    代理认证用户名
    
.. method:: std::string password() const

    代理认证密码
    
属性设置器：

.. method:: void setCapabilities(Socks5Proxy::Capabilities capabilities)

    设置代理能力标识
    
.. method:: void setHostName(const std::string &hostName)
    
    设置代理主机名
    
.. method:: void setPort(std::uint16_t port)

    设置代理端口
    
.. method:: void setUser(const std::string &user)

    设置认证用户
    
.. method:: void setPassword(const std::string &password)

    设置认证密码

2.4 SocketServer
^^^^^^^^^^^^^^^^

2.4.1 BaseStreamServer
+++++++++++++++++++++++
 ``BaseStreamServer`` 是构建其他SocketServer基础核心类，提供了一些Socket服务器基础方法，以及保留了一些接口，用于进一步实现 ``TcpServer`` 和 ``KcpServer`` 等类型

.. method:: BaseStreamServer(const HostAddress &serverAddress, std::uint16_t serverPort);

    初始化服务器监听的地址和端口，默认使用 HostAddress::Any 绑定到所有网络接口，同时初始化事件对象 started 和 stopped，用于跟踪服务器状态。

.. method:: bool serveForever()

    阻塞式运行服务器，循环接受客户端连接并处理请求。

.. method:: bool start()

    非阻塞式启动服务器，在后台协程中运行服务。

.. method:: void stop()

    立即关闭服务器套接字，终止所有连接

.. method:: bool wait()

    阻塞当前线程,直到服务器完全停止

.. method:: void setAllowReuseAddress(bool b)

    设置是否允许端口复用（SO_REUSEADDR）。

.. method:: bool isSecure()

    标识服务器是否使用加密协议（如SSL）。默认返回：false，子类（如 WithSsl）覆盖后返回 true。

.. method:: std::shared_ptr<SocketLike> serverSocket()

    获取底层服务器套接字对象，首次调用会触发 serverCreate() 创建套接字。

.. method:: std::uint16_t serverPort()

    获取服务器绑定的端口号

.. method:: HostAddress serverAddress()

    获取服务器绑定的ip地址

.. method:: virtual bool serverBind()

    绑定服务器到指定地址和端口，默认实现：设置 SO_REUSEADDR 选项（若允许复用地址），调用 Socket::bind() 完成系统调用。

.. method:: virtual bool serverActivate()

    将套接字置为监听状态,默认实现：调用 Socket::listen()，设置最大连接队列长度。

.. method:: virtual std::shared_ptr<SocketLike> prepareRequest(std::shared_ptr<SocketLike> request);

    预处理请求（如SSL握手）。

.. method:: virtual bool verifyRequest(std::shared_ptr<SocketLike> request);

    验证请求是否合法（如IP黑名单），默认实现：直接返回 true，接受所有连接。

2.4.2 WithSsl 
++++++++++++++
通过模板组合，为任意流式服务器无缝添加 SSL/TLS 加密功能。

.. method:: WithSsl(const HostAddress &serverAddress, std::uint16_t serverPort, const SslConfiguration &configuration);
    
    初始化 SSL 服务器，继承自 ServerType，还有几个其他类似方法

    .. code-block:: c++

        WithSsl(const HostAddress &serverAddress, std::uint16_t serverPort);
        WithSsl(std::uint16_t serverPort);
        WithSsl(std::uint16_t serverPort, const SslConfiguration &configuration);
    
.. method:: void setSslConfiguration(const SslConfiguration &configuration);

    动态设置SSL配置。

.. method:: SslConfiguration sslConfiguration() const;

    获取SSL配置。

.. method:: void setSslHandshakeTimeout(float sslHandshakeTimeout)

    控制SSL握手阶段的时间，防止客户端恶意占用

.. method:: float sslHandshakeTimeout()

    获取当前设置SSL握手的超时时长

.. method:: virtual bool isSecure()

    标识服务器使用加密协议，供外部代码检查。

.. method:: prepareRequest()
    :no-index:

    将原始 TCP 连接升级为 SSL 连接。


2.4.3 BaseRequestHandler
+++++++++++++++++++++++++
请求处理逻辑的基类，用户需继承并实现具体逻辑。

.. method:: void run()

    请求处理的主流程控制器，确保 setup → handle → finish 顺序执行。

.. method:: void setup()

    初始化请求处理环境（如验证权限、加载配置）。

.. method:: void handle()

    实现核心业务逻辑（如读取请求、处理数据、返回响应）。

.. method:: void finish()

    清理资源（如关闭连接、记录日志、释放内存），即使业务逻辑失败，finish() 也应确保资源释放。

.. method:: void userData()

    安全获取服务器关联的自定义数据（如数据库连接池、配置对象）。

2.4.4 Socks5RequestHandler
+++++++++++++++++++++++++++
``Socks5RequestHandler`` 是 SOCKS5 代理协议的具体实现，继承自 ``BaseRequestHandler``，用于处理客户端通过 SOCKS5 代理发起的连接请求。其核心功能包括协议握手、目标地址解析、连接建立和数据转发。

.. method:: virtual void handle()

    处理客户端 SOCKS5 请求的主入口。 

.. method:: bool handshake()

    处理 SOCKS5 握手与认证协商,返回值：true 表示握手成功，false 表示失败

.. method:: bool parseAddress(std::string *hostName, HostAddress *addr, std::uint16_t *port)

    解析客户端请求中的目标地址和端口。

.. method:: virtual std::shared_ptr<SocketLike> makeConnection(const std::string &hostName, const HostAddress &hostAddress,std::uint16_t port, HostAddress *forwardAddress)

    建立到目标服务器的连接。hostName：目标域名(如 ATYP=0x03),hostAddress：目标 IP 地址(如 ATYP=0x01 或 0x04),port：目标端口,forwardAddress：输出参数，记录实际连接的服务器地址。

.. method:: bool sendConnectReply(const HostAddress &hostAddress, std::uint16_t port)

    向客户端发送连接成功响应。

.. method:: bool sendFailedReply()

    发送连接失败响应。

.. method:: virtual void exchange(std::shared_ptr<SocketLike> request, std::shared_ptr<SocketLike> forward)

    在客户端和目标服务器之间双向转发数据。

.. method:: doConnect()
    :no-index:

    供子类扩展连接成功的行为。

.. method:: doFailed()
    :no-index:

    供子类扩展连接失败时的行为。

.. method:: virtual void logProxy(const std::string &hostName, const HostAddress &hostAddress, std::uint16_t port,const HostAddress &forwardAddress, bool success)

    记录代理请求的详细日志。 

2.4.5 TcpServer
++++++++++++++++
封装 TCP 服务器的创建、绑定、监听,通过模板参数 RequestHandler 实现业务逻辑解耦,基于协程的并发模型,支持高并发连接。

.. method:: TcpServer(const HostAddress &serverAddress, std::uint16_t serverPort);

    初始化TCP服务器，绑定到指定地址和端口，直接调用 ``BaseStreamServer`` 的构造函数，若未指定地址则默认绑定所有网络接口(HostAddress::Any)

.. method:: virtual std::shared_ptr<SocketLike> serverCreate();

    创建底层 TCP 服务器套接字。

.. method:: virtual void processRequest(std::shared_ptr<SocketLike> request)

    处理单个客户端连接请求。

.. code-block:: c++
    :caption: 示例 : 简单的Tcp服务器

        
        #include "qtng.h"
        using namespace  qtng;
        class EchoHandler : public BaseRequestHandler
        //需要继承BaseRequestHandle并重写handle方法
        {
        protected:
            void handle()  {
                ngDebug()<<"收到消息";
                std::int32_t size=1024;
                std::string data=request->recvall(size);
                ngDebug()<<std::string(data);
            }
        };
        int main()
        {
            // 创建服务器，监听 8080 端口
            TcpServer<EchoHandler> server(8080);
            // 配置服务器参数
            server.setRequestQueueSize(100); // 设置连接队列长度
            server.setAllowReuseAddress(true); // 允许端口复用
            // 启动服务器（阻塞式运行）
            if (!server.serveForever()) {
                ngDebug() << "服务器启动失败!";
                return 1;
            }
            return 0;
        }

2.4.6 KcpServer
++++++++++++++++
详细解释KcpServer 和 KcpServerV2这两个类和各个方法，并详细解释这两个类的实现区别

.. method:: KcpServer(const HostAddress &serverAddress, std::uint16_t serverPort)
    :no-index:

    初始化KCP服务器，绑定到指定地址和端口，直接调用 ``BaseStreamServer`` 的构造函数，若未指定地址则默认绑定所有网络接口(HostAddress::Any)

.. method:: virtual std::shared_ptr<SocketLike> serverCreate()

    调用KcpSocket::createServer(),创建KCP服务器，底层通过KcpSocket类实现。此方法会初始化KCP会话，绑定到指定地址和端口，并设置默认参数（如MTU大小、窗口大小等）。

.. method:: virtual void processRequest(std::shared_ptr<SocketLike> request)

    接收客户端连接后，实例化用户定义的RequestHandler，将KCP会话封装为SocketLike对象传递给业务逻辑处理模块。
    
2.4.7 KcpServerV2
++++++++++++++++++
更底层的KCP协议服务器实现，直接操作KCP会话实例。

.. method:: KcpServerV2(const HostAddress &serverAddress, std::uint16_t serverPort)
    :no-index:

    初始化KCP服务器，绑定到指定地址和端口，直接调用 ``BaseStreamServer`` 的构造函数，若未指定地址则默认绑定所有网络接口(HostAddress::Any)

.. method:: virtual std::shared_ptr<SocketLike> serverCreate()

    调用createKcpServer()函数创建服务器。与KcpServer不同，此处可能直接管理UDP套接字，并通过回调函数处理KCP会话的输入/输出

.. method:: virtual void processRequest(std::shared_ptr<SocketLike> request)

    与KcpServer类似，但可能直接操作KCP会话对象（如调用kcp_input()解析数据包、kcp_recv()提取应用层数据）

3. HTTP 客户端
--------------

``HttpSession`` 是支持 HTTP 1.0/1.1 的客户端，具备自动 Cookie 管理和自动重定向功能。核心方法 ``HttpSession::send()`` 用于发送请求并解析响应，同时提供快捷方法如 ``get()``、 ``post()``、 ``head()`` 等实现单行代码发起 HTTP 请求。

该组件支持 SOCKS5 代理（默认未启用），目前暂不支持 HTTP 代理。Cookie 管理通过 ``HttpSession::cookieJar()`` 实现，响应缓存使用 ``HttpSession::cacheManager()``（默认无缓存）。qtng 提供内存缓存组件 ``HttpMemoryCacheManager``。

.. code-block:: c++
    :caption: HTTP 请求示例
    
    HttpSession session;
    
    // 使用 send() 方法
    HttpRequest request;
    request.setUrl("https://qtng.org/");
    request.setMethod("GET");
    request.setTimeout(10.0f);
    HttpResponse response = session.send(request);
    ngDebug() << response.statusCode() << request.statusText() << response.isOk() << response.body().size();

    // 使用快捷方法
    HttpResponse response = session.get("https://qtng.org/");
    ngDebug() << response.statusCode() << request.statusText() << response.isOk() << response.body().size();
    
    std::map<std::string, std::string> query;
    query.insert("username", "panda");
    query.insert("password", "xoxoxoxox");
    HttpResponse response = session.post("https://qtng.org/login/", query);
    ngDebug() << response.statusCode() << request.statusText() << response.isOk() << response.body().size();
    
    // 启用缓存管理
    session.setCacheManager(std::shared_ptr<HttpCacheManager>::create());

3.1 HttpSession
^^^^^^^^^^^^^^^

.. method:: HttpResponse send(HttpRequest &request)

    发送 HTTP 请求至服务器并解析响应
    
.. method:: HttpCookieJar &cookieJar()

    返回 cookie 管理器
    
    注意：设置方法 ``setCookieJar(...)`` 暂未实现
    
.. method:: HttpCookie cookie(const std::string &url, const std::string &name)

    获取指定 URL 的特定 cookie
    
    cookie 始终与 URL 关联，需同时提供 ``url`` 和 ``name`` 参数
    
.. method:: void setMaxConnectionsPerServer(int maxConnectionsPerServer)

    设置单服务器最大连接数（默认10），超过该限制的请求将被阻塞
    
    若 ``maxConnectionsPerServer < 0`` 则禁用限制
    
.. method:: int maxConnectionsPerServer()

    返回当前单服务器最大连接数
    
.. method:: void setDebugLevel(int level)

    调试级别控制：
    ◦ >0：打印请求/响应摘要
    ◦ >1：打印完整内容（可能导致大量输出）
    
.. method:: void disableDebug()

    禁用调试输出
    
.. method:: void setDefaultUserAgent(const std::string &userAgent)

    设置默认 User-Agent（默认值为 Firefox 52 Linux 版）
    
.. method:: std::string defaultUserAgent() const

    获取默认 User-Agent
    
    单个请求可通过 ``HttpRequest::setUserAgent()`` 覆盖
    
.. method:: HttpVersion defaultVersion() const

    返回默认 HTTP 版本（默认 1.1）
    
.. method:: void setDefaultConnectionTimeout(float timeout)

    设置默认连接超时（单位：秒，默认10秒）
    
    仅影响连接建立阶段
    
.. method:: float defaultConnnectionTimeout() const

    获取默认连接超时
    
.. method:: void setSocks5Proxy(std::shared_ptr<Socks5Proxy> proxy)

    设置 SOCKS5 代理
    
.. method:: std::shared_ptr<Socks5Proxy> socks5Proxy() const

    获取 SOCKS5 代理
    
.. method:: void setCacheManager(std::shared_ptr<HttpCacheManager> cacheManager)

    设置缓存管理器
    
.. method:: std::shared_ptr<HttpCacheManager> cacheManager() const

    获取缓存管理器
    
.. method:: HttpResponse get(const std::string &url)

    发送 HTTP GET 请求
    
    支持多种参数形式：

    .. code-block:: c++

        HttpResponse get(const std::string &url, const std::map<std::string, std::string> &query);
        HttpResponse get(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
        HttpResponse get(const std::string &url, const qtng::utils::UrlQuery &query);
        HttpResponse get(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);
        HttpResponse get(const std::string &url, const std::map<std::string, std::string> &query);
        HttpResponse get(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
        HttpResponse get(const std::string &url, const qtng::utils::UrlQuery &query);
        HttpResponse get(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);
        
        HttpResponse head(const std::string &url, const std::map<std::string, std::string> &query);
        HttpResponse head(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
        HttpResponse head(const std::string &url, const qtng::utils::UrlQuery &query);
        HttpResponse head(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);
        HttpResponse head(const std::string &url, const std::map<std::string, std::string> &query);
        HttpResponse head(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
        HttpResponse head(const std::string &url, const qtng::utils::UrlQuery &query);
        HttpResponse head(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);

        HttpResponse options(const std::string &url, const std::map<std::string, std::string> &query);
        HttpResponse options(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
        HttpResponse options(const std::string &url, const qtng::utils::UrlQuery &query);
        HttpResponse options(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);
        HttpResponse options(const std::string &url, const std::map<std::string, std::string> &query);
        HttpResponse options(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
        HttpResponse options(const std::string &url, const qtng::utils::UrlQuery &query);
        HttpResponse options(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);

        HttpResponse delete_(const std::string &url, const std::map<std::string, std::string> &query);
        HttpResponse delete_(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
        HttpResponse delete_(const std::string &url, const qtng::utils::UrlQuery &query);
        HttpResponse delete_(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);
        HttpResponse delete_(const std::string &url, const std::map<std::string, std::string> &query);
        HttpResponse delete_(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
        HttpResponse delete_(const std::string &url, const qtng::utils::UrlQuery &query);
        HttpResponse delete_(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);

 .. method:: HttpResponse post(const std::string &url, const std::string &body)

    使用POST方法向web服务器发送HTTP请求。

    类似的函数有很多：

    .. code-block:: c++
    
        HttpResponse post(const std::string &url, const std::string &body);
        HttpResponse post(const std::string &url, const std::map<std::string, std::string> &body);
        HttpResponse post(const std::string &url, const qtng::utils::UrlQuery &body);
        HttpResponse post(const std::string &url, const FormData &body);
        HttpResponse post(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
        HttpResponse post(const std::string &url, const std::map<std::string, std::string> &body, const std::map<std::string, std::string> &headers);
        HttpResponse post(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
        HttpResponse post(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);
        HttpResponse post(const std::string &url, const std::string &body);
        HttpResponse post(const std::string &url, const std::map<std::string, std::string> &body);
        HttpResponse post(const std::string &url, const qtng::utils::UrlQuery &body);
        HttpResponse post(const std::string &url, const FormData &body);
        HttpResponse post(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
        HttpResponse post(const std::string &url, const std::map<std::string, std::string> &body, const std::map<std::string, std::string> &headers);
        HttpResponse post(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
        HttpResponse post(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);

        HttpResponse patch(const std::string &url, const std::string &body);
        HttpResponse patch(const std::string &url, const std::map<std::string, std::string> &body);
        HttpResponse patch(const std::string &url, const qtng::utils::UrlQuery &body);
        HttpResponse patch(const std::string &url, const FormData &body);
        HttpResponse patch(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
        HttpResponse patch(const std::string &url, const std::map<std::string, std::string> &body, const std::map<std::string, std::string> &headers);
        HttpResponse patch(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
        HttpResponse patch(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);
        HttpResponse patch(const std::string &url, const std::string &body);
        HttpResponse patch(const std::string &url, const std::map<std::string, std::string> &body);
        HttpResponse patch(const std::string &url, const qtng::utils::UrlQuery &body);
        HttpResponse patch(const std::string &url, const FormData &body);
        HttpResponse patch(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
        HttpResponse patch(const std::string &url, const std::map<std::string, std::string> &body, const std::map<std::string, std::string> &headers);
        HttpResponse patch(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
        HttpResponse patch(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);

        HttpResponse put(const std::string &url, const std::string &body);
        HttpResponse put(const std::string &url, const std::map<std::string, std::string> &body);
        HttpResponse put(const std::string &url, const qtng::utils::UrlQuery &body);
        HttpResponse put(const std::string &url, const FormData &body);
        HttpResponse put(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
        HttpResponse put(const std::string &url, const std::map<std::string, std::string> &body, const std::map<std::string, std::string> &headers);
        HttpResponse put(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
        HttpResponse put(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);
        HttpResponse put(const std::string &url, const std::string &body);
        HttpResponse put(const std::string &url, const std::map<std::string, std::string> &body);
        HttpResponse put(const std::string &url, const qtng::utils::UrlQuery &body);
        HttpResponse put(const std::string &url, const FormData &body);
        HttpResponse put(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
        HttpResponse put(const std::string &url, const std::map<std::string, std::string> &body, const std::map<std::string, std::string> &headers);
        HttpResponse put(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
        HttpResponse put(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);

3.2 HttpResponse
^^^^^^^^^^^^^^^^

.. method:: qtng::utils::Url url() const

    返回响应 URL。通常与请求 URL 一致，若存在重定向则为最终 URL
    
.. method:: void setUrl(const std::string &url)

    设置响应 URL（由 ``HttpSession`` 内部调用）
    
.. method:: int statusCode() const

    返回 HTTP 状态码（如 200 成功，404 未找到，500 服务器错误）
    
.. method:: void setStatusCode(int statusCode)

    设置状态码（由 ``HttpSession`` 内部调用）
    
.. method:: std::string statusText() const

    返回状态描述文本（如 "OK"、"Not Found"）

.. method:: void setStatusText(const std::string &statusText)

    设置状态描述文本（由 ``HttpSession`` 内部调用）
    
.. method:: std::vector<HttpCookie> cookies() const

    返回响应携带的 cookies
    
.. method:: void setCookies(const std::vector<HttpCookie> &cookies)

    设置 cookies（由 ``HttpSession`` 内部调用）
    
.. method:: HttpRequest request() const

    返回关联的请求对象（重定向时为最新请求）
    
.. method:: std::int64_t elapsed() const

    返回请求总耗时（毫秒），从发起请求到完成解析/出错
    
.. method:: void setElapsed(std::int64_t elapsed)

    设置耗时（由 ``HttpSession`` 内部调用）
    
.. method:: std::vector<HttpResponse> history() const

    返回重定向历史记录（若无重定向则为空列表）
    
.. method:: void setHistory(const std::vector<HttpResponse> &history)

    设置重定向历史（由 ``HttpSession`` 内部调用）
    
.. method:: HttpVersion version() const

    返回 HTTP 版本（当前支持 1.0/1.1）
    
.. method:: void setVersion(HttpVersion version)

    设置 HTTP 版本（由 ``HttpSession`` 内部调用）
    
.. method:: std::string body() const

    以字节数组形式返回响应体
    

    将响应体解析为 JSON 文档
    
.. method:: std::string text()

    将响应体解码为 UTF-8 字符串
    
.. method:: std::string html()

    根据 HTTP 头/HTML 文档检测编码并返回字符串（暂未实现，功能同 text()）
    
.. method:: bool isOk() const

    检测请求是否成功（应首先调用此方法）
    
.. method:: bool hasNetworkError() const

    检测是否发生网络错误
    
.. method:: bool hasHttpError() const

    检测是否发生 HTTP 错误（状态码 >= 400）

.. method:: std::shared_ptr<RequestError> error() const

    返回错误详情对象
    
.. method:: void setError(std::shared_ptr<RequestError> error)

    设置错误对象（由 ``HttpSession`` 内部调用）

.. method:: std::shared_ptr<SocketLike> takeStream(std::string *readBytes)

    当启用流式响应时（``HttpRequest::streamResponse(true)``），获取原始连接对象

3.3 HttpRequest
^^^^^^^^^^^^^^^

.. method:: std::string method() const

    返回 HTTP 方法（GET/POST 等）
    
.. method:: void setMethod(const std::string &method)

    设置 HTTP 方法（支持标准方法及自定义方法）
    
.. method:: qtng::utils::Url url() const

    返回请求 URL
    
.. method:: void setUrl(const std::string &url)

    设置请求 URL（qtng::utils::Url 格式）
    
.. method:: void setUrl(const std::string &url)

    设置请求 URL（字符串格式）
    
.. method:: qtng::utils::UrlQuery query() const

    返回 URL 查询参数
    
.. method:: void setQuery(const std::map<std::string, std::string> &query)

    通过 std::map 设置查询参数
    
.. method:: void setQuery(const qtng::utils::UrlQuery &query)

    通过 qtng::utils::UrlQuery 设置查询参数
    
.. method:: std::vector<HttpCookie> cookies() const

    返回请求携带的 cookies
    
.. method:: void setCookies(const std::vector<HttpCookie> &cookies)

    设置请求 cookies
    
.. method:: std::string body() const

    返回请求体数据

    .. method:: void setBody(const std::string &body)

    设置请求的正文。
    
    包含多个重载函数：
    
    .. code-block:: c++
        
        void setBody(const FormData &formData);
        void setBody(const std::map<std::string, std::string> form);
        void setBody(const qtng::utils::UrlQuery &form);

.. method:: std::string userAgent() const

    返回请求的用户代理字符串。
    
.. method:: void setUserAgent(const std::string &userAgent)

    设置请求的用户代理字符串。
    
.. method:: int maxBodySize() const

    返回响应的最大正文大小。
    
    注意：此限制应用于响应而非请求。若服务器返回超过此大小的响应，``HttpSession`` 将报告 ``UnrewindableBodyError`` 错误。
    
.. method:: void setMaxBodySize(int maxBodySize)

    设置响应的最大正文大小。
    
    注意：请参考 ``maxBodySize()``。
    
.. method:: int maxRedirects() const

    返回允许的最大重定向次数。设为0将禁用HTTP重定向。
    
    注意：超出此限制时，``HttpSession`` 将报告 ``TooManyRedirects`` 错误。
    
.. method:: void setMaxRedirects(int maxRedirects)

    设置允许的最大重定向次数。
    
    注意：请参考 ``maxRedirects()``。
    
.. method:: HttpVersion version() const

    返回请求的HTTP版本。默认为 ``Unkown``，表示使用 ``HttpSession::defaultVersion()``。
    
    注意：``HttpSession::defaultVersion()`` 默认使用 HTTP 1.1
    
.. method:: void setVersion(HttpVersion version)

    设置请求的HTTP版本。 
    
    注意：请参考 ``version()``。
    
.. method:: bool streamResponse() const

    若为true，表示返回的 ``HttpResponse`` 未读取HTTP内容。
    
    注意：请参考 ``HttpResponse::takeStream()``。
    
.. method:: void setStreamResponse(bool streamResponse)

    设为true以使 ``HttpSession`` 返回未读取HTTP内容的 ``HttpResponse``。
    
    注意：请参考 ``HttpResponse::takeStream()``。
    
.. method:: float tiemout() const

    返回连接超时时间（单位：秒）。
    
    注意：此限制仅作用于连接阶段。可使用 ``qtng::Timeout`` 管理整个请求的超时。
    
.. method:: void setTimeout(float timeout);

    设置连接超时时间。
    
    注意：请参考 ``timeout()``。


3.4 FormData
^^^^^^^^^^^^

``FormData`` 是用于POST的HTTP表单，用于文件上传。

注意：请参考 ``void HttpRequest::setBody(const FormData &formData)``。

.. method:: void addFile(const std::string &name, const std::string &filename, const std::string &data, const std::string &contentType = std::string())
    
    向表单的 ``name`` 字段添加文件。
    
.. method:: void addQuery(const std::string &key, const std::string &value)

    设置表单 ``name`` 字段的值为 ``value``。

3.4 HTTP errors
^^^^^^^^^^^^^^^

使用 ``HttpResponse`` 前应检查 ``HttpResonse::isOk()``。若返回false，则响应异常。此时 ``HttpResponse::error()`` 返回以下类型实例：

* RequestError

    所有错误均为请求错误。

* HTTPError

    服务器返回HTTP错误，错误码为 ``HTTPError::statusCode``。

* ConnectionError

    读写数据时连接中断。

* ProxyError

    无法通过代理连接服务器。

* SSLError

    SSL连接失败（握手错误）。

* RequestTimeout

    读写数据超时。

    ``RequestTimeout`` 同样属于 ``ConnectionError``。

* ConnectTimeout

    连接服务器超时。

    ``ConnectTimeout`` 同时属于 ``ConnectionError`` 和 ``RequestTimeout``。

* ReadTimeout

    读取超时。

    ``ReadTimeout`` 同样属于 ``RequestTimeout``。

* URLRequired

    请求中缺少URL。

* TooManyRedirects

    服务器返回过多重定向响应。

* MissingSchema

    请求URL缺少协议头。

    注意：``HttpSession`` 仅支持 ``http`` 和 ``https``。

* InvalidScheme

    请求URL包含不支持的协议（非 ``http``/``https``）。

* UnsupportedVersion

    不支持的HTTP版本。

    注意：``HttpSession`` 仅支持 HTTP 1.0 和 1.1。

* InvalidURL

    请求的URL无效。

* InvalidHeader

    服务器返回无效标头。

* ChunkedEncodingError

    服务器返回的分块编码正文错误。

* ContentDecodingError

    无法解码响应正文。

* StreamConsumedError

    读取正文时流已被消耗。

* UnrewindableBodyError

    正文过大无法回卷。

4. Http 服务器
--------------

4.1 Basic Http Server
^^^^^^^^^^^^^^^^^^^^^

4.1.1 BaseHttpRequestHandler
++++++++++++++++++++++++++++++
处理 HTTP 请求的基础类，提供 HTTP 协议解析、响应生成、错误处理等核心功能。

.. method:: BaseHttpRequestHandler()
    :no-index:

    初始化默认参数，HTTP 版本默认为 Http1_1，请求超时时间 requestTimeout 默认 1 小时，最大请求体大小 maxBodySize 默认 32MB，连接状态 closeConnection 初始为 Maybe

.. method:: virtual void handle()

    循环处理请求，直到 closeConnection 标记为 Yes，调用 handleOneRequest() 处理单个请求

.. method:: virtual void handleOneRequest()

    设置超时限制（Timeout timeout(requestTimeout);）,调用 parseRequest() 解析请求头,调用 doMethod() 分发到具体 HTTP 方法处理器

.. method:: virtual bool parseRequest()

    解析请求行（如 GET /path HTTP/1.1）,提取 method、path、version,解析请求头并存储到 headers,处理 Connection 头决定是否保持连接,返回值: true 表示解析成功，false 表示失败（自动发送 400 错误）

.. method:: void doMethod

    http方法分发，所有方法默认返回 501 Not implemented，以下方法都需要子类进行重写具体实现

    .. code-block:: c++

        virtual void doGET();
        virtual void doPOST();
        virtual void doPUT();
        virtual void doDELETE();
        virtual void doPATCH();
        virtual void doHEAD();
        virtual void doOPTIONS();
        virtual void doTRACE();
        virtual void doCONNECT();

.. method:: bool sendError(HttpStatus status, const std::string &message = std::string())

    生成标准错误页面（HTML 格式）,发送错误响应头（状态码、Content-Type 等）,记录错误日志（logError()）

.. method:: void sendCommandLine(HttpStatus status, const std::string &shortMessage)

    发送状态行（如 HTTP/1.1 200 OK）

.. method:: void sendHeader(const std::string &name, const std::string &value)

    添加响应头（自动处理 Connection 逻辑）

.. method:: void sendHeader(KnownHeader name, const std::string &value)

    同sendHeader功能

.. method:: bool endHeader()

    结束头部并发送 \r\n，返回 true 表示成功

.. method:: std::shared_ptr<FileLike> bodyAsFile(bool processEncoding = true)

    根据 Content-Length 或 Transfer-Encoding 读取请求体,自动处理 GZIP/DEFLATE 解压缩（需启用 QTNG_HAVE_ZLIB）,支持分块传输（Chunked Encoding,返回值: 返回可读的 FileLike 对象，包含请求体内容。

.. method:: bool switchToWebSocket()

    验证 Upgrade: websocket 和 Sec-WebSocket-Key,计算并返回 Sec-WebSocket-Accept,标记连接升级为 WebSocket

.. method:: virtual void logRequest(HttpStatus status, int bodySize);

    打印客户端地址、请求方法、状态码和响应体大小

.. method:: virtual void logError(HttpStatus status, const std::string &shortMessage, const std::string &longMessage);

    记录错误状态和消息

4.1.2 StaticHttpRequestHandler
+++++++++++++++++++++++++++++++
继承 ``BaseHttpRequestHandler``，处理静态资源请求，支持文件传输、目录列表、自动索引文件检测等功能,内置路径遍历防护、MIME类型自动识别、XSS防护

.. method:: std::shared_ptr<FileLike> serveStaticFiles(const PosixPath &dir, const std::string &subPath)

    根据给定的目录和子路径，返回对应的文件内容或目录列表。 

.. method:: std::shared_ptr<FileLike> listDirectory(const PosixPath &dir, const std::string &displayDir)

    生成目录列表的HTML页面。遍历目录中的文件和子目录，生成带有链接的HTML列表。

.. method:: PosixPath getIndexFile(const PosixPath &dir)

    检查目录中是否存在`index.html`或`index.htm`，如果存在则返回该文件的信息，否则返回空,这决定了当访问目录时是否显示默认索引文件。

.. method:: virtual bool loadMissingFile(const PosixPath &fileInfo);

    默认返回false，子类可以重写这个方法，尝试生成或获取缺失的文件。

4.1.3 SimpleHttpRequestHandler
+++++++++++++++++++++++++++++++
继承 ``StaticHttpRequestHandler``，预配置的静态文件服务器，提供开箱即用的基本 HTTP 文件服务功能。

.. method:: void setRootDir(const PosixPath &rootDir)

    设置允许修改的目录,应确保运行进程对目标目录有读取权限,建议在服务器启动前设置，避免运行时修改导致竞态条件

.. method:: virtual void doGET() override;

    响应Get请求，调用父类的serveStaticFiles方法，进行文件处理

.. method:: virtual void doHEAD() override;

    响应HEAD请求，调用父类的serveStaticFiles方法，进行文件处理

4.1.4 BaseHttpProxyRequestHandler

    实现 HTTP 代理的核心逻辑，支持正向代理和隧道代理（如 HTTPS CONNECT 方法）

.. method:: virtual void logRequest(qtng::HttpStatus status, int bodySize)

    用于记录请求日志,这里是空实现，需要子类进行具体实现

.. method:: virtual void logError(qtng::HttpStatus status, const std::string &shortMessage, const std::string &longMessage)

    用于记录错误日志,这里是空实现，需要子类进行具体实现

.. method:: virtual void logProxy(const std::string &remoteHostName, std::uint16_t remotePort, const HostAddress &forwardAddress,bool success)

    提供代理专用日志接口 logProxy(),默认关闭常规请求日志（避免重复记录）

.. method:: virtual void doMethod()

    HTTP 请求分发入口，根据请求方法决定处理逻辑。检查 method 是否为 CONNECT,其他方法（GET/POST等）走普通代理流程

.. method:: virtual void doCONNECT()

    处理 CONNECT 隧道请求，建立客户端与目标服务器的双向通道。

.. method:: virtual void doProxy()

    处理普通HTTP代理请求，转发客户端请求到目标服务器并返回响应。

.. method:: virtual std::shared_ptr<SocketLike> makeConnection(const std::string &remoteHostName, std::uint16_t remotePort,HostAddress *forwardAddress)

    负责根据传入的remoteHostName（目标主机名）和remotePort（目标端口），创建并初始化一个到目标服务器的Socket连接。此连接将用于后续的HTTP请求转发或HTTPS隧道代理（如CONNECT方法）。

4.2 Application Server
^^^^^^^^^^^^^^^^^^^^^^^
SimpleHttpServer : public TcpServer<SimpleHttpRequestHandler>
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
暂无具体实现

SimpleHttpsServer : public SslServer<SimpleHttpRequestHandler>
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
暂无具体实现

5. 密码学
---------------

5.1 密码哈希表
^^^^^^^^^^^^^^^^
MessageDigest
++++++++++++++
提供消息摘要（哈希）功能，支持多种哈希算法，允许分块处理数据并生成摘要。支持MD4和MD5算法，Sha1, Sha224, Sha256, Sha384, Sha512一系列SHA系列算法以及Ripemd160, Whirlpool哈希算法。

.. method:: MessageDigest(Algoritim algo)
    :no-index:

    初始化指定哈希算法的上下文

.. method:: addData(const char *data, int len)
    :no-index:

    将原始字节数据添加到哈希计算，调用 EVP_DigestUpdate 更新上下文，失败则标记错误。

.. method:: addData(const char *data)
    :no-index:

    addData的重载，内部计算data长度后调用上一个addData

.. method:: std::string result()

    结束哈希计算并返回最终摘要，若首次调用，调用 EVP_DigestFinal_ex 结束计算，缓存结果，后续调用直接返回缓存结果，失败返回空 std::string。

.. method:: void update(const std::string &data)

    同 addData，提供兼容常见哈希接口的方法。    

.. method:: void update(const char *data, int len)

    同 addData，提供兼容常见哈希接口的方法。

.. method:: std::string hexDigest()

    同 result()，返回原始摘要。    

.. method:: std::string digest()

    返回十六进制字符串形式的摘要。

.. method:: static std::string hash(const std::string &data, Algorithm algo)

    一次性计算数据的哈希值（十六进制）。

.. method:: static std::string digest(const std::string &data, Algorithm algo)

    一次性计算数据的哈希值（原始字节）。

.. method:: std::string PBKDF2_HMAC(int keylen, const std::string &password, const std::string &salt,  const MessageDigest::Algorithm hashAlgo = MessageDigest::Sha256, int i = 10000)

    调用 OpenSSL 的 PKCS5_PBKDF2_HMAC 函数生成密钥。

.. method:: std::string scrypt(int keylen, const std::string &password, const std::string &salt, int n = 1048576, int r = 8, int p = 1)

    暂未进行实现

5.2 对称加密和解密
^^^^^^^^^^^^^^^^^^^^
Clipher
+++++++
提供对称加密/解密功能，支持多种算法（如 AES、DES、ChaCha20 等）和模式（如 CBC、CTR、ECB 等），支持密码派生、填充控制。

.. method:: Cipher(Algorithm alog, Mode mode, Operation operation)
    :no-index:

    初始化加密上下文，通过 getOpenSSL_CIPHER() 获取对应的 OpenSSL EVP_CIPHER。创建 EVP_CIPHER_CTX 上下文，默认启用填充，失败时标记 hasError

.. method:: Cipher *copy(Operation operation)

    复制当前配置，创建新的 Cipher 实例

.. method:: bool isValid()

    检查上下文是否有效,条件：OpenSSL 上下文存在、未发生错误且已初始化。

.. method:: bool isStream()
    
    判断当前加密上下文是否使用流加密模式（如 CFB、OFB、CTR 等）。

.. method:: bool isBlock()
    
    判断是否使用分组加密模式（如 ECB、CBC 等），直接返回 !isStream()。

.. method:: void setKey(const std::string &key)

    设置原始密钥。

.. method:: std::string key()

    返回当前密钥

.. method:: setInitialVector(const std::string &iv)
    :no-index:

    设置初始化向量（IV）,存储 IV 并初始化上下文。

.. method:: std::string initialVector()

    返回当前IV

.. method:: std::string iv()

    同initialVector方法

.. method:: bool setPassword(const std::string &password, const std::string &salt,const MessageDigest::Algorithm hashAlgo = MessageDigest::Sha256, int i = 100000 )

    通过密码派生密钥（PBKDF2-HMAC）,参数：密码、盐值、哈希算法、迭代次数。生成随机盐（可选），调用 PBKDF2_HMAC 派生密钥和 IV。

.. method:: bool setOpensslPassword(const std::string &password, const std::string &salt,const MessageDigest::Algorithm hashAlgo = MessageDigest::Md5,int i = 1)

    兼容 OpenSSL 的密钥派生（EVP_BytesToKey）,参数：密码、盐值（必须 8 字节）、哈希算法、迭代次数。使用传统方法生成密钥，适合解密 OpenSSL 加密的数据。

.. method:: std::string addData(const std::string &data)

    分块处理数据，返回加密/解密后的结果。

.. method:: std::string addData(const char *data, int len)

    分块处理数据，返回加密/解密后的结果。

.. method:: std::string update(const std::string &data)

    分块处理数据，返回加密/解密后的结果。

.. method:: std::string update(const char *data, int len)

    分块处理数据，返回加密/解密后的结果。

.. method:: std::string finalData();

    结束加密/解密，返回剩余数据。

.. method:: std::string final()

    结束加密/解密，返回剩余数据。

.. method:: std::string saltHeader()

    生成 OpenSSL 格式的盐值头部（Salted__ + 8字节盐）,加密时保存盐值，供解密时使用。

.. method:: std::string parseSalt()

    从 OpenSSL 头部解析盐值,返回值：std::pair<std::string, std::string>（盐值 + 剩余数据）。

.. method:: bool setPadding(bool padding)

    启用或禁用 PKCS#7 填充：用于控制分组加密算法（如 AES-CBC、DES-ECB）在数据末尾自动添加填充字节的行为,仅对分组加密有效：在流加密模式（如 CTR、CFB）中自动忽略填充设置。

.. method:: bool padding()

    获取启用或禁用 PKCS#7 填充

.. method:: int keySize()

    获取密钥长度

.. method:: int ivSize()

    获取iv长度

.. method:: int blockSize()

    获取block长度

5.3 公钥算法
^^^^^^^^^^^^^^
5.3.1 PublicKey
++++++++++++++++
加密体系中的核心类，用于管理公钥操作。

.. method:: PublicKey()
    :no-index:

    创建空公钥对象，内部初始化OpenSSL的EVP_PKEY结构

.. method:: PublicKey(const PublicKey &other)
    :no-index:

    深拷贝底层OpenSSL密钥对象（通过EVP_PKEY_dup）,避免多个对象共享同一密钥内存，保证线程安全

.. method:: static PublicKey load(const std::string &data, Ssl::EncodingFormat format = Ssl::Pem)

    创建BIO内存对象读取密钥数据,调用PEM_read_bio_PUBKEY解析PEM格式,生成EVP_PKEY结构并存入PublicKeyPrivate

.. method:: std::string save(Ssl::EncodingFormat format = Ssl::Pem)

    通过PEM_write_bio_PUBKEY将密钥写入BIO对象

.. method:: std::string encrypt(const std::string &data)

    初始化加密上下文（算法自动识别），动态计算输出缓冲区大小（避免固定长度限制），执行加密并返回结果

.. method:: std::string rsaPublicEncrypt(const std::string &data,RsaPadding padding = PKCS1_PADDING)
    :no-index:

    使用 RSA 公钥加密（``EVP_PKEY_encrypt``）。``PKCS1_PADDING`` 兼容性最好（默认）；``PKCS1_OAEP_PADDING`` 更安全，推荐新协议使用；``NO_PADDING`` 需自行处理填充。

.. method:: std::string rsaPublicDecrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING)
    :no-index:

    使用 RSA 公钥做原始解密/恢复（``EVP_PKEY_verify_recover``），对应私钥侧的 ``rsaPrivateEncrypt``。支持 ``PKCS1_PADDING``（默认）与 ``NO_PADDING``。
    

.. method:: bool verify(const std::string &data, const std::string &hash, MessageDigest::Algorithm hashAlgo)

    使用指定哈希算法（如SHA256）处理数据,对比签名哈希值与计算值,返回true表示验证通过

.. method:: Algorithm algorithm()

    枚举类型标识密钥类型（RSA/DSA/EC）

.. method:: int bits()

    返回密钥长度，2048位RSA密钥返回2048

.. method:: PublicKey &operator=(const PublicKey &other)

    重载=,约等于拷贝构造函数

.. method:: bool operator==(const PublicKey &other) 

    重载==

.. method:: bool operator==(const PrivateKey &)

    重载==

.. method:: bool operator!=(const PublicKey &other)

    重载!=

.. method:: bool operator!=(const PrivateKey &)

    重载!=

.. method:: std::string digest(MessageDigest::Algorithm algorithm = MessageDigest::Sha256)

    生成唯一指纹（如SHA256哈希）用于密钥校验

.. method:: bool isNull()

    密钥判空检验

.. method:: bool isValid()

    密钥有效性检验

5.3.2 PrivateKey
+++++++++++++++++
封装私钥操作，包括密钥生成、签名、解密及特定于私钥的加密操作。

.. method:: PrivateKey()    
    :no-index:

    默认构造函数

.. method:: PrivateKey(const PrivateKey &other)
    :no-index:

    拷贝构造函数

.. method:: PrivateKey(PrivateKey &&other)
    :no-index:

    移动构造函数

.. method:: PrivateKey &operator=(const PublicKey &other)

    拷贝构造函数

.. method:: PrivateKey &operator=(const PrivateKey &other)

    拷贝构造函数
.. method:: bool operator==(const PrivateKey &other) 

    重载==运算符

.. method:: bool operator==(const PublicKey &) 

    重载==运算符

.. method:: bool operator!=(const PrivateKey &other) 

    重载!=运算符

.. method:: bool operator!=(const PublicKey &)

    重载!=运算符

.. method:: PublicKey publicKey()

    提取当前私钥对应的公钥。

.. method:: std::string sign(const std::string &data, MessageDigest::Algorithm hashAlgo)

    使用私钥对数据进行签名。

.. method:: std::string decrypt(const std::string &data)

    使用私钥解密数据。初始化解密上下文：EVP_PKEY_decrypt_init,计算解密后长度：EVP_PKEY_decrypt 两次调用，第一次获取长度，第二次解密数据,返回解密结果：调整 std::string 大小并填充数据。

.. method:: rsaPrivateEncrypt
    :no-index:

    使用 RSA 私钥做原始加密（``EVP_PKEY_sign``，无摘要），对应公钥侧的 ``rsaPublicDecrypt``。支持 ``PKCS1_PADDING``（默认）与 ``NO_PADDING``。

.. method:: rsaPrivateDecrypt
    :no-index:

    使用 RSA 私钥解密（``EVP_PKEY_decrypt``）。支持 ``PKCS1_PADDING``（默认）、``PKCS1_OAEP_PADDING`` 与 ``NO_PADDING``。

.. method:: static PrivateKey generate(Algorithm algo, int bits)

    通过 ``EVP_PKEY_keygen`` 生成指定算法和长度的私钥（RSA/DSA）。

.. method:: static PrivateKey load(const std::string &data, Ssl::EncodingFormat format = Ssl::Pem,const std::string &password = std::string())

    从 PEM/DER 格式加载私钥，支持密码解密

.. method:: std::string save(Ssl::EncodingFormat format = Ssl::Pem, const std::string &password = std::string())

    核心功能是序列化私钥，支持密码加密（需配合有效加密算法）,依赖 PrivateKeyWriter 处理 OpenSSL 底层细节，需完善 DER 格式和默认加密逻辑。

.. method:: std::string savePublic(Ssl::EncodingFormat format = Ssl::Pem)

    直接复用公钥的保存逻辑，确保输出仅包含公钥信息，无需处理密码，始终以明文形式保存。

5.3.3 PasswordCallback
+++++++++++++++++++++++
加密解密进度获取

.. method:: virtual std::string get(bool writing) = 0;

    获取加密解密进度，需子类进行重写实现

5.3.4 PrivateKeyWriter
+++++++++++++++++++++++
非对称加密密钥（如 RSA、DSA 密钥）序列化为特定格式（PEM/DER），支持加密私钥并保存到文件或内存。其核心职责是提供灵活的配置选项（加密算法、密码、是否仅保存公钥）并调用 OpenSSL 函数完成序列化。

.. method:: PrivateKeyWriter(const PrivateKey &key)
    :no-index:

    拷贝构造函数，通过私钥构造

.. method:: PrivateKeyWriter(const PublicKey &key)
    :no-index:

    拷贝构造函数，通过公钥构造

.. method:: PrivateKeyWriter &setCipher(Cipher::Algorithm algo, Cipher::Mode mode)

    指定加密私钥的算法（如 AES-256-CBC）,若不调用此方法，默认不加密（Cipher::Null）。

.. method:: PrivateKeyWriter &setPassword(const std::string &password)

    提供加密私钥所需的密码，直接传递获取。

.. method:: PrivateKeyWriter &setPassword(std::shared_ptr<PasswordCallback> callback)

    提供加密私钥所需的密码，通过回调动态获取。

.. method:: PrivateKeyWriter &setPublicOnly(bool publicOnly)

    强制仅保存公钥，即使传入的是私钥,从私钥提取公钥并保存。

.. method:: std::string asPem()

    将密钥序列化为 PEM 格式，支持加密私钥。

.. method:: std::string asDer()

    未完全实现，返回空数据,将密钥序列化为 DER 格式，支持 PKCS#8 加密。

.. method:: bool save(const std::string &filePath)

    将密钥保存到文件，默认使用 PEM 格式。

5.3.5 PrivateKeyReader
+++++++++++++++++++++++
负责从文件或内存数据中加载私钥或公钥，支持处理加密的私钥文件（通过密码或回调函数）。

.. method:: PrivateKeyReader()
    :no-index:

    初始化,生成PrivateKey对象

.. method:: ethod:: PrivateKeyReader &setPassword(const std::string &password)

    设置直接密码，用于解密加密的私钥。

.. method:: PrivateKeyReader &setPassword(std::shared_ptr<PasswordCallback> callback)

    设置密码回调对象，用于动态获取密码（例如 GUI 输入）。

.. method:: PrivateKeyReader &setFormat(Ssl::EncodingFormat format)

    指定输入数据的编码格式（目前仅支持 PEM）。

.. method:: PrivateKey read(const std::string &data)

    从内存中的字节数组读取私钥。

.. method:: PublicKey readPublic(const std::string &data)

    从内存中的字节数组读取公钥。

.. method:: PrivateKey read(const std::string &filePath)

    从文件读取私钥。

.. method:: PublicKey readPublic(const std::string &filePath)

    从文件读取公钥。

5.4 证书和证书请求
^^^^^^^^^^^^^^^^^^^
5.4.1 Certificate
++++++++++++++++++
封装证书操作，提供接口如加载/保存证书、获取证书信息、生成证书等。

.. method:: Certificate()
    :no-index:

    构造函数，进行初始化操作

.. method:: Certificate(const Certificate &other)
    :no-index:

    复制构造函数，进行初始化操作

.. method:: Certificate(Certificate &&other)
    :no-index:

    移动构造函数，进行初始化操作

.. method:: static Certificate load(const std::string &data, Ssl::EncodingFormat format = Ssl::Pem)

    从PEM或DER格式的字节流加载证书。

.. method:: static Certificate generate(const PublicKey &publickey, const PrivateKey &caKey, MessageDigest::Algorithm signAlgo,long serialNumber, const qtng::utils::DateTime &effectiveDate, const qtng::utils::DateTime &expiryDate,const std::multimap<SubjectInfo, std::string> &subjectInfoes)

    生成新的X.509证书，用CA私钥签名。

.. method:: static Certificate selfSign(const PrivateKey &key, MessageDigest::Algorithm signAlgo, long serialNumber,const qtng::utils::DateTime &effectiveDate, const qtng::utils::DateTime &expiryDate,const std::multimap<Certificate::SubjectInfo, std::string> &subjectInfoes)

    自签名快捷方法，作用是调用generate方法

.. method:: std::string save(Ssl::EncodingFormat format = Ssl::Pem)

    将证书保存为PEM或DER格式。

.. method:: std::string digest(MessageDigest::Algorithm algorithm = MessageDigest::Sha256)

    计算证书DER数据的哈希值（如SHA-256）。

.. method:: qtng::utils::DateTime effectiveDate() const

    在 CertificatePrivate::init 中解析 X509_getm_notBefore 和 X509_getm_notAfter。

.. method:: qtng::utils::DateTime expiryDate() const

    在 CertificatePrivate::init 中解析 X509_getm_notBefore 和 X509_getm_notAfter。

.. method::  std::stringList subjectInfo(SubjectInfo subject)

    通过 X509_get_subject_name 和 X509_get_issuer_name 获取 X509_NAME，解析为键值对。

.. method:: std::stringList subjectInfo(const std::string &attribute)

    通过 X509_get_subject_name 和 X509_get_issuer_name 获取 X509_NAME，解析为键值对。    

.. method::PublicKey publicKey()

    获取公钥

.. method::std::string serialNumber()

    获取序列号

.. method:: bool isBlacklisted()

    检查证书是否在预定义的黑名单中（如Comodo事件中的恶意证书）。

.. method:: bool isNull()

    检查证书是否为空

.. method:: bool isValid()

    检查证书的有效性（是否为空或者在预定义的黑名单内）

.. method:: std::string toString()

    将证书以字符串的方式进行返回

.. method:: std::string version()

    返回当前证书版本

.. method:: bool isSelfSigned()

    调用 X509_check_issued 检查证书是否由自身签发。

5.4.2 CertificateRequest
+++++++++++++++++++++++++
请求证书

.. method:: certificate()
    :no-index:

    返回与证书请求关联的 Certificate 对象。

5.5 TLS密码套件
^^^^^^^^^^^^^^^^^
5.5.1 SslCipher
++++++++++++++++
SSL/TLS 连接中使用的加密套件（Cipher Suite），包含加密算法、协议版本、密钥交换方法等详细信息。
.. method:: SslCipher();

    默认构造函数

.. method:: SslCipher(const std::string &name);

    构造函数，通过名称进行构造

.. method:: SslCipher(const std::string &name, Ssl::SslProtocol protocol);

    构造函数，通过名称和协议进行构造

.. method:: SslCipher(const SslCipher &other);

    拷贝构造函数

.. method:: std::string authenticationMethod()

    返回密钥认证方法（如 RSA）。

.. method:: std::string encryptionMethod()

    返回具体加密算法。

.. method:: bool isNull()

    判断对象是否有效（如构造函数未找到匹配项时返回 true）。

.. method:: std::string keyExchangeMethod()

    返回密钥交换方法（如 ECDHE）。

.. method:: std::string name()

    直接返回私有类中存储的名称。

.. method:: Ssl::SslProtocol protocol()

    直接返回私有类中存储的协议枚举值。

.. method:: std::string protocolString()

    直接返回私有类中存储的协议字符串。

.. method:: int supportedBits()

    返回加密位数。

.. method:: int usedBits()

    返回加密位数。

.. method:: inline bool operator!=(const SslCipher &other)

    通过名称和协议判断两个加密套件是否相同，而非比较所有属性。

.. method:: SslCipher &operator=(SslCipher &&other)

    通过名称和协议判断两个加密套件是否相同，而非比较所有属性。

.. method:: SslCipher &operator=(const SslCipher &other)

    通过名称和协议判断两个加密套件是否相同，而非比较所有属性。

.. method:: void swap(SslCipher &other)

    交换两个加密套件

.. method:: bool operator==(const SslCipher &other)

    通过名称和协议判断两个加密套件是否相同，而非比较所有属性。

6. 配置和构建
--------------
6.1 事件循环（Unix 上使用 libev）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

在 Unix 系统上，qtng 使用 libev 作为事件循环后端。CMake 会自动选择最佳机制：

1. **操作系统判断**：Linux、macOS 等非 Windows Unix 目标启用 libev 后端。

2. **后端选择**：检测 ``epoll_ctl`` 或 ``kqueue``。

   * Linux（epoll）：``EV_USE_EPOLL=1``、``EV_USE_EVENTFD=1``
   * BSD（kqueue）：``EV_USE_KQUEUE=1``
   * 否则回退到 ``poll()``

   宏 ``QTNG_USE_EV`` 表示正在使用 libev。

3. **源码集成**：libev 位于 ``src/ev/``；Unix 上由 ``src/eventloop_ev.cpp`` 实现事件循环。

4. **Windows**：使用独立的 Windows 事件循环实现。


6.2 SSL/TLS 配置
^^^^^^^^^^^^^^^^

6.2.1 构建时的 TLS 库选择
+++++++++++++++++++++++++

CMake 按以下顺序选择 TLS/加密库：

* 若存在带 ``CMakeLists.txt`` 的 ``libressl/`` 子目录，自动构建并链接内置 LibreSSL。
* 否则需要系统 OpenSSL（``find_package(OpenSSL REQUIRED)``）。

未使用内置 LibreSSL 时，Debian/Ubuntu 可安装 ``libssl-dev`` 开发包。


6.3 安装 qtng
^^^^^^^^^^^^^

构建完成后，使用 CMake 安装头文件与静态库：

.. code-block:: bash

    cmake --install . --prefix /usr/local

典型目录布局：

* ``${prefix}/include/qtng.h`` — 总头文件（``#include <qtng.h>``）
* ``${prefix}/include/qtng/`` — 各模块头文件（``coroutine.h``、``socket.h``、``private/``、``utils/`` 等）
* ``${prefix}/lib/libqtng.a`` — 静态库（部分 64 位 Linux 发行版为 ``lib64/``；MSVC 上为 ``qtng.lib``）

应用链接时使用 ``-lqtng``（并确保库搜索路径包含安装前缀）。使用 ``#include <qtng.h>`` 或 ``#include <qtng/coroutine.h>``（``#include <qtng/qtng.h>`` 与总头文件等价）。通过 ``add_subdirectory()`` 嵌入时写法相同。


6.2.2 直接使用基础Socket类
++++++++++++++++++++++++++
如果不需要任何加密，可直接使用基础的Socket类而非SslSocket类，直接使用 Socket 绕过了所有SSL/TLS层，数据以明文传输。
下面是一个简单的示例

.. code-block:: c++
    :caption: 示例 : 使用基础的TcpServer而非SslServer实现一个简单的http服务

        #include "qtng.h"
        using namespace qtng;
        class HelloRequestHandler: public SimpleHttpRequestHandler
        {
        public:
            virtual void doGET() override
            {
                if (path == "/hello/") {
                    sendResponse(HttpStatus::OK);
                    sendHeader("Content-Type", "text/plain");
                    std::string body = "hello";
                    sendHeader("Content-Length", std::to_string(body.size()));
                    endHeader();
                    request->sendall(body);
                } 
            }
        };
        class HelloHttpServer: public TcpServer<HelloRequestHandler>
        {
        public:
            HelloHttpServer(const HostAddress &serverAddress, std::uint16_t serverPort)
                : TcpServer(serverAddress, serverPort) {}
        };
        int main()
        {
            HelloHttpServer httpd(HostAddress::Any, 8443);
            httpd.serveForever();
            return 0;
        }

7.其他辅助类
------------
7.1 IO操作
^^^^^^^^^^
该模块提供了一套跨平台的文件和内存IO抽象，结合协程友好的非阻塞操作，以及安全的POSIX路径管理工具，适用于需要高效、安全文件处理的网络应用。

核心函数：

.. method:: bool sendfile(std::shared_ptr<FileLike> inputFile, std::shared_ptr<FileLike> outputFile, std::int64_t bytesToCopy = -1, int suitableBlockSize = 1024 * 8)

    输入文件内容复制到输出文件，支持大文件传输。参数：inputFile/outputFile：输入输出文件对象,bytesToCopy：要复制的字节数（-1 表示全部）,suitableBlockSize：缓冲区大小（默认8KB）。

7.1.1 FileLike
+++++++++++++++
抽象基类，定义文件操作的通用接口，支持读写、关闭、获取大小等操作。

.. method:: virtual std::int32_t read(char *data, std::int32_t size)

    从文件中读取数据到缓冲区，返回实际读取的字节数（纯虚函数）。

.. method:: virtual std::int32_t write(const char *data, std::int32_t size)

    将缓冲区数据写入文件，返回实际写入的字节数（纯虚函数）。

.. method:: virtual void close()

    关闭文件（纯虚函数）。

.. method:: virtual std::int64_t size()

    获取文件大小（纯虚函数）。

.. method:: virtual std::string readall(bool *ok);

    读取文件全部内容，通过 ok 返回是否成功。

.. method:: std::string read(std::int32_t size)

    读取指定大小的数据，返回 std::string。

.. method:: std::int32_t write(const std::string &data)

    写入 std::string 数据。

.. method:: static std::shared_ptr<FileLike> open(const std::string &filepath, const std::string &mode = std::string())

    打开文件并返回 FileLike 实例。

.. method:: static std::shared_ptr<FileLike> bytes(const std::string &data)

    创建基于内存的 BytesIO 实例。

.. method:: static std::shared_ptr<FileLike> bytes(std::string *data)

    创建基于内存的 BytesIO 实例。

7.1.2 BytesIO
++++++++++++++
内存中的字节流，模拟文件操作。
    
.. method:: virtual std::int32_t read(char *data, std::int32_t size)

    从内存缓冲区读数据。

.. method:: virtual std::int32_t write(const char *data, std::int32_t size)

    从内存缓冲区写数据。

.. method:: virtual void close()

    暂无操作，内存流无需关闭

.. method:: virtual std::int64_t size()

    返回缓冲区大小

.. method:: virtual std::string readall(bool *ok)

    返回缓冲区全部内容

.. method:: std::string data()

    获取底层的std::string

7.1.3 PosixPath
++++++++++++++++
POSIX 路径处理类，用于跨平台规范化与操作文件路径。

.. method:: PosixPath operator/(const std::string &path)

    直接拼接路径，可能包含 .. 或 .（需手动处理安全）。

.. method:: PosixPath operator|(const std::string &path)

    自动过滤 .. 和 .，生成 规范化路径。

.. method:: bool isNull()

    判断文件是否为空

.. method:: bool isFile()

    判断是否为文件

.. method:: bool isDir()

    判断是否为目录

.. method:: bool isSymLink()

    判断是否为符号链接

.. method:: bool isAbsolute()

    判断是否为绝对路径

.. method:: bool isExecutable()

    判断是否为可执行文件

.. method:: bool isReadable() 

    判断文件是否可读

.. method:: bool isRelative()

    判断文件路径是否是相对的

.. method:: bool isRoot()

    判断文件是否指向根目录

.. method:: bool isWritable()

    判断文件是否可写

.. method:: bool exists()

    判断文件是否存在

.. method:: std::int64_t size()

    返回文件大小

.. method:: std::string path() 

    返回文件路径

.. method:: std::string parentDir() const

    返回父目录路径

.. method:: PosixPath parentPath() const

    返回父目录 PosixPath 对象

.. method:: std::string name() const

    返回文件名（不包含扩展名）

.. method:: std::string baseName() const

    返回文件名（不包含扩展名）

.. method:: std::string suffix() const

    返回文件最后一级扩展名

.. method:: std::string completeBaseName() const

    返回多级文件名

.. method:: std::string completeSuffix() const

    返回多级扩展名

.. method:: std::string toAbsolute() const

    转换为绝对路径

.. method:: std::string relativePath(const std::string &other) const

    返回相对路径

.. method:: std::string relativePath(const PosixPath &other) const

    返回相对路径

.. method:: bool isChildOf(const PosixPath &other) const

    判断是否为子路径

.. method:: bool hasChildOf(const PosixPath &other) const

    判断是否包含子路径

.. method:: std::int64_t createdMsecsSinceEpoch() const

    返回文件创建时间（Unix 毫秒时间戳）

.. method:: std::int64_t lastModifiedMsecsSinceEpoch() const

    返回最后修改时间（Unix 毫秒时间戳）

.. method:: std::int64_t lastReadMsecsSinceEpoch() const

    返回最后访问时间（Unix 毫秒时间戳）

.. method:: std::vector<std::string> listdir() const

    列出目录内容

.. method:: std::vector<PosixPath> children() const

    返回子项的 PosixPath 对象

.. method:: bool mkdir(bool createParents = false)

    创建目录；``createParents=true`` 时递归创建父目录。

.. method:: bool touch()

    更新文件访问/修改时间

.. method:: std::shared_ptr<FileLike> open(const std::string &mode = std::string()) const

    以指定模式打开路径，返回 ``FileLike``（如 ``"rw+"``）。

.. method:: std::string readall(bool *ok) const

    读取整个文件内容。

.. method:: static PosixPath cwd()

    获取当前工作目录的 PosixPath 路径

7.1.4 其他函数
+++++++++++++++
.. method:: std::pair<std::string, std::string> safeJoinPath(const std::string &parentDir, const std::string &subPath)

    规范化子路径（处理 ``.`` 和 ``..`` 等符号），安全地将子路径附加到父目录后
