#include <limits>
#include <QtCore/qiodevice.h>
#include <QtCore/qbuffer.h>
#include <QtCore/qstringlist.h>
#include <QtCore/qhash.h>
#include <QtCore/qdebug.h>

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

inline qtng_core::FileLike *ioAdapterFileLike(const QSharedPointer<QIODeviceFileLike> &p)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
    return p.get();
#else
    return p.data();
#endif
}

enum class QVariantWireKind
{
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

// Mirror of qtng_core::detail::msgPackWireKind. The core version lives inside a
// C++17 guard (std::variant support), but the Qt binding must compile as C++11 for
// Qt5, so the wire-kind classification is duplicated here.
QVariantWireKind variantWireKind(quint8 b)
{
    namespace FB = qtng_core::FirstByte;
    if (b <= FB::POSITIVE_FIXINT || b >= FB::NEGATIVE_FIXINT) {
        return QVariantWireKind::Int;
    }
    if (b >= FB::FIXSTR && b <= FB::FIXSTR + 0x1f) {
        return QVariantWireKind::String;
    }
    if (b >= FB::FIXARRAY && b <= FB::FIXARRAY + 0x0f) {
        return QVariantWireKind::Array;
    }
    if (b >= FB::FIXMAP && b <= FB::FIXMAP + 0x0f) {
        return QVariantWireKind::Map;
    }
    if (b >= FB::FIXEXT1 && b <= FB::FIXEX16) {
        return QVariantWireKind::Ext;
    }
    switch (b) {
    case FB::NIL:
        return QVariantWireKind::Nil;
    case FB::MFALSE:
    case FB::MTRUE:
        return QVariantWireKind::Bool;
    case FB::FLOAT32:
    case FB::FLOAT64:
        return QVariantWireKind::Float;
    case FB::BIN8:
    case FB::BIN16:
    case FB::BIN32:
        return QVariantWireKind::Bin;
    case FB::STR8:
    case FB::STR16:
    case FB::STR32:
        return QVariantWireKind::String;
    case FB::UINT8:
    case FB::UINT16:
    case FB::UINT32:
    case FB::UINT64:
    case FB::INT8:
    case FB::INT16:
    case FB::INT32:
    case FB::INT64:
        return QVariantWireKind::Int;
    case FB::ARRAY16:
    case FB::ARRAY32:
        return QVariantWireKind::Array;
    case FB::MAP16:
    case FB::MAP32:
        return QVariantWireKind::Map;
    case FB::EXT8:
    case FB::EXT16:
    case FB::EXT32:
        return QVariantWireKind::Ext;
    default:
        break;
    }
    return QVariantWireKind::Unknown;
}

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
    d->core.setDevice(ioAdapterFileLike(d->ioAdapter));
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
    d->core.setDevice(ioAdapterFileLike(d->ioAdapter));
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
    d->core.setDevice(ioAdapterFileLike(d->ioAdapter));
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

bool MsgPackStream::readString(QString &str)
{
    Q_D(MsgPackStream);
    std::string s;
    if (!d->core.readString(s)) {
        return false;
    }
    str = toQString(s);
    return true;
}

MsgPackStream &MsgPackStream::operator>>(QString &str)
{
    readString(str);
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const QString &str)
{
    Q_D(MsgPackStream);
    d->core << toStdString(str);
    return *this;
}

bool MsgPackStream::readBytes(QByteArray &array)
{
    Q_D(MsgPackStream);
    std::string s;
    if (!d->core.readBytes(s)) {
        return false;
    }
    array = toQByteArray(s);
    return true;
}

MsgPackStream &MsgPackStream::operator>>(QByteArray &array)
{
    readBytes(array);
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const QByteArray &array)
{
    Q_D(MsgPackStream);
    d->core.writeBytes(toStdString(array));
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
    uint8_t b = 0;
    if (!s.peekByte(&b)) {
        if (s.isOk()) {
            s.setStatus(qtng_core::MsgPackStream::ReadPastEnd);
        }
        return *this;
    }
    const QVariantWireKind kind = variantWireKind(b);
    switch (kind) {
    case QVariantWireKind::Nil: {
        char c = 0;
        if (!s.readBytes(&c, 1)) {
            return *this;
        }
        v = QVariant();
        break;
    }
    case QVariantWireKind::Bool: {
        bool x = false;
        s >> x;
        v = x;
        break;
    }
    case QVariantWireKind::Int: {
        // Signed encodings are read as signed integers; everything else is treated as unsigned so that
        // values beyond the qint64 range are not truncated.
        if (b >= qtng_core::FirstByte::NEGATIVE_FIXINT || b == qtng_core::FirstByte::INT8
            || b == qtng_core::FirstByte::INT16 || b == qtng_core::FirstByte::INT32
            || b == qtng_core::FirstByte::INT64) {
            int64_t i = 0;
            s >> i;
            v = QVariant(qint64(i));
        } else {
            uint64_t u = 0;
            s >> u;
            if (u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                v = QVariant(qint64(u));
            } else {
                v = QVariant(quint64(u));
            }
        }
        break;
    }
    case QVariantWireKind::Float: {
        double x = 0;
        s >> x;
        v = x;
        break;
    }
    case QVariantWireKind::String: {
        std::string str;
        s.readString(str);
        v = toQString(str);
        break;
    }
    case QVariantWireKind::Bin: {
        std::string str;
        s.readBytes(str);
        v = toQByteArray(str);
        break;
    }
    case QVariantWireKind::Array: {
        uint32_t len = 0;
        if (!s.readArrayHeader(len)) {
            return *this;
        }
        QVariantList list;
        list.reserve(len);
        for (uint32_t i = 0; i < len; ++i) {
            QVariant item;
            *this >> item;
            if (s.status() != qtng_core::MsgPackStream::Ok) {
                break;
            }
            list.append(item);
        }
        v = list;
        break;
    }
    case QVariantWireKind::Map: {
        uint32_t len = 0;
        if (!s.readMapHeader(len)) {
            return *this;
        }
        QVariantMap map;
        for (uint32_t i = 0; i < len; ++i) {
            std::string key;
            s.readString(key);
            if (s.status() != qtng_core::MsgPackStream::Ok) {
                break;
            }
            QVariant value;
            *this >> value;
            if (s.status() != qtng_core::MsgPackStream::Ok) {
                break;
            }
            map.insert(toQString(key), value);
        }
        v = map;
        break;
    }
    case QVariantWireKind::Ext: {
        // Only the timestamp extension is supported (lafrpc serialization uses QDateTime as its only
        // extension type).
        qtng_core::utils::DateTime dt;
        s >> dt;
        if (s.status() == qtng_core::MsgPackStream::Ok) {
            v = QDateTime::fromMSecsSinceEpoch(dt.toMSecsSinceEpoch());
        } else {
            s.setStatus(qtng_core::MsgPackStream::ReadCorruptData);
            v = QVariant();
        }
        break;
    }
    default:
        qWarning() << "msgpack cannot read QVariant wire kind:" << static_cast<int>(kind)
                   << "first byte:" << static_cast<int>(b);
        s.setStatus(qtng_core::MsgPackStream::ReadCorruptData);
        v = QVariant();
        break;
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const QVariant &v)
{
    Q_D(MsgPackStream);
    qtng_core::MsgPackStream &s = d->core;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    switch (v.typeId()) {
#else
    switch (static_cast<QMetaType::Type>(v.type())) {
#endif
    case QMetaType::Bool:
        s << v.toBool();
        break;
    case QMetaType::Int:
        s << v.toInt();
        break;
    case QMetaType::UInt:
        s << v.toUInt();
        break;
    case QMetaType::LongLong:
        s << static_cast<int64_t>(v.toLongLong());
        break;
    case QMetaType::ULongLong:
        s << static_cast<uint64_t>(v.toULongLong());
        break;
    case QMetaType::Double:
        s << v.toDouble();
        break;
    case QMetaType::Float:
        s << static_cast<double>(v.toFloat());
        break;
    case QMetaType::QString:
        s << toStdString(v.toString());
        break;
    case QMetaType::QStringList: {
        const QStringList &list = v.toStringList();
        if (!s.writeArrayHeader(static_cast<uint32_t>(list.size()))) {
            break;
        }
        for (const QString &str : list) {
            s << toStdString(str);
            if (s.status() != qtng_core::MsgPackStream::Ok) {
                break;
            }
        }
        break;
    }
    case QMetaType::QByteArray:
        s.writeBytes(toStdString(v.toByteArray()));
        break;
    case QMetaType::QDateTime:
        s << qtng_core::utils::DateTime::fromMSecsSinceEpoch(v.toDateTime().toMSecsSinceEpoch());
        break;
    case QMetaType::QVariantList: {
        const QVariantList &list = v.toList();
        if (!s.writeArrayHeader(static_cast<uint32_t>(list.size()))) {
            break;
        }
        for (const QVariant &item : list) {
            *this << item;
            if (s.status() != qtng_core::MsgPackStream::Ok) {
                break;
            }
        }
        break;
    }
    case QMetaType::QVariantMap: {
        const QVariantMap &map = v.toMap();
        if (!s.writeMapHeader(static_cast<uint32_t>(map.size()))) {
            break;
        }
        for (QVariantMap::const_iterator it = map.constBegin(); it != map.constEnd(); ++it) {
            s << toStdString(it.key());
            *this << it.value();
            if (s.status() != qtng_core::MsgPackStream::Ok) {
                break;
            }
        }
        break;
    }
    case QMetaType::QVariantHash: {
        const QVariantHash &hash = v.toHash();
        if (!s.writeMapHeader(static_cast<uint32_t>(hash.size()))) {
            break;
        }
        for (QVariantHash::const_iterator it = hash.constBegin(); it != hash.constEnd(); ++it) {
            s << toStdString(it.key());
            *this << it.value();
            if (s.status() != qtng_core::MsgPackStream::Ok) {
                break;
            }
        }
        break;
    }
    default:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (v.metaType().id() == QMetaType::UnknownType) {
#else
        if (v.type() == QVariant::Invalid) {
#endif
            static const char nilByte[1] = {static_cast<char>(qtng_core::FirstByte::NIL)};
            s.writeBytes(nilByte, 1);
        } else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            qWarning() << "msgpack cannot write QVariant type:" << v.metaType().id() << "name:" << v.typeName();
#else
            qWarning() << "msgpack cannot write QVariant type:" << v.type() << "userType:" << v.userType()
                       << "name:" << v.typeName();
#endif
            s.setStatus(qtng_core::MsgPackStream::WriteFailed);
        }
        break;
    }
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

bool MsgPackStream::writeBytes(const QByteArray &array)
{
    Q_D(MsgPackStream);
    return d->core.writeBytes(toStdString(array));
}

bool MsgPackStream::writeString(const QString &str)
{
    Q_D(MsgPackStream);
    const QByteArray u8 = str.toUtf8();
    return d->core.writeString(u8.constData(), static_cast<quint32>(u8.size()));
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
