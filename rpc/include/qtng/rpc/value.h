#ifndef QTNG_RPC_VALUE_H
#define QTNG_RPC_VALUE_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "qtng/msgpack.h"
#include "qtng/utils/datetime.h"

#include "qtng/rpc/base.h"

BEGIN_QTNG_RPC_NAMESPACE

// The msgpack sid key used to tag registered classes, byte-compatible with the
// legacy lafrpc "__laf_sid__".
constexpr const char *SpecialSidKey = "__laf_sid__";

class Value
{
public:
    enum class Type { Nil, Bool, Int, Uint, Double, Str, Bin, Array, Map, DateTime, Ext, Object };

    Value()
        : data(std::monostate())
    {
    }
    Value(std::nullptr_t)
        : data(std::monostate())
    {
    }
    Value(bool v)
        : data(v)
    {
    }
    Value(int v)
        : data(static_cast<std::int64_t>(v))
    {
    }
    Value(std::int64_t v)
        : data(v)
    {
    }
    Value(std::uint64_t v)
        : data(v)
    {
    }
    Value(float v)
        : data(static_cast<double>(v))
    {
    }
    Value(double v)
        : data(v)
    {
    }
    Value(const char *s)
        : data(detail::ValueStr(s ? std::string(s) : std::string()))
    {
    }
    Value(const std::string &s)
        : data(detail::ValueStr(s))
    {
    }
    Value(std::vector<Value> v)
        : data(std::move(v))
    {
    }
    Value(std::map<std::string, Value> v)
        : data(std::move(v))
    {
    }
    Value(qtng::utils::DateTime dt)
        : data(dt)
    {
    }
    Value(qtng::MsgPackExtData ext)
        : data(std::move(ext))
    {
    }
    Value(std::shared_ptr<Serializable> obj)
        : data(std::move(obj))
    {
    }

    // explicit str/bin constructors.
    static Value str(std::string s) { return Value(detail::ValueStr(std::move(s))); }
    static Value bin(std::string b) { return Value(detail::ValueBin(std::move(b))); }

    Type type() const;
    bool isNull() const { return type() == Type::Nil; }

    bool asBool() const;
    std::int64_t asInt() const;
    std::uint64_t asUint() const;
    double asDouble() const;
    const std::string &asStr() const;
    const std::string &asBin() const;
    const std::vector<Value> &asArray() const;
    const std::map<std::string, Value> &asMap() const;
    qtng::utils::DateTime asDateTime() const;
    qtng::MsgPackExtData asExt() const;
    std::shared_ptr<Serializable> asObject() const;

    // map helpers.
    bool hasKey(const std::string &key) const;
    Value at(const std::string &key) const;  // missing key -> Nil
    const Value *find(const std::string &key) const;
    const Value *at(std::size_t index) const;  // array element, nullptr if out of range

    // generic conversion used by bindMethod(). Throws RpcSerializationException on mismatch.
    template<typename T>
    T getAs() const;

    template<typename T>
    std::shared_ptr<T> asShared() const;

    // wire encoding / decoding (msgpack, str/bin distinguished).
    static bool pack(qtng::MsgPackStream &s, const Value &v);
    static Value unpack(qtng::MsgPackStream &s);
    static std::string pack(const Value &v);
    static Value unpack(const std::string &data);

    // registered-class aware recursive conversion (saveState/restoreState).
    static Value saveState(const Value &v);
    static Value restoreState(const Value &v);

private:
    using VariantType = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double,
                                     detail::ValueStr, detail::ValueBin, std::vector<Value>,
                                     std::map<std::string, Value>, qtng::utils::DateTime, qtng::MsgPackExtData,
                                     std::shared_ptr<Serializable>>;
    explicit Value(detail::ValueStr s)
        : data(std::move(s))
    {
    }
    explicit Value(detail::ValueBin b)
        : data(std::move(b))
    {
    }

    VariantType data;

    friend class ValuePrivate;
};

