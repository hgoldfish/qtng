#include "qtng/rpc/value.h"

using namespace std;

namespace qtng {
namespace rpc {

Value::Type Value::type() const
{
    switch (data.index()) {
    case 0:
        return Type::Nil;
    case 1:
        return Type::Bool;
    case 2:
        return Type::Int;
    case 3:
        return Type::Uint;
    case 4:
        return Type::Double;
    case 5:
        return Type::Str;
    case 6:
        return Type::Bin;
    case 7:
        return Type::Array;
    case 8:
        return Type::Map;
    case 9:
        return Type::DateTime;
    case 10:
        return Type::Ext;
    case 11:
        return Type::Object;
    default:
        return Type::Nil;
    }
}

bool Value::asBool() const
{
    if (const bool *b = std::get_if<bool>(&data)) {
        return *b;
    }
    throw RpcSerializationException("value is not a bool.");
}

std::int64_t Value::asInt() const
{
    if (const std::int64_t *i = std::get_if<std::int64_t>(&data)) {
        return *i;
    }
    if (const std::uint64_t *u = std::get_if<std::uint64_t>(&data)) {
        return static_cast<std::int64_t>(*u);
    }
    if (const bool *b = std::get_if<bool>(&data)) {
        return *b ? 1 : 0;
    }
    throw RpcSerializationException("value is not an integer.");
}

std::uint64_t Value::asUint() const
{
    if (const std::uint64_t *u = std::get_if<std::uint64_t>(&data)) {
        return *u;
    }
    if (const std::int64_t *i = std::get_if<std::int64_t>(&data)) {
        return static_cast<std::uint64_t>(*i);
    }
    if (const bool *b = std::get_if<bool>(&data)) {
        return *b ? 1 : 0;
    }
    throw RpcSerializationException("value is not an unsigned integer.");
}

double Value::asDouble() const
{
    if (const double *d = std::get_if<double>(&data)) {
        return *d;
    }
    if (const std::int64_t *i = std::get_if<std::int64_t>(&data)) {
        return static_cast<double>(*i);
    }
    if (const std::uint64_t *u = std::get_if<std::uint64_t>(&data)) {
        return static_cast<double>(*u);
    }
    throw RpcSerializationException("value is not a floating point number.");
}

const std::string &Value::asStr() const
{
    if (const detail::ValueStr *s = std::get_if<detail::ValueStr>(&data)) {
        return s->data;
    }
    throw RpcSerializationException("value is not a string.");
}

const std::string &Value::asBin() const
{
    if (const detail::ValueBin *b = std::get_if<detail::ValueBin>(&data)) {
        return b->data;
    }
    throw RpcSerializationException("value is not a binary buffer.");
}

const std::vector<Value> &Value::asArray() const
{
    if (const std::vector<Value> *a = std::get_if<std::vector<Value>>(&data)) {
        return *a;
    }
    throw RpcSerializationException("value is not an array.");
}

const std::map<std::string, Value> &Value::asMap() const
{
    if (const std::map<std::string, Value> *m = std::get_if<std::map<std::string, Value>>(&data)) {
        return *m;
    }
    throw RpcSerializationException("value is not a map.");
}

qtng::utils::DateTime Value::asDateTime() const
{
    if (const qtng::utils::DateTime *dt = std::get_if<qtng::utils::DateTime>(&data)) {
        return *dt;
    }
    throw RpcSerializationException("value is not a datetime.");
}

qtng::MsgPackExtData Value::asExt() const
{
    if (const qtng::MsgPackExtData *e = std::get_if<qtng::MsgPackExtData>(&data)) {
        return *e;
    }
    throw RpcSerializationException("value is not an ext data.");
}

std::shared_ptr<Serializable> Value::asObject() const
{
    if (const std::shared_ptr<Serializable> *o = std::get_if<std::shared_ptr<Serializable>>(&data)) {
        return *o;
    }
    throw RpcSerializationException("value is not an object.");
}

bool Value::hasKey(const std::string &key) const
{
    const std::map<std::string, Value> *m = std::get_if<std::map<std::string, Value>>(&data);
    return m && m->find(key) != m->end();
}

Value Value::at(const std::string &key) const
{
    const Value *v = find(key);
    return v ? *v : Value();
}

const Value *Value::find(const std::string &key) const
{
    const std::map<std::string, Value> *m = std::get_if<std::map<std::string, Value>>(&data);
    if (!m) {
        throw RpcSerializationException("value is not a map.");
    }
    std::map<std::string, Value>::const_iterator it = m->find(key);
    return it == m->end() ? nullptr : &it->second;
}

const Value *Value::at(std::size_t index) const
{
    const std::vector<Value> *a = std::get_if<std::vector<Value>>(&data);
    if (!a || index >= a->size()) {
        return nullptr;
    }
    return &(*a)[index];
}

// ---------------------------------------------------------------------------
// wire encoding / decoding
// ---------------------------------------------------------------------------
namespace {

enum class WireKind { Nil, Bool, Int, Uint, Double, Str, Bin, Array, Map, Ext, Unknown };

WireKind wireKind(std::uint8_t b)
{
    if (b <= 0x7f) {
        return WireKind::Int;
    }
    if (b >= 0xe0) {
        return WireKind::Int;
    }
    if (b >= 0xa0 && b <= 0xbf) {
        return WireKind::Str;
    }
    if (b >= 0x90 && b <= 0x9f) {
        return WireKind::Array;
    }
    if (b >= 0x80 && b <= 0x8f) {
        return WireKind::Map;
    }
    if (b >= 0xd4 && b <= 0xd8) {
        return WireKind::Ext;
    }
    switch (b) {
    case 0xc0:
        return WireKind::Nil;
    case 0xc2:
    case 0xc3:
        return WireKind::Bool;
    case 0xc4:
    case 0xc5:
    case 0xc6:
        return WireKind::Bin;
    case 0xc7:
    case 0xc8:
    case 0xc9:
        return WireKind::Ext;
    case 0xca:
    case 0xcb:
        return WireKind::Double;
    case 0xcc:
    case 0xcd:
    case 0xce:
    case 0xcf:
        return WireKind::Uint;
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3:
        return WireKind::Int;
    case 0xd9:
    case 0xda:
    case 0xdb:
        return WireKind::Str;
    case 0xdc:
    case 0xdd:
        return WireKind::Array;
    case 0xde:
    case 0xdf:
        return WireKind::Map;
    default:
        return WireKind::Unknown;
    }
}

std::uint32_t loadBigEndian32(const char *p)
{
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) << 24)
            | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 16)
            | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 8)
            | static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3]));
}

