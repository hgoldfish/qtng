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

.. method:: bool put(const T &e, std::uint32_t msecs)

与 ``put(const T &e)`` 相同，但最多等待 ``msecs`` 毫秒；超时返回 ``false``。
``SizedQueue`` 提供同样的重载（容量按元素大小计量）。

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

    在新线程执行函数并返回结果。若 ``func`` 本身带参数，可在其后继续传入。
    

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

    国际化域名（IDN）会先经 ``utils::toAce()`` 转为 Punycode（ACE）再查询解析器，因此包含非 ASCII 字符的主机名（如 ``"bücher.com"``、``"中文.com"``）可用。该转换为最小 IDNA 外壳：纯 ASCII 标签原样通过，非 ASCII 标签以 ``xn--`` ACE 前缀编码。**不**做 Unicode 归一化（NFKC）、非 ASCII 大小写折叠及 Bidi/Joining 检查，需要 ASCII 小写归一化的调用方应自行处理。若需独立校验任意 UTF-8 文本，可使用 ``utils::isValidUtf8()``。
    
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
KCP 协议的服务器实现。

.. method:: KcpServer(const HostAddress &serverAddress, std::uint16_t serverPort)
    :no-index:

    初始化KCP服务器，绑定到指定地址和端口，直接调用 ``BaseStreamServer`` 的构造函数，若未指定地址则默认绑定所有网络接口(HostAddress::Any)

.. method:: virtual std::shared_ptr<SocketLike> serverCreate()

    调用KcpSocket::createServer(),创建KCP服务器，底层通过KcpSocket类实现。此方法会初始化KCP会话，绑定到指定地址和端口，并设置默认参数（如MTU大小、窗口大小等）。

.. method:: virtual void processRequest(std::shared_ptr<SocketLike> request)

    接收客户端连接后，实例化用户定义的RequestHandler，将KCP会话封装为SocketLike对象传递给业务逻辑处理模块。

3. HTTP 客户端
--------------

``HttpSession`` 是支持 HTTP 1.0/1.1 与 HTTP/2 的客户端（HTTPS 经 ALPN 协商 ``h2``；不支持 Server Push，亦不提供 HTTP/2 服务端），具备自动 Cookie 管理和自动重定向功能。HTTP/2 路径支持多路复用、流控（connection/stream WINDOW_UPDATE）、``MAX_CONCURRENT_STREAMS``、HEADERS/CONTINUATION 分片、trailers 消费，以及 ``streamResponse`` 下流式读取。核心方法 ``HttpSession::send()`` 用于发送请求并解析响应，同时提供快捷方法如 ``get()``、 ``post()``、 ``head()`` 等实现单行代码发起 HTTP 请求。

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

.. method:: int webSocketErrorCode() const

    返回 ``HttpSession::ws(...)`` 最近一次握手失败的错误码。
    最近一次握手成功时返回 ``0``（``WebSocketConnection::NoError``）。
    若因 HTTP 响应状态码不是 101 而失败，返回该 HTTP 状态码。
    若因握手头/Accept 校验失败，返回 ``1002``（``WebSocketConnection::ProtocolError``）。
    若属于传输/请求错误且没有可用 HTTP 状态码，返回 ``-1``。

.. method:: std::string webSocketErrorReason() const

    返回 ``HttpSession::ws(...)`` 最近一次握手失败的错误原因文本。
    空字符串表示无错误。

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

    注意：``HttpSession`` 支持 HTTP 1.0、1.1 与 HTTP/2 客户端。HTTP/3 尚未实现。不支持 Server Push。

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

    验证 Upgrade: websocket 和 Connection 首部包含 "Upgrade" token（大小写不敏感）,计算并返回 Sec-WebSocket-Accept,标记连接升级为 WebSocket

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
提供消息摘要（哈希）功能，支持多种哈希算法，允许分块处理数据并生成摘要。支持MD4和MD5算法，Sha1, Sha224, Sha256, Sha384, Sha512一系列SHA系列算法以及Ripemd160, Whirlpool哈希算法。可选算法是否可用取决于链接的 OpenSSL/LibreSSL 构建（尤其是 Whirlpool；LibreSSL 4.0+ 已移除 Whirlpool，MD4/RIPEMD 在部分构建中也可能不可用）。若算法不可用，构造会失败，``result()`` 返回空字符串。

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

``PublicKey`` / ``PrivateKey`` 提供两层与“加密/解密”相关的 API。

**``encrypt()`` / ``decrypt()``：面向算法的简便接口**

实现常见的*保密*方向：公钥加密、私钥解密。内部通过对象持有的 ``EVP_PKEY_CTX`` 调用 OpenSSL 的 ``EVP_PKEY_encrypt`` / ``EVP_PKEY_decrypt``。对 RSA 密钥，二者是固定填充的快捷方式：始终使用 ``PKCS1_PADDING``（PKCS#1 v1.5 加密块 type 2），分别等价于 ``rsaPublicEncrypt(data, PKCS1_PADDING)`` 与 ``rsaPrivateDecrypt(data, PKCS1_PADDING)``。对其他支持非对称加密的密钥类型，填充与参数由 OpenSSL 默认行为决定。

若只需标准的公钥加密、且不需要选择填充方式，优先使用 ``encrypt()`` / ``decrypt()``。

**``rsaPublicEncrypt`` 等四个 ``rsa*`` 方法：RSA 专用、语义完整**

RSA 在数学上允许四种不同的原始运算（对应旧版 ``RSA_public_encrypt`` / ``RSA_private_decrypt`` / ``RSA_private_encrypt`` / ``RSA_public_decrypt``）。除常规公钥加密、私钥解密外，还存在*反向*运算——私钥“加密”、公钥“解密/恢复”——用于旧协议兼容、无摘要的原始 PKCS#1 块（与高层 ``sign()`` / ``verify()`` 不同），或从私钥运算结果中恢复明文。PKCS#1 对加密（type 2）与签名（type 1）使用不同的填充格式；部分场景还需要 OAEP 或无填充。

因此单独提供 RSA 专用接口：

- ``rsaPublicEncrypt`` / ``rsaPrivateDecrypt``：保密方向。在 ``PKCS1_PADDING`` 下与 ``encrypt()`` / ``decrypt()`` 相同，但可选择 ``PKCS1_OAEP_PADDING``、``NO_PADDING`` 等填充。
- ``rsaPrivateEncrypt`` / ``rsaPublicDecrypt``：反向方向，分别基于 ``EVP_PKEY_sign`` / ``EVP_PKEY_verify_recover``（不计算摘要）。二者成对使用，不能替代 ``encrypt()`` / ``decrypt()``。

这些方法仅适用于 RSA；对 DSA/EC 密钥调用会失败。

对照一览：

- ``encrypt()`` → 公钥、保密方向，RSA 上固定 ``PKCS1_PADDING``
- ``decrypt()`` → 私钥、保密方向，RSA 上固定 ``PKCS1_PADDING``
- ``rsaPublicEncrypt`` / ``rsaPrivateDecrypt`` → 与上同向，填充可配置
- ``rsaPrivateEncrypt`` / ``rsaPublicDecrypt`` → 反向运算，用于旧版签名/恢复，不是保密加密

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

.. method:: std::string encrypt(const std::string &data) const

    使用对象内的 ``EVP_PKEY_CTX`` 做公钥加密（``EVP_PKEY_encrypt``）。RSA 上固定 ``PKCS1_PADDING``（PKCS#1 v1.5 type 2），与默认的 ``rsaPublicEncrypt`` 语义相同。

