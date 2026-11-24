#include "bridge/core_access.h"
#include "bencode.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

struct BencodeCoreBridge
{
    static QTNG_NAMESPACE::Bencode core(const Bencode &v);
    static void setCore(Bencode &v, const QTNG_NAMESPACE::Bencode &core);
};

namespace {

class QtIODeviceFileLike : public QTNG_NAMESPACE::FileLike
{
public:
    explicit QtIODeviceFileLike(QIODevice *device)
        : dev(device)
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
        return dev ? dev->size() : 0;
    }

    QIODevice *dev;
};

QTNG_NAMESPACE::Bencode toCoreBencode(const Bencode &v);
Bencode fromCoreBencode(const QTNG_NAMESPACE::Bencode &v);

}  // namespace

class BencodePrivate : public QSharedData
{
public:
    QTNG_NAMESPACE::Bencode core;
};

QTNG_NAMESPACE::Bencode BencodeCoreBridge::core(const Bencode &v)
{
    return v.d ? v.d->core : QTNG_NAMESPACE::Bencode();
}

void BencodeCoreBridge::setCore(Bencode &v, const QTNG_NAMESPACE::Bencode &core)
{
    if (!v.d) {
        v.d = new BencodePrivate;
    }
    v.d->core = core;
}

namespace {

QTNG_NAMESPACE::Bencode toCoreBencode(const Bencode &v)
{
    return BencodeCoreBridge::core(v);
}

Bencode fromCoreBencode(const QTNG_NAMESPACE::Bencode &v)
{
    Bencode result;
    BencodeCoreBridge::setCore(result, v);
    return result;
}

}  // namespace

class BencodeStreamPrivate
{
public:
    BencodeStreamPrivate()
        : stream(new QTNG_NAMESPACE::BencodeStream())
        , device(nullptr)
        , qtBuffer(nullptr)
    {
    }

    explicit BencodeStreamPrivate(QIODevice *d)
        : stream(new QTNG_NAMESPACE::BencodeStream(new QtIODeviceFileLike(d)))
        , device(d)
        , ownedDevice(new QtIODeviceFileLike(d))
        , qtBuffer(nullptr)
    {
        stream->setDevice(ownedDevice.get());
    }

    BencodeStreamPrivate(QByteArray *a, bool writeMode)
        : device(nullptr)
        , qtBuffer(a)
    {
        if (writeMode) {
            stringBuffer.reset(new string(toStdString(*a)));
            stream.reset(new QTNG_NAMESPACE::BencodeStream(stringBuffer.get(), QTNG_NAMESPACE::BufferOwnership::Borrow, true));
        } else {
            stream.reset(new QTNG_NAMESPACE::BencodeStream(toStdString(*a)));
        }
    }

    explicit BencodeStreamPrivate(const QByteArray &a)
        : stream(new QTNG_NAMESPACE::BencodeStream(toStdString(a)))
        , device(nullptr)
        , qtBuffer(nullptr)
    {
    }

    unique_ptr<QTNG_NAMESPACE::BencodeStream> stream;
    QIODevice *device;
    unique_ptr<QtIODeviceFileLike> ownedDevice;
    unique_ptr<string> stringBuffer;
    QByteArray *qtBuffer;
};

BencodeStream::BencodeStream()
    : d_ptr(new BencodeStreamPrivate)
{
}

BencodeStream::BencodeStream(QIODevice *d)
    : d_ptr(new BencodeStreamPrivate(d))
{
}

BencodeStream::BencodeStream(QByteArray *a, bool writeMode)
    : d_ptr(new BencodeStreamPrivate(a, writeMode))
{
}

BencodeStream::BencodeStream(const QByteArray &a)
    : d_ptr(new BencodeStreamPrivate(a))
{
}

BencodeStream::~BencodeStream()
{
    delete d_ptr;
}

void BencodeStream::setDevice(QIODevice *device)
{
    Q_D(BencodeStream);
    d->device = device;
    d->ownedDevice.reset(new QtIODeviceFileLike(device));
    d->stream->setDevice(d->ownedDevice.get());
}

QIODevice *BencodeStream::device() const
{
    Q_D(const BencodeStream);
    return d->device;
}

QByteArray BencodeStream::data() const
{
    Q_D(const BencodeStream);
    if (d->qtBuffer && d->stringBuffer) {
        *d->qtBuffer = toQByteArray(d->stream->data());
        return *d->qtBuffer;
    }
    return toQByteArray(d->stream->data());
}

bool BencodeStream::atEnd() const
{
    Q_D(const BencodeStream);
    return d->stream->atEnd();
}

BencodeStream::Status BencodeStream::status() const
{
    Q_D(const BencodeStream);
    return static_cast<Status>(d->stream->status());
}

