#ifndef QTNG_RPC_BASE_H
#define QTNG_RPC_BASE_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include "qtng/data_channel.h"
#include "qtng/locks.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/datetime.h"

#ifndef QTNG_RPC_NAMESPACE
#  define QTNG_RPC_NAMESPACE rpc
#endif
#define BEGIN_QTNG_RPC_NAMESPACE namespace qtng { namespace QTNG_RPC_NAMESPACE {
#define END_QTNG_RPC_NAMESPACE } }

BEGIN_QTNG_RPC_NAMESPACE

class Value;

using ValueList = std::vector<Value>;
using ValueMap = std::map<std::string, Value>;

// ---------------------------------------------------------------------------
// exceptions
// ---------------------------------------------------------------------------
class RpcException
{
public:
    RpcException() = default;
    RpcException(const RpcException &) = default;
    RpcException(RpcException &&) = default;
    explicit RpcException(const std::string &message)
        : message(message)
    {
    }
    virtual ~RpcException() = default;
    virtual std::string what() const
    {
        return message.empty() ? std::string("rpc exception.") : message;
    }
    virtual void raise() { throw *this; }
    std::string message;
};

class RpcInternalException : public RpcException
{
public:
    RpcInternalException() = default;
    explicit RpcInternalException(const std::string &message)
        : RpcException(message)
    {
    }
    virtual std::string what() const override
    {
        return message.empty() ? std::string("rpc got internal exception.") : message;
    }
    virtual void raise() override { throw *this; }
};

class RpcDisconnectedException : public RpcException
{
public:
    RpcDisconnectedException() = default;
    explicit RpcDisconnectedException(const std::string &message)
        : RpcException(message)
    {
    }
    virtual std::string what() const override
    {
        return message.empty() ? std::string("rpc is disconnected.") : message;
    }
    virtual void raise() override { throw *this; }
};

class RpcSerializationException : public RpcException
{
public:
    RpcSerializationException() = default;
    explicit RpcSerializationException(const std::string &message)
        : RpcException(message)
    {
    }
    virtual std::string what() const override
    {
        return message.empty() ? std::string("can not serialize object.") : message;
    }
    virtual void raise() override { throw *this; }
};

// ---------------------------------------------------------------------------
// serializable objects (registered classes with __laf_sid__ on the wire)
// ---------------------------------------------------------------------------
class Serializable
{
public:
    virtual ~Serializable() = default;
    virtual std::string lafrpcKey() const = 0;
    virtual Value saveState() const = 0;
    virtual bool restoreState(const Value &state) = 0;
    virtual std::shared_ptr<Serializable> clone() const = 0;
};

class RpcRemoteException : public RpcException, public Serializable
{
public:
    RpcRemoteException() = default;
    explicit RpcRemoteException(const std::string &message)
        : RpcException(message)
    {
    }
    virtual std::string what() const override
    {
        return message.empty() ? std::string("remote peer throw an exception.") : message;
    }
    virtual void raise() override { throw *this; }
    virtual std::shared_ptr<Serializable> clone() const override
    {
        return std::make_shared<RpcRemoteException>(message);
    }
    static std::string staticLafrpcKey() { return "RpcRemoteException"; }
    virtual std::string lafrpcKey() const override { return "RpcRemoteException"; }
    virtual Value saveState() const override;
    virtual bool restoreState(const Value &state) override;
};

// ---------------------------------------------------------------------------
// stream arguments / return values
// ---------------------------------------------------------------------------
struct UseStream : public Serializable
{
    enum Place {
        ServerSide = 1,
        ClientSide = 2,
        ParamInRequest = 4,
        ValueOfResponse = 8,
    };
    UseStream();
    ~UseStream() override;
    std::shared_ptr<qtng::VirtualChannel> channel;
    int place;
    bool preferRawSocket;
    std::shared_ptr<qtng::SocketLike> rawSocket;
    qtng::Event ready;
};

// A service instance that implements dispatch itself.
class Callable
{
public:
    virtual ~Callable() = default;
    virtual Value call(const std::string &methodName, const ValueList &args, const ValueMap &kwargs) = 0;
};

// ---------------------------------------------------------------------------
// request / response frames (wire layout is fixed, see the design doc).
// Defined in value.h where Value is complete.
// ---------------------------------------------------------------------------
struct Request;
struct Response;