.. method:: std::string rsaPublicEncrypt(const std::string &data,RsaPadding padding = PKCS1_PADDING)
    :no-index:

    使用 RSA 公钥加密（``EVP_PKEY_encrypt``），复用对象内的 ``EVP_PKEY_CTX``。``PKCS1_PADDING`` 兼容性最好（默认）；``PKCS1_OAEP_PADDING`` 更安全，推荐新协议使用；``NO_PADDING`` 需自行处理填充。

.. method:: std::string rsaPublicDecrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING)
    :no-index:

    使用 RSA 公钥做原始解密/恢复（``EVP_PKEY_verify_recover``），复用对象内的 ``EVP_PKEY_CTX``。对应旧版 ``RSA_public_decrypt``，以及私钥侧的 ``rsaPrivateEncrypt``。``PKCS1_PADDING`` 是签名用的 PKCS#1 v1.5 **type 1**（``00 01 FF..00``），不是加密用的 type 2；未设置 ``signature_md``，因此不会封装或剥离 DigestInfo。支持 ``PKCS1_PADDING``（默认）与 ``NO_PADDING``。
    

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

.. method:: std::string decrypt(const std::string &data) const

    使用私钥解密数据。初始化解密上下文：EVP_PKEY_decrypt_init。RSA 上固定 ``PKCS1_PADDING``，对应 ``encrypt()``。

.. method:: std::string rsaPrivateEncrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING) const
    :no-index:

    使用 RSA 私钥做原始加密（``EVP_PKEY_sign``，无摘要），复用对象内的 ``EVP_PKEY_CTX``。对应旧版 ``RSA_private_encrypt``，以及公钥侧的 ``rsaPublicDecrypt``。支持 ``PKCS1_PADDING``（默认）与 ``NO_PADDING``。

.. method:: std::string rsaPrivateDecrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING) const
    :no-index:

    使用 RSA 私钥解密（``EVP_PKEY_decrypt``），复用对象内的 ``EVP_PKEY_CTX``。支持 ``PKCS1_PADDING``（默认）、``PKCS1_OAEP_PADDING`` 与 ``NO_PADDING``。

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

    在 CertificatePrivate::init 中解析 X509_get0_notBefore 和 X509_get0_notAfter。

.. method:: qtng::utils::DateTime expiryDate() const

    在 CertificatePrivate::init 中解析 X509_get0_notBefore 和 X509_get0_notAfter。

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

5.6 Noise 协议
^^^^^^^^^^^^^^

提供 Noise Protocol Framework 的精简实现，支持 ``XX`` / ``PSK_XX`` / ``IK`` 握手，AEAD 可选
``ChaCha20-Poly1305``（默认，协议名 ``*_25519_ChaChaPoly_SHA256``）或 ``AES-256-GCM``
（``*_25519_AESGCM_SHA256``；头文件 ``qtng/noise.h``，经 ``Aead`` 实现）。

* ``NoiseKey`` — X25519 密钥对生成、从私钥导入、DH。
* ``NoiseCipherState`` — 通过 ``Aead`` 做 AEAD（``ChaCha20Poly1305`` 或 ``Aes256Gcm``），Noise 风格 12 字节 nonce
  （4 字节零 + 8 字节计数器：ChaChaPoly 小端，AESGCM 大端）。``split()`` 后 send / recv 各持独立计数器。
  ``encryptWithAd`` 始终从计数器取号并在成功后自增（无显式 nonce 加密重载）；``outNonce`` 返回所用值供
  线上包头（``NoiseDatagram`` / WireGuard 式 UDP：发送端单调递增、counter 写入包内）。两参数
  ``decryptWithAd`` 顺序使用并递增计数器（握手 / ``NoiseSocket``）。重载
  ``decryptWithAd(ad, ciphertext, nonce)`` 用包内 nonce 解密，
  **不**推进该计数器（UDP 接收 / 乱序）；本层不做防重放。
  传输 nonce 范围为 ``0 .. MaxNonce``（``2^64-2``）；``n`` 超过 ``MaxNonce`` 后
  ``EncryptWithAd`` / ``DecryptWithAd`` 失败。``setNonce(n)`` 仅接受 ``n <= MaxNonce``，
  超出时忽略并记录 warning。``2^64-1`` 保留给 ``rekey()``
  （``ENCRYPT(k, 2^64-1, zerolen, zeros)``），不用作传输 nonce。
  ``rekey()`` 按 Noise 规范替换密钥，不重置 nonce。
* ``NoiseHandshakeState`` — XX / PSK_XX / IK 握手状态机；``initialize()`` 支持
  prologue（始终 ``MixHash``，含空 prologue）与 ``Aead::Algorithm``（默认 ChaCha20-Poly1305；
  仅接受 32 字节密钥的 ``ChaCha20Poly1305`` / ``Aes256Gcm``，拒绝 AES-128-GCM）；PSK 按 psk0 做 ``MixKeyAndHash``；
  完成后调用 ``split()`` 得到传输层收发密码状态；``handshakeHash()`` 可用于通道绑定。
  IK 发起方必须提供对端静态公钥；若预先提供了期望的远端静态公钥且握手中解密出的不符，握手失败。
* ``NoiseDatagram`` — 同一套握手与传输，但是编解码器：调用方自己持有套接字（UDP 等），
  只把报文字节送进取出。``writeHandshake`` / ``readHandshake`` 处理 Noise 握手消息；
  完成后 ``encrypt`` / ``decrypt`` 使用 ``[8 字节大端 nonce][密文||tag]``。接收方用包内
  nonce 做 AEAD（允许乱序到达），再按 WireGuard / RFC 6479 的 8192 位滑动窗口拒绝重放
  和已滑出窗口的 nonce。未通过认证的包不会改窗口。收发在 WireGuard 的
  ``REJECT_AFTER_MESSAGES``（``2^64 - 8129``）处停止。
  本类不调用 ``Socket``、``DatagramLink`` 或任何其它 I/O 类型。会话对象不可拷贝：拷贝会
  复制 nonce 计数器，导致密钥流重用。可以移动，以便把已完成握手的会话交给另一个对象。
* ``NoiseSocket`` — 与 ``SslSocket`` 同类：在可靠流（TCP 等）上跑 Noise。握手由
  ``NoiseHandshakeState`` 完成；传输层用顺序 ``NoiseCipherState``（自增 nonce，线上不传 nonce，
  不做防重放）。每个 Noise 消息在 backend 上加 2 字节大端长度前缀（传输为 ``密文||tag``，
  握手为原始 Noise 握手字节）。``send`` / ``sendall`` / ``recv`` / ``recvall`` 为字节流接口。
  它本身不是 ``SocketLike``；需要 ``SocketLike`` 时用 ``asSocketLike()`` 包装。

.. code-block:: c++

    NoiseKey alice = NoiseKey::generate();
    NoiseKey bob = NoiseKey::generate();
    auto client = make_shared<NoiseSocket>(asSocketLike(tcpClient));
    auto server = make_shared<NoiseSocket>(asSocketLike(tcpServer));
    client->initialize(NoisePattern::XX, NoiseRole::Initiator, alice);
    server->initialize(NoisePattern::XX, NoiseRole::Responder, bob);
    // 两端分别调用 handshake() 后即可 sendall / recv
    // 需要 SocketLike 时：asSocketLike(client)

    NoiseDatagram udpClient;
    NoiseDatagram udpServer;
    udpClient.initialize(NoisePattern::XX, NoiseRole::Initiator, alice);
    udpServer.initialize(NoisePattern::XX, NoiseRole::Responder, bob);
    udpClient.writeHandshake("hello", &packet);
    udpSocket.sendto(packet, peerAddr, peerPort);  // I/O 由调用方完成
    packet = udpSocket.recvfrom(65535, &from, &fromPort);
    udpServer.readHandshake(packet, &payload);
    // 握手完成后：sendto(udpClient.encrypt(plain), ...)

