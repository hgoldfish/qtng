using namespace std;

#include "qtng/coroutine.h"
#include "qtng/private/coroutine_p.h"
#include "qtng/utils/logging.h"

NG_LOGGER("qtng.coroutine");

namespace qtng {

CoroutineException::CoroutineException() { }

CoroutineException::CoroutineException(CoroutineException &) { }

CoroutineException::~CoroutineException() { }

void CoroutineException::raise()
{
    throw *this;
}

string CoroutineException::what() const
{
    return "coroutine base exception.";
}

CoroutineException *CoroutineException::clone() const
{
    return new CoroutineException();
}

CoroutineExitException::CoroutineExitException() { }

void CoroutineExitException::raise()
{
    throw *this;
}

string CoroutineExitException::what() const
{
    return "coroutine was asked to quit.";
}

CoroutineException *CoroutineExitException::clone() const
{
    return new CoroutineExitException();
}

CoroutineInterruptedException::CoroutineInterruptedException() { }

void CoroutineInterruptedException::raise()
{
    throw *this;
}

string CoroutineInterruptedException::what() const
{
    return "coroutine was interrupted.";
}

CoroutineException *CoroutineInterruptedException::clone() const
{
    return new CoroutineInterruptedException();
}

uintptr_t BaseCoroutine::id() const
{
    return reinterpret_cast<uintptr_t>(this);
}

bool BaseCoroutine::isRunning() const
{
    return state() == BaseCoroutine::Started;
}

bool BaseCoroutine::isFinished() const
{
    return state() == BaseCoroutine::Stopped || state() == BaseCoroutine::Joined;
}

void BaseCoroutine::run() { }

CurrentCoroutineStorage &currentCoroutine()
{
    static CurrentCoroutineStorage storage;
    return storage;
}

BaseCoroutine *CurrentCoroutineStorage::get(bool createIfNotExists)
{
    if (storage.hasLocalData()) {
        return storage.localData();
    }
    if (createIfNotExists) {
        BaseCoroutine *main = createMainCoroutine();
        main->setObjectName("main_coroutine");
        storage.setLocalData(main);
        return main;
    }
    return nullptr;
}

void CurrentCoroutineStorage::set(BaseCoroutine *coroutine)
{
    storage.setLocalData(coroutine);
}

void CurrentCoroutineStorage::clean()
{
    if (storage.hasLocalData()) {
        storage.setLocalData(nullptr);
    }
}

BaseCoroutine *BaseCoroutine::current()
{
    return currentCoroutine().get();
}

}  // namespace qtng