void BencodeStream::resetStatus()
{
    Q_D(BencodeStream);
    d->stream->resetStatus();
}

void BencodeStream::setStatus(Status status)
{
    Q_D(BencodeStream);
    d->stream->setStatus(static_cast<QTNG_NAMESPACE::BencodeStream::Status>(status));
}

void BencodeStream::setLengthLimit(quint32 limit)
{
    Q_D(BencodeStream);
    d->stream->setLengthLimit(limit);
}

quint32 BencodeStream::lengthLimit() const
{
    Q_D(const BencodeStream);
    return d->stream->lengthLimit();
}

BencodeStream &BencodeStream::operator>>(qint64 &i)
{
    Q_D(BencodeStream);
    int64_t v = 0;
    (*d->stream) >> v;
    i = v;
    return *this;
}

BencodeStream &BencodeStream::operator>>(QString &str)
{
    Q_D(BencodeStream);
    string s;
    (*d->stream) >> s;
    str = toQString(s);
    return *this;
}

BencodeStream &BencodeStream::operator>>(QByteArray &array)
{
    Q_D(BencodeStream);
    string s;
    (*d->stream) >> s;
    array = toQByteArray(s);
    return *this;
}

BencodeStream &BencodeStream::operator>>(Bencode &v)
{
    Q_D(BencodeStream);
    QTNG_NAMESPACE::Bencode core;
    (*d->stream) >> core;
    BencodeCoreBridge::setCore(v, core);
    return *this;
}

bool BencodeStream::readBytes(char *data, qint64 len)
{
    Q_D(BencodeStream);
    return d->stream->readBytes(data, len);
}

bool BencodeStream::peekByte(quint8 *b) const
{
    Q_D(const BencodeStream);
    return d->stream->peekByte(b);
}

bool BencodeStream::readArrayHeader(quint32 &len)
{
    Q_D(BencodeStream);
    uint32_t l = 0;
    const bool ok = d->stream->readArrayHeader(l);
    len = l;
    return ok;
}

bool BencodeStream::readMapHeader(quint32 &len)
{
    Q_D(BencodeStream);
    uint32_t l = 0;
    const bool ok = d->stream->readMapHeader(l);
    len = l;
    return ok;
}

bool BencodeStream::readArrayEnd()
{
    Q_D(BencodeStream);
    return d->stream->readArrayEnd();
}

bool BencodeStream::readMapEnd()
{
    Q_D(BencodeStream);
    return d->stream->readMapEnd();
}

bool BencodeStream::peekContainerEnd() const
{
    Q_D(const BencodeStream);
    return d->stream->peekContainerEnd();
}

BencodeStream &BencodeStream::operator<<(qint64 i)
{
    Q_D(BencodeStream);
    (*d->stream) << static_cast<int64_t>(i);
    return *this;
}

BencodeStream &BencodeStream::operator<<(const QString &str)
{
    Q_D(BencodeStream);
    (*d->stream) << toStdString(str);
    return *this;
}

BencodeStream &BencodeStream::operator<<(const QByteArray &array)
{
    Q_D(BencodeStream);
    (*d->stream) << toStdString(array);
    return *this;
}

BencodeStream &BencodeStream::operator<<(const Bencode &v)
{
    Q_D(BencodeStream);
    (*d->stream) << toCoreBencode(v);
    return *this;
}

bool BencodeStream::writeBytes(const char *data, qint64 len)
{
    Q_D(BencodeStream);
    return d->stream->writeBytes(data, len);
}

bool BencodeStream::writeArrayHeader(quint32 len)
{
    Q_D(BencodeStream);
    return d->stream->writeArrayHeader(len);
}

bool BencodeStream::writeMapHeader(quint32 len)
{
    Q_D(BencodeStream);
    return d->stream->writeMapHeader(len);
}

bool BencodeStream::writeArrayEnd()
{
    Q_D(BencodeStream);
    return d->stream->writeArrayEnd();
}

bool BencodeStream::writeMapEnd()
{
    Q_D(BencodeStream);
    return d->stream->writeMapEnd();
}

Bencode::Bencode()
    : d(new BencodePrivate)
{
}

Bencode::Bencode(qint64 i)
    : d(new BencodePrivate)
{
    d->core = QTNG_NAMESPACE::Bencode(static_cast<int64_t>(i));
}

Bencode::Bencode(const QString &s)
    : d(new BencodePrivate)
{
    d->core = QTNG_NAMESPACE::Bencode(toStdString(s));
}

Bencode::Bencode(const QByteArray &s)
    : d(new BencodePrivate)
{
    d->core = QTNG_NAMESPACE::Bencode(toStdString(s));
}

Bencode::Bencode(const char *s)
    : d(new BencodePrivate)
{
    d->core = QTNG_NAMESPACE::Bencode(s ? s : "");
}

