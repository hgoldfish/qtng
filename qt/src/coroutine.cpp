#include <QtCore/qdebug.h>

#include "bridge/core_access.h"
#include "coroutine.h"
#include "private/coroutine_p.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

namespace {

class BridgedException : public qtng_core::CoroutineException
{
public:
    explicit BridgedException(QTNETWORKNG_NAMESPACE::CoroutineException *qtException)
        : qtException(qtException)
    {
    }
    ~BridgedException() override { delete qtException; }
    void raise() override
    {
        if (qtException) {
            qtException->raise();
        }
        throw *this;
    }
    std::string what() const override
    {
        return qtException ? toStdString(qtException->what()) : std::string("bridged coroutine exception");
    }
    qtng_core::CoroutineException *clone() const override
    {
        return new BridgedException(qtException ? qtException->clone() : new QTNETWORKNG_NAMESPACE::CoroutineException());
    }

    QTNETWORKNG_NAMESPACE::CoroutineException *qtException;
};

class CoreCoroutineAdapter : public qtng_core::Coroutine
{
public:
    CoreCoroutineAdapter(QTNETWORKNG_NAMESPACE::BaseCoroutine *wrapper, qtng_core::BaseCoroutine *previous, size_t stackSize)
        : qtng_core::Coroutine(stackSize)
        , wrapper(wrapper)
    {
        if (previous) {
            setPrevious(previous);
        }
        registerQtWrapper(this, wrapper);
    }
    ~CoreCoroutineAdapter() override { unregisterQtWrapper(this); }
    void run() override
    {
        if (wrapper) {
            wrapper->run();
        }
    }
    void applyState(qtng_core::BaseCoroutine::State state) { setState(state); }

    QTNETWORKNG_NAMESPACE::BaseCoroutine *wrapper;
};

}  // namespace

CoroutineException::CoroutineException() = default;
CoroutineException::CoroutineException(CoroutineException &) = default;
CoroutineException::~CoroutineException() = default;

void CoroutineException::raise()
{
    throw *this;
}

QString CoroutineException::what() const
{
    return QString::fromLatin1("coroutine base exception.");
}

CoroutineException *CoroutineException::clone() const
{
    return new CoroutineException();
}

CoroutineExitException::CoroutineExitException() = default;

void CoroutineExitException::raise()
{
    throw *this;
}

QString CoroutineExitException::what() const
{
    return QString::fromLatin1("coroutine was asked to quit.");
}

CoroutineException *CoroutineExitException::clone() const
{
    return new CoroutineExitException();
}

CoroutineInterruptedException::CoroutineInterruptedException() = default;

void CoroutineInterruptedException::raise()
{
    throw *this;
}

QString CoroutineInterruptedException::what() const
{
    return QString::fromLatin1("coroutine was interrupted.");
}

CoroutineException *CoroutineInterruptedException::clone() const
{
    return new CoroutineInterruptedException();
}

class BaseCoroutinePrivate
{
    Q_DECLARE_PUBLIC(BaseCoroutine)
public:
    BaseCoroutinePrivate(BaseCoroutine *q, BaseCoroutine *previous, size_t stackSize)
        : q_ptr(q)
        , ownsCore(true)
    {
        qtng_core::BaseCoroutine *corePrev = previous ? coreFor(previous) : nullptr;
        core = new CoreCoroutineAdapter(q, corePrev, stackSize);
    }
    ~BaseCoroutinePrivate()
    {
        if (ownsCore) {
            delete core;
        } else {
            unregisterQtWrapper(core);
        }
    }

    BaseCoroutine *q_ptr;
    qtng_core::BaseCoroutine *core;
    bool ownsCore;
};

BaseCoroutine::BaseCoroutine(BaseCoroutine *previous, size_t stackSize)
    : dd_ptr(new BaseCoroutinePrivate(this, previous, stackSize))
{
}

BaseCoroutine::~BaseCoroutine()
{
    delete dd_ptr;
}

void BaseCoroutine::run() { }

BaseCoroutine::State BaseCoroutine::state() const
{
    return static_cast<State>(dd_ptr->core->state());
}

bool BaseCoroutine::isRunning() const
{
    return dd_ptr->core->isRunning();
}

bool BaseCoroutine::isFinished() const
{
    return dd_ptr->core->isFinished();
}

bool BaseCoroutine::raise(CoroutineException *exception)
{
    return dd_ptr->core->raise(new BridgedException(exception));
}

bool BaseCoroutine::yield()
{
    return dd_ptr->core->yield();
}

quintptr BaseCoroutine::id() const
{
    return static_cast<quintptr>(dd_ptr->core->id());
}

BaseCoroutine *BaseCoroutine::previous() const
{
    return qtWrapperFor(dd_ptr->core->previous());
}

void BaseCoroutine::setPrevious(BaseCoroutine *previous)
{
    dd_ptr->core->setPrevious(previous ? coreFor(previous) : nullptr);
}

void BaseCoroutine::setState(BaseCoroutine::State state)
{
    if (auto *adapter = dynamic_cast<CoreCoroutineAdapter *>(dd_ptr->core)) {
        adapter->applyState(static_cast<qtng_core::BaseCoroutine::State>(state));
    }
}

void BaseCoroutine::cleanup()
{
    // Core coroutine drives its own cleanup; Qt subclasses override for join/yield policy.
}

BaseCoroutine *BaseCoroutine::current()
{
    qtng_core::BaseCoroutine *core = qtng_core::BaseCoroutine::current();
    if (!core) {
        return nullptr;
    }
    BaseCoroutine *wrapper = qtWrapperFor(core);
    if (wrapper) {
        return wrapper;
    }
    // Lazily wrap an existing core coroutine (e.g. main) without allocating a new stack.
    wrapper = new BaseCoroutine(nullptr, 1024);
    unregisterQtWrapper(wrapper->dd_ptr->core);
    delete wrapper->dd_ptr->core;
    wrapper->dd_ptr->core = core;
    wrapper->dd_ptr->ownsCore = false;
    registerQtWrapper(core, wrapper);
    return wrapper;
}

BaseCoroutine *createMainCoroutine()
{
    return BaseCoroutine::current();
}

CurrentCoroutineStorage &currentCoroutine()
{
    static CurrentCoroutineStorage storage;
    return storage;
}

BaseCoroutine *CurrentCoroutineStorage::get(bool createIfNotExists)
{
    if (storage.hasLocalData() && storage.localData().value) {
        return storage.localData().value;
    }
    if (!createIfNotExists) {
        return nullptr;
    }
    BaseCoroutine *main = BaseCoroutine::current();
    storage.localData().value = main;
    return main;
}

void CurrentCoroutineStorage::set(BaseCoroutine *coroutine)
{
    storage.localData().value = coroutine;
}

void CurrentCoroutineStorage::clean()
{
    if (storage.hasLocalData()) {
        storage.localData().value = nullptr;
    }
}

}  // namespace QTNETWORKNG_NAMESPACE

QDebug &operator<<(QDebug &out, const QTNETWORKNG_NAMESPACE::BaseCoroutine &coroutine)
{
    if (coroutine.objectName().isEmpty()) {
        return out << QString::fromLatin1("BaseCoroutine(id=%1)").arg(coroutine.id());
    }
    return out << QString::fromLatin1("%1(id=%2)").arg(coroutine.objectName()).arg(coroutine.id());
}
