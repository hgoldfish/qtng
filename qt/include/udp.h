#ifndef QTNG_UDP_H
#define QTNG_UDP_H

#include "socket.h"
#include "config.h"

QTNETWORKNG_NAMESPACE_BEGIN

class DatagramPath
{
public:
    DatagramPath() {}
    explicit DatagramPath(const QString &key)
        : m_key(key)
    {
    }

    QString key() const { return m_key; }
    bool isNull() const { return m_key.isEmpty(); }

    bool operator==(const DatagramPath &other) const { return m_key == other.m_key; }
    bool operator!=(const DatagramPath &other) const { return !(*this == other); }
    bool operator<(const DatagramPath &other) const { return m_key < other.m_key; }
private:
    QString m_key;
};

class DatagramLink
{
public:
    virtual ~DatagramLink();
    virtual qint32 recvfrom(char *data, qint32 size, DatagramPath *who) = 0;
    virtual qint32 sendto(const char *data, qint32 size, const DatagramPath &who) = 0;
    virtual void close() = 0;
    virtual void abort() = 0;
    virtual bool isValid() const = 0;
    virtual Socket::SocketError error() const;
    virtual QString errorString() const;
};

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_UDP_H
