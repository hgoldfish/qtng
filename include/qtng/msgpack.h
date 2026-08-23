#ifndef QTNG_MSGPACK_H
#define QTNG_MSGPACK_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "qtng/io_utils.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/platform.h"

#if defined(__has_include) && __has_include(<variant>) \
    && ((defined(_MSVC_LANG) ? _MSVC_LANG : __cplusplus) >= 201703L)
#include <variant>
#endif

namespace qtng {

struct MsgPackExtData
{
    std::uint8_t type;
    std::string payload;
};

class MsgPackExtUserData
{
public:
    virtual ~MsgPackExtUserData();
};

class MsgPackStreamPrivate;
class MsgPackStream
{
public:
    MsgPackStream();
    MsgPackStream(FileLike *d);
    MsgPackStream(std::string *a, bool writeMode = false);
    MsgPackStream(const std::string &a);
    virtual ~MsgPackStream();

    void setDevice(FileLike *d);
    FileLike *device() const;
    std::string data() const;
    bool atEnd() const;

    enum Status { Ok, ReadPastEnd, ReadCorruptData, WriteFailed };
    Status status() const;
    inline bool isOk() const { return status() == Ok; }
    void resetStatus();
    void setStatus(Status status);
    void setFlushWrites(bool flushWrites);
    bool willFlushWrites();
    void setLengthLimit(std::uint32_t limit);
    std::uint32_t lengthLimit() const;
    void setVersion(int version);
    int version() const;
    void setUserData(intptr_t key, MsgPackExtUserData *userData);
    template<typename T>
    T *userData(intptr_t key) const
    {
        return dynamic_cast<T *>(getUserData(key));
    }

    MsgPackStream &operator>>(bool &b);
    MsgPackStream &operator>>(std::uint8_t &u8);
    MsgPackStream &operator>>(std::uint16_t &u16);
    MsgPackStream &operator>>(std::uint32_t &u32);
    MsgPackStream &operator>>(std::uint64_t &u64);
    MsgPackStream &operator>>(std::int8_t &i8);
    MsgPackStream &operator>>(std::int16_t &i16);
    MsgPackStream &operator>>(std::int32_t &i32);
    MsgPackStream &operator>>(std::int64_t &i64);
    MsgPackStream &operator>>(float &f);
    MsgPackStream &operator>>(double &d);
    MsgPackStream &operator>>(qtng::utils::DateTime &dt);
    MsgPackStream &operator>>(qtng::utils::Date &d);
    MsgPackStream &operator>>(MsgPackExtData &ext);
    bool readBytes(char *data, std::int64_t len);
    bool readString(std::string &s);
    bool readBytes(std::string &s);
    bool peekByte(std::uint8_t *b) const;
    bool readArrayHeader(std::uint32_t &len);
    bool readMapHeader(std::uint32_t &len);
    bool readExtHeader(std::uint32_t &len, std::uint8_t msgpackType);

    MsgPackStream &operator<<(bool b);
    MsgPackStream &operator<<(std::uint8_t u8);
    MsgPackStream &operator<<(std::uint16_t u16);
    MsgPackStream &operator<<(std::uint32_t u32);
    MsgPackStream &operator<<(std::uint64_t u64);
    MsgPackStream &operator<<(std::int8_t i8);
    MsgPackStream &operator<<(std::int16_t i16);
    MsgPackStream &operator<<(std::int32_t i32);
    MsgPackStream &operator<<(std::int64_t i64);
    MsgPackStream &operator<<(float f);
    MsgPackStream &operator<<(double d);
    MsgPackStream &operator<<(const std::string &str);
    MsgPackStream &operator<<(const qtng::utils::DateTime &dt);
    MsgPackStream &operator<<(const qtng::utils::Date &d);
    MsgPackStream &operator<<(const MsgPackExtData &ext);

