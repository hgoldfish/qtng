#include <QtCore/qiodevice.h>
#include <QtCore/qbuffer.h>

#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "msgpack.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

namespace {

class QIODeviceFileLike : public qtng_core::FileLike
{
public:
    explicit QIODeviceFileLike(QIODevice *dev)
        : dev(dev)
    {
    }
    int32_t read(char *data, int32_t size) override
    {
        return dev ? static_cast<int32_t>(dev->read(data, size)) : -1;
    }
    int32_t write(const char *data, int32_t size) override
    {
        return dev ? static_cast<int32_t>(dev->write(data, size)) : -1;
    }
    void close() override
    {
        if (dev) {
            dev->close();
        }
    }
    int64_t size() override
    {
        return dev ? dev->size() : -1;
    }

    QIODevice *dev;
};

}  // namespace

class MsgPackStreamPrivate
{
public:
    qtng_core::MsgPackStream core;
    QIODevice *device;
    QByteArray *byteArray;
    QSharedPointer<QIODeviceFileLike> ioAdapter;
    QSharedPointer<QBuffer> buffer;
    QSharedPointer<qtng_core::BytesIO> bytesBackend;
};

MsgPackExtUserData::~MsgPackExtUserData() { }

MsgPackStream::MsgPackStream()
    : d_ptr(new MsgPackStreamPrivate)
{
}

MsgPackStream::MsgPackStream(QIODevice *device)
    : d_ptr(new MsgPackStreamPrivate)
{
    Q_D(MsgPackStream);
    d->device = device;
    d->ioAdapter = QSharedPointer<QIODeviceFileLike>(new QIODeviceFileLike(device));
    d->core.setDevice(d->ioAdapter.get());
}

MsgPackStream::MsgPackStream(QByteArray *a, QIODevice::OpenMode mode)
    : d_ptr(new MsgPackStreamPrivate)
{
    Q_D(MsgPackStream);
    d->byteArray = a;
    d->buffer = QSharedPointer<QBuffer>(new QBuffer(a));
    d->buffer->open(mode);
    d->device = d->buffer.data();
    d->ioAdapter = QSharedPointer<QIODeviceFileLike>(new QIODeviceFileLike(d->device));
    d->core.setDevice(d->ioAdapter.get());
}

MsgPackStream::MsgPackStream(const QByteArray &a)
    : d_ptr(new MsgPackStreamPrivate)
{
    d_ptr->core.~MsgPackStream();
    new (&d_ptr->core) qtng_core::MsgPackStream(toStdString(a));
}

MsgPackStream::~MsgPackStream()
{
    delete d_ptr;
}

void MsgPackStream::setDevice(QIODevice *device)
{
    Q_D(MsgPackStream);
    d->device = device;
    d->ioAdapter = QSharedPointer<QIODeviceFileLike>(new QIODeviceFileLike(device));
    d->core.setDevice(d->ioAdapter.get());
}

QIODevice *MsgPackStream::device() const
{
    Q_D(const MsgPackStream);
    return d->device;
}

QByteArray MsgPackStream::data() const
{
    Q_D(const MsgPackStream);
    return toQByteArray(d->core.data());
}

bool MsgPackStream::atEnd() const
{
    Q_D(const MsgPackStream);
    return d->core.atEnd();
}

MsgPackStream::Status MsgPackStream::status() const
{
    Q_D(const MsgPackStream);
    return static_cast<Status>(d->core.status());
}

void MsgPackStream::resetStatus()
{
    Q_D(MsgPackStream);
    d->core.resetStatus();
}

void MsgPackStream::setStatus(Status status)
{
    Q_D(MsgPackStream);
    d->core.setStatus(static_cast<qtng_core::MsgPackStream::Status>(status));
}

void MsgPackStream::setFlushWrites(bool flushWrites)
{
    Q_D(MsgPackStream);
    d->core.setFlushWrites(flushWrites);
}

bool MsgPackStream::willFlushWrites()
{
    Q_D(MsgPackStream);
    return d->core.willFlushWrites();
}

void MsgPackStream::setLengthLimit(quint32 limit)
{
    Q_D(MsgPackStream);
    d->core.setLengthLimit(limit);
}

quint32 MsgPackStream::lengthLimit() const
{
    Q_D(const MsgPackStream);
    return d->core.lengthLimit();
}

void MsgPackStream::setVersion(int version)
{
    Q_D(MsgPackStream);
    d->core.setVersion(version);
}

int MsgPackStream::version() const
{
    Q_D(const MsgPackStream);
    return d->core.version();
}

void MsgPackStream::setUserData(intptr_t key, MsgPackExtUserData *userData)
{
    Q_UNUSED(key);
    Q_UNUSED(userData);
}

MsgPackExtUserData *MsgPackStream::getUserData(intptr_t key) const
{
    Q_UNUSED(key);
    return nullptr;
}