5.7 AEAD 与 HKDF 辅助接口
^^^^^^^^^^^^^^^^^^^^^^^^^^

头文件 ``qtng/aead.h`` 提供可复用的加密原语（供 QUIC 使用，也可直接给应用调用）：

* ``Aead`` — AES-128-GCM、AES-256-GCM、ChaCha20-Poly1305 的 seal/open（含 AAD）；
  密文布局为 ``ciphertext || tag``。``NoiseCipherState`` 使用其中的 AES-256-GCM 与
  ChaCha20-Poly1305（Noise 规范要求 32 字节密钥，不使用 AES-128-GCM）。
* ``hkdfExtract`` / ``hkdfExpand`` / ``hkdf`` — RFC 5869 HKDF。
* ``hkdfExpandLabel`` — TLS 1.3 / QUIC 的 ``HKDF-Expand-Label``（带 ``tls13 `` 前缀）。
* ``aesEcbEncryptBlock`` — 单块 AES-ECB，用于 QUIC header protection mask。

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

CMake 按如下顺序选择 TLS/加密库：

* 若 ``QTNG_DISABLE_CRYPTO=ON``，跳过加密并定义 ``QTNG_NO_CRYPTO``。
* 否则若已初始化含可用源码的 ``3rdparty/libressl`` git 子模块，自动构建并链接内置 LibreSSL。
* 否则调用 ``find_package(OpenSSL 1.1.1 QUIET)``；找到则链接系统 OpenSSL（Noise 需要 1.1.1+ 的 X25519 raw key API）。
* 若 LibreSSL 与 OpenSSL 皆不可用，CMake 发出警告并继续构建（``QTNG_NO_CRYPTO``）：不编译 TLS/SSL、Noise、AEAD、QUIC。``MessageDigest`` 仍通过软件实现支持 MD5/SHA-1/SHA-256。

不完善协议模块（默认开启）：

* ``QTNG_WITH_HTTP2`` — HTTP/2 客户端与 HPACK
* ``QTNG_WITH_QUIC`` — QUICv1 传输层 MVP（无加密时强制关闭）
* ``QTNG_WITH_HTTP3`` — HTTP/3 占位实现（无 QUIC 时强制关闭）

未使用内置 LibreSSL 时，Debian/Ubuntu 可安装 ``libssl-dev`` 开发包。

OpenSSL 和 LibreSSL 会在首次使用时自行初始化，并在整个进程生命周期内保持可用。应用程序无需调用 qtng 专用的初始化或清理函数。


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

7. 其他辅助类
-------------
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

    安全地将 ``subPath`` 拼到 ``parentDir`` 下。``subPath`` 开头的分隔符会被去掉，
    按相对父目录处理（与 HTTP URL 路径一致）。含 ``..`` 的路径会被拒绝。
    失败时返回的 pair 两个字符串都为空。

7.2 Bencode
^^^^^^^^^^^

BitTorrent **bencode** 是一种紧凑的二进制序列化格式，贯穿整个 BitTorrent 生态：
``.torrent`` 文件、HTTP tracker 响应、扩展协议载荷，以及全部 DHT（BEP-5）UDP 报文。
qtng 在 ``qtng/bencode.h`` 中提供与 ``MsgPackStream`` 对齐的两套 API：

* ``BencodeStream`` — 基于 ``FileLike`` 或 ``std::string`` 的流式编解码
* ``Bencode`` — 内存中的值树（整数 / 字符串 / 列表 / 字典）

DHT（``DhtNode``）与 BitTorrent 下载栈（§8.4 ``TorrentSession``）共用本模块。

7.2.1 线格式
++++++++++++

Bencode 只有四种类型：

=========== =================================================== =======================
类型        编码                                                示例
=========== =================================================== =======================
整数        ``i`` *十进制* ``e``                                ``i42e``、``i-3e``、``i0e``
字节串      *长度* ``:`` *原始字节*                             ``4:spam``、``3:\\x00\\x01\\xff``
列表        ``l`` *元素…* ``e``                                 ``l4:spami42ee``
字典        ``d`` *键* *值* … ``e``                             ``d3:bar4:spam3:fooi42ee``
=========== =================================================== =======================

互通时需注意：

* 整数**不能有前导零**（``0`` 本身除外）；``-0`` 非法。
* 字符串是**任意字节**，不是 UTF-8。节点 ID、infohash、``pieces`` 位图都以字符串编码。
* 字典的**键必须是字节串**；*编码*时按**原始字节序**（字典序）输出。规范键序是
  ``info`` 字典的硬性要求：``infohash = SHA1(bencode(info))``。
* *解码*时 qtng 接受未排序键（部分实现对端较松）；``encode()`` / ``std::map`` 始终产出有序结果。

7.2.2 Stream 与 Value 的选择
++++++++++++++++++++++++++++

* 模式在编译期已知时用 ``BencodeStream``（类型化 ``operator<<`` / ``>>`` 写入
  ``std::map``、``std::vector`` 等），或从文件增量读取。
* 结构动态时用 ``Bencode``（DHT 可选字段、未知扩展键）。组装
  ``std::map<std::string, Bencode>`` / ``std::vector<Bencode>``，用 ``Bencode``
  构造函数包装后调用 ``encode()``。

7.2.3 BencodeStream
+++++++++++++++++++

.. class:: BencodeStream

    .. method:: BencodeStream()
    .. method:: BencodeStream(FileLike *d)
    .. method:: BencodeStream(std::string *a, bool writeMode = false)
    .. method:: BencodeStream(const std::string &a)

        构造空流、绑定设备，或以字符串为后端。
        ``writeMode=true`` 向 ``*a`` 追加；const 字符串为只读输入。

    .. method:: void setDevice(FileLike *d)
    .. method:: FileLike *device() const
    .. method:: std::string data() const

        流持有字符串设备时返回当前缓冲。

    .. method:: bool atEnd() const
    .. method:: Status status() const
    .. method:: bool isOk() const
    .. method:: void resetStatus()
    .. method:: void setStatus(Status status)

        ``Status`` 为 ``Ok``、``ReadPastEnd``、``ReadCorruptData`` 或 ``WriteFailed``。

    .. method:: void setLengthLimit(std::uint32_t limit)
    .. method:: std::uint32_t lengthLimit() const

        解码字符串 / 嵌套预算上限（默认 ``16 MiB``），防止恶意包无限分配。

    .. method:: BencodeStream &operator>>(std::int64_t &i)
    .. method:: BencodeStream &operator>>(std::string &str)
    .. method:: BencodeStream &operator>>(Bencode &v)
    .. method:: bool readBytes(char *data, std::int64_t len)
    .. method:: bool peekByte(std::uint8_t *b) const

        ``peekByte`` 对任意 ``FileLike`` 最多预读一个字节，后续读取会消费同一字节
        （用于判断下一类型标签）。

    .. method:: bool readArrayHeader(std::uint32_t &len)
    .. method:: bool readMapHeader(std::uint32_t &len)
    .. method:: bool readArrayEnd()
    .. method:: bool readMapEnd()
    .. method:: bool peekContainerEnd() const

        容器头辅助函数对齐 ``MsgPackStream``（``Array`` / ``Map`` 命名及 ``len`` 参数）。
        Bencode **没有长度前缀**，因此 ``read*Header`` 将 ``len`` 设为 ``UINT32_MAX``
        （不定长）。用 ``peekContainerEnd()`` 迭代，再以 ``readArrayEnd()`` /
        ``readMapEnd()`` 结束。

    .. method:: BencodeStream &operator<<(std::int64_t i)
    .. method:: BencodeStream &operator<<(const std::string &str)
    .. method:: BencodeStream &operator<<(const Bencode &v)
    .. method:: bool writeBytes(const char *data, std::int64_t len)
    .. method:: bool writeArrayHeader(std::uint32_t len)
    .. method:: bool writeMapHeader(std::uint32_t len)
    .. method:: bool writeArrayEnd()
    .. method:: bool writeMapEnd()

        ``write*Header`` 接受 ``len`` 仅为 API 一致，实际只写入 ``l`` / ``d`` 标记；
        写完后必须配对 ``writeArrayEnd()`` / ``writeMapEnd()``。

    模板 ``operator<<`` / ``operator>>`` 还支持 ``std::vector``、``std::list``、
    ``std::set``、``std::unordered_set``（编码为列表）以及 ``std::map``、
    ``std::unordered_map``（编码为字典）。需要规范编码（如 infohash）时请使用
    ``std::map``。``std::unordered_map`` **不保证**键序。