std::uint64_t loadBigEndian64(const char *p)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[i]));
    }
    return v;
}

qtng::utils::DateTime unpackTimestamp(const std::string &payload)
{
    if (payload.size() == 4) {
        return qtng::utils::DateTime::fromSecsSinceEpoch(
                static_cast<std::int64_t>(loadBigEndian32(payload.data())));
    }
    if (payload.size() == 8) {
        const std::uint64_t t = loadBigEndian64(payload.data());
        const std::uint64_t nsec = (t >> 34) & 0x3fffffffULL;
        const std::uint64_t secs = t & 0x3ffffffffULL;
        return qtng::utils::DateTime::fromMSecsSinceEpoch(
                static_cast<std::int64_t>(secs * 1000 + nsec / 1000000));
    }
    if (payload.size() == 12) {
        const std::uint32_t nsec = loadBigEndian32(payload.data());
        const std::uint64_t secs = loadBigEndian64(payload.data() + 4);
        return qtng::utils::DateTime::fromMSecsSinceEpoch(
                static_cast<std::int64_t>(secs * 1000 + nsec / 1000000));
    }
    return qtng::utils::DateTime();
}

void writeValue(qtng::MsgPackStream &s, const Value &v)
{
    switch (v.type()) {
    case Value::Type::Nil:
        if (!s.writeBytes("\xc0", 1)) {
            throw RpcSerializationException("can not write nil.");
        }
        break;
    case Value::Type::Bool:
        s << v.asBool();
        break;
    case Value::Type::Int:
        s << v.asInt();
        break;
    case Value::Type::Uint:
        s << v.asUint();
        break;
    case Value::Type::Double:
        s << v.asDouble();
        break;
    case Value::Type::Str:
        s << v.asStr();
        break;
    case Value::Type::Bin:
        if (!s.writeBytes(v.asBin())) {
            throw RpcSerializationException("can not write binary data.");
        }
        break;
    case Value::Type::Array: {
        const std::vector<Value> &a = v.asArray();
        if (!s.writeArrayHeader(static_cast<std::uint32_t>(a.size()))) {
            throw RpcSerializationException("can not write array header.");
        }
        for (const Value &e : a) {
            writeValue(s, e);
        }
        break;
    }
    case Value::Type::Map: {
        const std::map<std::string, Value> &m = v.asMap();
        if (!s.writeMapHeader(static_cast<std::uint32_t>(m.size()))) {
            throw RpcSerializationException("can not write map header.");
        }
        for (const std::pair<const std::string, Value> &e : m) {
            s << e.first;
            writeValue(s, e.second);
        }
        break;
    }
    case Value::Type::DateTime: {
        const qtng::utils::DateTime &dt = v.asDateTime();
        if (dt.isValid()) {
            s << dt;
        } else {
            if (!s.writeBytes("\xc0", 1)) {
                throw RpcSerializationException("can not write nil.");
            }
        }
        break;
    }
    case Value::Type::Ext:
        s << v.asExt();
        break;
    case Value::Type::Object:
        // objects are converted by saveState() before packing.
        throw RpcSerializationException("can not pack an object directly, call Value::saveState first.");
    }
    if (!s.isOk()) {
        throw RpcSerializationException("msgpack write failed.");
    }
}

