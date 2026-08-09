#ifndef QTNG_BENCODE_H
#define QTNG_BENCODE_H

#include <QtCore/qbytearray.h>
#include <QtCore/qiodevice.h>
#include <QtCore/qlist.h>
#include <QtCore/qmap.h>
#include <QtCore/qset.h>
#include <QtCore/qstring.h>
#include <QtCore/qsharedpointer.h>
#include "config.h"

QTNETWORKNG_NAMESPACE_BEGIN

class Bencode;

class BencodeStreamPrivate;
class BencodeStream
{
public:
    BencodeStream();
    BencodeStream(QIODevice *d);
    BencodeStream(QByteArray *a, bool writeMode = false);
    BencodeStream(const QByteArray &a);
    virtual ~BencodeStream();

    void setDevice(QIODevice *d);
    QIODevice *device() const;
    QByteArray data() const;
    bool atEnd() const;

    enum Status { Ok, ReadPastEnd, ReadCorruptData, WriteFailed };
    Status status() const;
    inline bool isOk() const { return status() == Ok; }
    void resetStatus();
    void setStatus(Status status);
    void setLengthLimit(quint32 limit);
    quint32 lengthLimit() const;

    BencodeStream &operator>>(qint64 &i);
    BencodeStream &operator>>(QString &str);
    BencodeStream &operator>>(QByteArray &array);
    BencodeStream &operator>>(Bencode &v);
    bool readBytes(char *data, qint64 len);
    bool peekByte(quint8 *b) const;
    bool readArrayHeader(quint32 &len);
    bool readMapHeader(quint32 &len);
    bool readArrayEnd();
    bool readMapEnd();
    bool peekContainerEnd() const;

    BencodeStream &operator<<(qint64 i);
    BencodeStream &operator<<(const QString &str);
    BencodeStream &operator<<(const QByteArray &array);
    BencodeStream &operator<<(const Bencode &v);
    bool writeBytes(const char *data, qint64 len);
    bool writeArrayHeader(quint32 len);
    bool writeMapHeader(quint32 len);
    bool writeArrayEnd();
    bool writeMapEnd();

private:
    BencodeStreamPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(BencodeStream)
    Q_DISABLE_COPY(BencodeStream);
};

template<typename T>
BencodeStream &operator<<(BencodeStream &s, const QList<T> &list)
{
    if (!s.writeArrayHeader(static_cast<quint32>(list.size()))) {
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
BencodeStream &operator<<(BencodeStream &s, const QVector<T> &list)
{
    if (!s.writeArrayHeader(static_cast<quint32>(list.size()))) {
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
BencodeStream &operator<<(BencodeStream &s, const QSet<T> &set)
{
    if (!s.writeArrayHeader(static_cast<quint32>(set.size()))) {
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
BencodeStream &operator<<(BencodeStream &s, const QMap<K, V> &map)
{
    if (!s.writeMapHeader(static_cast<quint32>(map.size()))) {
        return s;
    }
    QMapIterator<K, V> itor(map);
    while (itor.hasNext()) {
        itor.next();
        s << itor.key() << itor.value();
        if (!s.isOk()) {
            break;
        }
    }
    s.writeMapEnd();
    return s;
}

template<typename K, typename V>
BencodeStream &operator<<(BencodeStream &s, const QHash<K, V> &map)
{
    if (!s.writeMapHeader(static_cast<quint32>(map.size()))) {
        return s;
    }
    QHashIterator<K, V> itor(map);
    while (itor.hasNext()) {
        itor.next();
        s << itor.key() << itor.value();
        if (!s.isOk()) {
            break;
        }
    }
    s.writeMapEnd();
    return s;
}

template<typename T>
BencodeStream &operator>>(BencodeStream &s, QList<T> &list)
{
    quint32 len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    Q_UNUSED(len);
    list.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        T item = T();
        s >> item;
        if (!s.isOk()) {
            break;
        }
        list.append(item);
    }
    s.readArrayEnd();
    return s;
}

template<typename T>
BencodeStream &operator>>(BencodeStream &s, QVector<T> &list)
{
    quint32 len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    Q_UNUSED(len);
    list.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        T item = T();
        s >> item;
        if (!s.isOk()) {
            break;
        }
        list.append(item);
    }
    s.readArrayEnd();
    return s;
}

template<typename T>
BencodeStream &operator>>(BencodeStream &s, QSet<T> &set)
{
    quint32 len = 0;
    if (!s.readArrayHeader(len)) {
        return s;
    }
    Q_UNUSED(len);
    set.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        T item = T();
        s >> item;
        if (!s.isOk()) {
            break;
        }
        set.insert(item);
    }
    s.readArrayEnd();
    return s;
}

template<typename K, typename V>
BencodeStream &operator>>(BencodeStream &s, QMap<K, V> &map)
{
    quint32 len = 0;
    if (!s.readMapHeader(len)) {
        return s;
    }
    Q_UNUSED(len);
    map.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        K key = K();
        V value = V();
        s >> key >> value;
        if (!s.isOk()) {
            break;
        }
        map.insert(key, value);
    }
    s.readMapEnd();
    return s;
}

template<typename K, typename V>
BencodeStream &operator>>(BencodeStream &s, QHash<K, V> &map)
{
    quint32 len = 0;
    if (!s.readMapHeader(len)) {
        return s;
    }
    Q_UNUSED(len);
    map.clear();
    while (s.isOk() && !s.peekContainerEnd()) {
        K key = K();
        V value = V();
        s >> key >> value;
        if (!s.isOk()) {
            break;
        }
        map.insert(key, value);
    }
    s.readMapEnd();
    return s;
}

class BencodePrivate;
class Bencode
{
public:
    enum Type { Invalid = 0, Integer, String, List, Dict };

    Bencode();
    Bencode(qint64 i);
    Bencode(const QString &s);
    Bencode(const QByteArray &s);
    Bencode(const char *s);
    Bencode(const QList<Bencode> &list);
    Bencode(QList<Bencode> &&list);
    Bencode(const QMap<QString, Bencode> &dict);
    Bencode(QMap<QString, Bencode> &&dict);
    Bencode(const Bencode &other);
    Bencode &operator=(const Bencode &other);
    ~Bencode();

    static Bencode dict();
    static Bencode list();

    Type type() const;
    bool isValid() const;
    bool isInteger() const;
    bool isString() const;
    bool isList() const;
    bool isDict() const;

    qint64 toInteger(qint64 defaultValue = 0) const;
    QString toString() const;
    QByteArray toByteArray() const;
    QList<Bencode> toList() const;
    QMap<QString, Bencode> toMap() const;

    QByteArray encode() const;
    static Bencode decode(const QByteArray &data, QString *error = nullptr,
                          quint32 lengthLimit = 16 * 1024 * 1024);
    static Bencode decode(QIODevice *device, QString *error = nullptr,
                          quint32 lengthLimit = 16 * 1024 * 1024);

    bool operator==(const Bencode &other) const;
    bool operator!=(const Bencode &other) const { return !(*this == other); }

private:
    friend class BencodeStreamPrivate;
    friend struct BencodeCoreBridge;
    QSharedDataPointer<BencodePrivate> d;
};

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_BENCODE_H