    bool writeBytes(const char *data, std::int64_t len);
    bool writeBytes(const std::string &s);
    bool writeString(const std::string &s);
    bool writeString(const char *data, std::uint32_t len);
    bool writeArrayHeader(std::uint32_t len);
    bool writeMapHeader(std::uint32_t len);
    bool writeExtHeader(std::uint32_t len, std::uint8_t msgpackType);
private:
    MsgPackExtUserData *getUserData(intptr_t key) const;
private:
    MsgPackStreamPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(MsgPackStream)
    NG_DISABLE_COPY(MsgPackStream);
};

namespace FirstByte {
const std::uint8_t POSITIVE_FIXINT = 0x7f;
const std::uint8_t FIXMAP = 0x80;
const std::uint8_t FIXARRAY = 0x90;
const std::uint8_t FIXSTR = 0xa0;
const std::uint8_t NIL = 0xc0;
const std::uint8_t NEVER_USED = 0xc1;
const std::uint8_t MFALSE = 0xc2;
const std::uint8_t MTRUE = 0xc3;
const std::uint8_t BIN8 = 0xc4;
const std::uint8_t BIN16 = 0xc5;
const std::uint8_t BIN32 = 0xc6;
const std::uint8_t EXT8 = 0xc7;
const std::uint8_t EXT16 = 0xc8;
const std::uint8_t EXT32 = 0xc9;
const std::uint8_t FLOAT32 = 0xca;
const std::uint8_t FLOAT64 = 0xcb;
const std::uint8_t UINT8 = 0xcc;
const std::uint8_t UINT16 = 0xcd;
const std::uint8_t UINT32 = 0xce;
const std::uint8_t UINT64 = 0xcf;
const std::uint8_t INT8 = 0xd0;
const std::uint8_t INT16 = 0xd1;
const std::uint8_t INT32 = 0xd2;
const std::uint8_t INT64 = 0xd3;
const std::uint8_t FIXEXT1 = 0xd4;
const std::uint8_t FIXEXT2 = 0xd5;
const std::uint8_t FIXEXT4 = 0xd6;
const std::uint8_t FIXEXT8 = 0xd7;
const std::uint8_t FIXEX16 = 0xd8;
const std::uint8_t STR8 = 0xd9;
const std::uint8_t STR16 = 0xda;
const std::uint8_t STR32 = 0xdb;
const std::uint8_t ARRAY16 = 0xdc;
const std::uint8_t ARRAY32 = 0xdd;
const std::uint8_t MAP16 = 0xde;
const std::uint8_t MAP32 = 0xdf;
const std::uint8_t NEGATIVE_FIXINT = 0xe0;
}  // namespace FirstByte

inline void _msgpack_store8(std::uint8_t *p, std::uint8_t i) { *p = i; }
inline void _msgpack_store8(std::uint8_t *p, std::int8_t i) { *p = static_cast<std::uint8_t>(static_cast<std::int32_t>(i)); }
inline std::uint8_t _msgpack_load8(std::uint8_t *p) { return *p; }

template<typename T>
struct is_shared_ptr : std::false_type
{
};

template<typename Tp>
struct is_shared_ptr<std::shared_ptr<Tp>> : std::true_type
{
};

template<typename T>
inline typename std::enable_if<!is_shared_ptr<T>::value, T>::type s_allocate()
{
    return T();
}

template<typename T>
inline typename std::enable_if<is_shared_ptr<T>::value, T>::type s_allocate()
{
    return std::make_shared<typename T::element_type>();
}

template<typename T>
MsgPackStream &operator<<(MsgPackStream &s, const std::vector<T> &list)
{
    if (!s.writeArrayHeader(static_cast<std::uint32_t>(list.size()))) {
        return s;
    }
    for (const T &item : list) {
        s << item;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
    }
    return s;
}

template<typename T>
MsgPackStream &operator<<(MsgPackStream &s, const std::unordered_set<T> &set)
{
    if (!s.writeArrayHeader(static_cast<std::uint32_t>(set.size()))) {
        return s;
    }
    for (const T &item : set) {
        s << item;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
    }
    return s;
}

template<typename K, typename V>
MsgPackStream &operator<<(MsgPackStream &s, const std::map<K, V> &map)
{
    if (!s.writeMapHeader(static_cast<std::uint32_t>(map.size()))) {
        return s;
    }
    for (const auto &entry : map) {
        s << entry.first << entry.second;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
    }
    return s;
}

template<typename K, typename V>
MsgPackStream &operator<<(MsgPackStream &s, const std::unordered_map<K, V> &map)
{
    if (!s.writeMapHeader(static_cast<std::uint32_t>(map.size()))) {
        return s;
    }
    for (const auto &entry : map) {
        s << entry.first << entry.second;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
    }
    return s;
}

template<typename T>
MsgPackStream &operator>>(MsgPackStream &s, std::vector<T> &list)
{
    std::uint32_t len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    list.clear();
    list.reserve(len);
    for (std::uint32_t i = 0; i < len; ++i) {
        T t = s_allocate<T>();
        s >> t;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
        list.push_back(t);
        if (s.atEnd()) {
            break;
        }
    }
    return s;
}

template<typename T>
MsgPackStream &operator>>(MsgPackStream &s, std::unordered_set<T> &set)
{
    std::uint32_t len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    set.clear();
    set.reserve(len);
    for (std::uint32_t i = 0; i < len; ++i) {
        T t = s_allocate<T>();
        s >> t;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
        set.insert(t);
        if (s.atEnd()) {
            break;
        }
    }
    return s;
}

template<typename K, typename V>
MsgPackStream &operator>>(MsgPackStream &s, std::map<K, V> &map)
{
    std::uint32_t len = 0;
    if (!s.readMapHeader(len)) {
        return s;
    }
    map.clear();
    for (std::uint32_t i = 0; i < len; ++i) {
        K k = s_allocate<K>();
        s >> k;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
        V v = s_allocate<V>();
        s >> v;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
        map.emplace(std::move(k), std::move(v));
    }
    return s;
}

template<typename K, typename V>
MsgPackStream &operator>>(MsgPackStream &s, std::unordered_map<K, V> &map)
{
    std::uint32_t len = 0;
    if (!s.readMapHeader(len)) {
        return s;
    }
    map.clear();
    map.reserve(len);
    for (std::uint32_t i = 0; i < len; ++i) {
        K k = s_allocate<K>();
        s >> k;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
        V v = s_allocate<V>();
        s >> v;
        if (s.status() != MsgPackStream::Ok) {
            break;
        }
        map.emplace(std::move(k), std::move(v));
    }
    return s;
}

#if defined(__has_include) && __has_include(<variant>) \
    && ((defined(_MSVC_LANG) ? _MSVC_LANG : __cplusplus) >= 201703L)
inline MsgPackStream &operator<<(MsgPackStream &s, std::monostate)
{
    static const char nilByte[1] = {static_cast<char>(FirstByte::NIL)};
    s.writeBytes(nilByte, 1);
    return s;
}

inline MsgPackStream &operator>>(MsgPackStream &s, std::monostate)
{
    char b = 0;
    if (!s.readBytes(&b, 1)) {
        return s;
    }
    if (static_cast<std::uint8_t>(b) != FirstByte::NIL) {
        s.setStatus(MsgPackStream::ReadCorruptData);
    }
    return s;
}

namespace detail {

enum class MsgPackWireKind {
    Unknown,
    Nil,
    Bool,
    Int,
    Float,
    String,
    Bin,
    Array,
    Map,
    Ext,
};

inline MsgPackWireKind msgpack_wire_kind(std::uint8_t b)
{
    if (b <= FirstByte::POSITIVE_FIXINT) {
        return MsgPackWireKind::Int;
    }
    if (b >= FirstByte::NEGATIVE_FIXINT) {
        return MsgPackWireKind::Int;
    }
    if (b >= FirstByte::FIXSTR && b <= FirstByte::FIXSTR + 0x1f) {
        return MsgPackWireKind::String;
    }
    if (b >= FirstByte::FIXARRAY && b <= FirstByte::FIXARRAY + 0x0f) {
        return MsgPackWireKind::Array;
    }
    if (b >= FirstByte::FIXMAP && b <= FirstByte::FIXMAP + 0x0f) {
        return MsgPackWireKind::Map;
    }
    if (b >= FirstByte::FIXEXT1 && b <= FirstByte::FIXEX16) {
        return MsgPackWireKind::Ext;
    }
    switch (b) {
    case FirstByte::NIL:
        return MsgPackWireKind::Nil;
    case FirstByte::MFALSE:
    case FirstByte::MTRUE:
        return MsgPackWireKind::Bool;
    case FirstByte::FLOAT32:
    case FirstByte::FLOAT64:
        return MsgPackWireKind::Float;
    case FirstByte::BIN8:
    case FirstByte::BIN16:
    case FirstByte::BIN32:
        return MsgPackWireKind::Bin;
    case FirstByte::STR8:
    case FirstByte::STR16:
    case FirstByte::STR32:
        return MsgPackWireKind::String;
    case FirstByte::UINT8:
    case FirstByte::UINT16:
    case FirstByte::UINT32:
    case FirstByte::UINT64:
    case FirstByte::INT8:
    case FirstByte::INT16:
    case FirstByte::INT32:
    case FirstByte::INT64:
        return MsgPackWireKind::Int;
    case FirstByte::ARRAY16:
    case FirstByte::ARRAY32:
        return MsgPackWireKind::Array;
    case FirstByte::MAP16:
    case FirstByte::MAP32:
        return MsgPackWireKind::Map;
    case FirstByte::EXT8:
    case FirstByte::EXT16:
    case FirstByte::EXT32:
        return MsgPackWireKind::Ext;
    default:
        break;
    }
    return MsgPackWireKind::Unknown;
}

template<typename T, typename = void>
struct variant_wire_match
{
    static bool accepts(MsgPackWireKind) { return false; }
};

template<typename T>
struct variant_wire_match<T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value>::type>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Int; }
};