Value readValue(qtng::MsgPackStream &s)
{
    std::uint8_t b = 0;
    if (!s.peekByte(&b)) {
        throw RpcSerializationException("can not read value.");
    }
    switch (wireKind(b)) {
    case WireKind::Nil: {
        char c = 0;
        s.readBytes(&c, 1);
        return Value();
    }
    case WireKind::Bool: {
        bool x = false;
        s >> x;
        return Value(x);
    }
    case WireKind::Int: {
        std::int64_t i = 0;
        s >> i;
        return Value(i);
    }
    case WireKind::Uint: {
        std::uint64_t u = 0;
        s >> u;
        return Value(u);
    }
    case WireKind::Double: {
        double d = 0;
        s >> d;
        return Value(d);
    }
    case WireKind::Str: {
        std::string str;
        if (!s.readString(str)) {
            throw RpcSerializationException("can not read string.");
        }
        return Value::str(std::move(str));
    }
    case WireKind::Bin: {
        std::string bin;
        if (!s.readBytes(bin)) {
            throw RpcSerializationException("can not read binary data.");
        }
        return Value::bin(std::move(bin));
    }
    case WireKind::Array: {
        std::uint32_t len = 0;
        if (!s.readArrayHeader(len)) {
            throw RpcSerializationException("can not read array header.");
        }
        std::vector<Value> l;
        l.reserve(len);
        for (std::uint32_t i = 0; i < len; ++i) {
            l.push_back(readValue(s));
            if (!s.isOk()) {
                throw RpcSerializationException("array element read failed.");
            }
        }
        return Value(std::move(l));
    }
    case WireKind::Map: {
        std::uint32_t len = 0;
        if (!s.readMapHeader(len)) {
            throw RpcSerializationException("can not read map header.");
        }
        std::map<std::string, Value> m;
        for (std::uint32_t i = 0; i < len; ++i) {
            std::string key;
            if (!s.readString(key)) {
                throw RpcSerializationException("can not read map key.");
            }
            Value v = readValue(s);
            if (!s.isOk()) {
                throw RpcSerializationException("map value read failed.");
            }
            m.emplace(std::move(key), std::move(v));
        }
        return Value(std::move(m));
    }
    case WireKind::Ext: {
        qtng::MsgPackExtData ext;
        s >> ext;
        if (!s.isOk()) {
            throw RpcSerializationException("can not read ext data.");
        }
        if (ext.type() == 0xff) {
            const qtng::utils::DateTime dt = unpackTimestamp(ext.payload());
            if (dt.isValid()) {
                return Value(dt);
            }
        }
        return Value(ext);
    }
    default:
        throw RpcSerializationException("unknown msgpack type.");
    }
}

}  // namespace

