#include <QtCore/qdir.h>
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
    return QSharedPointer<FileLike>(new BytesIO(data));
}

QSharedPointer<FileLike> FileLike::bytes(QByteArray *data)
{
    return QSharedPointer<FileLike>(new BytesIO(data));
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

namespace {

bool isTheMode(const QString &mode, const QString &essential)
{
    QString t = mode;
    t.remove(QLatin1Char('+'));
    t.remove(QLatin1Char('b'));
    return t == essential;
}

}  // namespace

QSharedPointer<RawFile> RawFile::open(const QString &filepath, const QString &mode)
{
    QSharedPointer<QFile> file(new QFile(filepath));
    QIODevice::OpenMode openMode = QIODevice::NotOpen;
    if (mode == QString() || isTheMode(mode, QStringLiteral("r"))) {
        openMode |= QIODevice::ReadOnly;
        if (mode.contains(QLatin1Char('+'))) {
            openMode |= QIODevice::WriteOnly;
        }
    } else if (isTheMode(mode, QStringLiteral("w")) || isTheMode(mode, QStringLiteral("rw"))
               || isTheMode(mode, QStringLiteral("wr"))) {
        openMode |= QIODevice::WriteOnly | QIODevice::Truncate;
        if (mode.contains(QLatin1Char('+')) || mode.contains(QLatin1Char('r'))) {
            openMode |= QIODevice::ReadOnly;
        }
    } else if (isTheMode(mode, QStringLiteral("a"))) {
        openMode |= QIODevice::WriteOnly | QIODevice::Append;
        if (mode.contains(QLatin1Char('+'))) {
            openMode |= QIODevice::ReadOnly;
        }
    } else {
        // an unknown mode means the open fails, like qtnetworkng.
        return QSharedPointer<RawFile>();
    }
    if (!file->open(openMode)) {
        return QSharedPointer<RawFile>();
    }
    QSharedPointer<RawFile> openFile(new RawFile(file));
    if ((openMode & QIODevice::Append) && !openFile->seek(file->size())) {
        return QSharedPointer<RawFile>();
    }
    return openFile;
}

QSharedPointer<RawFile> RawFile::open(const QString &filepath, QIODevice::OpenMode mode)
{
    QSharedPointer<QFile> file(new QFile(filepath));
    if (!file->open(mode)) {
        return QSharedPointer<RawFile>();
    }
    QSharedPointer<RawFile> openFile(new RawFile(file));
    if ((mode & QIODevice::Append) && !openFile->seek(file->size())) {
        return QSharedPointer<RawFile>();
    }
    return openFile;
}

class BytesIOPrivate
{
public:
    explicit BytesIOPrivate(qint32 pos)
        : buf(nullptr)
        , pos(pos)
        , ownbuf(false)
    {
    }
    QByteArray *buf;
    qint32 pos;
    bool ownbuf;
};

BytesIO::BytesIO(const QByteArray &buf, qint32 pos)
    : d_ptr(new BytesIOPrivate(pos))
{
    Q_D(BytesIO);
    d->buf = new QByteArray(buf);
    d->ownbuf = true;
}

BytesIO::BytesIO(QByteArray *buf, qint32 pos)
    : d_ptr(new BytesIOPrivate(pos))
{
    Q_D(BytesIO);
    // The external QByteArray is a transparent window, exactly like qtnetworkng:
    // read/write operate on it in place and it is never owned by this BytesIO.
    d->buf = buf;
    d->ownbuf = false;
}

BytesIO::BytesIO()
    : d_ptr(new BytesIOPrivate(0))
{
    Q_D(BytesIO);
    d->buf = new QByteArray();
    d->ownbuf = true;
}

BytesIO::~BytesIO()
{
    Q_D(BytesIO);
    if (d->ownbuf) {
        delete d->buf;
    }
    delete d_ptr;
}

qint32 BytesIO::read(char *data, qint32 size)
{
    Q_D(BytesIO);
    if (!d->buf) {
        return -1;
    }
    const qint32 leftBytes = qMax(d->buf->size() - d->pos, 0);
    const qint32 readBytes = qMin(leftBytes, size);
    memcpy(data, d->buf->constData() + d->pos, static_cast<size_t>(readBytes));
    d->pos += readBytes;
    return readBytes;
}

qint32 BytesIO::write(const char *data, qint32 size)
{
    Q_D(BytesIO);
    if (!d->buf) {
        return -1;
    }
    if (d->pos + size > d->buf->size()) {
        d->buf->resize(d->pos + size);
    }
    memcpy(d->buf->data() + d->pos, data, static_cast<size_t>(size));
    d->pos += size;
    return size;
}

void BytesIO::close()
{
    // qtnetworkng semantics: BytesIO::close() is a no-op, the buffer stays
    // readable and writable after close().
}

qint64 BytesIO::size()
{
    Q_D(BytesIO);
    return d->buf ? d->buf->size() : 0;
}

QByteArray BytesIO::readall(bool *ok)
{
    Q_D(BytesIO);
    if (ok) {
        *ok = true;
    }
    if (!d->buf) {
        return QByteArray();
    }
    if (d->pos == 0) {
        d->pos = d->buf->size();
        return *d->buf;
    }
    const QByteArray t = d->buf->mid(d->pos);
    d->pos = d->buf->size();
    return t;
}

QByteArray BytesIO::data()
{
    Q_D(BytesIO);
    return d->buf ? *d->buf : QByteArray();
}

bool sendfile(QSharedPointer<FileLike> inputFile, QSharedPointer<FileLike> outputFile, qint64 bytesToCopy,
              int suitableBlockSize)
{
    return qtng_core::sendfile(toCoreFileLike(inputFile), toCoreFileLike(outputFile), bytesToCopy, suitableBlockSize);
}

class PipePrivate
{
public:
    PipePrivate(Pipe *q, qint32 maxBufferSize)
        : q_ptr(q)
        , queue(1024)
        , closed(false)
        , maxBufferSize(maxBufferSize)
        , debugLevel(0)
        , shouldEmitReadyRead(false)
        , shouldEmitBytesWritten(false)
    {
    }
public:
    bool readMore(QByteArray &localBuffer, int &offset);
    qint32 takeBytes(QByteArray &localBuffer, int &offset, char *data, qint32 size, bool force);
    qint32 flushThreshold() const
    {
        // accumulate writes until a reasonable chunk forms. the original qtnetworkng
        // condition ("queue is empty -> flush immediately") turns a byte-by-byte writer
        // into one locked queue put per byte, which is catastrophic for throughput.
        // 64KB keeps 1:1 flushing for block writers (deep pipeline) while batching
        // byte-wise writers into few large chunks.
        return qMin<qint32>(maxBufferSize, 64 * 1024);
    }
public:
    Pipe * const q_ptr;
    qtng_core::ThreadQueue<QByteArray> queue;
    QAtomicInteger<bool> closed;
    const qint32 maxBufferSize;
    QAtomicInteger<qint8> debugLevel;
    QAtomicInteger<bool> shouldEmitReadyRead;
    QAtomicInteger<bool> shouldEmitBytesWritten;
};

bool PipePrivate::readMore(QByteArray &localBuffer, int &offset)
{
    qint64 bytesWritten = 0;
    bool reachedEof = false;
    do {
        const QByteArray packet = queue.get();
        if (packet.isEmpty()) {
            // an empty packet is the EOF sentinel pushed by the writer's close();
            // it is the last element, so the queue is empty once we have it. note
            // that closed may not be set yet: close() enqueues before it flips the
            // flag, so do not assert on it here.
            Q_ASSERT(queue.isEmpty());
            reachedEof = true;
            if (debugLevel >= 2) {
                qWarning() << "got empty packet. the pipe is closed in another peer.";
            }
            break;
        } else {
            bytesWritten += packet.size();
            if (offset > 0) {
                localBuffer.remove(0, offset);
                offset = 0;
            }
            localBuffer.append(packet);
        }
    } while (!queue.isEmpty());

    if (shouldEmitBytesWritten && bytesWritten > 0) {
        if (debugLevel >= 2) {
            qWarning() << "invoking bytes written.";
        }
        QMetaObject::invokeMethod(q_ptr, "bytesWritten", Qt::AutoConnection, Q_ARG(qint64, bytesWritten));
    }
    return reachedEof;
}

qint32 PipePrivate::takeBytes(QByteArray &localBuffer, int &offset, char *data, qint32 size, bool force)
{
    qint32 bytesToRead = qMin<qint32>(localBuffer.size() - offset, size);
    Q_ASSERT(offset >= 0);
    if (bytesToRead >= size || (bytesToRead > 0 && force)) {
        memcpy(data, localBuffer.constData() + offset, static_cast<size_t>(bytesToRead));
        offset += bytesToRead;
        if (debugLevel >= 2) {
            if (!force) {
                qWarning() << "the size is fit in local buffer, return" << size << "bytes. left the local buffer"
                           << localBuffer.size() - offset << "bytes.";
            } else {
                qWarning() << "got data from another peer and returned" << bytesToRead << "bytes, left the local buffer"
                           << localBuffer.size() - offset << "bytes";
            }
        }
        return bytesToRead;
    }
    return 0;
}

Pipe::Pipe(qint32 maxBufferSize)
    : d(QSharedPointer<PipePrivate>::create(this, maxBufferSize))
{
}

void Pipe::setDebugLevel(qint8 debugLevel)
{
    d->debugLevel = debugLevel;
}

// FileToRead and FileToWrite are copied from core, 
class FileToRead : public FileLike
{
public:
    explicit FileToRead(QSharedPointer<PipePrivate> pp)
        : pp(pp)
        , offset(0)
        , eof(false)
    {
        localBuffer.reserve(pp->maxBufferSize);
    }
    virtual ~FileToRead() override { close(); }
public:
    virtual qint32 read(char *data, qint32 size) override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull()) {
            return -1;
        }
        if (size <= 0) {
            if (pp->debugLevel >= 1) {
                qWarning() << "can not read data the pipe is closed or size is invalid:" << size;
            }
            return -1;
        }

        qint32 bytesToRead = pp->takeBytes(localBuffer, offset, data, size, false);
        if (bytesToRead > 0) {
            return bytesToRead;
        }

        // buffer is empty: pull more data. readMore() blocks while the writer is
        // still open (every putForcedly wakes it) and stops at the EOF sentinel.
        // once eof is set we never touch the queue again, so this cannot deadlock
        // regardless of the order in which the writer flips its closed flag.
        if (!eof && localBuffer.size() - offset <= 0) {
            if (pp->readMore(localBuffer, offset)) {
                eof = true;
            }
        }

        bytesToRead = pp->takeBytes(localBuffer, offset, data, size, true);
        if (bytesToRead == 0) {
            Q_ASSERT(size > 0);
            this->pp.clear();
        }
        return bytesToRead;
    }

    virtual qint32 write(const char *, qint32) override { return -1; }

    virtual void close() override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull()) {
            return;
        }
        // drain the unread queue and report it as written. the writer may still be
        // waiting on bytesWritten() to confirm its data was consumed; discarding the
        // bytes silently would leave it hanging. this matches qtnetworkng 1.0.
        qint64 bytesWritten = 0;
        while (!pp->queue.isEmpty()) {
            bytesWritten += pp->queue.get().size();
        }
        pp->queue.clear();
        pp->closed = true;
        localBuffer.clear();
        if (pp->shouldEmitBytesWritten && bytesWritten > 0) {
            QMetaObject::invokeMethod(pp->q_ptr, "bytesWritten", Qt::AutoConnection, Q_ARG(qint64, bytesWritten));
        }
    }
    virtual qint64 size() override { return -1; }