7.2.4 Bencode 值树
++++++++++++++++++

.. class:: Bencode

    动态 bencode 值。列表/字典内容通过 ``toList()`` / ``toMap()`` 取得
    ``std::vector<Bencode>`` / ``std::map<std::string, Bencode>``；在标准容器上组装后，
    再用 ``Bencode`` 构造函数包装。

    .. method:: Bencode()
    .. method:: Bencode(std::int64_t i)
    .. method:: Bencode(const std::string &s)
    .. method:: Bencode(const char *s)
    .. method:: Bencode(const std::vector<Bencode> &list)
    .. method:: Bencode(std::vector<Bencode> &&list)
    .. method:: Bencode(const std::map<std::string, Bencode> &dict)
    .. method:: Bencode(std::map<std::string, Bencode> &&dict)

    .. method:: static Bencode dict()
    .. method:: static Bencode list()

        空容器。

    .. method:: Type type() const
    .. method:: bool isValid() const
    .. method:: bool isInteger() const
    .. method:: bool isString() const
    .. method:: bool isList() const
    .. method:: bool isDict() const

    .. method:: std::int64_t toInteger(std::int64_t defaultValue = 0) const
    .. method:: std::string toString() const
    .. method:: const std::vector<Bencode> &toList() const
    .. method:: const std::map<std::string, Bencode> &toMap() const

        类型不匹配时返回空 / 默认值。

    .. method:: std::string encode() const

        序列化，字典键已排序。

    .. method:: static Bencode decode(const std::string &data, std::string *error = nullptr, std::uint32_t lengthLimit = 16 * 1024 * 1024)
    .. method:: static Bencode decode(FileLike *device, std::string *error = nullptr, std::uint32_t lengthLimit = 16 * 1024 * 1024)

        解析完整值。失败返回无效 ``Bencode``，并可写入 ``*error``。
        值之后若有多余字节视为错误。

7.2.5 示例
++++++++++

类型化 map 的流式往返::

    qtng::BencodeStream stream;
    std::map<std::string, std::int64_t> payload{{"spam", 42}};
    stream << payload;
    std::string wire = stream.data();  // "d4:spami42ee"

    qtng::Bencode back = qtng::Bencode::decode(wire);
    std::int64_t n = back.toMap().at("spam").toInteger();

    qtng::BencodeStream parsed(wire);
    std::map<std::string, std::int64_t> roundTrip;
    parsed >> roundTrip;

用动态树构造 DHT 风格查询::

    std::map<std::string, qtng::Bencode> args;
    args["id"] = qtng::Bencode(std::string(20, '\\x01'));
    std::map<std::string, qtng::Bencode> msg;
    msg["a"] = qtng::Bencode(args);
    msg["q"] = "ping";
    msg["t"] = "aa";
    msg["y"] = "q";
    std::string encoded = qtng::Bencode(std::move(msg)).encode();

手动列表/字典标记（MsgPack 风格控制流）::

    qtng::BencodeStream s;
    s.writeMapHeader(0);
    s << std::string("foo") << std::int64_t(1);
    s.writeMapEnd();
    // "d3:fooi1ee"

7.3 MQTT 客户端
^^^^^^^^^^^^^^^

``qtng/mqtt.h`` 在 ``SocketLike``（TCP 或 TLS）之上提供 **MQTT 3.1.1** 客户端。
API 对齐 ``WebSocketConnection``：协程阻塞式 ``publish`` / ``subscribe`` / ``recv``，
内部使用收发与 keepalive 协程。

本版本支持：

* QoS 0 / 1 / 2
* Clean Session、Will、用户名密码
* Keep Alive ``PINGREQ`` / ``PINGRESP``
* ``MqttClient::connect``（TCP，默认端口 1883）与 ``MqttClient::connectTls``（TLS，默认 8883）

不包含：broker/服务端、MQTT 5.0、MQTT over WebSocket。

明文示例::

    #include "qtng/mqtt.h"
    #include <iostream>

    int main()
    {
        qtng::MqttConfiguration config;
        config.setClientId("sensor-1");
        config.setKeepAlive(60);

        std::shared_ptr<qtng::MqttClient> client =
            qtng::MqttClient::connect("127.0.0.1", 1883, config);
        if (!client) {
            return 1;
        }

        client->subscribe("sensors/#", qtng::MqttQos::AtLeastOnce);
        client->publish(qtng::MqttMessage("sensors/temp", "22.5", qtng::MqttQos::AtLeastOnce));

        qtng::MqttMessage msg = client->recv();
        std::cout << msg.topic << ": " << msg.payload << std::endl;
        client->disconnect();
        return 0;
    }

注入已连接的 ``SocketLike``（代理、自定义 TLS 等）::

    auto raw = qtng::asSocketLike(qtng::Socket::createConnection("broker.local", 1883));
    qtng::MqttClient client(raw, config);
    if (!client.isConnected()) {
        // 查看 client.error() / errorString()
    }

.. method:: std::shared_ptr<MqttClient> MqttClient::connect(const std::string &host, std::uint16_t port = 1883, const MqttConfiguration &config = MqttConfiguration())

    建立 TCP 连接，发送 ``CONNECT`` 并等待 ``CONNACK``。失败时返回空 ``shared_ptr``。

.. method:: std::shared_ptr<MqttClient> MqttClient::connectTls(const std::string &host, std::uint16_t port = 8883, const MqttConfiguration &config = MqttConfiguration(), const SslConfiguration &ssl = SslConfiguration())

    与 ``connect`` 相同，内部使用 ``SslSocket::createConnection``。

.. method:: explicit MqttClient::MqttClient(std::shared_ptr<SocketLike> connection, const MqttConfiguration &config = MqttConfiguration())

    在已有字节流上完成 MQTT 握手。之后检查 ``isConnected()``。

.. method:: bool MqttClient::publish(const MqttMessage &msg)

    发布并等待对应 QoS 握手完成（QoS 0：写出后即成功）。

.. method:: bool MqttClient::publishAsync(const MqttMessage &msg)

    仅将发布请求入队，不等待完成。