bool Value::pack(qtng::MsgPackStream &s, const Value &v)
{
    try {
        writeValue(s, saveState(v));
        return s.isOk();
    } catch (...) {
        return false;
    }
}

Value Value::unpack(qtng::MsgPackStream &s)
{
    return restoreState(readValue(s));
}

std::string Value::pack(const Value &v)
{
    std::string buf;
    qtng::MsgPackStream s(&buf, true);
    writeValue(s, saveState(v));
    if (!s.isOk()) {
        throw RpcSerializationException("can not pack value.");
    }
    return s.data();
}

Value Value::unpack(const std::string &data)
{
    qtng::MsgPackStream s(data);
    return restoreState(readValue(s));
}

// ---------------------------------------------------------------------------
// registered-class aware recursive conversion
// ---------------------------------------------------------------------------
Value Value::saveState(const Value &v)
{
    switch (v.type()) {
    case Type::Array: {
        std::vector<Value> l;
        l.reserve(v.asArray().size());
        for (const Value &e : v.asArray()) {
            l.push_back(saveState(e));
        }
        return Value(std::move(l));
    }
    case Type::Map: {
        std::map<std::string, Value> m;
        for (const std::pair<const std::string, Value> &e : v.asMap()) {
            m[e.first] = saveState(e.second);
        }
        return Value(std::move(m));
    }
    case Type::Object: {
        std::shared_ptr<Serializable> obj = v.asObject();
        if (!obj) {
            throw RpcSerializationException("null object can not be serialized.");
        }
        Value state = obj->saveState();
        std::map<std::string, Value> m = state.asMap();
        m[SpecialSidKey] = Value::str(obj->lafrpcKey());
        return Value(std::move(m));
    }
    default:
        return v;
    }
}

Value Value::restoreState(const Value &v)
{
    switch (v.type()) {
    case Type::Array: {
        std::vector<Value> l;
        l.reserve(v.asArray().size());
        for (const Value &e : v.asArray()) {
            l.push_back(restoreState(e));
        }
        return Value(std::move(l));
    }
    case Type::Map: {
        std::map<std::string, Value> m;
        for (const std::pair<const std::string, Value> &e : v.asMap()) {
            m[e.first] = restoreState(e.second);
        }
        std::map<std::string, Value>::iterator sid = m.find(SpecialSidKey);
        if (sid != m.end()) {
            detail::ClassMap &cm = detail::classes();
            detail::ClassMap::iterator it = cm.find(sid->second.asStr());
            if (it == cm.end()) {
                throw RpcSerializationException("unknown class sid: " + sid->second.asStr());
            }
            std::shared_ptr<Serializable> obj = it->second.factory();
            if (!obj->restoreState(Value(std::move(m)))) {
                throw RpcSerializationException("can not restore object: " + it->first);
            }
            return Value(obj);
        }
        return Value(std::move(m));
    }
    default:
        return v;
    }
}

}  // namespace rpc
}  // namespace qtng
