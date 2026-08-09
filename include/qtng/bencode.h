#ifndef QTNG_BENCODE_H
#define QTNG_BENCODE_H

#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtng/io_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

class Bencode;

class BencodeStreamPrivate;
class BencodeStream
{
public:
    BencodeStream();
    BencodeStream(FileLike *d);
    BencodeStream(std::string *a, bool writeMode = false);
    BencodeStream(const std::string &a);
    virtual ~BencodeStream();

    void setDevice(FileLike *d);
    FileLike *device() const;
    std::string data() const;
    bool atEnd() const;

    enum Status { Ok, ReadPastEnd, ReadCorruptData, WriteFailed };
    Status status() const;
    inline bool isOk() const { return status() == Ok; }
    void resetStatus();
    void setStatus(Status status);
    void setLengthLimit(std::uint32_t limit);
    std::uint32_t lengthLimit() const;

    BencodeStream &operator>>(std::int64_t &i);
    BencodeStream &operator>>(std::string &str);
    BencodeStream &operator>>(Bencode &v);
    bool readBytes(char *data, std::int64_t len);
    bool peekByte(std::uint8_t *b) const;
    // Bencode lists/dicts have no length prefix; len is set to UINT32_MAX (indefinite).
    // Use peekContainerEnd() + readArrayEnd()/readMapEnd() when iterating.
    bool readArrayHeader(std::uint32_t &len);
    bool readMapHeader(std::uint32_t &len);
    bool readArrayEnd();
    bool readMapEnd();
    bool peekContainerEnd() const;

    BencodeStream &operator<<(std::int64_t i);
    BencodeStream &operator<<(const std::string &str);
    BencodeStream &operator<<(const Bencode &v);
    bool writeBytes(const char *data, std::int64_t len);
    // len is accepted for MsgPackStream API parity; bencode encodes only the 'l'/'d' marker.
    bool writeArrayHeader(std::uint32_t len);
    bool writeMapHeader(std::uint32_t len);
    bool writeArrayEnd();
    bool writeMapEnd();

private:
    BencodeStreamPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(BencodeStream)
    NG_DISABLE_COPY(BencodeStream);
};

template<typename T>
BencodeStream &operator<<(BencodeStream &s, const std::vector<T> &list)
{
    if (!s.writeArrayHeader(static_cast<std::uint32_t>(list.size()))) {
        return s;
    }
    for (const T &item : list) {
        s << item;
        if (!s.isOk()) {
            break;
        }
    }
    s.writeArrayEnd();
    return s;
}

template<typename T>
BencodeStream &operator<<(BencodeStream &s, const std::list<T> &list)
{
    if (!s.writeArrayHeader(static_cast<std::uint32_t>(list.size()))) {
        return s;
    }
    for (const T &item : list) {
        s << item;
        if (!s.isOk()) {
            break;
        }
    }
    s.writeArrayEnd();
    return s;
}

template<typename T>
BencodeStream &operator<<(BencodeStream &s, const std::set<T> &set)
{
    if (!s.writeArrayHeader(static_cast<std::uint32_t>(set.size()))) {
        return s;
    }
    for (const T &item : set) {
        s << item;
        if (!s.isOk()) {
            break;
        }
    }
    s.writeArrayEnd();
    return s;
}

template<typename T>
BencodeStream &operator<<(BencodeStream &s, const std::unordered_set<T> &set)
{
    if (!s.writeArrayHeader(static_cast<std::uint32_t>(set.size()))) {
        return s;
    }
    for (const T &item : set) {
        s << item;
        if (!s.isOk()) {
            break;
        }
    }
    s.writeArrayEnd();
    return s;
}

template<typename K, typename V>
BencodeStream &operator<<(BencodeStream &s, const std::map<K, V> &map)
{
    if (!s.writeMapHeader(static_cast<std::uint32_t>(map.size()))) {
        return s;
    }
    for (const auto &entry : map) {
        s << entry.first << entry.second;
        if (!s.isOk()) {
            break;
        }
    }
    s.writeMapEnd();
    return s;
}

template<typename K, typename V>
BencodeStream &operator<<(BencodeStream &s, const std::unordered_map<K, V> &map)
{
    if (!s.writeMapHeader(static_cast<std::uint32_t>(map.size()))) {
        return s;
    }
    for (const auto &entry : map) {
        s << entry.first << entry.second;
        if (!s.isOk()) {
            break;
        }
    }
    s.writeMapEnd();
    return s;
}