Bencode::Bencode(const QList<Bencode> &list)
    : d(new BencodePrivate)
{
    vector<QTNG_NAMESPACE::Bencode> coreList;
    coreList.reserve(static_cast<size_t>(list.size()));
    for (const Bencode &item : list) {
        coreList.push_back(toCoreBencode(item));
    }
    d->core = QTNG_NAMESPACE::Bencode(coreList);
}

Bencode::Bencode(QList<Bencode> &&list)
    : d(new BencodePrivate)
{
    vector<QTNG_NAMESPACE::Bencode> coreList;
    coreList.reserve(static_cast<size_t>(list.size()));
    for (Bencode &item : list) {
        coreList.push_back(toCoreBencode(item));
    }
    d->core = QTNG_NAMESPACE::Bencode(std::move(coreList));
}

Bencode::Bencode(const QMap<QString, Bencode> &dict)
    : d(new BencodePrivate)
{
    map<string, QTNG_NAMESPACE::Bencode> coreMap;
    for (QMap<QString, Bencode>::const_iterator it = dict.constBegin(); it != dict.constEnd(); ++it) {
        coreMap.emplace(toStdString(it.key()), toCoreBencode(it.value()));
    }
    d->core = QTNG_NAMESPACE::Bencode(coreMap);
}

Bencode::Bencode(QMap<QString, Bencode> &&dict)
    : d(new BencodePrivate)
{
    map<string, QTNG_NAMESPACE::Bencode> coreMap;
    for (QMap<QString, Bencode>::iterator it = dict.begin(); it != dict.end(); ++it) {
        coreMap.emplace(toStdString(it.key()), toCoreBencode(it.value()));
    }
    d->core = QTNG_NAMESPACE::Bencode(std::move(coreMap));
}

Bencode::Bencode(const Bencode &other)
    : d(other.d)
{
}

Bencode &Bencode::operator=(const Bencode &other)
{
    d = other.d;
    return *this;
}

Bencode::~Bencode() = default;

Bencode Bencode::dict()
{
    return fromCoreBencode(QTNG_NAMESPACE::Bencode::dict());
}

Bencode Bencode::list()
{
    return fromCoreBencode(QTNG_NAMESPACE::Bencode::list());
}

Bencode::Type Bencode::type() const
{
    return static_cast<Type>(d->core.type());
}

bool Bencode::isValid() const
{
    return d->core.isValid();
}

bool Bencode::isInteger() const
{
    return d->core.isInteger();
}

bool Bencode::isString() const
{
    return d->core.isString();
}

bool Bencode::isList() const
{
    return d->core.isList();
}

bool Bencode::isDict() const
{
    return d->core.isDict();
}

qint64 Bencode::toInteger(qint64 defaultValue) const
{
    return d->core.toInteger(defaultValue);
}

QString Bencode::toString() const
{
    return toQString(d->core.toString());
}

QByteArray Bencode::toByteArray() const
{
    return toQByteArray(d->core.toString());
}

QList<Bencode> Bencode::toList() const
{
    QList<Bencode> result;
    const vector<QTNG_NAMESPACE::Bencode> &list = d->core.toList();
    result.reserve(static_cast<int>(list.size()));
    for (const QTNG_NAMESPACE::Bencode &item : list) {
        result.append(fromCoreBencode(item));
    }
    return result;
}

QMap<QString, Bencode> Bencode::toMap() const
{
    QMap<QString, Bencode> result;
    const map<string, QTNG_NAMESPACE::Bencode> &dict = d->core.toMap();
    for (const pair<const string, QTNG_NAMESPACE::Bencode> &entry : dict) {
        result.insert(toQString(entry.first), fromCoreBencode(entry.second));
    }
    return result;
}

QByteArray Bencode::encode() const
{
    return toQByteArray(d->core.encode());
}

Bencode Bencode::decode(const QByteArray &data, QString *error, quint32 lengthLimit)
{
    string coreError;
    const QTNG_NAMESPACE::Bencode value =
            QTNG_NAMESPACE::Bencode::decode(toStdString(data), error ? &coreError : nullptr, lengthLimit);
    if (error) {
        *error = toQString(coreError);
    }
    return fromCoreBencode(value);
}

Bencode Bencode::decode(QIODevice *device, QString *error, quint32 lengthLimit)
{
    QtIODeviceFileLike fileLike(device);
    string coreError;
    const QTNG_NAMESPACE::Bencode value =
            QTNG_NAMESPACE::Bencode::decode(&fileLike, error ? &coreError : nullptr, lengthLimit);
    if (error) {
        *error = toQString(coreError);
    }
    return fromCoreBencode(value);
}

bool Bencode::operator==(const Bencode &other) const
{
    return d->core == other.d->core;
}

}  // namespace QTNETWORKNG_NAMESPACE
