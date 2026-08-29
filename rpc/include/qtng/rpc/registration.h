#ifndef QTNG_RPC_REGISTRATION_H
#define QTNG_RPC_REGISTRATION_H

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "qtng/rpc/base.h"
#include "qtng/rpc/value.h"

BEGIN_QTNG_RPC_NAMESPACE

// ---------------------------------------------------------------------------
// toValue: convert a C++ value back to a rpc::Value
// ---------------------------------------------------------------------------
inline Value toValue(const Value &v)
{
    return v;
}

inline Value toValue(bool v)
{
    return Value(v);
}

inline Value toValue(int v)
{
    return Value(v);
}

inline Value toValue(std::int64_t v)
{
    return Value(v);
}

inline Value toValue(unsigned int v)
{
    return Value(static_cast<std::uint64_t>(v));
}

inline Value toValue(std::uint64_t v)
{
    return Value(v);
}

inline Value toValue(float v)
{
    return Value(v);
}

inline Value toValue(double v)
{
    return Value(v);
}

inline Value toValue(const char *v)
{
    return Value::str(v ? std::string(v) : std::string());
}

inline Value toValue(const std::string &v)
{
    return Value::str(v);
}

inline Value toValue(qtng::utils::DateTime v)
{
    return Value(v);
}

inline Value toValue(qtng::MsgPackExtData v)
{
    return Value(std::move(v));
}

template<typename T>
inline typename std::enable_if<std::is_base_of<Serializable, T>::value, Value>::type toValue(const std::shared_ptr<T> &v)
{
    return Value(std::static_pointer_cast<Serializable>(v));
}

// ---------------------------------------------------------------------------
// bindMethod: compile-time member function binding (replaces QMetaObject
// reflection). A bound method accepts positional arguments only; kwargs are
// ignored (the legacy QObject dispatch also ignored them).
// ---------------------------------------------------------------------------
namespace detail {

template<typename T, typename R, typename... Args, std::size_t... I>
Value callBoundMethod(std::shared_ptr<T> obj, R (T::*method)(Args...), const ValueList &args,
                      std::index_sequence<I...>)
{
    if (args.size() != sizeof...(Args)) {
        throw RpcRemoteException("the size of passed arguments does not match the method parameter count.");
    }
    std::tuple<Args...> parsed(args[I].template getAs<Args>()...);
    if constexpr (std::is_void<R>::value) {
        (obj.get()->*method)(std::get<I>(std::move(parsed))...);
        return Value();
    } else {
        return toValue((obj.get()->*method)(std::get<I>(std::move(parsed))...));
    }
}

}  // namespace detail

template<typename T, typename R, typename... Args>
RpcFunction bindMethod(std::shared_ptr<T> obj, R (T::*method)(Args...))
{
    return [obj, method](const ValueList &args, const ValueMap &) -> Value {
        return detail::callBoundMethod(obj, method, args, std::index_sequence_for<Args...>());
    };
}

// ---------------------------------------------------------------------------
// Service: a dispatch table of named methods. A service object derives from
// Service (or composes one), binds each method in its constructor, then is
// registered with Rpc::registerInstance(service, "name").
// ---------------------------------------------------------------------------
class Service : public Callable
{
public:
    Service() = default;
    ~Service() override = default;
    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;

    void bind(const std::string &method, RpcFunction f) { methods[method] = std::move(f); }

    bool has(const std::string &method) const { return methods.find(method) != methods.end(); }

    Value call(const std::string &methodName, const ValueList &args, const ValueMap &kwargs) override
    {
        std::map<std::string, RpcFunction>::const_iterator it = methods.find(methodName);
        if (it == methods.end()) {
            throw RpcRemoteException("service method not found: " + methodName);
        }
        return it->second(args, kwargs);
    }

private:
    std::map<std::string, RpcFunction> methods;
};

END_QTNG_RPC_NAMESPACE

#endif  // QTNG_RPC_REGISTRATION_H