// ---------------------------------------------------------------------------
// multi-cast callback dispatcher (replaces Qt signals)
// ---------------------------------------------------------------------------
template<typename... Args>
class EventDispatcher
{
public:
    using Callback = std::function<void(Args...)>;
    using Connection = std::shared_ptr<void>;

    EventDispatcher() = default;
    ~EventDispatcher() = default;
    EventDispatcher(const EventDispatcher &) = delete;
    EventDispatcher &operator=(const EventDispatcher &) = delete;

    Connection connect(Callback cb)
    {
        auto slot = std::make_shared<Callback>(std::move(cb));
        std::lock_guard<std::mutex> lock(mutex);
        slots.push_back(slot);
        return slot;
    }

    // Equivalent of a QObject receiver: the slot is skipped once obj is gone.
    template<typename Self, typename F>
    Connection connectWeak(const std::shared_ptr<Self> &obj, F &&f)
    {
        std::weak_ptr<Self> w(obj);
        return connect([w, f = std::forward<F>(f)](Args... args) {
            if (w.expired()) {
                return;
            }
            f(std::forward<Args>(args)...);
        });
    }

    void disconnect(const Connection &conn)
    {
        if (!conn) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = slots.begin(); it != slots.end(); ++it) {
            if (*it == conn) {
                slots.erase(it);
                return;
            }
        }
    }

    void emit(Args... args) const
    {
        std::vector<std::shared_ptr<Callback>> copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            copy = slots;
        }
        for (const std::shared_ptr<Callback> &slot : copy) {
            if (slot) {
                (*slot)(args...);
            }
        }
    }

private:
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Callback>> slots;
};

// ---------------------------------------------------------------------------
// class registration registry
// ---------------------------------------------------------------------------
namespace detail {

struct ValueStr
{
    std::string data;
    ValueStr() = default;
    explicit ValueStr(std::string d)
        : data(std::move(d))
    {
    }
};

struct ValueBin
{
    std::string data;
    ValueBin() = default;
    explicit ValueBin(std::string d)
        : data(std::move(d))
    {
    }
};

struct ClassInfo
{
    std::string name;
    std::function<std::shared_ptr<Serializable>()> factory;
};

using ClassMap = std::map<std::string, ClassInfo>;
inline ClassMap &classes()
{
    static ClassMap m;
    return m;
}

using ExceptionRaiser = std::function<void(const std::shared_ptr<RpcRemoteException> &)>;
inline std::vector<ExceptionRaiser> &exceptionRaisers()
{
    static std::vector<ExceptionRaiser> m;
    return m;
}

using UseStreamConvertor = std::function<std::shared_ptr<UseStream>(const std::shared_ptr<Serializable> &)>;
inline std::vector<UseStreamConvertor> &useStreamConvertors()
{
    static std::vector<UseStreamConvertor> m;
    return m;
}

template<typename T>
std::shared_ptr<Serializable> makeSerializable()
{
    return std::make_shared<T>();
}

template<typename T>
void registerExceptionClass(T * = nullptr,
                            typename std::enable_if<std::is_base_of<RpcRemoteException, T>::value>::type * = nullptr)
{
    exceptionRaisers().push_back([](const std::shared_ptr<RpcRemoteException> &e) {
        std::shared_ptr<T> t = std::dynamic_pointer_cast<T>(e);
        if (t) {
            t->raise();
        }
    });
}

template<typename T>
void registerExceptionClass(T * = nullptr,
                            typename std::enable_if<!std::is_base_of<RpcRemoteException, T>::value>::type * = nullptr)
{
}

template<typename T>
void registerUseStreamClass(T * = nullptr,
                            typename std::enable_if<std::is_base_of<UseStream, T>::value>::type * = nullptr)
{
    useStreamConvertors().push_back([](const std::shared_ptr<Serializable> &s) -> std::shared_ptr<UseStream> {
        return std::dynamic_pointer_cast<UseStream>(s);
    });
}

template<typename T>
void registerUseStreamClass(T * = nullptr,
                            typename std::enable_if<!std::is_base_of<UseStream, T>::value>::type * = nullptr)
{
}

template<typename T>
void registerClass()
{
    ClassMap &m = classes();
    const std::string key = T::staticLafrpcKey();
    if (m.find(key) == m.end()) {
        m[key] = ClassInfo{ typeid(T).name(), &makeSerializable<T> };
    }
    registerExceptionClass<T>();
    registerUseStreamClass<T>();
}

}  // namespace detail

END_QTNG_RPC_NAMESPACE

#endif  // QTNG_RPC_BASE_H