public:
    QWeakPointer<PipePrivate> pp;
    QSharedPointer<Pipe> pipe;
    QByteArray localBuffer;
    int offset;
    bool eof;
};

QSharedPointer<FileLike> Pipe::fileToRead(bool takePipe)
{
    QSharedPointer<FileToRead> f = QSharedPointer<FileToRead>::create(d);
    if (takePipe) {
        f->pipe = sharedFromThis();
    }
    return f;
}

class FileToWrite : public FileLike
{
public:
    explicit FileToWrite(QSharedPointer<PipePrivate> pp)
        : pp(pp)
    {
        localBuffer.reserve(pp->maxBufferSize);
    }
    virtual ~FileToWrite() override { close(); }
public:
    virtual qint32 read(char *, qint32) override { return -1; }
    virtual qint32 write(const char *data, qint32 size) override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull() || pp->closed.loadRelaxed() || size <= 0) {
            return -1;
        }
        if (pp->debugLevel >= 2) {
            qWarning() << "write" << size << "bytes to pipe.";
        }
        // accumulate into localBuffer; flush only when a reasonable chunk has formed.
        // aggregating small writes (e.g. 1-byte writes) avoids one locked queue put per
        // call, which is the dominant cost for byte-wise writers. flush when the chunk
        // reaches the threshold (>=), so a 64KB block writer still flushes 1:1.
        localBuffer.append(data, size);
        if (localBuffer.size() < pp->flushThreshold()) {
            return size;
        }
        if (!pp->queue.put(std::move(localBuffer))) {
            return -1;
        }
        // after move the capacity is gone; reserve only the aggregation chunk size
        // (reserving maxBufferSize here would reallocate a huge buffer per flush)
        localBuffer.clear();
        localBuffer.reserve(pp->flushThreshold());
        if (pp->shouldEmitReadyRead) {
            QMetaObject::invokeMethod(pp->q_ptr, "readyRead", Qt::AutoConnection);
        }
        return size;
    }
    virtual void close() override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull() || pp->closed.loadRelaxed()) {
            return;
        }
        if (pp->debugLevel >= 2) {
            qWarning() << "close writing file of pipe.";
        }
        // enqueue the remaining bytes and the EOF sentinel BEFORE setting closed.
        // otherwise a reader that checks the condition between closed=true and the
        // enqueue would skip readMore and lose this tail chunk.
        if (!localBuffer.isEmpty()) {
            pp->queue.putForcedly(std::move(localBuffer));
            localBuffer = QByteArray();
        }
        pp->queue.putForcedly(QByteArray());
        pp->closed = true;
        if (pp->shouldEmitReadyRead) {
            QMetaObject::invokeMethod(pp->q_ptr, "readyRead", Qt::AutoConnection);
        }
    }
    virtual qint64 size() override { return -1; }