.. method:: bool MqttClient::subscribe(const std::string &topic, MqttQos qos = MqttQos::AtMostOnce)
.. method:: bool MqttClient::unsubscribe(const std::string &topic)

    订阅 / 取消订阅，并等待 ``SUBACK`` / ``UNSUBACK``。

.. method:: MqttMessage MqttClient::recv()

    阻塞直到收到下一条入站 ``PUBLISH``（已完成 QoS 处理）。连接关闭时返回空消息。

.. method:: void MqttClient::disconnect()

    发送 ``DISCONNECT`` 后关闭连接。

.. method:: void MqttClient::abort()

    不发送优雅 ``DISCONNECT``，直接中止。

``MqttConfiguration`` 主要项：``clientId``、``cleanSession``（默认 true）、
``keepAlive``（秒）、``username`` / ``password``、``setWill(MqttMessage)``、
队列容量、``maxPacketSize``、``connectTimeout``。


8. 高级编程
-----------

本章介绍通常只在特殊场景中使用的高级功能。

8.0 DataChannel
^^^^^^^^^^^^^^^

``DataChannel`` / ``SocketChannel`` / ``VirtualChannel`` 在一条连接上多路复用逻辑通道。
对端创建、尚未被 ``takeChannel()`` 取走的通道会进入 pending 队列。

.. method:: void DataChannel::setMaxPendingChannels(std::uint32_t count)
.. method:: std::uint32_t DataChannel::maxPendingChannels() const

    未取走的 pending 通道数量上限。超出时丢弃最旧的 pending 通道，并以
    ``PendingChannelLimitError`` 中止。``0`` 表示不限制。默认 ``8``。

.. method:: void DataChannel::setSendingTimeout(float timeout)
.. method:: float DataChannel::sendingTimeout() const

    ``goThrough`` 等待与阻塞式 ``sendPacket`` 的超时（秒）。
    ``timeout <= 0`` 表示一直等待。默认 ``30`` 秒。

8.1 MultiStream
^^^^^^^^^^^^^^^

``MultiStream`` 是基于一条 ``SocketLike`` 连接的扁平多路复用工具。与 ``VirtualChannel`` 不同，它只有两层：

* ``MultiStreamMaster`` — 持有物理连接，只负责管理 Slave（不收发业务数据）。
* ``MultiStreamSlave`` — 一条逻辑流；支持包接口，并可通过 ``asSocketLike()`` 适配为字节流。

连接两端各自持有一个 ``MultiStreamMaster``。任意一端都可调用 ``makeSlave()``，对端用 ``takeSlave()`` 接受。Slave 不能再创建下层流。

**队头阻塞（HOL）。** MultiStream 保证的是*逻辑*多路（独立流号、每流队列、信贷流控与优先级调度），
**不**保证跨流独立时延：所有 Slave 共享底层连接上的有序字节流。TCP 丢包或延迟会卡住所有流。选型建议：

* **TCP**（或 TLS over TCP）：需要可靠有序、能容忍 HOL 时——批量传输、控制面、或并发流较少的场景。
* **KCP**（或其他抗丢包传输）：多流共享同一路径，且某一流的丢包/延迟不能拖死其它流时——交互或时延敏感场景。

发送调度：命令流优先级最高；业务流按加权轮询，``priority()`` 越大优先级越高（默认 ``0``，WRR 权重为
``priority + 1``）。同一批会尽量合并多个包，直到更高优先级工作排空，或批量大小达到 ``maxPacketSize()``。

线上帧格式（大端）::

    [payloadSize: u32][streamNumber: u32][payload]

流号 ``0`` 为命令通道。业务数据使用双编号空间分配流号
（``MultiStreamPositivePole`` 从 ``1`` 递增；``MultiStreamNegativePole`` 从 ``0xffffffff`` 递减），因此双方可同时创建 Slave 而不会冲突。

流 ``0`` 上的命令 payload（大端）::

    MAKE_SLAVE / SLAVE_MADE  [u8=1|2][u32 streamNumber][u32 initialWindow]
    RESET                   [u8=3][u32 streamNumber][u32 resetCode]
    WINDOW_UPDATE           [u8=4][u32 streamNumber][u32 creditIncrement]
    KEEPALIVE               [u8=6]

``resetCode``：``0`` 正常关闭，``1`` abort，``2`` 协议错误，``3`` 拒绝。
流控为信贷模型：``MAKE``/``SLAVE_MADE`` 通告初始接收窗口；``recvPacket()`` 通过 ``WINDOW_UPDATE`` 归还信贷。

.. code-block:: c++
    :caption: 双向创建 Slave 并交换数据包

    MultiStreamMaster local(clientSocket, MultiStreamPositivePole);
    MultiStreamMaster remote(serverSocket, MultiStreamNegativePole);

    // 对端接受传入的 Slave。
    std::shared_ptr<Coroutine> acceptor = Coroutine::spawn([&] {
        std::shared_ptr<MultiStreamSlave> incoming = remote.takeSlave();
        std::string packet = incoming->recvPacket();
        incoming->sendPacket("pong");
    });

    std::shared_ptr<MultiStreamSlave> outgoing = local.makeSlave();
    outgoing->sendPacket("ping");
    std::string reply = outgoing->recvPacket();  // "pong"

    // 字节流用法：
    std::shared_ptr<SocketLike> stream = asSocketLike(outgoing);
    stream->sendall("hello");

.. class:: MultiStreamMaster

    使用已连接的 ``Socket``、``SslSocket``、``KcpSocket`` 或 ``SocketLike``，并指定 ``MultiStreamPole`` 构造。

.. method:: std::shared_ptr<MultiStreamSlave> MultiStreamMaster::makeSlave()

    立即在本地创建 Slave 并通知对端。不等待对端确认。

.. method:: std::shared_ptr<MultiStreamSlave> MultiStreamMaster::takeSlave()

    阻塞直到对端创建 Slave 后返回。若 Master 已断开则返回空指针。

.. method:: std::shared_ptr<MultiStreamSlave> MultiStreamMaster::takeSlave(std::uint32_t streamNumber)

    非阻塞：返回指定流号的 pending Slave；若不在 pending 队列中则返回空指针。

.. method:: void MultiStreamMaster::setMaxPacketSize(std::uint32_t size)
.. method:: void MultiStreamMaster::setPayloadSizeHint(std::uint32_t payloadSizeHint)
.. method:: void MultiStreamMaster::setSlaveReceivingCapacity(std::uint32_t bytes)
.. method:: std::uint32_t MultiStreamMaster::slaveReceivingCapacity() const
.. method:: void MultiStreamMaster::setSlaveSendingCapacity(std::uint32_t bytes)
.. method:: std::uint32_t MultiStreamMaster::slaveSendingCapacity() const
.. method:: void MultiStreamMaster::setKeepaliveTimeout(float timeout)
.. method:: void MultiStreamMaster::setKeepaliveInterval(float keepaliveInterval)

    配置分帧、新建 Slave 的默认收发队列容量，以及保活参数。保活仅在 Master 连接上运行。
    容量变更只影响之后新建的 Slave。``slaveReceivingCapacity`` 同时作为 ``MAKE``/``SLAVE_MADE`` 通告的初始窗口。

.. method:: void MultiStreamMaster::abort()

    关闭 Master。所有 Slave 变为 broken，正在等待的 ``takeSlave()`` 会以空指针唤醒。

.. class:: MultiStreamSlave

    只能通过 ``makeSlave()`` / ``takeSlave()`` 获得。没有 ``makeSlave`` API（强制两层结构）。

