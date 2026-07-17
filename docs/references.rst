References of qtng
=========================

1. Use Coroutines
-----------------

1.1 The Essential And Examples
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Coroutine is light-weight thread. In other programming languages, it is called *fiber*, *goroutine*, *greenlet*, etc. Coroutine has its own stack. One coroutine can yield(switch) to another coroutine manually.

.. code-block:: c++
    :caption: Example 1: switch between two BaseCoroutine

    // warning: yield() is rarelly used, this is just an example showing the ability of coroutine.
    #include <qtng.h>
    

    using namespace qtng;

    class MyCoroutine: public BaseCoroutine {
    public:
        MyCoroutine()
        :BaseCoroutine(nullptr) {
            // remember the current coroutine which we will switch to.
            old = BaseCoroutine::current();
        }
        void run() {
            ngDebug() << "my coroutine is here.";
            // switch to the main coroutine
            old->yield();
        }
    private:
        BaseCoroutine *old;
    };

    int main() {
        // once a new coroutine is created, the main thread is convert to main corotuine implicitly.
        MyCoroutine m;
        ngDebug() << "main coroutine is here.";
        // switch to new coroutine, yield() function return until switch back.
        m.yield();
        ngDebug() << "return to main coroutine.";
        return 0;
    }

In last example, we define a ``MyCoroutine`` which derived from ``BaseCoroutine`` first, and overwrite its ``run()`` member function. Be convenient, the main thread is converted to Coroutine implicitly. After create ``MyCoroutine`` object, we can yield to it. The example output:

.. code-block:: text
    :caption: output of example 1

    main coroutine is here.
    my coroutine is here.
    return to main coroutine.

Let's keep eyes on those two ``yield()`` function.

There is another ``BaseCoroutine::raise()`` function similar to ``BaseCoroutine::yield()`` function but send a ``CoroutineException`` to another coroutine. The target coroutine will throw that exception from its ``yield()``.

Now we know how to switch coroutine manually, but is useless for real-life programming. The ``yield()`` is rarelly used indeed. Instead, we use ``Coroutine::start()`` and ``Coroutine::kill()`` of ``Coroutine`` class.

qtng's coroutine functions are splited to ``BaseCoroutine`` and ``Coroutine`` classes. The ``BaseCoroutine`` class implements the basic construction and ``yield()`` function to switch between coroutines. The ``Coroutine`` class derives from ``BaseCoroutine``. It use an extra eventloop coroutine, and introduce ``Coroutine::start()`` and ``Coroutine::kill()``.

If ``Coroutine::start()`` is called, the ``Coroutine`` schedules an event in the eventloop coroutine, and returns immediately. Once the current ``Coroutine`` is blocked in somewhere, such as ``Coroutine::join()``, ``Socket::recv()``, ``Socket::send()`` or ``Event::wait()``, the current coroutine will switch to eventloop coroutine. The eventloop coroutine process scheduled events, and start the coroutine which is schduled before.

Here comes an example showing two coroutines output message in turn.