public:
    QWeakPointer<PipePrivate> pp;
    QSharedPointer<Pipe> pipe;
    QByteArray localBuffer;
};

QSharedPointer<FileLike> Pipe::fileToWrite(bool takePipe)
{
    QSharedPointer<FileToWrite> f = QSharedPointer<FileToWrite>::create(d);
    if (takePipe) {
        f->pipe = sharedFromThis();
    }
    return f;
}

class DeviceToRead : public QIODevice
{
public:
    explicit DeviceToRead(QSharedPointer<PipePrivate> pp, bool connectSignals)
        : pp(pp)
        , offset(0)
        , eof(false)
    {
        bool ok = QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        Q_ASSERT(ok);
        if (connectSignals) {
            pp->shouldEmitReadyRead = true;
            QObject::connect(pp->q_ptr, SIGNAL(readyRead()), this, SIGNAL(readyRead()));
        }
    }
    virtual ~DeviceToRead() override { close(); }
public:
    virtual bool atEnd() const override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull()) {
            return true;
        }
        // may has some bytes left in the internal buffer of QIODevice.
        // for example, some func called peek() before.
        if (!QIODevice::atEnd()) {
            return false;
        }
        return eof && localBuffer.size() <= offset;
    }

    virtual qint64 bytesAvailable() const override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull()) {
            return 0;
        }
        qint64 bytesInQueue = pp->queue.peek().size();
        // QIODevice::bytesAvailable() can be greater than 0 when peek() is called.
        return localBuffer.size() - offset + bytesInQueue + QIODevice::bytesAvailable();
    }

    virtual qint64 bytesToWrite() const override { return 0; }

    virtual bool canReadLine() const override { return false; }

    virtual bool isSequential() const override { return true; }

    virtual void close() override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull()) {
            return;
        }
        // to emit aboutToClose()
        QIODevice::close();
        pp->queue.clear();
        pp->closed = true;
        localBuffer.clear();
        // no need to emit bytesWritten() as the bytes is discarded.
    }

    virtual qint64 readData(char *data, qint64 size) override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull() || size < 0) {
            return -1;
        }
        // Qt may call readData() with a maxSize of 0 to trigger post-reading ops.
        if (size == 0) {
            return 0;
        }

        // Qt's docs require a reimplemented readData() to read all the requested
        // data before returning (QDataStream relies on it), so keep pulling until
        // the request is satisfied or EOF. readMore() blocks while the writer is
        // open (every putForcedly wakes it) and stops at the EOF sentinel; the
        // per-reader eof flag replaces 1.0's `!closed` condition, which raced with
        // the writer flipping closed after enqueueing EOF and deadlocked.
        while (!eof && offset + size > localBuffer.size()) {
            if (pp->readMore(localBuffer, offset)) {
                eof = true;
            }
        }

        qint32 bytesToRead = pp->takeBytes(localBuffer, offset, data, size, true);
        if (bytesToRead == 0) {
            Q_ASSERT(size > 0);
            this->pp.clear();
            // the pipe is closed and everything was consumed; no more bytes can
            // ever arrive, which Qt defines as an error (-1), not "no data now" (0).
            return -1;
        }
        return bytesToRead;
    }
    virtual qint64 writeData(const char *, qint64) override { return -1; }
    virtual bool waitForBytesWritten(int) override { return false; }
    virtual bool waitForReadyRead(int msecs) override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull()) {
            return false;
        }
        if (localBuffer.size() - offset > 0) {
            return true;
        }
        if (eof) {
            return false;  // no more data will ever arrive
        }
        if (!pp->queue.isEmpty()) {
            return true;
        }
        if (pp->closed.loadRelaxed()) {
            return false;
        }
        // honor the timeout: a timed-out wait must report false, not spin forever.
        return pp->queue.waitNotEmpty(msecs < 0 ? UINT_MAX : static_cast<uint32_t>(msecs));
    }