.. method:: bool MultiStreamSlave::sendPacket(const std::string &packet, bool waitSent = true)
.. method:: bool MultiStreamSlave::sendPacket(std::string &&packet, bool waitSent = true)
.. method:: bool MultiStreamSlave::sendPacketAsync(const std::string &packet)
.. method:: bool MultiStreamSlave::sendPacketAsync(std::string &&packet)
.. method:: std::string MultiStreamSlave::recvPacket()

    保留消息边界的包接口。``recvPacket()`` 返回空字符串表示 Slave 已关闭或出错。
    传入临时字符串或使用 ``std::move(packet)`` 会选择右值重载，将 payload 的缓冲区直接转移到发送队列，
    避免复制字节内容。左值重载维持原有的不消费调用方数据语义；队列内部传递和接收返回同样使用移动语义。
    ``sendPacket`` 会等待发送信贷；``sendPacketAsync`` 在信贷不足时立即失败。
    ``recvPacket`` 通过对端 ``WINDOW_UPDATE`` 归还信贷。

.. method:: void MultiStreamSlave::setReceivingCapacity(std::uint32_t bytes)
.. method:: std::uint32_t MultiStreamSlave::receivingCapacity() const

    单条 Slave 的接收队列容量。增大容量时会发送 ``WINDOW_UPDATE`` 通告增量。

.. method:: void MultiStreamSlave::setPriority(int priority)
.. method:: int MultiStreamSlave::priority() const

    加权轮询的调度优先级。数值越大越优先；默认 ``0``。有效 WRR 权重为 ``priority + 1``
    （负优先级仍至少为权重 ``1``）。

.. method:: MultiStreamResetCode MultiStreamSlave::resetCode() const

    当 ``error()`` 为 ``RemotePeerClosedError`` 时，表示对端 RESET 原因：
    ``MultiStreamResetNormalClose``、``MultiStreamResetAbort``、``MultiStreamResetProtocolError``、
    ``MultiStreamResetRefused``。可用于上层重试策略（例如 abort 可重试，正常关闭不重试）。

.. method:: void MultiStreamSlave::close()

    优雅关闭。进入 ``closing`` 状态（拒绝新的发送），先排空本 Slave 已入队的发送数据，再发送 ``RESET(NormalClose)``，
    保证对端先收完数据再看到关闭。``close()`` 会阻塞直到 RESET 写入底层连接。
    本地接收队列会被清空，阻塞中的 ``recvPacket()`` 会以空结果唤醒；待发送数据不会被丢弃，
    在线路顺序中仍位于 RESET 之前。

.. method:: void MultiStreamSlave::abort()

    快速拆除。丢弃本 Slave 待发送数据并清空接收队列，然后立即发送 ``RESET(Abort)``。
    适用于不需要等待在途数据、直接丢弃该流的场景。

.. method:: bool MultiStreamSlave::isClosing() const

    若正在执行优雅 ``close()``（发送队列尚未排空）则返回 true。

.. function:: std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<MultiStreamSlave> slave)

    将 Slave 适配为 ``SocketLike`` 字节流（发送按 ``maxPayloadSize`` 自动分包；接收将多个包拼接）。

8.2 KcpStream 与 DatagramLink
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``KcpStream``（私有头 ``qtng/private/kcp.h``）是传输无关的 KCP 会话核心：连接管理、
listen/connect/accept、keepalive、发送队列水位与 Mode。它只做可靠字节流，不实现 ``SocketLike``，
也不暴露 bind / 组播 / DNS / 原始 UDP 收发。

底层通过精简的 ``DatagramLink``（``recvfrom`` / ``sendto`` / ``close`` / ``abort``）收发报文；
对端身份用 ``DatagramPath`` 表示——它只是不透明路径键（``key()``），与 IP/端口无关，因此同一套
会话逻辑可以跑在 UDP、ICMP 或其它自定义报文通道上。

``Mode`` 包括 ``LargeDelayInternet``、``Internet``、``FastInternet``、``Ethernet``、
``Loopback`` 与 ``AsymmetricInternet``（``ikcp_nodelay(..., resend=1, nc=0)``，适合非对称链路）。
``KcpSocket::createConnection()`` 默认使用 ``AsymmetricInternet``；服务端通常仍用 ``Internet``。

优雅 ``close()`` 最多等待 3 秒排空发送队列。``CLOSE`` 控制包仅在源 ``DatagramPath`` 与已记录
对端路径一致时生效；其它路径发来的伪造关闭会被忽略。

日常 UDP 场景请使用公开的 ``KcpSocket``（``udp.h``）。``KcpSocket`` 内部用私有
``UdpDatagramLink`` / ``UdpDatagramPath`` 把 ``HostAddress``+端口映射为 ``DatagramPath``。
``asSocketLike`` 仅针对 ``KcpSocket``，不适用于 ``KcpStream``。

线协议与历史 ``KcpSocket`` 实现兼容（帧类型 DATA / MULTIPATH / CLOSE / KEEPALIVE）。

线成帧统一为：

* DatagramLink 投递 ``[1 字节 cmd][payload...]``，不含前导 ikcp ``conv``。
  接收缓冲区在线载荷前预留 4 字节 headroom，使原生 ikcp 命令（0x51–0x54）
  无需拷贝即可交给 ``ikcp_input``（conv 置零）；旧版 ``DATA``（0x01）则原地
  清零叠加的 conversation 字段。
* KcpStream 控制包（CREATE_MULTIPATH / CLOSE / KEEPALIVE）始终在字节 1–4 携带
  4 字节大端 ``sessionId``。构造时传入，或使用 ``setSessionId()``。
* ``protocolVersion`` 选择 ikcp 输出格式：版本 1（``KcpSocket`` 默认）将段包装为
  ``DATA`` 并在 conv 上叠加 ``sessionId``；版本 2（``SlowSocket`` 默认）剥掉
  4 字节 ``conv``，直接发送 ``[cmd][payload...]``。对端按入站线格式协商为
  ``min(local, peer)``。

``wrapKcpStreamAsSocket`` 为公开 API：可将任意 ``DatagramLink`` 上的 ``KcpStream``
包装为 ``KcpSocket``；底层非 UDP 时，仅 UDP 相关方法会失败或空操作。
包装不会在共享链路上安装 UDP 接收 ``filter``（``accept`` 得到的 slave 与 master
共用 ``UdpDatagramLink``）；若需过滤报文，请在持有套接字的监听端 ``KcpSocket``
上重写 ``filter``。

8.2.1 UtpStream 与 UtpSocket
+++++++++++++++++++++++++++

``UtpStream``（私有头 ``qtng/private/utp.h``）在同样的 ``DatagramLink`` / ``DatagramPath``
抽象上对齐 ``KcpStream`` 会话形态：由 link 构造，然后 ``markBound`` / ``listen`` /
``accept`` 或 ``connect``，以及字节流 ``peek`` / ``recv`` / ``recvall`` / ``send`` /
``sendall``、``busy`` / ``notBusy``、``peerPath``。

它**不**提供 KCP 专有 API（``Mode``、``setMode``、``setSendQueueSize``、``setTearDownTime``、
ikcp MTU 相关接口），而是使用 BEP-29 / LEDBAT 参数：