namespace detail {

// dispatch entry used by Value::getAs<T>().
template<typename T, typename = void>
struct ValueConverter
{
    static T convert(const Value &v) { return T(v); }
};

template<>
struct ValueConverter<bool>
{
    static bool convert(const Value &v) { return v.asBool(); }
};

template<>
struct ValueConverter<int>
{
    static int convert(const Value &v) { return static_cast<int>(v.asInt()); }
};

template<>
struct ValueConverter<std::int64_t>
{
    static std::int64_t convert(const Value &v) { return v.asInt(); }
};

template<>
struct ValueConverter<unsigned int>
{
    static unsigned int convert(const Value &v) { return static_cast<unsigned int>(v.asUint()); }
};

template<>
struct ValueConverter<std::uint64_t>
{
    static std::uint64_t convert(const Value &v) { return v.asUint(); }
};

template<>
struct ValueConverter<float>
{
    static float convert(const Value &v) { return static_cast<float>(v.asDouble()); }
};

template<>
struct ValueConverter<double>
{
    static double convert(const Value &v) { return v.asDouble(); }
};

template<>
struct ValueConverter<std::string>
{
    static std::string convert(const Value &v) { return v.asStr(); }
};

template<>
struct ValueConverter<qtng::utils::DateTime>
{
    static qtng::utils::DateTime convert(const Value &v) { return v.asDateTime(); }
};

template<>
struct ValueConverter<qtng::MsgPackExtData>
{
    static qtng::MsgPackExtData convert(const Value &v) { return v.asExt(); }
};

template<>
struct ValueConverter<ValueList>
{
    static ValueList convert(const Value &v) { return v.asArray(); }
};

template<>
struct ValueConverter<ValueMap>
{
    static ValueMap convert(const Value &v) { return v.asMap(); }
};

template<>
struct ValueConverter<Value>
{
    static Value convert(const Value &v) { return v; }
};

template<typename T>
struct ValueConverter<std::shared_ptr<T>>
{
    static std::shared_ptr<T> convert(const Value &v) { return v.asShared<T>(); }
};

}  // namespace detail

template<typename T>
inline T Value::getAs() const
{
    return detail::ValueConverter<T>::convert(*this);
}

template<typename T>
inline std::shared_ptr<T> Value::asShared() const
{
    switch (type()) {
    case Type::Object: {
        std::shared_ptr<Serializable> obj = asObject();
        std::shared_ptr<T> t = std::dynamic_pointer_cast<T>(obj);
        if (t) {
            return t;
        }
        break;
    }
    case Type::Map: {
        const Value *sid = find(SpecialSidKey);
        if (sid) {
            const std::string key = sid->asStr();
            detail::ClassMap &cm = detail::classes();
            detail::ClassMap::iterator it = cm.find(key);
            if (it != cm.end()) {
                std::shared_ptr<Serializable> obj = it->second.factory();
                if (obj->restoreState(*this)) {
                    std::shared_ptr<T> t = std::dynamic_pointer_cast<T>(obj);
                    if (t) {
                        return t;
                    }
                }
            }
        }
        break;
    }
    default:
        break;
    }
    throw RpcSerializationException("can not convert value to the requested shared_ptr type.");
}

using RpcFunction = std::function<Value(const ValueList &, const ValueMap &)>;

// ---------------------------------------------------------------------------
// request / response frames (wire layout is fixed, see the design doc)
// ---------------------------------------------------------------------------
struct Request
{
    std::string id;
    std::string methodName;
    ValueList args;
    ValueMap kwargs;
    ValueMap header;
    std::uint32_t channel = 0;
    std::string rawSocket;
    bool oneway = false;
    bool isOk() const { return !id.empty() && !methodName.empty(); }
};

struct Response
{
    std::string id;
    Value result;
    Value exception;
    std::uint32_t channel = 0;
    std::string rawSocket;
    bool isOk() const { return !id.empty(); }
};

END_QTNG_RPC_NAMESPACE

#endif  // QTNG_RPC_VALUE_H
