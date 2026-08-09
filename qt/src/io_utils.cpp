#include <QtCore/qdir.h>
#include <QtCore/qbuffer.h>
#include <QtCore/qdebug.h>

#include "bridge/core_access.h"
#include "bridge/stream_bridge.h"
#include "io_utils.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

FileLike::~FileLike() { }

QByteArray FileLike::readall(bool *ok)
{
    QByteArray data;
    const qint64 s = size();
    if (s >= static_cast<qint64>(INT32_MAX)) {
        if (ok) {
            *ok = false;
        }
        return data;
    }
    if (s > 0) {
        data.reserve(static_cast<int>(s));
    }
    char buf[1024 * 8];
    while (true) {
        const qint32 readBytes = read(buf, static_cast<qint32>(sizeof(buf)));
        if (readBytes <= 0) {
            if (ok) {
                *ok = (s < 0 || data.size() == s);
            }
            return data;
        }
        data.append(buf, readBytes);
    }
}

QByteArray FileLike::read(qint32 size)
{
    QByteArray buf(size, Qt::Uninitialized);
    const qint32 readBytes = read(buf.data(), size);
    if (readBytes <= 0) {
        return QByteArray();
    }
    if (readBytes < size) {
        buf.resize(readBytes);
    }
    return buf;
}

qint32 FileLike::write(const QByteArray &data)
{
    return write(data.constData(), data.size());
}

QSharedPointer<FileLike> FileLike::rawFile(QSharedPointer<QFile> f)
{
    return QSharedPointer<FileLike>(new RawFile(f));
}

QSharedPointer<FileLike> FileLike::open(const QString &filepath, const QString &mode)
{
    return toQtFileLike(qtng_core::FileLike::open(toStdString(filepath), toStdString(mode)));
}

QSharedPointer<FileLike> FileLike::bytes(const QByteArray &data)
{
    return toQtFileLike(qtng_core::FileLike::bytes(toStdString(data)));
}

QSharedPointer<FileLike> FileLike::bytes(QByteArray *data)
{
    return toQtFileLike(qtng_core::FileLike::bytes(reinterpret_cast<string *>(data)));
}

qint32 RawFile::read(char *data, qint32 size)
{
    if (f.isNull()) {
        return -1;
    }
    return static_cast<qint32>(f->read(data, size));
}

qint32 RawFile::write(const char *data, qint32 size)
{
    if (f.isNull()) {
        return -1;
    }
    return static_cast<qint32>(f->write(data, size));
}

void RawFile::close()
{
    if (!f.isNull()) {
        f->close();
    }
}

qint64 RawFile::size()
{
    if (f.isNull()) {
        return -1;
    }
    return f->size();
}

bool RawFile::seek(qint64 pos)
{
    return !f.isNull() && f->seek(pos);
}

QString RawFile::fileName() const
{
    return f.isNull() ? QString() : f->fileName();
}

QSharedPointer<RawFile> RawFile::open(const QString &filepath, const QString &mode)
{
    QSharedPointer<QFile> file(new QFile(filepath));
    QIODevice::OpenMode openMode = QIODevice::ReadOnly;
    if (mode == QStringLiteral("w")) {
        openMode = QIODevice::WriteOnly | QIODevice::Truncate;
    } else if (mode == QStringLiteral("a")) {
        openMode = QIODevice::WriteOnly | QIODevice::Append;
    } else if (mode == QStringLiteral("r+")) {
        openMode = QIODevice::ReadWrite;
    } else if (mode == QStringLiteral("w+")) {
        openMode = QIODevice::ReadWrite | QIODevice::Truncate;
    } else if (mode == QStringLiteral("a+")) {
        openMode = QIODevice::ReadWrite | QIODevice::Append;
    }
    if (!file->open(openMode)) {
        return QSharedPointer<RawFile>();
    }
    return QSharedPointer<RawFile>(new RawFile(file));
}