template<typename T>
BencodeStream &operator>>(BencodeStream &s, std::vector<T> &list)
{
    std::uint32_t len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    (void) len;
    list.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        T item = T();
        s >> item;
        if (!s.isOk()) {
            break;
        }
        list.push_back(std::move(item));
    }
    s.readArrayEnd();
    return s;
}

template<typename T>
BencodeStream &operator>>(BencodeStream &s, std::list<T> &list)
{
    std::uint32_t len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    (void) len;
    list.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        T item = T();
        s >> item;
        if (!s.isOk()) {
            break;
        }
        list.push_back(std::move(item));
    }
    s.readArrayEnd();
    return s;
}

template<typename T>
BencodeStream &operator>>(BencodeStream &s, std::set<T> &set)
{
    std::uint32_t len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    (void) len;
    set.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        T item = T();
        s >> item;
        if (!s.isOk()) {
            break;
        }
        set.insert(std::move(item));
    }
    s.readArrayEnd();
    return s;
}

template<typename T>
BencodeStream &operator>>(BencodeStream &s, std::unordered_set<T> &set)
{
    std::uint32_t len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    (void) len;
    set.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        T item = T();
        s >> item;
        if (!s.isOk()) {
            break;
        }
        set.insert(std::move(item));
    }
    s.readArrayEnd();
    return s;
}

template<typename K, typename V>
BencodeStream &operator>>(BencodeStream &s, std::map<K, V> &map)
{
    std::uint32_t len = 0;
    if (!s.readMapHeader(len)) {
        return s;
    }
    (void) len;
    map.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        K key = K();
        V value = V();
        s >> key >> value;
        if (!s.isOk()) {
            break;
        }
        map.emplace(std::move(key), std::move(value));
    }
    s.readMapEnd();
    return s;
}

template<typename K, typename V>
BencodeStream &operator>>(BencodeStream &s, std::unordered_map<K, V> &map)
{
    std::uint32_t len = 0;
    if (!s.readMapHeader(len)) {
        return s;
    }
    (void) len;
    map.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        K key = K();
        V value = V();
        s >> key >> value;
        if (!s.isOk()) {
            break;
        }
        map.emplace(std::move(key), std::move(value));
    }
    s.readMapEnd();
    return s;
}

class Bencode
{
public:
    enum Type { Invalid = 0, Integer, String, List, Dict };

    Bencode();
    Bencode(std::int64_t i);
    Bencode(const std::string &s);
    Bencode(const char *s);
    Bencode(const std::vector<Bencode> &list);
    Bencode(std::vector<Bencode> &&list);
    Bencode(const std::map<std::string, Bencode> &dict);
    Bencode(std::map<std::string, Bencode> &&dict);
    Bencode(const Bencode &other);
    Bencode &operator=(const Bencode &other);
    ~Bencode();

    static Bencode dict();
    static Bencode list();

    Type type() const { return m_type; }
    bool isValid() const { return m_type != Invalid; }
    bool isInteger() const { return m_type == Integer; }
    bool isString() const { return m_type == String; }
    bool isList() const { return m_type == List; }
    bool isDict() const { return m_type == Dict; }

    std::int64_t toInteger(std::int64_t defaultValue = 0) const;
    std::string toString() const;
    const std::vector<Bencode> &toList() const;
    const std::map<std::string, Bencode> &toMap() const;

    std::string encode() const;
    static Bencode decode(const std::string &data, std::string *error = nullptr,
                          std::uint32_t lengthLimit = 16 * 1024 * 1024);
    static Bencode decode(FileLike *device, std::string *error = nullptr,
                          std::uint32_t lengthLimit = 16 * 1024 * 1024);

    bool operator==(const Bencode &other) const;
    bool operator!=(const Bencode &other) const { return !(*this == other); }

private:
    friend class BencodeStreamPrivate;
    Type m_type;
    std::int64_t m_integer;
    std::string m_string;
    std::vector<Bencode> m_list;
    std::map<std::string, Bencode> m_dict;
};

}  // namespace qtng

#endif  // QTNG_BENCODE_H