* ``setDelayTarget`` / ``delayTarget`` — LEDBAT 目标单向时延（默认 100 ms）
* ``setMaxWindow`` / ``maxWindow`` — 拥塞窗口上限
* ``setPacketSize`` / ``packetSize`` / ``payloadSizeHint`` — DATA 载荷大小
* ``setReceiveBufferSize`` / ``receiveBufferSize`` — 通告接收窗口
* ``setIdleTimeout`` / ``idleTimeout`` — 可选空闲断开（0 表示禁用）

线协议为 µTP v1（``ST_DATA`` / ``ST_FIN`` / ``ST_STATE`` / ``ST_RESET`` / ``ST_SYN``），
按 ``connection_id`` 解复用。运行时不依赖 libutp。

``UtpSocket``（``udp.h``）是 ``UdpDatagramLink`` + ``UtpStream`` 的薄 UDP 门面，用法对齐
``KcpSocket``。可用 ``wrapUtpStreamAsSocket``、``asSocketLike`` 与 ``UtpServer``。工厂
``UtpSocket::createConnection`` / ``createServer`` 不接受 KCP ``Mode`` 参数。

8.2.2 QuicConnection 与 QuicStream（QUIC 传输层 MVP）
++++++++++++++++++++++++++++++++++++++++++++++++++++

头文件 ``qtng/quic.h`` 提供 **QUIC v1 传输层 MVP**（RFC 9000/9001/9002 子集）。
**不包含** HTTP/3。

* ``QuicConfiguration`` — ALPN、空闲超时、是否校验对端证书、服务端证书/私钥、流控窗口。
* ``QuicConnection`` — ``connect``（UDP 地址、主机名或 ``DatagramPath``）、``serve``
  （在已绑定链路上完成单连接服务端握手）、``openStream`` / ``acceptStream``、
  ``close`` / ``abort``、``handshakeDone`` 事件。
* ``QuicStream`` — 协程阻塞式 ``recv`` / ``send`` / ``close`` / ``reset``；
  ``asSocketLike(shared_ptr<QuicStream>)`` 可接入需要 ``SocketLike`` 的组件。

与 ``KcpSocket`` / ``UtpSocket``（每会话一条可靠字节流）不同，QUIC 以**连接**多路复用流。
收发包走 ``DatagramLink``（UDP 内部适配，或测试用自定义链路）。

MVP 能力：``TLS_AES_128_GCM_SHA256`` + X25519 的 TLS 1.3 握手，Initial / Handshake /
1-RTT 包保护（含合并包与客户端 Initial ≥1200 字节），CRYPTO / STREAM 帧，简化 ACK +
PTO 重传，``CONNECTION_CLOSE``。可选通过 ``qtng_test_quic_picoquic`` 与
``picoquicdemo`` 做进程级互通（见 ``3rdparty/README.md``）。
本阶段不含：HTTP/3、0-RTT、连接迁移、完整拥塞控制。

8.3 Kademlia / BitTorrent DHT（BEP-5）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``DhtNode``（头文件 ``qtng/kademlia.h``）实现 `BEP-5 <http://www.bittorrent.org/beps/bep_0005.html>`_
规定的 **BitTorrent DHT** 节点。报文使用 bencode（见 §7.2），经**普通 UDP 数据报**传输——
不是 µTP。µTP（BEP-29）只用于后续 peer 传片，不在本模块范围内。

节点支持四个标准 RPC：``ping``、``find_node``、``get_peers``、``announce_peer``。
热数据在内存中；持久化走可插拔的 ``DhtStore``。

8.3.1 概念
++++++++++

**节点 ID / infohash。** 二者都是 20 字节 SHA-1，由 ``NodeId`` 表示。距离为按位 XOR：
``d(a,b) = a XOR b``。公共前缀越长，在键空间中越近。

**路由表。** 最多 160 个 k-bucket（``k = 8``）。桶 *i* 存放与本机 id 的 XOR 距离落在
对应比特区间的联系人。成功 RPC 会刷新联系人；桶满且出现新节点时，对最久未见节点
发 ping，无应答则替换。

**迭代查找。** ``find_node`` / ``get_peers`` 共用 α 并行搜索（``α = 3``）：查询当前
已知的最近联系人，合并返回的 ``nodes`` / ``nodes6``，直到最近集合稳定，再返回最多
``k`` 个联系人（``get_peers`` 时还可能带 ``values`` peer 列表）。

**Token。** ``get_peers`` 响应含短时 token，与请求方 IP 绑定。``announce_peer`` 必须
携带有效 token；qtng 的 token 为 ``SHA1(secret || IP || time_slot)[:8]``，时间槽约
10 分钟。

**Peer 与 DHT 节点。** ``DhtNodeInfo`` 是 DHT 联系人（id + UDP 端点）。``DhtPeer`` 是
torrent peer（IP + 下载端口），由 ``get_peers`` 返回、由 ``announce_peer`` 写入。

实现常量（私有）：RPC 超时 3 秒，peer TTL 约 30 分钟，token TTL 约 10 分钟。

8.3.2 Compact 编码
++++++++++++++++++

BEP-5 将联系人打包进 bencode 字段 ``nodes``、``nodes6``、``values`` 的不透明字节串：

==================== ===========================================
字段                 每条布局
==================== ===========================================
``nodes``（IPv4）    20 字节 id + 4 字节 IP + 2 字节大端端口（26）
``nodes6``（IPv6）   20 字节 id + 16 字节 IP + 2 字节大端端口（38）
``values`` peer v4   4 字节 IP + 2 字节大端端口（6）
``values`` peer v6   16 字节 IP + 2 字节大端端口（18）
==================== ===========================================

辅助函数：``encodeCompactNodes`` / ``decodeCompactNodes``（以及 ``*6`` / ``*Peers`` 变体）。

8.3.3 NodeId
++++++++++++

.. class:: NodeId

    .. attribute:: static const int Size

        固定为 ``20``。

    .. method:: static NodeId random()
    .. method:: static NodeId fromBytes(const std::string &raw20)
    .. method:: static NodeId fromHex(const std::string &hex40)

    .. method:: bool isValid() const
    .. method:: std::string toBytes() const
    .. method:: std::string toHex() const

    .. method:: NodeId operator^(const NodeId &other) const
    .. method:: int commonPrefixLength(const NodeId &other) const
    .. method:: int bucketIndex(const NodeId &other) const

        相对 ``*this`` 的桶下标为 ``0..159``；相等时为 ``-1``。

8.3.4 联系人与 Peer
+++++++++++++++++++

.. class:: DhtEndpoint

    DHT 节点的 UDP ``HostAddress`` + 端口。``isValid()`` 要求地址非空且端口非 0。

.. class:: DhtNodeInfo

    ``NodeId`` 加 ``DhtEndpoint``——路由表 / ``find_node`` 联系人。

.. class:: DhtPeer

    ``get_peers`` / ``announce_peer`` 中的 torrent peer 地址与端口。

8.3.5 DhtStore
++++++++++++++

.. class:: DhtStore

    持久化抽象。``DhtNode`` 在 ``open()`` 时加载，并在维护与查找过程中保存路由 /
    peer 变更。

    .. class:: DhtStore::StoredPeer

        ``DhtPeer peer`` 与 ``expireUnix``（Unix 秒；``<= now`` 时删除）。

    .. method:: virtual bool loadMeta(NodeId *id, std::string *tokenSecret) = 0
    .. method:: virtual bool saveMeta(const NodeId &id, const std::string &tokenSecret) = 0

        本机节点 id 与用于签发 announce token 的密钥。首次启动（尚无数据）时
        ``loadMeta`` 返回 false。

    .. method:: virtual std::vector<DhtNodeInfo> loadNodes() = 0
    .. method:: virtual bool saveNodes(const std::vector<DhtNodeInfo> &nodes) = 0

    .. method:: virtual std::vector<StoredPeer> loadPeers(const NodeId &infoHash) = 0
    .. method:: virtual bool putPeer(const NodeId &infoHash, const DhtPeer &peer, std::int64_t expireUnix) = 0
    .. method:: virtual bool removeExpiredPeers(std::int64_t nowUnix) = 0

    .. method:: virtual std::string errorString() const = 0