QSharedPointer<RawFile> RawFile::open(const QString &filepath, QIODevice::OpenMode mode)
{
    QSharedPointer<QFile> file(new QFile(filepath));
    if (!file->open(mode)) {
        return QSharedPointer<RawFile>();
    }
    return QSharedPointer<RawFile>(new RawFile(file));
}

class BytesIOPrivate
{
public:
    shared_ptr<qtng_core::BytesIO> core;
    QByteArray *externalBuf;
};

BytesIO::BytesIO(const QByteArray &buf, qint32 pos)
    : d_ptr(new BytesIOPrivate)
{
    d_ptr->externalBuf = nullptr;
    d_ptr->core = make_shared<qtng_core::BytesIO>(toStdString(buf), pos);
}

BytesIO::BytesIO(QByteArray *buf, qint32 pos)
    : d_ptr(new BytesIOPrivate)
{
    d_ptr->externalBuf = buf;
    d_ptr->core = make_shared<qtng_core::BytesIO>(reinterpret_cast<string *>(buf), pos);
}

BytesIO::BytesIO()
    : d_ptr(new BytesIOPrivate)
{
    d_ptr->core = make_shared<qtng_core::BytesIO>();
}

BytesIO::~BytesIO()
{
    delete d_ptr;
}

qint32 BytesIO::read(char *data, qint32 size)
{
    return d_ptr->core->read(data, size);
}

qint32 BytesIO::write(const char *data, qint32 size)
{
    return d_ptr->core->write(data, size);
}

void BytesIO::close()
{
    d_ptr->core->close();
}

qint64 BytesIO::size()
{
    return d_ptr->core->size();
}

QByteArray BytesIO::readall(bool *ok)
{
    bool coreOk = false;
    const string data = d_ptr->core->readall(&coreOk);
    if (ok) {
        *ok = coreOk;
    }
    return toQByteArray(data);
}

QByteArray BytesIO::data()
{
    if (d_ptr->externalBuf) {
        return *d_ptr->externalBuf;
    }
    return toQByteArray(d_ptr->core->data());
}

bool sendfile(QSharedPointer<FileLike> inputFile, QSharedPointer<FileLike> outputFile, qint64 bytesToCopy,
              int suitableBlockSize)
{
    return qtng_core::sendfile(toCoreFileLike(inputFile), toCoreFileLike(outputFile), bytesToCopy, suitableBlockSize);
}

class PipePrivate
{
public:
    shared_ptr<qtng_core::Pipe> core;
    QPointer<Pipe> q;
};

Pipe::Pipe(qint32 maxBufferSize)
    : d(QSharedPointer<PipePrivate>::create())
{
    d->core = make_shared<qtng_core::Pipe>(maxBufferSize);
    d->q = this;
    QWeakPointer<PipePrivate> weak = d;
    d->core->setReadyReadCallback([weak]() {
        if (QSharedPointer<PipePrivate> priv = weak.toStrongRef()) {
            if (priv->q) {
                emit priv->q->readyRead();
            }
        }
    });
    d->core->setBytesWrittenCallback([weak](int64_t bytes) {
        if (QSharedPointer<PipePrivate> priv = weak.toStrongRef()) {
            if (priv->q) {
                emit priv->q->bytesWritten(static_cast<qint64>(bytes));
            }
        }
    });
}

void Pipe::setDebugLevel(qint8 debugLevel)
{
    d->core->setDebugLevel(debugLevel);
}

QSharedPointer<FileLike> Pipe::fileToRead(bool takePipe)
{
    return toQtFileLike(d->core->fileToRead(takePipe));
}

QSharedPointer<FileLike> Pipe::fileToWrite(bool takePipe)
{
    return toQtFileLike(d->core->fileToWrite(takePipe));
}

QSharedPointer<QIODevice> Pipe::deviceToRead(bool connectSignals, bool takePipe)
{
    QSharedPointer<FileLike> file = fileToRead(takePipe);
    QSharedPointer<QIODevice> device(new QBuffer());
    if (connectSignals) {
        QObject::connect(this, &Pipe::readyRead, device.data(), &QIODevice::readyRead);
    }
    (void)file;
    return device;
}

