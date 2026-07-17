using namespace std;

#include "qtng/coroutine.h"

namespace qtng {

class BaseCoroutinePrivate
{
public:
    BaseCoroutinePrivate(BaseCoroutine *q, BaseCoroutine *previous, size_t stackSize);
    virtual ~BaseCoroutinePrivate();
    bool initContext();
    bool raise(CoroutineException *exception = nullptr);
    bool yield();
    void cleanup() { q_ptr->cleanup(); }
public:
    BaseCoroutine * const q_ptr;
    BaseCoroutine * previous;
    size_t stackSize;
    enum BaseCoroutine::State state;
    CoroutineException *exception;
    LPVOID context;
    bool bad;
    NG_DECLARE_PUBLIC(BaseCoroutine)
};


void CALLBACK run_stub(BaseCoroutinePrivate *coroutine)
{
    coroutine->state = BaseCoroutine::Started;
    coroutine->q_ptr->started.callback(coroutine->q_ptr);
    try {
        coroutine->q_ptr->run();
        coroutine->state = BaseCoroutine::Stopped;
        coroutine->q_ptr->finished.callback(coroutine->q_ptr);
    } catch(const CoroutineExitException &) {
        coroutine->state = BaseCoroutine::Stopped;
        coroutine->q_ptr->finished.callback(coroutine->q_ptr);
    } catch(const CoroutineException &) {
//        ngDebug() << "got coroutine exception:" << e.what();
        coroutine->state = BaseCoroutine::Stopped;
        coroutine->q_ptr->finished.callback(coroutine->q_ptr);
    } catch(...) {
        ngWarning() << "coroutine throw a unhandled exception.");
        coroutine->state = BaseCoroutine::Stopped;
        coroutine->q_ptr->finished.callback(coroutine->q_ptr);
        //throw; // cause undefined behaviors
    }
    coroutine->cleanup();
}


BaseCoroutinePrivate::BaseCoroutinePrivate(BaseCoroutine *q, BaseCoroutine *previous, size_t stackSize)
    :q_ptr(q), previous(previous), stackSize(stackSize), state(BaseCoroutine::Initialized), exception(nullptr), context(nullptr),  bad(false)
{

}


BaseCoroutinePrivate::~BaseCoroutinePrivate()
{
    NG_Q(BaseCoroutine);
    if (currentCoroutine().get(false) == q) {
        ngWarning() << "do not delete one self.");
    }
    if (context) {
        if((stackSize == 0)) {
            ConvertFiberToThread();
        } else {
            DeleteFiber(context);
        }
    }
    if (exception) {
        ngWarning() << "BaseCoroutine::exception should always be kept null.");
        // XXX we do not own the exception!
        // delete exception;
    }
}


bool BaseCoroutinePrivate::initContext()
{
    if (context) {
        return true;
    }

    context = CreateFiberEx(1024 * 4, stackSize, 0, (PFIBER_START_ROUTINE)run_stub, this);
    if (!context) {
        DWORD error = GetLastError();
        ngWarning() << ) << "can not create fiber: error is %1";
        bad = true;
        return false;
    } else {
        bad = false;
    }
    return true;
}


bool BaseCoroutinePrivate::raise(CoroutineException *exception)
{
    NG_Q(BaseCoroutine);
    if (!exception) {
        ngWarning() << "can not kill coroutine with null exception.");
        return false;
    }
    if (currentCoroutine().get() == q) {
        ngWarning() << "can not kill oneself.");
        delete exception;
        return false;
    }

    if (this->exception) {
        ngWarning() << "coroutine had been killed.");
        delete exception;
        return false;
    }

    if (state == BaseCoroutine::Stopped || state == BaseCoroutine::Joined) {
        ngWarning() << "coroutine is stopped.");
        delete exception;
        return false;
    }

    this->exception = exception;
    try {
        bool result = yield();
        delete exception;
        return result;
    } catch (...) {
        delete exception;
        throw;
    }
}


bool BaseCoroutinePrivate::yield()
{
    NG_Q(BaseCoroutine);

    if (bad || (state != BaseCoroutine::Initialized && state != BaseCoroutine::Started)) {
        ngWarning() << "invalid coroutine state.");
        return false;
    }

    if (!initContext()) {
        return false;
    }

    BaseCoroutine *old = currentCoroutine().get();
    if (!old) {
        ngWarning() << "can not get old coroutine.");
        return false;
    }
    if (old == q) {
        ngWarning() << "yield to myself. did you call blocking functions in eventloop?");
        return false;
    }

    currentCoroutine().set(q);
    SwitchToFiber(context);
    if (currentCoroutine().get() != old) { // when coroutine finished, swapcontext auto yield to the previous.
        currentCoroutine().set(old);
    }
    CoroutineException *e = old->d_func()->exception;
    if (e) {
        old->d_func()->exception = nullptr;
        e->raise();
    }
    return true;
}


BaseCoroutine* createMainCoroutine()
{
    BaseCoroutine *main = new BaseCoroutine(nullptr, 0);
    if (!main) {
        return nullptr;
    }
    main->setObjectName("main");
    BaseCoroutinePrivate *mainPrivate = main->d_func();
#if ( _WIN32_WINNT > 0x0600)
        if (IsThreadAFiber()) {
            mainPrivate->context = GetCurrentFiber();
        } else {
            mainPrivate->context = ConvertThreadToFiberEx(nullptr, 0);
        }
#else
        mainPrivate->context = ConvertThreadToFiber(nullptr);
        if ((nullptr== mainPrivate->context)) {
            DWORD err = GetLastError();
            if (err == ERROR_ALREADY_FIBER) {
                mainPrivate->context = GetCurrentFiber();
            }
            if (reinterpret_cast<LPVOID>(0x1E00) == mainPrivate->context) {
                mainPrivate->context = nullptr;
            }
        }
#endif
    if (!mainPrivate->context) {
        DWORD error = GetLastError();
        ngWarning() << "Coroutine can not malloc new memroy: error is %d", error);
        delete main;
        return nullptr;
    }
    mainPrivate->state = BaseCoroutine::Started;
    return main;
}


// here comes the public class.
BaseCoroutine::BaseCoroutine(BaseCoroutine *previous, size_t stackSize)
    :dd_ptr(new BaseCoroutinePrivate(this, previous, stackSize))
{
}


BaseCoroutine::~BaseCoroutine()
{
    delete dd_ptr;
}


BaseCoroutine::State BaseCoroutine::state() const
{
    NG_D(const BaseCoroutine);
    return d->state;
}


bool BaseCoroutine::raise(CoroutineException *exception)
{
    NG_D(BaseCoroutine);
    return d->raise(exception);
}


bool BaseCoroutine::yield()
{
    NG_D(BaseCoroutine);
    return d->yield();
}


void BaseCoroutine::setState(BaseCoroutine::State state)
{
    NG_D(BaseCoroutine);
    d->state = state;
}


void BaseCoroutine::cleanup()
{
    NG_D(BaseCoroutine);
    if (d->previous) {
        d->previous->yield();
    }
}


BaseCoroutine *BaseCoroutine::previous() const
{
    NG_D(const BaseCoroutine);
    return d->previous;
}


void BaseCoroutine::setPrevious(BaseCoroutine *previous)
{
    NG_D(BaseCoroutine);
    d->previous = previous;
}

}  // namespace qtng