.. class:: MemoryDhtStore

    进程内 map。``DhtNode::open()`` 传入空 store 时自动使用。适合测试与临时节点。

.. class:: LmdbDhtStore

    基于 LMDB，命名库为 ``meta``、``nodes``、``peers``。构造路径经 ``LmdbBuilder``，
    默认 ``MDB_NOSUBDIR``（通常是单个文件而非目录）。

    .. method:: explicit LmdbDhtStore(const std::string &dirPath)
    .. method:: bool isOpen() const

8.3.6 DhtNode
+++++++++++++

协程阻塞式 API。``open()`` 绑定 UDP，并启动收包与定期维护协程（peer 过期清理、
经 ``find_node(self)`` 刷新桶）。请在协程上下文中调用 DHT 方法（与其它 qtng 网络
API 相同）。

.. class:: DhtNode

    .. method:: explicit DhtNode(const NodeId &id = NodeId())

        若 ``id`` 无效，``open()`` 从 store 加载或生成随机 id 并持久化。

    .. method:: bool open(std::uint16_t bindPort, std::shared_ptr<DhtStore> store = std::shared_ptr<DhtStore>(), HostAddress::NetworkLayerProtocol proto = HostAddress::IPv4Protocol)

        绑定 UDP（``0`` = 临时端口）。空 ``store`` 会创建 ``MemoryDhtStore``。
        绑定失败返回 false，见 ``errorString()``。

    .. method:: void close()
    .. method:: bool isOpen() const

    .. method:: NodeId id() const
    .. method:: std::uint16_t localPort() const
    .. method:: std::shared_ptr<DhtStore> store() const

    .. method:: bool bootstrap(const std::vector<DhtEndpoint> &seeds)

        对每个 seed 发 ping，再迭代 ``find_node(self)`` 填充路由表。
        结束后表非空则返回 true。

    .. method:: std::vector<DhtNodeInfo> findNode(const NodeId &target)

        迭代 ``find_node``；最多 ``k`` 个最近联系人。

    .. method:: std::vector<DhtPeer> getPeers(const NodeId &infoHash)

        迭代 ``get_peers``，并与本地已存的该 infohash peer 合并（去重）。

    .. method:: bool announcePeer(const NodeId &infoHash, std::uint16_t peerPort, const std::string &token = std::string())

        为 ``infoHash`` 宣告 ``peerPort``。``token`` 为空时，通过对最近节点的
        ``get_peers`` 获取 token。同时写入本地 peer，供后续 ``getPeers`` 使用。

    .. method:: int routingTableSize() const
    .. method:: std::string errorString() const

8.3.7 示例
++++++++++

持久化节点、bootstrap 与 peer 查找::

    auto store = std::make_shared<qtng::LmdbDhtStore>("/var/lib/myapp/dht.mdb");
    qtng::DhtNode node;
    if (!node.open(6881, store)) {
        std::cerr << node.errorString() << std::endl;
        return 1;
    }

    std::vector<qtng::DhtEndpoint> seeds;
    seeds.push_back(qtng::DhtEndpoint(qtng::HostAddress("87.98.162.88"), 6881));
    node.bootstrap(seeds);

    qtng::NodeId info = qtng::NodeId::fromHex("0123456789abcdef0123456789abcdef01234567");
    std::vector<qtng::DhtPeer> peers = node.getPeers(info);
    node.announcePeer(info, /*torrent 监听端口*/ 51413);

本地双节点（测试）::

    qtng::DhtNode a, b;
    a.open(0);
    b.open(0);
    std::vector<qtng::DhtEndpoint> seeds;
    seeds.push_back(qtng::DhtEndpoint(qtng::HostAddress::LocalHost, b.localPort()));
    a.bootstrap(seeds);
    auto contacts = a.findNode(b.id());

8.3.8 范围与非目标
++++++++++++++++++

本模块**不**实现：

* BitTorrent peer 线协议或分片下载（见 §8.4 ``TorrentSession``）
* BEP-32 IPv6 DHT 扩展的完整矩阵（已支持基础 ``nodes6`` / IPv6 绑定）
* BEP-44 / BEP-46 可变 DHT 存储

µTP 见 §8.2.1（``UtpSocket``）。本模块的 peer 发现结果供给 §8.4 使用。

8.4 BitTorrent 下载栈
---------------------

``TorrentSession`` / ``TorrentHandle`` / ``TorrentMeta`` / ``MagnetLink``（头文件
``qtng/bt.h``，实现 ``src/bt.cpp``）提供面向网络程序的 **BitTorrent 核心下载栈**：
加载 ``.torrent`` 或 magnet URI、发现 peer、经 peer wire 传片、SHA-1 校验并写盘。

本栈**复用**已有 qtng 组件，不重复实现：

* §7.2 ``Bencode``：``.torrent`` / tracker / 扩展协议报文
* §8.3 ``DhtNode``：``get_peers`` / ``announce_peer``（默认开启；
  可向构造函数或 ``setDhtNode()`` 传入 ``shared_ptr<DhtNode>`` 以共享同一节点；
  无 tracker 的 magnet 依赖 DHT）
* §8.2.1 ``UtpSocket`` 与 TCP ``Socket``，均经 ``SocketLike`` 接入 peer 传输
  （出站优先 µTP，失败回退 TCP；入站尽可能同时监听）
* ``HttpSession``：HTTP(S) tracker；UDP tracker 遵循 BEP-15
* ``MessageDigest::Sha1``：infohash 与分片哈希
* ``InfoHash`` 是 ``NodeId`` 的类型别名（20 字节 SHA-1）

**Magnet（BEP-9）。** ``MagnetLink::parse`` 支持 v1 magnet（``xt=urn:btih:``，
40 位十六进制或 32 位 base32 infohash，可选 ``dn`` / ``tr`` / ``x.pe``）。
``TorrentSession::addMagnet`` / ``addMagnetUri`` 先以 infohash 入群，再经 BEP-10
扩展握手与 BEP-9 ``ut_metadata`` 拉取 info 字典，随后开盘下载。该阶段对应
``TorrentStats::Metadata``。尚不支持 BitTorrent v2 magnet（``urn:btmh``）。

第一期支持多文件种子、rarest-first 选片、简单 endgame，以及 magnet 元数据交换。
尚未包含：MSE 协议加密、PEX、webseed、BitTorrent v2。

示例::

    auto dht = std::make_shared<qtng::DhtNode>();
    qtng::TorrentSession session(dht);  // 或默认构造后再 session.setDhtNode(dht)
    session.setDownloadDir("/tmp/downloads");
    session.setDhtEnabled(true);
    session.setUtpEnabled(true);
    qtng::TorrentHandle h = session.addMagnetUri(
        "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567");
    // 或: session.addTorrentFile("ubuntu.torrent");
    session.start();
    h.wait();  // 协程内阻塞直至完成或出错

    qtng::TorrentStats st = h.stats();
    // st.progress, st.peersConnected, st.state, ...

Qt Widgets 演示见 ``examples/btclient/``（支持 ``.torrent`` 路径或 magnet URI）。