QSharedPointer<QIODevice> Pipe::deviceToWrite(bool connectSignals, bool takePipe)
{
    QSharedPointer<FileLike> file = fileToWrite(takePipe);
    QSharedPointer<QIODevice> device(new QBuffer());
    if (connectSignals) {
        QObject::connect(this, &Pipe::bytesWritten, device.data(), &QIODevice::bytesWritten);
    }
    (void)file;
    return device;
}

class PosixPathPrivate : public QSharedData
{
public:
    qtng_core::PosixPath core;
};

PosixPath::PosixPath()
    : d(nullptr)
{
}

PosixPath::PosixPath(const QString &path)
    : d(new PosixPathPrivate)
{
    d->core = qtng_core::PosixPath(toStdString(path));
}

PosixPath::PosixPath(const PosixPath &other)
    : d(other.d)
{
}

PosixPath::PosixPath(PosixPath &&other)
    : d(std::move(other.d))
{
}

PosixPath::~PosixPath() = default;

PosixPath &PosixPath::operator=(const PosixPath &other)
{
    d = other.d;
    return *this;
}

PosixPath &PosixPath::operator=(PosixPath &&other) noexcept
{
    d = std::move(other.d);
    return *this;
}

bool PosixPath::operator==(const PosixPath &other) const
{
    if (!d && !other.d) {
        return true;
    }
    if (!d || !other.d) {
        return false;
    }
    return d->core == other.d->core;
}

PosixPath PosixPath::operator/(const QString &path) const
{
    PosixPath result;
    result.d = new PosixPathPrivate;
    result.d->core = d ? d->core / toStdString(path) : qtng_core::PosixPath();
    return result;
}

PosixPath PosixPath::operator|(const QString &path) const
{
    return *this / path;
}

bool PosixPath::isNull() const
{
    return !d || d->core.isNull();
}

bool PosixPath::isFile() const { return !isNull() && d->core.isFile(); }
bool PosixPath::isDir() const { return !isNull() && d->core.isDir(); }
bool PosixPath::isSymLink() const { return !isNull() && d->core.isSymLink(); }
bool PosixPath::isAbsolute() const { return !isNull() && d->core.isAbsolute(); }
bool PosixPath::isExecutable() const { return !isNull() && d->core.isExecutable(); }
bool PosixPath::isReadable() const { return !isNull() && d->core.isReadable(); }
bool PosixPath::isRelative() const { return !isNull() && d->core.isRelative(); }
bool PosixPath::isRoot() const { return !isNull() && d->core.isRoot(); }
bool PosixPath::isWritable() const { return !isNull() && d->core.isWritable(); }
bool PosixPath::exists() const { return !isNull() && d->core.exists(); }

qint64 PosixPath::size() const
{
    return isNull() ? -1 : d->core.size();
}

QString PosixPath::path() const
{
    return isNull() ? QString() : toQString(d->core.path());
}

QFileInfo PosixPath::fileInfo() const
{
    return QFileInfo(path());
}

QString PosixPath::parentDir() const
{
    return isNull() ? QString() : toQString(d->core.parentDir());
}

PosixPath PosixPath::parentPath() const
{
    PosixPath result;
    result.d = new PosixPathPrivate;
    result.d->core = isNull() ? qtng_core::PosixPath() : d->core.parentPath();
    return result;
}

QString PosixPath::name() const
{
    return isNull() ? QString() : toQString(d->core.name());
}

QString PosixPath::baseName() const
{
    return isNull() ? QString() : toQString(d->core.baseName());
}

QString PosixPath::suffix() const
{
    return isNull() ? QString() : toQString(d->core.suffix());
}

QString PosixPath::completeBaseName() const
{
    return isNull() ? QString() : toQString(d->core.completeBaseName());
}

QString PosixPath::completeSuffix() const
{
    return isNull() ? QString() : toQString(d->core.completeSuffix());
}

QString PosixPath::toAbsolute() const
{
    return isNull() ? QString() : toQString(d->core.toAbsolute());
}

