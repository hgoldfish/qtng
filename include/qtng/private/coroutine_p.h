#ifndef QTNG_COROUTINE_P_H
#define QTNG_COROUTINE_P_H

#include "qtng/utils/thread_local.h"
#include "qtng/coroutine.h"
#include "qtng/utils/platform.h"

namespace qtng {

// this class is not used by other code, but just a stub figure out how to write a new coroutine implementation.
class BaseCoroutinePrivate;
class BaseCoroutinePrivatePlatformCommon
{
public:
    BaseCoroutinePrivatePlatformCommon(BaseCoroutine *q, BaseCoroutine *previous, size_t stackSize);
    virtual ~BaseCoroutinePrivatePlatformCommon();
    bool raise(CoroutineException *exception = 0);
    bool yield();
protected:
    BaseCoroutine * const q_ptr;
    BaseCoroutine * const previous;
    size_t stackSize;
    void *stack;
    enum BaseCoroutine::State state;
    bool bad;
    CoroutineException *exception;
    NG_DECLARE_PUBLIC(BaseCoroutine)
};

BaseCoroutine *createMainCoroutine();

class CurrentCoroutineStorage
{
public:
    BaseCoroutine *get(bool createIfNotExists = true);
    void set(BaseCoroutine *coroutine);
    void clean();
private:
    qtng::utils::ThreadLocal<BaseCoroutine *> storage;
};

CurrentCoroutineStorage &currentCoroutine();

}  // namespace qtng

#endif  // QTNG_COROUTINE_P_H
