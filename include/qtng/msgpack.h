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
    MsgPackStream &operator>>(std::string &str);
    MsgPackStream &operator>>(qtng::utils::DateTime &dt);
    MsgPackStream &operator>>(MsgPackExtData &ext);
    bool readBytes(char *data, std::int64_t len);
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
    MsgPackStream &operator<<(const MsgPackExtData &ext);

    bool writeBytes(const char *data, std::int64_t len);
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
#include <variant>

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
template<std::size_t I, typename Variant>
inline bool variant_unpack_one(MsgPackStream &s, Variant &v, std::size_t index)
{
    if (index != I) {
        return false;
    }
    using T = typename std::variant_alternative<I, Variant>::type;
    T val = s_allocate<T>();
    s >> val;
    if (s.status() == MsgPackStream::Ok) {
        v.template emplace<I>(std::move(val));
    }
    return true;
}

template<typename Variant, std::size_t... Is>
inline void variant_unpack_dispatch(MsgPackStream &s, Variant &v, std::size_t index,
                                    std::index_sequence<Is...>)
{
    const bool matched = (variant_unpack_one<Is>(s, v, index) || ...);
    (void) matched;
}
}  // namespace detail

template<typename... Ts>
MsgPackStream &operator<<(MsgPackStream &s, const std::variant<Ts...> &v)
{
    if (v.valueless_by_exception()) {
        s.setStatus(MsgPackStream::WriteFailed);
        return s;
    }
    if (!s.writeArrayHeader(2)) {
        return s;
    }
    s << static_cast<std::uint32_t>(v.index());
    if (s.status() != MsgPackStream::Ok) {
        return s;
    }
    std::visit([&s](const auto &val) { s << val; }, v);
    return s;
}

template<typename... Ts>
MsgPackStream &operator>>(MsgPackStream &s, std::variant<Ts...> &v)
{
    std::uint32_t len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    if (len != 2) {
        s.setStatus(MsgPackStream::ReadCorruptData);
        return s;
    }
    std::uint32_t index = 0;
    s >> index;
    if (s.status() != MsgPackStream::Ok) {
        return s;
    }
    if (index >= sizeof...(Ts)) {
        s.setStatus(MsgPackStream::ReadCorruptData);
        return s;
    }
    detail::variant_unpack_dispatch<std::variant<Ts...>>(
        s, v, static_cast<std::size_t>(index), std::index_sequence_for<Ts...>{});
    return s;
}
#endif  // C++17 std::variant support

}  // namespace qtng
#endif  // QTNG_MSGPACK_H