.. code-block:: c++
    :caption: Example 2: switch between two Coroutine.

    #include "qtng.h"

    using namespace qtng;

    struct MyCoroutine: public Coroutine {
        MyCoroutine(const std::string &name)
            : name(name) {}
        void run() override {
            for (int i = 0; i < 3; ++i) {
                ngDebug() << name << i;
                // switch to eventloop coroutine, will switch back in 100 ms.See 1.7 for details.
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
        // switch to the main coroutine
        coroutine1.join();
        // switch to the second coroutine to finish it.
        coroutine2.join();
        return 0;
    }

As you can see, ``join()`` and ``sleep()`` is blocking call, coroutine switching is taking place. This example outputs:

.. code-block:: text
    :caption: output of example 2

    "coroutine1" 0
    "coroutine2" 0
    "coroutine1" 1
    "coroutine2" 1
    "coroutine1" 2
    "coroutine2" 2

1.2 Start Coroutines
^^^^^^^^^^^^^^^^^^^^

.. note::

    Use ``CoroutineGroup::spawn()`` or ``CoroutineGroup::spawnWithName()`` to start and manage new coroutine.

There are many ways to start new coroutine.

* Inherit ``Coroutine`` and override the ``Coroutine::run()`` function which will run in the new coroutine.

.. code-block:: c++
    :caption: Example 3: the first method to start coroutine

    class MyCoroutine: public Coroutine {
    public:
        virtual void run() override {
            // run in the new coroutine.
        }
    };

    void start() {
        MyCoroutine coroutine;
        coroutine.join();
    }

* Pass a function to ``Coroutine::spawn()`` function which returns the new coroutine. The passed function will be called in the new coroutine.

.. code-block:: c++
    :caption: Example 4: the second method to start coroutine

    void sendMessage() {
        // run in the new coroutine.
    }
    Coroutine *coroutine = Corotuine::spawn(sendMessage);

* The ``Coroutine::spawn()`` accepts ``std::function<void()>`` functor, so c++11 lambda is also acceptable.

.. code-block:: c++
    :caption: Example 5: the third method to start coroutine

    std::shared_ptr<Event> event = std::make_shared<Event>();
    Coroutine *coroutine = Coroutine::spawn([event]{
        // run in the new coroutine.
    });

.. note::

    Captured objects must exists after the coroutine starts. More detail refer to Best Pracice.

1.3 Operate Coroutines
^^^^^^^^^^^^^^^^^^^^^^

Most-used functions posist in ``Coroutine`` class.

.. method:: bool Coroutine::isRunning() const

    Check whether the coroutine is running now, return true or false.

.. method:: bool Coroutine::isFinished() const

    Check whether the coroutine is finished. If the coroutine is not started yet or running, this function returns false, otherwise returns `true`.

.. method:: Coroutine *Coroutine::start(int msecs = 0);

    Schedule the coroutine to start when current coroutine is blocked, and return immediately. The parameter ``msecs`` specifies how many microseconds to wait before the coroutine started, timing from ``start()`` is called. This function returns `this` coroutine object for chained call. For example:

    .. code-block:: c++
        :caption: Example 7: start coroutine

        std::shared_ptr<Coroutine> coroutine(new MyCoroutine);
        coroutine->start()->join();

.. method:: void Coroutine::kill(CoroutineException *e = 0, int msecs = 0)

    Schedule the coroutine to raise exception ``e`` of type ``CoroutineException`` when current coroutine is blocked, and return immediately. The parameter ``msecs`` specifies how many microseconds to wait before the coroutine started, timing from ``kill()`` is called.

    If the parameter ``e`` is not specified, a ``CoroutineExitException`` will be sent to the coroutine.

    If the coroutine is not started yet, calling ``kill()`` may cause the coroutine start and throw an exception. If you don't want this behavior, use ``cancelStart()`` instead.

.. method:: void Coroutine::cancelStart()

    If the coroutine was scheduled to start, ``cancelStart()`` can cancel it. If the coroutine is started, ``cancelStart()`` kill the coroutine. After all, coroutine is set to ``Stop`` state.

.. method:: bool Coroutine::join()

    Block current coroutine and wait for the coroutine to stop. This function switch current coroutine to eventloop coroutine which runs the scheduled tasks, such as start new coroutines, check whether the socket can read/write.

.. method:: virtual void Coroutine::run()

    Override ``run()`` function to create new coroutine. Refer to *1.2 Start Coroutines*

.. method:: static Coroutine *Coroutine::current()

    This static function returns the current coroutine object. Do not save the returned pointer.

.. method:: static void Coroutine::msleep(int msecs)

    This static function block current coroutine, wake up after ``msecs`` microseconds.

.. method:: static void Coroutine::sleep(float secs)

    This static function block current coroutine, wake up after ``secs`` seconds.

.. method:: static Coroutine *Coroutine::spawn(std::function<void()> f)

    This static function start new coroutine from functor ``f``. Refer to *1.2 Start Coroutines*

The ``BaseCoroutine`` has some rarely used functions. Use them at your own risk.

.. method:: State BaseCoroutine::state() const

    Return the current state of coroutine. Can be one of ``Initialized``, ``Started``, ``Stopped`` and ``Joined``. Use this function is not encouraged, you may use `Coroutine::isRunning()` or ``Coroutine::isFinished()`` instead.

.. method:: bool BaseCoroutine::raise(CoroutineException *exception = 0)

    Switch to the coroutine immediately and throw an ``exception`` of type ``CoroutineException``. If the parameter ``exception`` is not specified, a ``CoroutineExitException`` is passed.

    Use the ``Coroutine::kill()`` is more roburst.

.. method:: bool BaseCoroutine::yield()

    Switch to the coroutine immediately.

    Use the ``Coroutine::start()`` is more roburst.

.. method:: std::uintptr_t BaseCoroutine::id() const

    Return an unique imutable id for the coroutine. Basicly, the id is the pointer of coroutine.

.. method:: BaseCoroutine *BaseCoroutine::previous() const

    Return an pointer of ``BaseCoroutine`` which will switch to after this coroutine finished.

.. method:: void BaseCoroutine::setPrevious(BaseCoroutine *previous)

    Set the pointer of ``BaseCoroutine`` which will switch to after this coroutine finished.

.. method:: Deferred<BaseCoroutine*> BaseCoroutine::started`

    This is not a function but ``Deferred`` object. Callbacks can be registered to run after the coroutine is started.

.. method:: Deferred<BaseCoroutine*> BaseCoroutine::finished

    This is not a function but ``Deferred`` object. Callbacks can be registered to run after the coroutine is finished.

1.4 Manage Many Coroutines Using CoroutineGroup
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Creating and deleting coroutine is complicated in C++ programming language, for the complicated memory management in C++. In general, always consider the resource used in coroutine can be deleted outside coroutine, and coroutines must exit before all the resource used are deleted.

Some rules must be followed.

* The immutable object captured by lambda must be passed by value, not pointer nor reference.
* To capture a mutable object for lambda, should use smart pointer such as ``std::shared_ptr<>``.
* If ``this`` pointer is captured, coroutine must take care for the exists of ``this`` object.
* Delete coroutines before all used resource is deleted.

The use pattern of ``CoroutineGroup`` which is a utility class for managing many coroutines, follow these three rules.

* First, create a ``CoroutineGroup`` pointer filed in class, but not a value. Because C++ delete value implicitly.
* Second, delete ``CoroutineGroup`` in the destructor of class. before any other fields.
* The last, always spawn coroutine using ``CoroutineGroup``.

Here comes an example.

.. code-block:: c++
    :caption: using CoroutineGroup

    class WebLoader {
    public:
        WebLoader();
        ~WebLoader();
        const std::string &lastHtml() const { return html; }
    private:
        void loadDataFromWeb();
        std::string html;
        CoroutineGroup *operations; // a pointer, but not a value.
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

Functions in ``CorotuineGroup``.

.. method:: bool add(std::shared_ptr<Coroutine> coroutine, const std::string &name = std::string())

    Add a coroutine which is specified by a smart pointer to group. If the parameter ``name`` is specified, we can use ``CoroutineGroup::get(name)`` to fetch the coroutine later.

.. method:: bool add(Coroutine *coroutine, const std::string &name = std::string())

    Add a coroutine which is specified by a raw pointer to group. If the parameter ``name`` is specified, we can use ``CoroutineGroup::get(name)`` to fetch the coroutine later.

.. method:: bool start(Coroutine *coroutine, const std::string &name = std::string())

    Start a coroutine, and add it to group. If the parameter ``name`` is specified, we can use ``CoroutineGroup::get(name)`` to fetch the coroutine later.

.. method:: std::shared_ptr<Coroutine> get(const std::string &name)

    Fetch a coroutine by name. If no coroutine match the names, an empty pointer is return.

.. method:: bool kill(const std::string &name, bool join = true)`

    Kill a coroutine by name and return true if coroutine is found. If the parameter ``join`` is true, the coroutine is joined and removed, otherwise this function is return immediately.

.. method:: bool killall(bool join = true)

    Kill all coroutines in group, and return true if any coroutine was killed. If the parameter `join` is true, the coroutine is joined and removed, otherwise this function is return immediately.

.. method:: bool joinall()

    Join all coroutines in group. and return true if any coroutine is joined.

.. method:: int size() const

    Return the number of corouitnes in group.

.. method:: bool isEmpty() const

    Return whether there is any coroutine in the group.

.. method:: std::shared_ptr<Coroutine> spawnWithName(const std::string &name, const std::function<void()> &func, bool replace = false)`

    Start a new coroutine to run ``func``, and add it to group with ``name``. If the parameter ``replace`` is false, and there is already a coroutine with the same name exists, no action is taken. Otherwise, if there is already a coroutine with the same name exists, the old one is returned. This function returns the new coroutine.

.. method:: std::shared_ptr<Coroutine> spawn(const std::function<void()> &func)

    Start a new coroutine to run ``func``, and add it to group. This function return the new coroutine.

.. method:: std::shared_ptr<Coroutine> spawnInThreadWithName(const std::string &name, const std::function<void()> &func, bool replace = false)`

    Start a new thread to run ``func``. Create a new coroutine which waits for the new thread finishing, and add it to group with ``name``. If the parameter ``replace`` is false, and there is already a coroutine with the same name exists, no action is taken. Otherwise, if there is already a coroutine with the same name exists, the old one is returned. This function returns the new coroutine.

.. method:: std::shared_ptr<Coroutine> spawnInThread(const std::function<void()> &func)

    Start a new thread to run ``func``. Create a new coroutine which waits for the new thread finishing, and add it to group. This function returns the new coroutine.

.. method:: static std::vector<T> map(std::function<T(S)> func, const std::vector<S> &l)

    Create many coroutines to process the content of ``l``. Each element in ``l`` is passed to ``func`` which run in new coroutine, and the return value of `func` is collected as return value of ``map()``.

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

    Create many coroutines to process the content of ``l``. Each element in ``l`` is passed to ``func`` which run in new coroutine.

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


1.5 Communicate Between Two Coroutine
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The most significant advantage of qtng with respect to `boost::coroutine` is that qtng has a well-established coroutine communication mechanism.

1.5.1 RLock
+++++++++++

`Reentrant Lock` is a mutual exclusion (mutex) device that may be locked multiple times by the same coroutine, without causing a deadlock.

.. _Reentrant Lock: https://en.wikipedia.org/wiki/Reentrant_mutex

``Lock``, ``RLock``, ``Semaphore`` are usually acquired and released using ``ScopedLock<T>`` which releases locks before function returns.

.. code-block:: c++
    :caption: using RLock

    #include "qtng.h"

    using namespace qtng;

    void output(std::shared_ptr<RLock> lock, const std::string &name)
    {
        ScopedLock<RLock> l(*lock);    // acquire lock now, release before function returns. comment out this line and try again later.
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

The output is

.. code-block:: text
    :caption: output of using RLock

    "first" 1
    "first" 2
    "second" 1
    "second" 2

If you comment out the line ``ScopedLock l(lock);``, the output is:

.. code-block:: text
    :caption: output without RLock

    "first" 1
    "second" 1
    "first" 2
    "second" 2

.. method:: bool acquire(bool blocking = true)

    Acquire the lock. If the lock is acquired by other coroutine, and the paremter ``blocking`` is true, block current coroutine until the lock is released by other coroutine. Otherwise this function returns immediately.

    Return whether the lock is acquired.

.. method:: void release()

    Release the lock. The coroutine waiting at this lock will resume after current coroutine switching to eventloop coroutine later.

.. method:: bool isLocked() const

    Check whether any coroutine hold this lock.

.. method:: bool isOwned() const

    Check whether current coroutine hold this lock.

1.5.2 Event
+++++++++++

An `Event` (also called event semaphore) is a type of synchronization mechanism that is used to indicate to waiting coroutines when a particular condition has become true.

.. _Event: https://en.wikipedia.org/wiki/Event_(synchronization_primitive)

.. method:: bool wait(bool blocking = true)

    Waiting event. If this ``Event`` is not set, and the parameter ``blocking`` is true, block current coroutine until this event is set. Otherwise returns immediately.

    Return whether the event is set.

.. method:: void set()

    Set event. The coroutine waiting at this event will resume after current coroutine switching to eventloop coroutine later.

.. method:: void clear()

    Clear event.

.. method:: bool isSet() const

    Check whether this event is set.

.. method:: int getting() const

    Get the number of coroutines waiting at this event.

1.5.3 ValueEvent<>
++++++++++++++++++

``ValueEvent<>`` extends ``Event``. Two coroutines can use ``ValueEvent<>`` to send value.

.. code-block:: c++
    :caption: use ValueEvent<> to send value.

    
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

The output is:

.. code-block:: text

    3

.. method:: void send(const Value &value)

    Send a value to other coroutine, and set this event.

    The coroutines waiting at this event will resume after current coroutine switching to eventloop coroutine.

.. method:: Value wait(bool blocking = true)

    Waiting event. If this ``Event`` is not set, and the parameter ``blocking`` is true, block current coroutine until this event is set. Otherwise returns immediately.

    Return the value sent by other coroutine. If failed, construct a value usning default constructor.

.. method:: void set()

    Set event. The coroutines waiting at this event will resume after current coroutine switching to eventloop coroutine.

.. method:: void clear()

    Clear event.

.. method:: bool isSet() const

    Check whether this event is set.

.. method:: int getting() const

    Get the number of coroutines waiting at this event.

1.5.4 Gate
++++++++++

``Gate`` is a special interface to ``Event``. This type can be used to control data transmit rate.

.. method:: bool goThrough(bool blocking = true)

    It is the same as ``Event::wait()``.

.. method:: bool wait(bool blocking = true)

    It is the same as ``Event::wait()``.

.. method:: void open();

    It is the same as ``Event::set()``.

.. method:: void close();

    It is the same as ``Event::clear()``.

.. method:: bool isOpen() const;

    It is the same as ``Event::isSet()``.

1.5.5 Semaphore
+++++++++++++++

A `semaphore` is a variable or abstract data type used to control access to a common resource by multiple coroutines.

.. _semaphore: https://en.wikipedia.org/wiki/Semaphore_(programming)

.. code-block:: c++
    :caption: using Semaphore to control the concurrent number of request.

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

The last example spawns 100 corotuines, but only 5 coroutines is making request to http server.

.. method:: Semaphore(int value = 1)

    This constructor requires a ``value`` indicating the maximum number of resources.

.. method:: bool acquire(bool blocking = true)

    Acquire the semaphore. If all resouces are used, and the parameter ``blocking`` is true, blocks current coroutine until any other coroutine release a resource. Otherwise this function returns immediately.

    Return whether the semaphore is acquired.

.. method:: void release()

    Release the semaphore. The coroutine waiting at this semaphore will resume after current coroutine switching to eventloop coroutine later.

.. method:: bool isLocked() const

    Check whether this semaphore is hold by any coroutine.

1.5.6 Queue
+++++++++++

A queue between two coroutines.

.. method:: Queue(int capacity)

This constructor requires a ``capacity`` indicating the maximum number of elements can hold.

.. method:: void setCapacity(int capacity)

Set the the maximum number of elements this queue can hold.

.. method:: bool put(const T &e)

Put a element ``e`` to this queue. If the size of queue reaches the capacity, blocks current coroutine until any other coroutine take elements from this queue.

.. method:: T get()

Get (take) a element from this queue. If this queue is empty, blocks current coroutine until any other coroutine put elements to this queue.

.. method:: bool isEmpty() const

Check whether this queue is empty.

.. method:: bool isFull() const

Check whether this queue reaches the maximum size.

.. method:: int getCapacity() const

Get the capacity of this queue.

.. method:: int size() const

Return how many elements in this queue.

.. method:: int getting() const

Return the number of coroutines waiting for elements.

1.5.7 Lock
++++++++++

The ``Lock`` is similar to ``RLock``, but cause dead lock if same corotine locks twice.

1.5.8 Condition
+++++++++++++++

Monitor variable value between coroutines.

.. method:: bool wait()

Block current coroutine until being waked up by ``notify()`` or ``notifyAll()`` by other corotuines.

.. method:: void notify(int value = 1)

Wake up coroutines. The number of coroutines is indicated by ``value``.

.. method:: void notifyAll()

Wake up all coroutines waiting at this condition.

.. method:: int getting() const

Return the number of coroutines waiting at this condition.

1.6 Utilities
^^^^^^^^^^^^^

Several utilities help run work on the internal event loop or in background threads.

*The biggest error* in qtng programming is calling blocking functions such as ``Socket``, ``RLock``, or ``Event`` operations from the event-loop coroutine itself. That leads to undefined behavior. qtng prints a warning when it detects this mistake.

.. method:: T callInEventLoop(std::function<T ()> func)

    Schedule ``func`` on the library event loop and return its result. Use this when non-coroutine code must interact with coroutine-aware APIs on the correct thread.

    .. code-block:: c++

        int value = callInEventLoop<int>([] {
            return 42;
        });

.. method:: void callInEventLoopAsync(std::function<void ()> func, std::uint32_t msecs = 0)

    Asynchronous version of ``callInEventLoop()``. Returns immediately and runs ``func`` after ``msecs`` milliseconds on the event loop.

    .. code-block:: c++

        callInEventLoopAsync([] {
            ngDebug() << "scheduled on event loop";
        });

    ``callInEventLoopAsync()`` is lighter than ``callInEventLoop()`` when the return value is not needed.

.. method:: T callInThread(std::function<T()> func)

    Run ``func`` in a new thread and return its value.


1.7 The Internal: How Coroutines Switch
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

1.7.1 Functor
-------------
Abstract callback interface that defines a unified ``operator()`` method. All concrete callbacks should inherit from this class (e.g., timer callbacks, I/O event callbacks).

.. method:: virtual bool operator()() = 0

    Pure virtual base class; subclasses must implement concrete logic.


1.7.2 DoNothingFunctor
----------------------
No-operation callback that can be used as a placeholder or default callback.

.. method:: bool operator()()

    No-operation callback that directly returns ``false``.


1.7.3 YieldCurrentFunctor
-------------------------
Yields execution rights of the current operation.

.. method:: explicit YieldCurrentFunctor()

    Preserves the pointer to the current coroutine.

.. method:: virtual bool operator()()

    Reawakens the preserved coroutine pointer.


1.7.4 DeleteLaterFunctor<T>
---------------------------
Delays object deletion to avoid direct destruction within callbacks.

.. method:: virtual bool operator()()

    Releases dynamically allocated objects of type ``T``.


1.7.5 LambdaFunctor
-------------------
Wraps a lambda expression to allow it to act as a callback.

.. method:: virtual bool operator()()

    Invokes the stored ``callback()`` to execute user-defined logic.


1.7.6 callInEventLoopCoroutine
------------------------------
Core class of the coroutine event loop, serving as the carrier of the event loop.Responsible for managing I/O event monitoring, timer scheduling, coroutine suspension/resumption, and coordinating interactions between coroutines and the underlying event-driven mechanisms.

Types of I/O operations

    .. code-block:: c++

        enum EventType {
            Read = 1,
            Write = 2,
            ReadWrite = 3,
        };

.. method:: int createWatcher(EventType event, std::intptr_t fd, Functor *callback)

    Creates a read/write event watcher for file descriptor ``fd``, binding the callback function ``callback``.


.. method:: void startWatcher(int watcherId)

    Starts the watcher with specified ID. Used for dynamic event monitoring control.


.. method:: void stopWatcher(int watcherId)

    Stops the watcher with specified ID. Used for dynamic event monitoring control.


.. method:: void removeWatcher(int watcherId)

    Removes the watcher and releases associated resources.


.. method:: void triggerIoWatchers(std::intptr_t fd)

    Manually triggers all registered event callbacks associated with ``fd``. Used for external event notifications.


.. method:: void callLaterThreadSafe(std::uint32_t msecs, Functor *callback)

    Schedules an asynchronous callback to be executed after a delay of ``msecs`` milliseconds in a thread-safe manner.


.. method:: int callLater(std::uint32_t msecs, Functor *callback)

    Executes ``callback`` once after delaying ``msecs`` milliseconds. Returns timer ID.


.. method:: int callRepeat(std::uint32_t msecs, Functor *callback)

    Repeatedly executes ``callback`` every ``msecs`` milliseconds. Returns timer ID.


.. method:: void cancelCall(int callbackId)

    Cancels the timer with specified ID to prevent callback execution.


.. method:: bool runUntil(BaseCoroutine *coroutine)

    Runs event loop until ``coroutine`` completes. Used to block waiting for coroutine finish.


.. method:: bool yield()

    Suspends current coroutine and yields CPU to other coroutines. Typically called while waiting for events.


.. method:: int exitCode()

    Returns event loop's termination status code for judging operation result.


.. method:: bool isQt()

    Determines if the event loop backend implementation is Qt.


.. method:: bool isEv()

    Determines if the event loop backend implementation is libev.


.. method:: bool isWin()

    Determines if the event loop backend implementation is winev.


.. method:: static EventLoopCoroutine *get()

    Unified entry point for event loop, manages instance lifecycle via thread-local storage and adapts to multi-platform backends.
    Serves as the core hub for asynchronous programming. Its design philosophy aligns with Python's ``asyncio.get_event_loop()``, but implements lower-level control leveraging C++ features.


1.7.7 ScopedIoWatcher
---------------------
RAII wrapper for IO event watcher that automatically manages resources.

.. method:: ScopedIoWatcher(EventType event, std::intptr_t fd)

    Creates a watcher for specified event type (read/write) on file descriptor ``fd``.

.. method:: bool start()

    Starts the watcher.


1.7.8 CurrentLoopStorage
------------------------
Abstract base class for event loops that defines platform-dependent interfaces.

.. method:: std::shared_ptr<EventLoopCoroutine> getOrCreate()

    Gets the event loop instance for current thread; creates a new instance if none exists.

.. method:: std::shared_ptr<EventLoopCoroutine> get()

    Only retrieves current thread's event loop instance; returns null pointer if uninitialized.

.. method:: void set(std::shared_ptr<EventLoopCoroutine> eventLoop)

    Explicitly sets current thread's event loop instance (overrides auto-creation logic).

.. method:: void clean()

    Clears current thread's event loop instance, triggering ``std::shared_ptr``'s reference-counted destruction.

2. Basic Network Programming
----------------------------

qtng support IPv4 and IPV6. It is aim to provide an OOP Socket interface as the Python socket module.

In addition to basic socket interface, qtng provide Socks5 proxy support, and a group of classes among `SocketServer` makeing server converently.

2.1 Socket
^^^^^^^^^^

Create socket is very simple, just instantiate ``Socket`` class. Or pass the platform-specific socket descriptor to constructor.

.. code-block:: c++
    :caption: Socket constructor

    Socket(HostAddress::NetworkLayerProtocol protocol = AnyIPProtocol, SocketType type = TcpSocket);

    Socket(std::intptr_t socketDescriptor);

The parameter ``protocol`` can be used to restrict protocol to IPv4 or IPv6. If this parameter is ommited, ``Socket`` will determine the prefered protocol automatically, basically, IPv6 is chosen first. TODO: describe the mehtod.

The parameter ``type`` specify the socket type. Only TCP and UDP is supported now. If this parameter is ommited, TCP is used.

The second form of constructor is useful to convert socket which created by other network programming toolkits to qtng socket. The passed socket must in connected state.

These are the member functions of ``Socket`` type.

.. method:: Socket *accept()

    If the socket is currently listening, ``accept()`` block current coroutine, and return new ``Socket`` object after new client connected. The returned new ``Socket`` object has connected to the new client. This function returns ``0`` to indicate the socket is closed by other coroutine.

.. method:: bool bind(HostAddress &address, std::uint16_t port = 0, BindMode mode = DefaultForPlatform)

    Bind the socket to ``address`` and ``port``. If the parameter ``port`` is ommited, the Operating System choose an unused random port for you. The chosen port can obtained from ``port()`` function later. The parameter ``mode`` is not used now.

    This function returns true if the port is bound successfully.

.. method:: bool bind(std::uint16_t port = 0, BindMode mode = DefaultForPlatform)

    Bind the socket to any address and ``port``. This function overloads ``bind(address, port)``.

.. method:: bool connect(const HostAddress &host, std::uint16_t port)

    Connect to remote host specified by parameters ``host`` and ``port``. Block current coroutine until the connection is established or failed.

    This function returns true if the connection is established.

.. method:: bool connect(const std::string &hostName, std::uint16_t port, HostAddress::NetworkLayerProtocol protocol = AnyIPProtocol)

    Connect to remote host specified by parameters ``hostName`` and ``port``, using ``protocol``. If ``hostName`` is not an IP address, qtng will make a DNS query before connecting. Block current coroutine until the connection is established or failed.

    As the DNS query is a time consuming task, you might use ``setDnsCache()`` to cache query result if you connect few remote host frequently.

    If the parameter ``protocol`` is ommited or specified as ``AnyIPProtocol``, qtng will first try to connect to IPv6 address, then try IPv4 if failed. If the DNS server returns many IPs, qtng will try connecting to those IPs in order.

    This function returns true if the connection is established.

.. method:: bool close()

    Close the socket.

.. method:: bool listen(int backlog)

    The socket is set to listening mode. You can use ``accept()`` to get new client request later. The meaning of parameter ``backlog`` is platform-specific, refer to ``man listen`` please.

.. method:: bool setOption(SocketOption option, int value)

    Set the given ``option`` to the value described by ``value``.

    The options can be  set on a socket.

    +---------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
    | Name                               | Description                                                                                                                          |
    +====================================+======================================================================================================================================+
    | ``BroadcastSocketOption``          | UDP socket send broadcast datagram.                                                                                                  |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``AddressReusable``                | Indicates that the bind() call should allow reuse of local addresses.                                                                |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``ReceiveOutOfBandData``           | If this option is enabled, out-of-band data is directly placed into the receive data stream.                                         |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``ReceivePacketInformation``       | Reserved. Not supported yet.                                                                                                         |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``ReceiveHopLimit``                | Reserved. Not supported yet.                                                                                                         |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``LowDelayOption``                 | If set, disable the Nagle algorithm.                                                                                                 |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``KeepAliveOption``                | Enable sending of keep-alive messages on connection-oriented sockets. Expects an integer boolean flag.                               |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``MulticastTtlOption``             | Set or read the time-to-live value of outgoing multicast packets for this socket.                                                    |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``MulticastLoopbackOption``        | Set or read a boolean integer argument that determines whether sent multicast packets should be looped back to the local sockets.    |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``TypeOfServiceOption``            | Set or receive the Type-Of-Service (TOS) field that is sent with every IP packet originating from this socket.                       |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``SendBufferSizeSocketOption``     | Sets or gets the maximum socket send buffer in bytes.                                                                                |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``ReceiveBufferSizeSocketOption``  | Sets or gets the maximum socket receive buffer in bytes.                                                                             |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``MaxStreamsSocketOption``         | Reserved. STCP is not supported yet.                                                                                                 |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``NonBlockingSocketOption``        | Reserved. `Socket` internally require that socket is nonblocking.                                                                    |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+
    | ``BindExclusively``                | Reserved. Not supported yet.                                                                                                         |
    +------------------------------------+--------------------------------------------------------------------------------------------------------------------------------------+

    Note: On Windows Runtime, Socket::KeepAliveOption must be set before the socket is connected.

.. method:: int option(SocketOption option) const

    Return the value of the option option.

    See also ``setOption()`` for more information.

.. method:: std::int32_t recv(char *data, std::int32_t size)

    Receive not more than ``size`` of data from connection. Blocks current coroutine until some data arrived.

    Return the size of data received. This function returns `0` if connection is closed.

    If some error occured, function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

.. method:: std::int32_t recvall(char *data, std::int32_t size)

    Receive not more than ``size`` of data from connection. Blocks current coroutine until the size of data equals ``size`` or connection is closed.

    This function is similar to ``recv()``, but block current coroutine until all data is received. If you can not be sure the size of data, use ``recv()`` instead. Otherwise that current coroutine might be blocked forever.

    Return the size of data received. Usually the return value is equals to the parameter ``size``, but might be smaller than ``size`` if the connection is closed. You might consider that is an exception.

    If some error occured, this function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

.. method:: std::int32_t send(const char *data, std::int32_t size)

    Send ``size`` of ``data`` to remote host. Block current coroutine until some data sent.

    Return the size of data sent. Usually, the returned value is smaller than the parameter ``size``.

    If some error occured, function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

.. method:: std::int32_t sendall(const char *data, std::int32_t size)

    Send ``size`` of ``data`` to remote host. Block current coroutine until all data sent or the connection closed.

    Return the size of data sent. Usually the return value is equals to the parameter ``size``, but might be smaller than ``size`` if the connection is closed. You might consider that is an exception.

    If some error occured, this function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

.. method:: std::int32_t recvfrom(char *data, std::int32_t size, HostAddress *addr, std::uint16_t *port)

    Receive not more than ``size`` of data from connection. Blocks current coroutine until some data arrived.

    This is used for datagram socket only.

    Return the size of data received.

    If some error occured, function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

.. method:: std::int32_t sendto(const char *data, std::int32_t size, const HostAddress &addr, std::uint16_t port)

    Send ``size`` of ``data`` to remote host specified by ``addr`` and ``port``. Block current coroutine until some data sent.

    This is used for datagram socket only.

    Return the size of data sent. Usually, the returned value is smaller than the parameter ``size``.

    If some error occured, function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

.. method:: std::string recvall(std::int32_t size)

    Receive not more than ``size`` of data from connection. Blocks current coroutine until the size of data equals ``size`` or connection is closed.

    This function is similar to ``recv()``, but block current coroutine until all data is received. If you can not be sure the size of data, use ``recv()`` instead. Otherwise that current coroutine might be blocked forever.

    Return the data received. Usually the size of returned value is equals to the parameter ``size``, but might be smaller than ``size`` if the connection is closed. You might consider that is an exception.

    If some error occured, this function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

    This function overloads ``recvall(char*, std::int32_t)``;

.. method:: std::string recv(std::int32_t size)

    Receive not more than ``size`` of data from connection. Blocks current coroutine until some data arrived.

    Return the data received. This function returns empty ``std::string`` if connection is closed.

    This function can not indicate whether there is any error occured. If this function returns empty data, use ``error()`` to check error, and ``errorString()`` to get the error message.

    This function overloads ``recv(char*, std::int32_t)``.

.. method:: std::int32_t send(const std::string &data)

    Send ``data`` to remote host. Block current coroutine until some data sent.

    Return the size of data sent. Usually, the returned value is smaller than the parameter ``size``.

    If some error occured, this function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

    This function overloads ``send(char*, std::int32_t)``.

.. method:: std::int32_t sendall(const std::string &data)

    Send ``data`` to remote host. Block current coroutine until all data sent or the connection closed.

    Return the size of data sent. Usually the return value is equals to the parameter ``size``, but might be smaller than ``size`` if the connection is closed. You might consider that is an exception.

    If some error occured, this function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

    This function overloads ``sendall(char*, std::int32_t)``.

.. method:: std::string recvfrom(std::int32_t size, HostAddress *addr, std::uint16_t *port)

    Receive not more than ``size`` of data from connection. Blocks current coroutine until some data arrived.

    This is used for datagram socket only.

    Return the data received. This function returns empty ``std::string`` if connection is closed.

    This function can not indicate whether there is any error occured. If this function returns empty data, use ``error()`` to check error, and ``errorString()`` to get the error message.

    This function overloads ``recvfrom(char*, std::int32_t, HostAddress*, std::uint16_t*)``.

.. method:: std::int32_t sendto(const std::string &data, const HostAddress &addr, std::uint16_t port)

    Send ``data`` to remote host specified by ``addr`` and ``port``. Block current coroutine until some data sent.

    This is used for datagram socket only.

    Return the size of data sent. Usually, the returned value is smaller than the parameter ``size``.

    If some error occured, function returns `-1`. You can use ``error()`` and ``errorString()`` to get the error message.

.. method:: SocketError error() const

    Return the type of error that last occurred.

    TODO: A error table.

.. method:: std::string errorString() const

    Return a human-readable description of the last device error that occurred.

.. method:: bool isValid() const

    Return true if the socket is not closed.

.. method:: HostAddress localAddress() const

    Return the host address of the local socket if available; otherwise returns ``HostAddress::Null``.

    This is normally the main IP address of the host, but can be ``HostAddress::LocalHost`` (127.0.0.1) for connections to the local host.

.. method:: std::uint16_t localPort() const

    Return the host port number (in native byte order) of the local socket if available; otherwise returns `0`.

.. method:: HostAddress peerAddress() const

    Return the address of the connected peer if the socket is in ``ConnectedState``; otherwise returns ``HostAddress::Null``.

.. method:: std::string peerName() const

    Return the name of the peer as specified by ``connect()``, or an empty ``std::string`` if ``connect()`` has not been called.

.. method:: std::uint16_t peerPort() const

    Return the port of the connected peer if the socket is in ``ConnectedState``; otherwise returns `0`.

.. method:: std::intptr_t fileno() const

    Return the native socket descriptor of the ``Socket`` object if this is available; otherwise returns `-1`.

    The socket descriptor is not available when ``Socket`` is in ``UnconnectedState``.

.. method:: SocketType type() const

    Return the socket type (TCP, UDP, or other).

.. method:: SocketState state() const

    Return the state of the socket.

    TODO: a state table.

.. method:: NetworkLayerProtocol protocol() const

    Return the protocol of the socket.

.. method:: static std::vector<HostAddress> resolve(const std::string &hostName)

    Make a DNS query to resolve the ``hostName``. If the ``hostName`` is an IP address, return the IP immediately.

    Internationalized domain names (IDN) are converted to Punycode (ACE) via ``utils::toAce()`` before querying the resolver, so hostnames containing non-ASCII characters (e.g. ``"bücher.com"``, ``"中文.com"``) are supported. The conversion is a minimal IDNA shell: pure-ASCII labels pass through unchanged, non-ASCII labels are encoded with the ``xn--`` ACE prefix. Unicode normalization (NFKC), case folding for non-ASCII, and Bidi/Joining checks are **not** performed, so callers should ASCII-lowercase hostnames themselves when needed.

.. method:: void setDnsCache(std::shared_ptr<SocketDnsCache> dnsCache)

    Set a ``SocketDnsCache`` to ``Socket`` object. Every call to ``connect(hostName, port)`` will check the cache first.

2.2 SslSocket
^^^^^^^^^^^^^

The ``SslSocket`` is designed to be similar to ``Socket``. It take most functions of ``Socket`` such as ``connect()``, ``recv()``, ``send()``, ``peerName()``, etc.. But exclude ``recvfrom()`` and ``sendto()`` which are only used for UDP socket.

There are three constructors to create ``SslSocket``.

.. code-block:: c++
    :caption: the constructors of SslSocket

    SslSocket(HostAddress::NetworkLayerProtocol protocol = Socket::AnyIPProtocol,
            const SslConfiguration &config = SslConfiguration());

    SslSocket(std::intptr_t socketDescriptor, const SslConfiguration &config = SslConfiguration());

    SslSocket(std::shared_ptr<Socket> rawSocket, const SslConfiguration &config = SslConfiguration());

In addition, there are many function provided for obtain information from SslSocket.

.. method:: bool handshake(bool asServer, const std::string &verificationPeerName = std::string())

    Do handshake to other peer. If the parameter ``asServer`` is true, this ``SslSocket`` acts as SSL server.

    Use this function only if the ``SslSocket`` is created from plain socket.

.. method:: Certificate localCertificate() const

    Return the the topest certificate of local peer.

    Usually this function returns the same certificate as ``SslConfiguration::localCertificate()``.

.. method:: std::vector<Certificate> localCertificateChain() const

    Return the certificate chain of local peer.

    Usually this function returns the same certificate as ``SslConfiguration::localCertificate()`` and ``localCertificateChain``, plus some CA certificates from ``SslConfiguration::caCertificates``.

.. method:: std::string nextNegotiatedProtocol() const

    Return the next negotiated protocol used by the ssl connection.

    `The Application-Layer Protocol Negotiation` is needed by HTTP/2.

    .. _The Application-Layer Protocol Negotiation: https://en.wikipedia.org/wiki/Application-Layer_Protocol_Negotiation

.. method:: NextProtocolNegotiationStatus nextProtocolNegotiationStatus() const

    Return the status of the next protocol negotiation.

.. method:: SslMode mode() const

    Return the mode the ssl connection. (Server or client)

.. method:: Certificate peerCertificate() const

    Return the topest certificate of remote peer.

.. method:: std::vector<Certificate> peerCertificateChain() const

    Return the certificate chain of remote peer.

.. method:: int peerVerifyDepth() const

    Return the depth of verification. If the certificate chain of remote peer is longer than depth, the verification is failed.

.. method:: Ssl::PeerVerifyMode peerVerifyMode() const

    Return the mode of verification.

    +----------------------+--------------------------------------------------------------------------------------+
    | PeerVerifyMode       | Description                                                                          |
    +======================+======================================================================================+
    | ``VerifyNone``       | ``SslSocket`` will not request a certificate from the peer. You can set this mode    |
    |                      | if you are not interested in the identity of the other side of the connection.       |
    |                      | The connection will still be encrypted, and your socket will still send its          |
    |                      | local certificate to the peer if it's requested.                                     |
    +----------------------+--------------------------------------------------------------------------------------+
    | ``QueryPeer``        | ``SslSocket`` will request a certificate from the peer, but does not require this    |
    |                      | certificate to be valid. This is useful when you want to display peer certificate    |
    |                      | details to the user without affecting the actual SSL handshake. This mode is         |
    |                      | the default for servers.                                                             |
    +----------------------+--------------------------------------------------------------------------------------+
    | ``VerifyPeer``       | ``SslSocket`` will request a certificate from the peer during the SSL handshake      |
    |                      | phase, and requires that this certificate is valid.                                  |
    +----------------------+--------------------------------------------------------------------------------------+
    | ``AutoVerifyPeer``   | ``SslSocket`` will automatically use QueryPeer for server sockets and                |
    |                      | VerifyPeer for client sockets.                                                       |
    +----------------------+--------------------------------------------------------------------------------------+

.. method:: std::string peerVerifyName() const

    Return the name of remote peer.

.. method:: PrivateKey privateKey() const

    Return the private key used by this connection.

    This function returns the same private key to ``SslConfiguration::privateKey()``.

.. method:: SslCipher cipher() const

    Get the cipher used by this connection. If there is no cipher used, this function returns empty cipher. ``Cipher::isNull()`` returns true in that case.

    The cipher is available only after handshaking.

.. method:: Ssl::SslProtocol sslProtocol() const

    Return the ssl protocol used by this connection.

.. method:: SslConfiguration sslConfiguration() const

    Return the configuration used by this connection.

.. method:: std::vector<SslError> sslErrors() const

    Return the errors occured while handshaking and communication.

.. method:: void setSslConfiguration(const SslConfiguration &configuration)

    Set the configuration to use. This function must called before ``handshake()`` is called.

2.3 Socks5 Proxy
^^^^^^^^^^^^^^^^

``Socks5Proxy`` provides SOCKS5 client support. You can use it to make connection to remote host via SOCKS5 proxy.

There are two constructors.

.. code-block:: c++
    :caption: the constructors of Socks5Proxy

    Socks5Proxy();

    Socks5Proxy(const std::string &hostName, std::uint16_t port,
                 const std::string &user = std::string(), const std::string &password = std::string());

The first construct an empty ``Socks5Proxy``. The address of proxy server is needed to connect to remote host.

The second constructor use the ``hostName`` and ``port`` to create a valid Socks5 Proxy.

.. method:: std::shared_ptr<Socket> connect(const std::string &remoteHost, std::uint16_t port);

    Use this function to connect to ``remoteHost`` at ``port`` via this proxy.

    Return new ``Socket`` connect to ``remoteHost`` if success, otherwise returns an zero pointer.

    This function block current coroutine until the connection is made, or failed.

    The DNS query of ``remoteHost`` is made at the proxy server.

.. method:: std::shared_ptr<Socket> connect(const HostAddress &remoteHost, std::uint16_t port)

    Connect to ``remoteHost`` at ``port`` via this proxy.

    Return new ``Socket`` connect to ``remoteHost`` if success, otherwise returns an zero pointer.

    This function block current coroutine until the connection is made, or failed.

    This function is similar to ``connect(std::string, std::uint16_t)`` except that there is no DNS query made.

.. method:: std::shared_ptr<SocketLike> listen(std::uint16_t port)

    Tell the Socks5 proxy to Listen at ``port``.

    Return a ``SocketLike`` object if success, otherwise returns zero pointer.

    You can call ``SocketLike::accept()`` to obtain new requests to that ``port``.

    This function block current coroutine until the server returns whether success or failed.

    The ``SocketLike::accept()`` is blocked until new request arrived.

.. method:: bool isNull() const

    Return true if there is no ``hostName`` or ``port`` of proxy server is provided.

.. method:: Capabilities capabilities() const

    Return the capabilities of proxy server.

.. method:: std::string hostName() const

    Return the ``hostName`` of proxy server.

.. method:: std::uint16_t port() const;

    Return the ``port`` of proxy server.

.. method:: std::string user() const

    Return the ``user`` used for autherication of proxy server.

.. method:: std::string password() const

    Return the ``password`` used for autherication of proxy server.

.. method:: void setCapabilities(Socks5Proxy::Capabilities capabilities)

    Set the capabilities of proxy server.

.. method:: void setHostName(const std::string &hostName)

    Set the ``hostName`` of proxy server.

.. method:: void setPort(std::uint16_t port)

    Set the ``port`` of proxy server.

.. method:: void setUser(const std::string &user)

    Set the ``user`` used for autherication of proxy server.

.. method:: void setPassword(const std::string &password)

    Set the ``password`` used for autherication of proxy server.

2.4 SocketServer
^^^^^^^^^^^^^^^^

2.4.1 BaseStreamServer
+++++++++++++++++++++++

BaseStreamServer is the foundational core class for building other SocketServers, providing basic socket server methods and reserving interfaces for further implementation of server types like TcpServer and KcpServer.

.. method:: BaseStreamServer(const HostAddress &serverAddress, std::uint16_t serverPort);

    Initializes the server's listening address and port, defaults to binding all network interfaces using HostAddress::Any. Also initializes event objects started and stopped to track server status.

.. method:: bool serveForever()

    Blocks to run the server, cyclically accepting client connections and processing requests.

.. method:: bool start()

    Starts the server non-blockingly, running the service in background coroutine.

.. method:: void stop()

    Immediately closes server socket and terminates all connections.

.. method:: bool wait()

    Blocks current thread until server completely stops.

.. method:: void setAllowReuseAddress(bool b)

    Sets whether to allow port reuse (SO_REUSEADDR).

.. method:: bool isSecure()

    Identifies if the server uses encrypted protocols (e.g. SSL). Default returns: false, subclasses (e.g. WithSsl) override to return true.

.. method:: std::shared_ptr<SocketLike> serverSocket()

    Gets underlying server socket object. First call will trigger serverCreate() to create socket.

.. method:: std::uint16_t serverPort()

    Gets port number bound by the server.

.. method:: HostAddress serverAddress()

    Gets IP address bound by the server.

.. method:: virtual bool serverBind()

    Binds the server to specified address and port. Default implementation: sets SO_REUSEADDR option (if allowing address reuse), calls Socket::bind() for system call.

.. method:: virtual bool serverActivate()

    Sets socket to listening state. Default implementation: calls Socket::listen(), sets maximum connection queue length.

.. method:: virtual std::shared_ptr<SocketLike> prepareRequest(std::shared_ptr<SocketLike> request);

    Preprocesses requests (e.g. SSL handshake).

.. method:: virtual bool verifyRequest(std::shared_ptr<SocketLike> request);

    Verifies request validity (e.g. IP blacklist). Default implementation: directly returns true, accepting all connections.

2.4.2 WithSsl
++++++++++++++
Adds SSL/TLS encryption to any streaming server seamlessly through template composition.

.. method:: WithSsl(const HostAddress &serverAddress, std::uint16_t serverPort, const SslConfiguration &configuration);

    Initializes SSL server, inherits from ServerType, with several other similar constructors:

.. code-block:: c++

    WithSsl(const HostAddress &serverAddress, std::uint16_t serverPort);
    WithSsl(std::uint16_t serverPort);
    WithSsl(std::uint16_t serverPort, const SslConfiguration &configuration);
.. method:: void setSslConfiguration(const SslConfiguration &configuration);

    Dynamically sets SSL configuration.

.. method:: SslConfiguration sslConfiguration() const;

    Gets SSL configuration.

.. method:: void setSslHandshakeTimeout(float sslHandshakeTimeout)

    Controls SSL handshake phase duration to prevent client-side malicious occupation.

.. method:: float sslHandshakeTimeout()

    Gets current SSL handshake timeout setting.

.. method:: virtual bool isSecure()

    Indicates server uses encrypted protocol for external code inspection.

.. method:: prepareRequest()

    Upgrades raw TCP connection to SSL connection.

2.4.3 BaseRequestHandler
+++++++++++++++++++++++++
Base class for request handling logic, users should inherit and implement concrete logic.

.. method:: void run()

    Main flow controller ensuring execution order: setup → handle → finish.

.. method:: void setup()

    Initializes request handling environment (e.g. verifying permissions, loading configurations).

.. method:: void handle()

    Implements core business logic (e.g. reading requests, processing data, returning responses).

.. method:: void finish()

    Cleans up resources (e.g. closing connections, logging, memory release). finish() should ensure resource cleanup even if business logic fails.

.. method:: void userData()

    Safely retrieves server-associated custom data (e.g. database connection pools, configuration objects).

2.4.4 Socks5RequestHandler
+++++++++++++++++++++++++++
Socks5RequestHandler implements SOCKS5 proxy protocol, inheriting from BaseRequestHandler to handle client connection requests through SOCKS5 proxy. Core features include protocol handshake, target address resolution, connection establishment, and data forwarding.

.. method:: virtual void handle()

    Main entry point for handling client SOCKS5 requests.

.. method:: bool handshake()

    Handles SOCKS5 handshake and authentication negotiation. Return value: true indicates successful handshake, false indicates failure.

.. method:: bool parseAddress(std::string *hostName, HostAddress *addr, std::uint16_t *port)

    Parses target address and port from client request.

.. method:: virtual std::shared_ptr<SocketLike> makeConnection(const std::string &hostName, const HostAddress &hostAddress,std::uint16_t port, HostAddress *forwardAddress)

    Establishes connection to target server. hostName: Target domain name (e.g. ATYP=0x03), hostAddress: Target IP address (e.g. ATYP=0x01 or 0x04), port: Target port, forwardAddress: Output parameter recording actual connected server address.

.. method:: bool sendConnectReply(const HostAddress &hostAddress, std::uint16_t port)

    Sends connection success response to client.

.. method:: bool sendFailedReply()

    Sends connection failure response.

.. method:: virtual void exchange(std::shared_ptr<SocketLike> request, std::shared_ptr<SocketLike> forward)

    Bidirectionally forwards data between client and target server.

.. method:: doConnect()

    Allows subclass extension for connection success behavior.

.. method:: doFailed()

    Allows subclass extension for connection failure behavior.

.. method:: virtual void logProxy(const std::string &hostName, const HostAddress &hostAddress, std::uint16_t port,const HostAddress &forwardAddress, bool success)

    Logs detailed proxy request information.

2.4.5 TcpServer
++++++++++++++++

Encapsulates the creation, binding, and listening of TCP servers. Implements business logic decoupling through the template parameter RequestHandler. Supports high-concurrency connections based on coroutine concurrency model.

.. method:: TcpServer(const HostAddress &serverAddress, std::uint16_t serverPort);

    Initialize the TCP server, bind to the specified address and port. Directly calls the constructor of ``BaseStreamServer``. If no address is specified, it defaults to binding all network interfaces (HostAddress::Any).

.. method:: virtual std::shared_ptr<SocketLike> serverCreate();

    Create the underlying TCP server socket.

.. method:: virtual void processRequest(std::shared_ptr<SocketLike> request)

    Handle a single client connection request.

.. code-block:: c++
    :caption: Example: Simple TCP Server

    
    #include "qtng.h"
    using namespace  qtng;
    class EchoHandler : public BaseRequestHandler // Inherit BaseRequestHandler and override handle()
    {
    protected:
        void handle()  {
            ngDebug()<<"Received message";
            std::int32_t size=1024;
            std::string data=request->recvall(size);
            ngDebug()<<std::string(data);
        }
    };
    int main()
    {
        // Create the server, listen on port 8080
        TcpServer<EchoHandler> server(8080);
        // Configure server parameters
        server.setRequestQueueSize(100); // Set connection queue length
        server.setAllowReuseAddress(true); // Allow port reuse
        // Start the server (blocking operation)
        if (!server.serveForever()) {
            ngDebug() << "Server startup failed!";
            return 1;
        }
        return 0;
    }

2.4.6 KcpServer
++++++++++++++++
Detailed explanation of the KcpServer and KcpServerV2 classes, their methods, and implementation differences.

.. method:: KcpServer(const HostAddress &serverAddress, std::uint16_t serverPort)

    Initialize the KCP server, bind to the specified address and port. Directly calls the constructor of ``BaseStreamServer``. If no address is specified, it defaults to binding all network interfaces (HostAddress::Any).

.. method:: virtual std::shared_ptr<SocketLike> serverCreate()

    Call ``KcpSocket::createServer()`` to create the KCP server, implemented via the KcpSocket class. This method initializes KCP sessions, binds to the specified address/port, and sets default parameters (e.g., MTU size, window size).

.. method:: virtual void processRequest(std::shared_ptr<SocketLike> request)

    After accepting a client connection, instantiate the user-defined RequestHandler and pass the KCP session (encapsulated as a SocketLike object) to the business logic processing module.


3. Http Client
--------------

``HttpSession`` is a HTTP 1.0/1.1 client with automatical cookie management and automatical redirection. ``HttpSession::send()`` is the core function, which sends request to web server, then parses the response. Other than these, ``HttpSession`` provides many shortcut function, such as ``get()``, ``post()``, ``head()``, etc. Those functions help you to make http request in one line code.

``HttpSession`` can use Socks5 proxy which is default to none. However the support for HTTP proxy has not been implemented yet.

Cookies are parsed and stored using ``HttpSession::cookieJar()``. All response can be stored using ``HttpSession::cacheManager()`` which default to none. qtng provides a ``HttpMemoryCacheManager`` which stores all cacheable responses in memory.

.. code-block:: c++
    :caption: examples to send http request

    HttpSession session;

    // use send()
    HttpRequest request;
    request.setUrl("https://qtng.org/");
    request.setMethod("GET");
    request.setTimeout(10.0f);
    HttpResponse response = session.send(request);
    ngDebug() << response.statusCode() << request.statusText() << response.isOk() << response.body().size();

    // use shortcuts
    HttpResponse response = session.get("https://qtng.org/");
    ngDebug() << response.statusCode() << request.statusText() << response.isOk() << response.body().size();

    std::map<std::string, std::string> query;
    query.insert("username", "panda");
    query.insert("password", "xoxoxoxox");
    HttpResponse response = session.post("https://qtng.org/login/", query);
    ngDebug() << response.statusCode() << request.statusText() << response.isOk() << response.body().size();

    // use cache cache manager
    session.setCacheManager(std::shared_ptr<HttpCacheManager>::create());

The ``HttpRequest`` provides a number of functions for fine-grained control of requests to the web server. The most used functions are ``setMethod()``, ``setUrl()``, ``setBody()``, ``setTimeout()``.

The ``HttpResponse`` provides functions to parse HTTP response. If some error occured, such as connection timout, HTTP 500 error, and others, ``HttpResonse::isOk()`` returns false. So, always check it before use ``HttpResonse``. The detail of errors is ``HttpResonse::error()``.

There is a special function ``HttpRequest::setStreamResponse()`` which indicate that ``HttpResponse`` do not parse the response body. Then, you can take the HTTP connection as plain Socket using ``HttpResponse::takeStream()``.


3.1 HttpSession
^^^^^^^^^^^^^^^

.. method:: HttpResponse send(HttpRequest &request)

    Send http request to web server, and parses the response.

.. method:: HttpCookieJar &cookieJar()

    Return the cookie manager.

    Note: the setter ``setCookieJar(...)`` has not been implemented yet.

.. method:: HttpCookie cookie(const std::string &url, const std::string &name)

    Return the specified cookie of ``url``.

    Cookies are always associated with a URL. So you should provide two parameters ``url`` and ``name`` together.

.. method:: void setMaxConnectionsPerServer(int maxConnectionsPerServer)

    Set the max connections per server to connect. The default value is 10, means that if you make more than 10 requests to a web server, some requests would be blocked untils the first 10 requests finished.

    If ``maxConnectionsPerServer`` less than 0, ``HttpSession`` omit the limit.

.. method:: int maxConnectionsPerServer()

    Return the current max connections per server to connect.

.. method:: void setDebugLevel(int level)

    If debug level is more than 0, ``HttpSession`` will print the digest sent to or received from web server.

    If debug level is more than 1, ``HttpSession`` will print the full content sent to or received from web server, especially the full response body. This can lead to a lot of screen scrolling.

.. method:: void disableDebug()

    Disable printing debug information.

.. method:: void setDefaultUserAgent(const std::string &userAgent)

    Set the default user agent string.

    The default value is "Mozilla/5.0 (X11; Linux x86_64; rv:52.0) Gecko/20100101 Firefox/52.0", which is my favourite browser.

.. method:: std::string defaultUserAgent() const

    Return the default user agent string.

    Each individual ``HttpRequest`` can set its own user agent string using ``HttpRequest::setUserAgent()``

.. method:: HttpVersion defaultVersion() const

    Return the default HTTP version to use.

    The default value is Http 1.1

    Each individual ``HttpRequest`` can set its own http version using ``HttpRequest::setVersion()``

.. method:: HttpVersion defaultVersion() const

    Return the default http version.

.. method:: void setDefaultConnectionTimeout(float timeout)

    Set the default connection timeout, which default to 10 seconds.

    This limit only apply before connection established. If the ``HttpSession`` can not connect to web server, a ``ConnectTimeout`` error is set to ``HttpResponse``.

    Each individual ``HttpRequest`` can set its own timeout.

.. method:: float defaultConnnectionTimeout() const

    Return the default connection timeout.

.. method:: void setSocks5Proxy(std::shared_ptr<Socks5Proxy> proxy)

    Set the SOCKS5 proxy.

.. method:: std::shared_ptr<Socks5Proxy> socks5Proxy() const

    Return the SOCKS5 proxy.

.. method:: void setCacheManager(std::shared_ptr<HttpCacheManager> cacheManager)

    Set the cache manager.

.. method:: std::shared_ptr<HttpCacheManager> cacheManager() const

    Return the cache manager.

.. method:: HttpResponse get(const std::string &url)

    Send HTTP request to web server using GET method.

    There are many similar functions:

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

    Send HTTP request to web server using POST method.

    There are many similar functions:

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

    Return the url of response. In most cases, it is the url of request. If there are redirections, it is the url of last response.

.. method:: void setUrl(const std::string &url)

    Set the url of response. This function is called by ``HttpSession``.

.. method:: int statusCode() const

    Return the status code of response, such as 200 for success, 404 for not found, and 500 for internal error of server.

.. method:: void setStatusCode(int statusCode)

    Set the status code of response. This function is called by ``HttpSession``.

.. method:: std::string statusText() const

    Return the status text of response, such as ``OK`` for success, ``Not Found`` or ``Bad Gateway`` for failed.

.. method:: void setStatusText(const std::string &statusText)

    Set the status text of response. This function is called by ``HttpSession``.

.. method:: std::vector<HttpCookie> cookies() const

    Return the cookies of repsonse.

.. method:: void setCookies(const std::vector<HttpCookie> &cookies)

    Set the cookies of response. This function is called by ``HttpSession``.

.. method:: HttpRequest request() const

    Return the request sent to server. In most cases, it is the request you sent. If there are redirections, it is the new request made by ``HttpSession``.

.. method:: std::int64_t elapsed() const

    The elapsed time in milliseconds, which started from ``HttpSession`` getting request, end at error occured or finished parsing.

.. method:: void setElapsed(std::int64_t elapsed)

    Set the elapsed time. This function is called by ``HttpSession``.

.. method:: std::vector<HttpResponse> history() const

    The previous responses. In most cases, it is an empty list. If there are redirections, it is not empty.

.. method:: void setHistory(const std::vector<HttpResponse> &history)

    Set the previous response. This function is called by ``HttpSession``.

.. method:: HttpVersion version() const

    Return the HTTP version of response. The value can be HTTP 1.0 or HTTP 1.1.

    Note: HTTP 2.0 is not supported yet.

.. method:: void setVersion(HttpVersion version)

    Set the HTTP version of response. This function is called by ``HttpSession``.

.. method:: std::string body() const

    Return the content of response as ``std::string``.



.. method:: std::string text()

    Return the content of response as UTF-8 string.

.. method:: std::string html()

    Return the content of response as string. The encoding is detected from HTTP header and HTML document.

    Note: This function has not been implemented and is currently equivalent to text.

.. method:: bool isOk() const

    Return false if some error occured.

    Note: This function should always be called first before using other functions.

.. method:: bool hasNetworkError() const

    Return true if some network error occured.

.. method:: bool hasHttpError() const

    Return true if an HTTP error occured.

.. method:: std::shared_ptr<RequestError> error() const

    Return the error.

.. method:: void setError(std::shared_ptr<RequestError> error)

    Set the error. This function is called by ``HttpSession``.

.. method:: std::shared_ptr<SocketLike> takeStream(std::string *readBytes)

    In most cases, ``HttpSession`` returns ``HttpResponse`` only if it read all headers and content from server. But you can set ``HttpRequest::streamResponse()`` to ``true``, ``HttpSession`` will return ``HttpResonse`` immediately after reading the HTTP headers.

    ``takeStream()`` returns the http connection.

3.3 HttpRequest
^^^^^^^^^^^^^^^

.. method:: std::string method() const

    Return the method of request.

.. method:: void setMethod(const std::string &method)

    Set the method of request. Can be ``GET``, ``POST``, ``PUT``, etc.

.. method:: qtng::utils::Url url() const

    Return the url of request.

.. method:: void setUrl(const std::string &url)

    Set the url of request.

.. method:: void setUrl(const std::string &url)

    Set the url of request.

.. method:: qtng::utils::UrlQuery query() const

    Return the query string of request.

.. method:: void setQuery(const std::map<std::string, std::string> &query)

    Set the query string of request.

.. method:: void setQuery(const qtng::utils::UrlQuery &query)

    Set the query string of request.

.. method:: std::vector<HttpCookie> cookies() const

    Set the cookies of request.

.. method:: void setCookies(const std::vector<HttpCookie> &cookies)

    Set the cookies of request.

.. method:: std::string body() const

    Return the body of request.

.. method:: void setBody(const std::string &body)

    Set the body of request.

    There are serveral variant functions:

    .. code-block:: c++

        void setBody(const FormData &formData);
        void setBody(const std::map<std::string, std::string> form);
        void setBody(const qtng::utils::UrlQuery &form);

.. method:: std::string userAgent() const

    Return the user agent string of request.

.. method:: void setUserAgent(const std::string &userAgent)

    Set the user agent string of request.

.. method:: int maxBodySize() const

    Return the max body size of response.

    Note: this limit apply to response, not request. If server returns a response larger that this size, ``HttpSession`` will report an ``UnrewindableBodyError`` error.

.. method:: void setMaxBodySize(int maxBodySize)

    Set the max body size of response.

    Note: see ``maxBodySize()``.

.. method:: int maxRedirects() const

    Return the max redirections allow. Set to 0 will disable HTTP redirection.

    Note: When this limit is exceeded, ``HttpSession`` will report an ``TooManyRedirects`` error.

.. method:: void setMaxRedirects(int maxRedirects)

    Set the max redirections allow.

    Note: see ``maxRedirects()``.

.. method:: HttpVersion version() const

    Return the HTTP version of request. Default to ``Unkown``, means that ``HttpSession::defaultVersion()`` is used instead.

    Note:: ``HttpSession::defaultVersion()`` is default to HTTP 1.1

.. method:: void setVersion(HttpVersion version)

    Set the HTTP version of request.

    Note:: see ``version()``.

.. method:: bool streamResponse() const

    If true, indicate that ``HttpResponse`` is returned without reading HTTP content.

    Note: see ``HttpResponse::takeStream()``.

.. method:: void setStreamResponse(bool streamResponse)

    Set true to let ``HttpSession`` return ``HttpResponse`` without reading HTTP content.

    Note: see ``HttpResponse::takeStream()``.

.. method:: float tiemout() const

    Return the connection timeout.

    Note: this restriction only apply in connecting phase. You could use ``qtng::Timeout`` to manage the timeout over the entire request.

.. method:: void setTimeout(float timeout);

    Set the connection timeut.

    Note: see ``timeout()``.


3.4 FormData
^^^^^^^^^^^^

``FormData`` is the HTTP form for POST. It is needed for uploading files.

Note: see ``void HttpRequest::setBody(const FormData &formData)``.

.. method:: void addFile(const std::string &name, const std::string &filename, const std::string &data, const std::string &contentType = std::string())

    Add a file to the field in ``name`` of form.

.. method:: void addQuery(const std::string &key, const std::string &value)

    Set the field in ``name`` of form to ``value``.


3.4 HTTP errors
^^^^^^^^^^^^^^^

Before using the ``HttpResponse``, you should check ``HttpResonse::isOk()``. If the function returns false,  the response is bad. At this point, ``HttpResponse::error()`` returns an instance of following types:

* RequestError

    All error is request error.

* HTTPError

    Web server returns an HTTP error. The error code is ``HTTPError::statusCode``.

* ConnectionError

    Connection is broken while reading or sending data.

* ProxyError

    Can not connect to web server through proxy.

* SSLError

    Can not make SSL connection, handshake failed.

* RequestTimeout

    Timeout while reading or sending data.

    ``RequestTimeout`` is also a ``ConnectionError``.

* ConnectTimeout

    Timeout while conneting to server.

    ``ConnectTimeout`` is also a ``ConnectionError`` and a ``RequestTimeout``.

* ReadTimeout

    Timeout while reading.

    ``ReadTimeout`` is also a ``RequestTimeout``.

* URLRequired

    There is not url in request.

* TooManyRedirects

    Web server return too many redirection responses.

* MissingSchema

    The url of request misses schema.

    Note: ``HttpSession`` only supports ``http`` and ``https``.

* InvalidScheme

    The url of request has an unsupported schema other than ``http`` and ``https``.

* UnsupportedVersion

    The HTTP version is not supported.

    Note: ``HttpSession`` only supports HTTP 1.0 and 1.1.

* InvalidURL

    The url of request is invalid.

* InvalidHeader

    The server returns invalid header.

* ChunkedEncodingError

    The server returns bad chuncked encoding body.

* ContentDecodingError

    Can not decode the body of response.

* StreamConsumedError

    The stream is consumed while reading body.

* UnrewindableBodyError

    The body is too large.


4. Http Server
--------------

4.1 Basic Http Server
^^^^^^^^^^^^^^^^^^^^^

4.1.1 BaseHttpRequestHandler
++++++++++++++++++++++++++++

Base class for handling HTTP requests, providing core functionality for HTTP protocol parsing, response generation, and error handling.

.. method:: BaseHttpRequestHandler()

    Initializes default parameters: HTTP version defaults to Http1_1, request timeout (requestTimeout) defaults to 1 hour, maximum request body size (maxBodySize) defaults to 32MB, connection state (closeConnection) initially set to Maybe.

.. method:: virtual void handle()

    Processes requests in a loop until closeConnection is marked as Yes, calls handleOneRequest() to process individual requests.

.. method:: virtual void handleOneRequest()

    Sets timeout limit (Timeout timeout(requestTimeout)), calls parseRequest() to parse request headers, dispatches to specific HTTP method handlers via doMethod().

.. method:: virtual bool parseRequest()

    Parses request line (e.g. GET /path HTTP/1.1), extracts method/path/version, parses and stores headers, handles Connection header to determine keep-alive, returns true on success or false on failure (automatically sends 400 error).

.. method:: void doMethod

    HTTP method dispatcher. All methods return 501 Not implemented by default. The following methods require subclass implementation:

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

    Generates standard error page (HTML format), sends error response headers (status code, Content-Type, etc.), logs error via logError().

.. method:: void sendCommandLine(HttpStatus status, const std::string &shortMessage)

    Sends status line (e.g. HTTP/1.1 200 OK).

.. method:: void sendHeader(const std::string &name, const std::string &value)

    Adds response header (automatically handles Connection logic).

.. method:: void sendHeader(KnownHeader name, const std::string &value)

    Same functionality as sendHeader.

.. method:: bool endHeader()

    Finalizes headers with \r\n, returns true on success.

.. method:: std::shared_ptr<FileLike> bodyAsFile(bool processEncoding = true)

    Reads request body via Content-Length or Transfer-Encoding, handles GZIP/DEFLATE decompression (requires QTNG_HAVE_ZLIB), supports chunked encoding. Returns readable FileLike object containing request body.

.. method:: bool switchToWebSocket()

    Validates Upgrade: websocket and Sec-WebSocket-Key headers, calculates and returns Sec-WebSocket-Accept, marks connection upgrade to WebSocket.

.. method:: virtual void logRequest(HttpStatus status, int bodySize);

    Logs client address, request method, status code, and response body size.

.. method:: virtual void logError(HttpStatus status, const std::string &shortMessage, const std::string &longMessage);

    Logs error status and messages.

4.1.2 StaticHttpRequestHandler
++++++++++++++++++++++++++++++
Inherits ``BaseHttpRequestHandler``. Handles static resource requests with file transfer, directory listing, auto-index file detection. Includes path traversal protection, automatic MIME type detection, and XSS protection.

.. method:: std::shared_ptr<FileLike> serveStaticFiles(const PosixPath &dir, const std::string &subPath)

    Returns file content or directory listing based on given directory and subpath.

.. method:: std::shared_ptr<FileLike> listDirectory(const PosixPath &dir, const std::string &displayDir)

    Generates HTML directory listing page with clickable links for files/subdirectories.

.. method:: PosixPath getIndexFile(const PosixPath &dir)

    Checks for index.html/index.htm in directory. Returns file info if exists, otherwise empty. Determines whether to display default index file when accessing directories.

.. method:: virtual bool loadMissingFile(const PosixPath &fileInfo);

    Returns false by default. Subclasses can override to generate/retrieve missing files.

4.1.3 SimpleHttpRequestHandler
+++++++++++++++++++++++++++++++
Inherits ``StaticHttpRequestHandler``. Preconfigured static file server with out-of-the-box basic HTTP file serving.

.. method:: void setRootDir(const PosixPath &rootDir)

    Sets accessible root directory. Ensure process has read permissions. Recommended to set before server startup to avoid race conditions.

.. method:: virtual void doGET() override;

    Handles GET requests using parent class's serveStaticFiles method.

.. method:: virtual void doHEAD() override;

    Handles HEAD requests using parent class's serveStaticFiles method.

4.1.4 BaseHttpProxyRequestHandler
++++++++++++++++++++++++++++++++++
Implements core logic for HTTP proxy, supporting forward proxy and tunnel proxy (e.g. HTTPS CONNECT method).

.. method:: virtual void logRequest(qtng::HttpStatus status, int bodySize)

    Empty implementation for request logging. Requires subclass implementation.

.. method:: virtual void logError(qtng::HttpStatus status, const std::string &shortMessage, const std::string &longMessage)

    Empty implementation for error logging. Requires subclass implementation.

.. method:: virtual void logProxy(const std::string &remoteHostName, std::uint16_t remotePort, const HostAddress &forwardAddress,bool success)

    Provides proxy-specific logging via logProxy(). Disables regular request logging by default to avoid duplication.

.. method:: virtual void doMethod()

    HTTP request dispatcher. Checks if method is CONNECT for tunnel handling, routes other methods (GET/POST/etc.) through standard proxy flow.

.. method:: virtual void doCONNECT()

    Handles CONNECT tunnel requests by establishing bidirectional client-target server channels.

.. method:: virtual void doProxy()

    Handles standard HTTP proxy requests by forwarding client requests to target servers and returning responses.

.. method:: virtual std::shared_ptr<SocketLike> makeConnection(const std::string &remoteHostName, std::uint16_t remotePort,HostAddress *forwardAddress)

    It is responsible for creating and initializing a Socket connection to the target server, given the passed remoteHostName and remotePort. This connection will be used for subsequent HTTP request forwarding or HTTPS tunnel proxy (such as CONNECT method).

4.2 Application Server
^^^^^^^^^^^^^^^^^^^^^^
SimpleHttpServer : public TcpServer<SimpleHttpRequestHandler>
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
There is no specific implementation yet

SimpleHttpsServer : public SslServer<SimpleHttpRequestHandler>
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
There is no specific implementation yet

5.1 Password Hash Table
^^^^^^^^^^^^^^^^^^^^^^^
MessageDigest
++++++++++++++
Provides message digest (hash) functionality, supporting multiple hash algorithms, allows processing data in chunks and generating digests. Supports MD4 and MD5 algorithms, Sha1, Sha224, Sha256, Sha384, Sha512 series of SHA algorithms, as well as Ripemd160 and Whirlpool hash algorithms. Availability of optional algorithms (notably Whirlpool, and sometimes MD4/RIPEMD) depends on the linked OpenSSL/LibreSSL build; LibreSSL 4.0+ no longer provides Whirlpool. If an algorithm is unavailable, construction fails and ``result()`` returns an empty string.

.. method:: MessageDigest(Algoritim algo)

    Initializes the context with the specified hash algorithm.

.. method:: addData(const char *data, int len)

    Adds raw byte data to the hash calculation. Calls EVP_DigestUpdate to update the context. Marks error on failure.

.. method:: addData(const char *data)

    Overload of addData. Internally calculates data length and calls the previous addData.

.. method:: std::string result()

    Finalizes the hash calculation and returns the final digest. If called for the first time, calls EVP_DigestFinal_ex to finalize the calculation and caches the result. Subsequent calls return the cached result directly. Returns empty std::string on failure.

.. method:: void update(const std::string &data)

    Same as addData, provides compatibility with common hash interfaces.

.. method:: void update(const char *data, int len)

    Same as addData, provides compatibility with common hash interfaces.

.. method:: std::string hexDigest()

    Same as result(), returns the raw digest.

.. method:: std::string digest()

    Returns the digest in hexadecimal string form.

.. method:: static std::string hash(const std::string &data, Algorithm algo)

    One-time calculation of the hash value (hexadecimal) of the data.

.. method:: static std::string digest(const std::string &data, Algorithm algo)

    One-time calculation of the hash value (raw bytes) of the data.

.. method:: std::string PBKDF2_HMAC(int keylen, const std::string &password, const std::string &salt, const MessageDigest::Algorithm hashAlgo = MessageDigest::Sha256, int i = 10000)

    Calls OpenSSL's PKCS5_PBKDF2_HMAC function to generate the key.

.. method:: std::string scrypt(int keylen, const std::string &password, const std::string &salt, int n = 1048576, int r = 8, int p = 1)

    Not yet implemented.

5.2 Symmetric Encryption and Decryption
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Cipher
+++++++
Provides symmetric encryption/decryption functionality. Supports multiple algorithms (e.g. AES, DES, ChaCha20) and modes (e.g. CBC, CTR, ECB). Supports password derivation and padding control.

.. method:: Cipher(Algorithm alog, Mode mode, Operation operation)

    Initializes the encryption context. Obtains the corresponding OpenSSL EVP_CIPHER via getOpenSSL_CIPHER(). Creates EVP_CIPHER_CTX context. Enables padding by default. Marks hasError on failure.

.. method:: Cipher *copy(Operation operation)

    Copies the current configuration and creates a new Cipher instance.

.. method:: bool isValid()

    Checks if the context is valid. Conditions: OpenSSL context exists, no errors occurred, and it has been initialized.

.. method:: bool isStream()

    Determines if the current encryption context uses stream cipher mode (e.g. CFB, OFB, CTR).

.. method:: bool isBlock()

    Determines if block cipher mode is used (e.g. ECB, CBC). Directly returns !isStream().

.. method:: void setKey(const std::string &key)

    Sets the raw key.

.. method:: std::string key()

    Returns the current key.

.. method:: setInitialVector(const std::string &iv)

    Sets the initialization vector (IV). Stores the IV and initializes the context.

.. method:: std::string initialVector()

    Returns the current IV.

.. method:: std::string iv()

    Same as initialVector method.

.. method:: bool setPassword(const std::string &password, const std::string &salt, const MessageDigest::Algorithm hashAlgo = MessageDigest::Sha256, int i = 100000)

    Derives key via password using PBKDF2-HMAC. Parameters: password, salt, hash algorithm, iteration count. Generates random salt (optional), calls PBKDF2_HMAC to derive key and IV.

.. method:: bool setOpensslPassword(const std::string &password, const std::string &salt, const MessageDigest::Algorithm hashAlgo = MessageDigest::Md5, int i = 1)

    Compatible with OpenSSL's key derivation (EVP_BytesToKey). Parameters: password, salt (must be 8 bytes), hash algorithm, iteration count. Uses legacy method to generate keys, suitable for decrypting data encrypted by OpenSSL.

.. method:: std::string addData(const std::string &data)

    Processes data in chunks and returns encrypted/decrypted result.

.. method:: std::string addData(const char *data, int len)

    Processes data in chunks and returns encrypted/decrypted result.

.. method:: std::string update(const std::string &data)

    Processes data in chunks and returns encrypted/decrypted result.

.. method:: std::string update(const char *data, int len)

    Processes data in chunks and returns encrypted/decrypted result.

.. method:: std::string finalData()

    Finalizes encryption/decryption and returns remaining data.

.. method:: std::string final()

    Finalizes encryption/decryption and returns remaining data.

.. method:: std::string saltHeader()

    Generates OpenSSL-style salt header ("Salted__" + 8-byte salt). Saves salt during encryption for decryption use.

.. method:: std::string parseSalt()

    Parses salt value from OpenSSL header. Return value: std::pair<std::string, std::string> (salt + remaining data).

.. method:: bool setPadding(bool padding)

    Enables or disables PKCS#7 padding: Controls the automatic addition of padding bytes at the end of data for block cipher algorithms (e.g. AES-CBC, DES-ECB). Only effective for block ciphers: automatically ignores padding settings in stream cipher modes (e.g. CTR, CFB).

.. method:: bool padding()

    Gets enable/disable status of PKCS#7 padding.

.. method:: int keySize()

    Gets key length.

.. method:: int ivSize()

    Gets IV length.

.. method:: int blockSize()

    Gets block length.

5.3 Public Key Algorithms
^^^^^^^^^^^^^^^^^^^^^^^^^
5.3.1 PublicKey
++++++++++++++++
Core class in the encryption system, used for managing public key operations.

.. method:: PublicKey()

    Creates an empty public key object. Initializes OpenSSL's EVP_PKEY structure internally.

.. method:: PublicKey(const PublicKey &other)

    Deep copies the underlying OpenSSL key object (via EVP_PKEY_dup). Prevents multiple objects sharing the same key memory, ensuring thread safety.

.. method:: static PublicKey load(const std::string &data, Ssl::EncodingFormat format = Ssl::Pem)

    Creates BIO memory object to read key data. Calls PEM_read_bio_PUBKEY to parse PEM format. Generates EVP_PKEY structure and stores it in PublicKeyPrivate.

.. method:: std::string save(Ssl::EncodingFormat format = Ssl::Pem)

    Writes the key to BIO object via PEM_write_bio_PUBKEY.

.. method:: std::string encrypt(const std::string &data)

    Initializes encryption context (algorithm auto-detected). Dynamically calculates output buffer size (avoids fixed length limitation). Executes encryption and returns result.

.. method:: std::string rsaPublicEncrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING)

    Encrypt with an RSA public key (``EVP_PKEY_encrypt``). ``PKCS1_PADDING`` has the best compatibility (default); ``PKCS1_OAEP_PADDING`` is preferred for new protocols; ``NO_PADDING`` requires manual padding.

.. method:: std::string rsaPublicDecrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING)

    Raw public-key decrypt/recover (``EVP_PKEY_verify_recover``), matching ``rsaPrivateEncrypt`` on the private side. Supports ``PKCS1_PADDING`` (default) and ``NO_PADDING``.

.. method:: bool verify(const std::string &data, const std::string &hash, MessageDigest::Algorithm hashAlgo)

    Processes data with specified hash algorithm (e.g. SHA256). Compares signature hash value with computed value. Returns true if verification passes.

.. method:: Algorithm algorithm()

    Enum type identifying key type (RSA/DSA/EC).

.. method:: int bits()

    Returns key length. 2048-bit RSA key returns 2048.

.. method:: PublicKey &operator=(const PublicKey &other)

    Overloaded = operator. Functionally equivalent to copy constructor.

.. method:: bool operator==(const PublicKey &other)

    Overloaded == operator.

.. method:: bool operator==(const PrivateKey &)

    Overloaded == operator.

.. method:: bool operator!=(const PublicKey &other)

    Overloaded != operator.

.. method:: bool operator!=(const PrivateKey &)

    Overloaded != operator.

.. method:: std::string digest(MessageDigest::Algorithm algorithm = MessageDigest::Sha256)

    Generates unique fingerprint (e.g. SHA256 hash) for key verification.

.. method:: bool isNull()

    Checks if key is empty.

.. method:: bool isValid()

    Checks key validity.

5.3.2 PrivateKey
+++++++++++++++++
Encapsulates private key operations including key generation, signing, decryption, and private key-specific encryption operations.

.. method:: PrivateKey()

    Default constructor.

.. method:: PrivateKey(const PrivateKey &other)

    Copy constructor.

.. method:: PrivateKey(PrivateKey &&other)

    Move constructor.

.. method:: PrivateKey &operator=(const PublicKey &other)

    Copy assignment operator.

.. method:: PrivateKey &operator=(const PrivateKey &other)

    Copy assignment operator.

.. method:: bool operator==(const PrivateKey &other)

    Overloaded == operator.

.. method:: bool operator==(const PublicKey &)

    Overloaded == operator.

.. method:: bool operator!=(const PrivateKey &other)

    Overloaded != operator.

.. method:: bool operator!=(const PublicKey &)

    Overloaded != operator.

.. method:: PublicKey publicKey()

    Extracts the public key corresponding to current private key.

.. method:: std::string sign(const std::string &data, MessageDigest::Algorithm hashAlgo)

    Signs data using private key.

.. method:: std::string decrypt(const std::string &data)

    Decrypts data using private key. Initializes decryption context: EVP_PKEY_decrypt_init. Calculates decrypted length: Calls EVP_PKEY_decrypt twice (first to get length, second to decrypt data). Returns decrypted result: Resizes std::string and fills data.

.. method:: rsaPrivateEncrypt

    Raw private-key encrypt (``EVP_PKEY_sign`` without digest), matching ``rsaPublicDecrypt`` on the public side. Supports ``PKCS1_PADDING`` (default) and ``NO_PADDING``.

.. method:: rsaPrivateDecrypt

    Decrypt with an RSA private key (``EVP_PKEY_decrypt``). Supports ``PKCS1_PADDING`` (default), ``PKCS1_OAEP_PADDING``, and ``NO_PADDING``.

.. method:: static PrivateKey generate(Algorithm algo, int bits)

    Generates a private key of the given algorithm and bit length via ``EVP_PKEY_keygen`` (RSA/DSA).

.. method:: static PrivateKey load(const std::string &data, Ssl::EncodingFormat format = Ssl::Pem, const std::string &password = std::string())

    Loads private key from PEM/DER format with password decryption support.

.. method:: std::string save(Ssl::EncodingFormat format = Ssl::Pem, const std::string &password = std::string())

    Core functionality serializes private key. Supports password encryption (requires valid encryption algorithm). Relies on PrivateKeyWriter to handle OpenSSL low-level details. Needs DER format and default encryption logic improvement.

.. method:: std::string savePublic(Ssl::EncodingFormat format = Ssl::Pem)

    Directly reuses public key saving logic. Ensures output contains only public key information. No password handling required, always saves in plaintext.

5.3.3 PasswordCallback
+++++++++++++++++++++++
Encryption/decryption progress tracking.

.. method:: virtual std::string get(bool writing) = 0;

    Gets encryption/decryption progress. Must be implemented by subclass.

5.3.4 PrivateKeyWriter
+++++++++++++++++++++++
Serializes asymmetric encryption keys (e.g. RSA, DSA keys) to specific formats (PEM/DER). Supports encrypting private keys and saving to files or memory. Core responsibility: Provides flexible configuration options (encryption algorithm, password, public-only saving) and calls OpenSSL functions for serialization.

.. method:: PrivateKeyWriter(const PrivateKey &key)

    Copy constructor via private key.

.. method:: PrivateKeyWriter(const PublicKey &key)

    Copy constructor via public key.

.. method:: PrivateKeyWriter &setCipher(Cipher::Algorithm algo, Cipher::Mode mode)

    Specifies encryption algorithm for private key (e.g. AES-256-CBC). If not called, defaults to no encryption (Cipher::Null).

.. method:: PrivateKeyWriter &setPassword(const std::string &password)

    Provides password for private key encryption via direct input.

.. method:: PrivateKeyWriter &setPassword(std::shared_ptr<PasswordCallback> callback)

    Provides password for private key encryption via dynamic callback.

.. method:: PrivateKeyWriter &setPublicOnly(bool publicOnly)

    Forces saving public key only, even when private key is passed. Extracts public key from private key and saves.

.. method:: std::string asPem()

    Serializes key to PEM format. Supports encrypted private keys.

.. method:: std::string asDer()

    Not fully implemented, returns empty data. Serializes key to DER format. Supports PKCS#8 encryption.

.. method:: bool save(const std::string &filePath)

    Saves key to file. Uses PEM format by default.

5.3.5 PrivateKeyReader
+++++++++++++++++++++++
Responsible for loading private/public keys from files or memory data. Supports handling encrypted private key files (via password or callback).

.. method:: PrivateKeyReader()

    Initialization. Generates PrivateKey object.

.. method:: PrivateKeyReader &setPassword(const std::string &password)

    Sets direct password for decrypting encrypted private keys.

.. method:: PrivateKeyReader &setPassword(std::shared_ptr<PasswordCallback> callback)

    Sets password callback object for dynamic password retrieval (e.g. GUI input).

.. method:: PrivateKeyReader &setFormat(Ssl::EncodingFormat format)

    Specifies input data encoding format (currently only PEM supported).

.. method:: PrivateKey read(const std::string &data)

    Reads private key from in-memory byte array.

.. method:: PublicKey readPublic(const std::string &data)

    Reads public key from in-memory byte array.

.. method:: PrivateKey read(const std::string &filePath)

    Reads private key from file.

.. method:: PublicKey readPublic(const std::string &filePath)

    Reads public key from file.

5.4 Certificates and Certificate Requests
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
5.4.1 Certificate
++++++++++++++++++
Encapsulates certificate operations. Provides interfaces like load/save certificate, retrieve certificate information, generate certificates.

.. method:: Certificate()

    Constructor. Performs initialization.

.. method:: Certificate(const Certificate &other)

    Copy constructor. Performs initialization.

.. method:: Certificate(Certificate &&other)

    Move constructor. Performs initialization.

.. method:: static Certificate load(const std::string &data, Ssl::EncodingFormat format = Ssl::Pem)

    Loads certificate from PEM or DER formatted byte stream.

.. method:: static Certificate generate(const PublicKey &publickey, const PrivateKey &caKey, MessageDigest::Algorithm signAlgo, long serialNumber, const qtng::utils::DateTime &effectiveDate, const qtng::utils::DateTime &expiryDate, const std::multimap<SubjectInfo, std::string> &subjectInfoes)

    Generates new X.509 certificate. Signs with CA private key.

.. method:: static Certificate selfSign(const PrivateKey &key, MessageDigest::Algorithm signAlgo, long serialNumber, const qtng::utils::DateTime &effectiveDate, const qtng::utils::DateTime &expiryDate, const std::multimap<Certificate::SubjectInfo, std::string> &subjectInfoes)

    Self-sign shortcut method. Calls generate method internally.

.. method:: std::string save(Ssl::EncodingFormat format = Ssl::Pem)

    Saves certificate in PEM or DER format.

.. method:: std::string digest(MessageDigest::Algorithm algorithm = MessageDigest::Sha256)

    Computes hash value (e.g. SHA-256) of certificate DER data.

.. method:: qtng::utils::DateTime effectiveDate() const

    Parses X509_getm_notBefore and X509_getm_notAfter in CertificatePrivate::init.

.. method:: qtng::utils::DateTime expiryDate() const

    Parses X509_getm_notBefore and X509_getm_notAfter in CertificatePrivate::init.

.. method:: std::stringList subjectInfo(SubjectInfo subject)

    Retrieves X509_NAME via X509_get_subject_name and X509_get_issuer_name. Parses into key-value pairs.

.. method:: std::stringList subjectInfo(const std::string &attribute)

    Retrieves X509_NAME via X509_get_subject_name and X509_get_issuer_name. Parses into key-value pairs.

.. method:: PublicKey publicKey()

    Gets public key.

.. method:: std::string serialNumber()

    Gets serial number.

.. method:: bool isBlacklisted()

    Checks if certificate is in predefined blacklist (e.g. malicious certificates from Comodo incident).

.. method:: bool isNull()

    Checks if certificate is empty.

.. method:: bool isValid()

    Checks certificate validity (non-empty and not blacklisted).

.. method:: std::string toString()

    Returns certificate as string representation.

.. method:: std::string version()

    Returns current certificate version.

.. method:: bool isSelfSigned()

    Calls X509_check_issued to check if certificate is self-signed.

5.4.2 CertificateRequest
+++++++++++++++++++++++++
Certificate request operations.

.. method:: certificate()

    Returns Certificate object associated with the certificate request.

5.5 TLS Cipher Suites
^^^^^^^^^^^^^^^^^^^^^^
5.5.1 SslCipher
++++++++++++++++
Encryption cipher suite used in SSL/TLS connections. Contains detailed information like encryption algorithm, protocol version, key exchange method.

.. method:: SslCipher()

    Default constructor.

.. method:: SslCipher(const std::string &name)

    Constructor via name.

.. method:: SslCipher(const std::string &name, Ssl::SslProtocol protocol)

    Constructor via name and protocol.

.. method:: SslCipher(const SslCipher &other)

    Copy constructor.

.. method:: std::string authenticationMethod()

    Returns key authentication method (e.g. RSA).

.. method:: std::string encryptionMethod()

    Returns specific encryption algorithm.

.. method:: bool isNull()

    Determines if object is valid (returns true if constructor found no match).

.. method:: std::string keyExchangeMethod()

    Returns key exchange method (e.g. ECDHE).

.. method:: std::string name()

    Directly returns name stored in private class.

.. method:: Ssl::SslProtocol protocol()

    Directly returns protocol enum value stored in private class.

.. method:: std::string protocolString()

    Directly returns protocol string stored in private class.

.. method:: int supportedBits()

    Returns supported encryption bits.

.. method:: int usedBits()

    Returns used encryption bits.

.. method:: inline bool operator!=(const SslCipher &other)

    Determines cipher equality via name and protocol comparison, not all attributes.

.. method:: SslCipher &operator=(SslCipher &&other)

    Move assignment operator.

.. method:: SslCipher &operator=(const SslCipher &other)

    Copy assignment operator.

.. method:: void swap(SslCipher &other)

    Swaps two cipher suites.

.. method:: bool operator==(const SslCipher &other)

    Determines cipher equality via name and protocol comparison, not all attributes.

6. Configuration and Building
------------------------------
6.1 Event loop (libev on Unix)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

On Unix systems, qtng uses libev as its event loop backend. CMake selects the best available mechanism automatically:

1. **OS judgment**: On Linux, macOS, and other non-Windows Unix targets, the libev backend is enabled.

2. **Backend selection**: CMake checks for ``epoll_ctl`` or ``kqueue``.

   * Linux with epoll: ``EV_USE_EPOLL=1`` and ``EV_USE_EVENTFD=1``
   * BSD with kqueue: ``EV_USE_KQUEUE=1``
   * Otherwise: fall back to ``poll()``

   The macro ``QTNG_USE_EV`` indicates that libev is in use.

3. **Source integration**: libev sources live under ``src/ev/``; ``src/eventloop_ev.cpp`` implements the event loop on Unix.

4. **Windows**: Uses a separate Windows-specific event loop implementation.


6.2 SSL/TLS configuration
^^^^^^^^^^^^^^^^^^^^^^^^^

6.2.1 TLS library selection during build
++++++++++++++++++++++++++++++++++++++++

CMake chooses the TLS/crypto library as follows:

* If a ``libressl/`` subdirectory with its own ``CMakeLists.txt`` is present, bundled LibreSSL is built and linked automatically.
* Otherwise, system OpenSSL is required (``find_package(OpenSSL REQUIRED)``).

Install development packages on Debian/Ubuntu with ``libssl-dev`` when not using bundled LibreSSL.


6.3 Installing qtng
^^^^^^^^^^^^^^^^^^^^^

After building, install headers and the static library with CMake:

.. code-block:: bash

    cmake --install . --prefix /usr/local

Typical layout:

* ``${prefix}/include/qtng.h`` — umbrella header (``#include <qtng.h>``)
* ``${prefix}/include/qtng/`` — module headers (``coroutine.h``, ``socket.h``, ``private/``, ``utils/``, …)
* ``${prefix}/lib/libqtng.a`` — static library (``lib64/`` on some 64-bit Linux distributions; ``qtng.lib`` on MSVC)

Link your application with ``-lqtng`` (and ensure the install prefix is on the library path). Use ``#include <qtng.h>`` or ``#include <qtng/coroutine.h>`` (``#include <qtng/qtng.h>`` is equivalent to the umbrella). The same forms work when embedding via ``add_subdirectory()``.


6.2.2 Using Base Socket Classes Directly
+++++++++++++++++++++++++++++++++++++++++
If encryption is not required, use base Socket classes instead of SslSocket. Using Socket directly bypasses all SSL/TLS layers, transmitting data in plaintext.
A simple example:

.. code-block:: c++
    :caption: Example: Implementing a simple HTTP server using base TcpServer instead of SslServer

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

7. Other Auxiliary Classes
---------------------------

7.1 IO Operations
^^^^^^^^^^^^^^^^^^

This module provides cross-platform file and memory I/O abstractions with coroutine-friendly non-blocking operations and secure POSIX path management utilities, suitable for network applications requiring efficient and safe file handling.

Core Functions:

.. method:: bool sendfile(std::shared_ptr<FileLike> inputFile, std::shared_ptr<FileLike> outputFile, std::int64_t bytesToCopy = -1, int suitableBlockSize = 1024 * 8)

    Copies content between files with large file support. Parameters:

    * inputFile/outputFile: File objects for I/O
    * bytesToCopy: Bytes to copy (-1 for full content)
    * suitableBlockSize: Buffer size (default 8KB).

7.1.1 FileLike
+++++++++++++++

Abstract base class defining common file operation interfaces with read/write/close/size capabilities.

.. method:: virtual std::int32_t read(char *data, std::int32_t size)

    Read data to buffer (pure virtual).

.. method:: virtual std::int32_t write(const char *data, std::int32_t size)

    Write buffer data (pure virtual).

.. method:: virtual void close()

    Close file (pure virtual).

.. method:: virtual std::int64_t size()

    Get file size (pure virtual).

.. method:: virtual std::string readall(bool *ok);

    Read entire file, returns success via 'ok'.

.. method:: std::string read(std::int32_t size)

    Read specified data size.

.. method:: std::int32_t write(const std::string &data)

    Write std::string data.

.. method:: static std::shared_ptr<FileLike> open(const std::string &filepath, const std::string &mode = std::string())

    Open file as FileLike instance.

.. method:: static std::shared_ptr<FileLike> bytes(const std::string &data)

    Create memory-based BytesIO.

.. method:: static std::shared_ptr<FileLike> bytes(std::string *data)

    Create BytesIO with existing data.


7.1.2 BytesIO
+++++++++++++++

In-memory byte stream simulating file operations.

.. method:: virtual std::int32_t read(char *data, std::int32_t size)

    Read from memory buffer.

.. method:: virtual std::int32_t write(const char *data, std::int32_t size)

    Write to memory buffer.

.. method:: virtual void close()

    No-op (no close needed for memory).

.. method:: virtual std::int64_t size()

    Get buffer size.

.. method:: virtual std::string readall(bool *ok)

    Return entire buffer content.

.. method:: std::string data()

    Access underlying std::string.

7.1.4 PosixPath
+++++++++++++++++

POSIX-compliant path handling for cross-platform file operations.

.. method:: PosixPath operator/(const std::string &path)

    Path concatenation (may contain ../.).

.. method:: PosixPath operator|(const std::string &path)

    Auto-normalize path (filter ../.).

.. method:: bool isNull()

    Check empty path.

.. method:: bool isFile()

    Check regular file.

.. method:: bool isDir()

    Check directory.

.. method:: bool isSymLink()

    Check symbolic link.

.. method:: bool isAbsolute()

    Check absolute path.

.. method:: bool isExecutable()

    Check executable flag.

.. method:: bool isReadable()

    Check read permission.

.. method:: bool isRelative()

    Check relative path.

.. method:: bool isRoot()

    Check root directory.

.. method:: bool isWritable()

    Check write permission.

.. method:: bool exists()

    Check path existence.

.. method:: std::int64_t size()

    Get file size.

.. method:: std::string path()

    Get full path string.

.. method:: std::int64_t createdMsecsSinceEpoch() const

    Get creation time as milliseconds since Unix epoch.

.. method:: std::int64_t lastModifiedMsecsSinceEpoch() const

    Get last modification time as milliseconds since Unix epoch.

.. method:: std::int64_t lastReadMsecsSinceEpoch() const

    Get last access time as milliseconds since Unix epoch.

.. method:: std::vector<std::string> listdir() const

    List directory contents.

.. method:: std::vector<PosixPath> children() const

.. method:: std::string parentDir() const

    Get parent directory path.

.. method:: PosixPath parentPath() const

    Get parent as PosixPath.

.. method:: std::string name() const

    Get filename (without extension).

.. method:: std::string baseName() const

    Alias of name().

.. method:: std::string suffix() const

    Get last extension.

.. method:: std::string completeBaseName() const

    Get multi-segment filename.

.. method:: std::string completeSuffix() const

    Get multi-segment extension.

.. method:: std::string toAbsolute() const

    Convert to absolute path.

.. method:: std::string relativePath(const std::string &other) const

    Get relative path (string version).

.. method:: std::string relativePath(const PosixPath &other) const

    Get relative path (object version).

.. method:: bool isChildOf(const PosixPath &other) const

    Check descendant relationship.

.. method:: bool hasChildOf(const PosixPath &other) const

    Check ancestor relationship.

.. method:: bool mkdir(bool createParents = false)

    Create directory (with parent creation if enabled).

.. method:: bool touch()

    Update file access/modification times.

.. method:: std::shared_ptr<FileLike> open(const std::string &mode = std::string()) const

    Open the path as a ``FileLike`` with a mode string (e.g. ``"rw+"``).

.. method:: std::string readall(bool *ok) const

    Read entire file content.

.. method:: static PosixPath cwd()

    Get current working directory.

7.1.5 Additional Functions
+++++++++++++++++++++++++++

.. method:: std::pair<std::string, std::string> safeJoinPath(const std::string &parentDir, const std::string &subPath)

    Normalize path joining with security checks.