public:
    QWeakPointer<PipePrivate> pp;
    QSharedPointer<Pipe> pipe;
    QByteArray localBuffer;
    qint32 offset;
    bool eof;
};

QSharedPointer<QIODevice> Pipe::deviceToRead(bool connectSignals, bool takePipe)
{
    QSharedPointer<DeviceToRead> v = QSharedPointer<DeviceToRead>::create(d, connectSignals);
    if (takePipe) {
        v->pipe = sharedFromThis();
    }
    return v;
}

class DeviceToWrite : public QIODevice
{
public:
    explicit DeviceToWrite(QSharedPointer<PipePrivate> pp, bool connectSignals)
        : pp(pp)
    {
        bool ok = QIODevice::open(QIODevice::WriteOnly | QIODevice::Unbuffered);
        Q_ASSERT(ok);
        if (connectSignals) {
            pp->shouldEmitBytesWritten = true;
            QObject::connect(pp->q_ptr, SIGNAL(bytesWritten(qint64)), this, SIGNAL(bytesWritten(qint64)));
        }
        localBuffer.reserve(pp->maxBufferSize);
    }
    virtual ~DeviceToWrite() override { close(); }
public:
    virtual bool atEnd() const override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull()) {
            return true;
        }
        return pp->closed.loadRelaxed();
    }

    virtual qint64 bytesAvailable() const override { return 0; }

    virtual qint64 bytesToWrite() const override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull() || pp->closed.loadRelaxed()) {
            return 0;
        }
        // qtnetworkng reports the spare pipe capacity (maxBufferSize minus the
        // buffered bytes), which is always > 0 as long as the pipe is open.
        return qMax<qint64>(pp->maxBufferSize - pp->queue.peek().size() - localBuffer.size(), 0);
    }

    virtual bool canReadLine() const override { return false; }

    virtual bool isSequential() const override { return true; }

    virtual void close() override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull() || pp->closed.loadRelaxed()) {
            return;
        }

        // enqueue the remaining bytes and the EOF sentinel BEFORE setting closed.
        // otherwise a reader that checks the condition between closed=true and the
        // enqueue would skip readMore and lose this tail chunk.
        if (!localBuffer.isEmpty()) {
            pp->queue.putForcedly(std::move(localBuffer));
            localBuffer = QByteArray();
        }
        pp->queue.putForcedly(QByteArray());
        pp->closed = true;
        if (pp->shouldEmitReadyRead) {
            QMetaObject::invokeMethod(pp->q_ptr, "readyRead", Qt::AutoConnection);
        }
    }

    virtual qint64 readData(char *, qint64) override { return -1; }

    virtual qint64 writeData(const char *data, qint64 size) override
    {
        // according to the document, we must write all data!
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull() || pp->closed.loadRelaxed() || size < 0) {
            return -1;
        } else if (size > 0) {
            // accumulate into localBuffer; flush only when a reasonable chunk has formed.
            // aggregating small writes avoids one locked queue put per call. flush when the
            // chunk reaches the threshold (>=), so a 64KB block writer still flushes 1:1.
            localBuffer.append(data, static_cast<int>(size));
            if (localBuffer.size() < pp->flushThreshold()) {
                return size;
            }
        } else {
            // the qt document says size can be 0.
            // write(0) == flush()
            Q_ASSERT(size == 0);
        }

        if (!localBuffer.isEmpty()) {
            // putting empty packet means closing pipe.
            if (!pp->queue.put(std::move(localBuffer))) {
                return -1;
            }
            // after move the capacity is gone; reserve only the aggregation chunk size
            // (reserving maxBufferSize here would reallocate a huge buffer per flush)
            localBuffer.clear();
            localBuffer.reserve(pp->flushThreshold());
            if (pp->shouldEmitReadyRead) {
                QMetaObject::invokeMethod(pp->q_ptr, "readyRead", Qt::AutoConnection);
            }
        }
        return size;
    }

    virtual bool waitForBytesWritten(int msecs) override
    {
        QSharedPointer<PipePrivate> pp = this->pp.toStrongRef();
        if (pp.isNull() || pp->closed.loadRelaxed()) {
            return false;
        }
        // every write is accepted synchronously, so there is never buffered data
        // to wait for; the only failure is a closed pipe.
        Q_UNUSED(msecs);
        return true;
    }

    virtual bool waitForReadyRead(int) override { return false; }
public:
    QWeakPointer<PipePrivate> pp;
    QSharedPointer<Pipe> pipe;
    QByteArray localBuffer;
};

QSharedPointer<QIODevice> Pipe::deviceToWrite(bool connectSignals, bool takePipe)
{
    QSharedPointer<DeviceToWrite> v = QSharedPointer<DeviceToWrite>::create(d, connectSignals);
    if (takePipe) {
        v->pipe = sharedFromThis();
    }
    return v;
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
    return isNull() ? 0 : d->core.size();
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
    // qtnetworkng semantics: a path is a child of itself and of a null path.
    return !isNull() && d->core.isChildOf(other.isNull() ? qtng_core::PosixPath() : other.d->core);
}

bool PosixPath::hasChildOf(const PosixPath &other) const
{
    return !isNull() && d->core.hasChildOf(other.isNull() ? qtng_core::PosixPath() : other.d->core);
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