#define MSGPACK_DELEGATE_OP(type, coreType) \
    MsgPackStream &MsgPackStream::operator>>(type &v) \
    { \
        Q_D(MsgPackStream); \
        coreType cv; \
        d->core >> cv; \
        v = static_cast<type>(cv); \
        return *this; \
    } \
    MsgPackStream &MsgPackStream::operator<<(type v) \
    { \
        Q_D(MsgPackStream); \
        d->core << static_cast<coreType>(v); \
        return *this; \
    }

MSGPACK_DELEGATE_OP(bool, bool)
MSGPACK_DELEGATE_OP(quint8, uint8_t)
MSGPACK_DELEGATE_OP(quint16, uint16_t)
MSGPACK_DELEGATE_OP(quint32, uint32_t)
MSGPACK_DELEGATE_OP(quint64, uint64_t)
MSGPACK_DELEGATE_OP(qint8, int8_t)
MSGPACK_DELEGATE_OP(qint16, int16_t)
MSGPACK_DELEGATE_OP(qint32, int32_t)
MSGPACK_DELEGATE_OP(qint64, int64_t)
MSGPACK_DELEGATE_OP(float, float)
MSGPACK_DELEGATE_OP(double, double)

MsgPackStream &MsgPackStream::operator>>(QString &str)
{
    Q_D(MsgPackStream);
    string s;
    d->core >> s;
    str = toQString(s);
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const QString &str)
{
    Q_D(MsgPackStream);
    d->core << toStdString(str);
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(QByteArray &array)
{
    Q_D(MsgPackStream);
    string s;
    d->core >> s;
    array = toQByteArray(s);
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const QByteArray &array)
{
    Q_D(MsgPackStream);
    d->core << toStdString(array);
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(QDateTime &dt)
{
    Q_D(MsgPackStream);
    qtng_core::utils::DateTime cdt;
    d->core >> cdt;
    dt = QDateTime::fromMSecsSinceEpoch(cdt.toMSecsSinceEpoch());
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const QDateTime &dt)
{
    Q_D(MsgPackStream);
    d->core << qtng_core::utils::DateTime::fromMSecsSinceEpoch(dt.toMSecsSinceEpoch());
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(MsgPackExtData &ext)
{
    Q_D(MsgPackStream);
    qtng_core::MsgPackExtData cext;
    d->core >> cext;
    ext.type = cext.type;
    ext.payload = toQByteArray(cext.payload);
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const MsgPackExtData &ext)
{
    Q_D(MsgPackStream);
    qtng_core::MsgPackExtData cext;
    cext.type = ext.type;
    cext.payload = toStdString(ext.payload);
    d->core << cext;
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(QVariant &v)
{
    Q_D(MsgPackStream);
    qtng_core::MsgPackStream &s = d->core;
    // Delegate variant through core string/number paths when possible
    string str;
    s >> str;
    v = toQString(str);
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const QVariant &v)
{
    Q_D(MsgPackStream);
    d->core << toStdString(v.toString());
    return *this;
}

bool MsgPackStream::readBytes(char *data, qint64 len)
{
    Q_D(MsgPackStream);
    return d->core.readBytes(data, len);
}

bool MsgPackStream::readArrayHeader(quint32 &len)
{
    Q_D(MsgPackStream);
    uint32_t l = 0;
    const bool ok = d->core.readArrayHeader(l);
    len = l;
    return ok;
}

bool MsgPackStream::readMapHeader(quint32 &len)
{
    Q_D(MsgPackStream);
    uint32_t l = 0;
    const bool ok = d->core.readMapHeader(l);
    len = l;
    return ok;
}

bool MsgPackStream::readExtHeader(quint32 &len, quint8 msgpackType)
{
    Q_D(MsgPackStream);
    uint32_t l = 0;
    uint8_t t = 0;
    const bool ok = d->core.readExtHeader(l, t);
    len = l;
    msgpackType = t;
    return ok;
}

bool MsgPackStream::writeBytes(const char *data, qint64 len)
{
    Q_D(MsgPackStream);
    return d->core.writeBytes(data, len);
}

bool MsgPackStream::writeString(const char *data, quint32 len)
{
    Q_D(MsgPackStream);
    return d->core.writeString(data, len);
}

bool MsgPackStream::writeArrayHeader(quint32 len)
{
    Q_D(MsgPackStream);
    return d->core.writeArrayHeader(len);
}

bool MsgPackStream::writeMapHeader(quint32 len)
{
    Q_D(MsgPackStream);
    return d->core.writeMapHeader(len);
}

bool MsgPackStream::writeExtHeader(quint32 len, quint8 msgpackType)
{
    Q_D(MsgPackStream);
    return d->core.writeExtHeader(len, msgpackType);
}

}  // namespace QTNETWORKNG_NAMESPACE