QString PosixPath::relativePath(const QString &other) const
{
    return isNull() ? QString() : toQString(d->core.relativePath(toStdString(other)));
}

QString PosixPath::relativePath(const PosixPath &other) const
{
    if (isNull() || other.isNull()) {
        return QString();
    }
    return toQString(d->core.relativePath(other.d->core));
}

bool PosixPath::isChildOf(const PosixPath &other) const
{
    return !isNull() && !other.isNull() && d->core.isChildOf(other.d->core);
}

bool PosixPath::hasChildOf(const PosixPath &other) const
{
    return !isNull() && !other.isNull() && d->core.hasChildOf(other.d->core);
}

QDateTime PosixPath::created() const
{
    return isNull() ? QDateTime() : toQDateTime(qtng_core::utils::DateTime::fromMSecsSinceEpoch(d->core.createdMsecsSinceEpoch()));
}

QDateTime PosixPath::lastModified() const
{
    return isNull() ? QDateTime()
                    : toQDateTime(qtng_core::utils::DateTime::fromMSecsSinceEpoch(d->core.lastModifiedMsecsSinceEpoch()));
}

QDateTime PosixPath::lastRead() const
{
    return isNull() ? QDateTime()
                    : toQDateTime(qtng_core::utils::DateTime::fromMSecsSinceEpoch(d->core.lastReadMsecsSinceEpoch()));
}

QStringList PosixPath::listdir() const
{
    if (isNull()) {
        return QStringList();
    }
    const vector<string> entries = d->core.listdir();
    QStringList result;
    for (const string &entry : entries) {
        result.append(toQString(entry));
    }
    return result;
}

QList<PosixPath> PosixPath::children() const
{
    QList<PosixPath> result;
    if (isNull()) {
        return result;
    }
    for (const qtng_core::PosixPath &child : d->core.children()) {
        PosixPath item;
        item.d = new PosixPathPrivate;
        item.d->core = child;
        result.append(item);
    }
    return result;
}

bool PosixPath::mkdir(bool createParents)
{
    return !isNull() && d->core.mkdir(createParents);
}

bool PosixPath::touch()
{
    return !isNull() && d->core.touch();
}

QSharedPointer<RawFile> PosixPath::open(const QString &mode) const
{
    if (isNull()) {
        return QSharedPointer<RawFile>();
    }
    return RawFile::open(path(), mode);
}

QByteArray PosixPath::readall(bool *ok) const
{
    if (isNull()) {
        if (ok) {
            *ok = false;
        }
        return QByteArray();
    }
    bool coreOk = false;
    const string data = d->core.readall(&coreOk);
    if (ok) {
        *ok = coreOk;
    }
    return toQByteArray(data);
}

PosixPath PosixPath::cwd()
{
    PosixPath result;
    result.d = new PosixPathPrivate;
    result.d->core = qtng_core::PosixPath::cwd();
    return result;
}

QChar PosixPath::point = QChar::fromLatin1(qtng_core::PosixPath::point);
QString PosixPath::pointpoint = QString::fromLatin1(qtng_core::PosixPath::pointpoint);
QChar PosixPath::seperator = QChar::fromLatin1(qtng_core::PosixPath::seperator);

QDebug &operator<<(QDebug &debug, const PosixPath &path)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "PosixPath(" << path.path() << ')';
    return debug;
}

uint qHash(const PosixPath &path, uint seed)
{
    return qHash(path.path(), seed);
}

QPair<QString, QString> safeJoinPath(const QString &parentDir, const QString &subPath)
{
    const pair<string, string> joined =
            qtng_core::safeJoinPath(toStdString(parentDir), toStdString(subPath));
    return qMakePair(toQString(joined.first), toQString(joined.second));
}

QPair<QFileInfo, QString> safeJoinPath(const QDir &parentDir, const QString &subPath)
{
    const QPair<QString, QString> joined = safeJoinPath(parentDir.absolutePath(), subPath);
    return qMakePair(QFileInfo(joined.first), joined.second);
}

}  // namespace QTNETWORKNG_NAMESPACE