template<>
struct variant_wire_match<bool>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Bool; }
};

template<>
struct variant_wire_match<float>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Float; }
};

template<>
struct variant_wire_match<double>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Float; }
};

template<>
struct variant_wire_match<std::string>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::String; }
};

template<>
struct variant_wire_match<std::monostate>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Nil; }
};

template<typename T>
struct variant_wire_match<std::vector<T>>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Array; }
};

template<typename T>
struct variant_wire_match<std::unordered_set<T>>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Array; }
};

template<typename K, typename V>
struct variant_wire_match<std::map<K, V>>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Map; }
};

template<typename K, typename V>
struct variant_wire_match<std::unordered_map<K, V>>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Map; }
};

template<>
struct variant_wire_match<MsgPackExtData>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Ext; }
};

template<>
struct variant_wire_match<qtng::utils::DateTime>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Ext; }
};

template<>
struct variant_wire_match<qtng::utils::Date>
{
    static bool accepts(MsgPackWireKind kind) { return kind == MsgPackWireKind::Ext; }
};

template<typename T>
inline void variant_read_value(MsgPackStream &s, T &val)
{
    s >> val;
}

// std::string must be read through readString(); the generic operator>> is not
// available because reading strings via >> is intentionally forbidden.
template<>
inline void variant_read_value<std::string>(MsgPackStream &s, std::string &val)
{
    s.readString(val);
}

template<std::size_t I, typename Variant>
inline bool variant_read_if(MsgPackStream &s, Variant &v, MsgPackWireKind kind, bool &matched)
{
    if (matched) {
        return false;
    }
    using T = typename std::variant_alternative<I, Variant>::type;
    if (!variant_wire_match<T>::accepts(kind)) {
        return false;
    }
    T val = s_allocate<T>();
    variant_read_value(s, val);
    if (s.status() == MsgPackStream::Ok) {
        v.template emplace<I>(std::move(val));
        matched = true;
        return true;
    }
    return false;
}

template<typename Variant, std::size_t... Is>
inline bool variant_read_dispatch(MsgPackStream &s, Variant &v, MsgPackWireKind kind,
                                  std::index_sequence<Is...>)
{
    bool matched = false;
    (variant_read_if<Is>(s, v, kind, matched) || ...);
    return matched;
}

}  // namespace detail

template<typename... Ts>
MsgPackStream &operator<<(MsgPackStream &s, const std::variant<Ts...> &v)
{
    if (v.valueless_by_exception()) {
        s.setStatus(MsgPackStream::WriteFailed);
        return s;
    }
    std::visit([&s](const auto &val) { s << val; }, v);
    return s;
}

template<typename... Ts>
MsgPackStream &operator>>(MsgPackStream &s, std::variant<Ts...> &v)
{
    std::uint8_t b = 0;
    if (!s.peekByte(&b)) {
        if (s.isOk()) {
            s.setStatus(MsgPackStream::ReadPastEnd);
        }
        return s;
    }
    const detail::MsgPackWireKind kind = detail::msgpack_wire_kind(b);
    if (kind == detail::MsgPackWireKind::Unknown) {
        s.setStatus(MsgPackStream::ReadCorruptData);
        return s;
    }
    if (!detail::variant_read_dispatch<std::variant<Ts...>>(
            s, v, kind, std::index_sequence_for<Ts...>{})) {
        if (s.isOk()) {
            s.setStatus(MsgPackStream::ReadCorruptData);
        }
    }
    return s;
}
#endif  // C++17 std::variant support

}  // namespace qtng
#endif  // QTNG_MSGPACK_H
