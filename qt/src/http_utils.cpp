#include "bridge/core_access.h"
#include "bridge/http_access.h"
#include "bridge/stream_bridge.h"
#include "http_utils.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

bool toMessage(HttpStatus status, QString *shortMessage, QString *longMessage)
{
    string shortMsg;
    string longMsg;
    const bool ok = QTNG_NAMESPACE::toMessage(static_cast<QTNG_NAMESPACE::HttpStatus>(status),
                                         shortMessage ? &shortMsg : nullptr, longMessage ? &longMsg : nullptr);
    if (shortMessage) {
        *shortMessage = toQString(shortMsg);
    }
    if (longMessage) {
        *longMessage = toQString(longMsg);
    }
    return ok;
}

QString normalizeHeaderName(const QString &headerName)
{
    return toQString(QTNG_NAMESPACE::normalizeHeaderName(toStdString(headerName)));
}

QDateTime fromHttpDate(const QByteArray &value)
{
    return toQDateTime(QTNG_NAMESPACE::fromHttpDate(toStdString(value)));
}

QByteArray toHttpDate(const QDateTime &dt)
{
    return toQByteArray(QTNG_NAMESPACE::toHttpDate(toCoreDateTime(dt)));
}

QString toString(KnownHeader knownHeader)
{
    return toQString(QTNG_NAMESPACE::toString(static_cast<QTNG_NAMESPACE::KnownHeader>(knownHeader)));
}

QDataStream &operator>>(QDataStream &ds, HttpHeader &header)
{
    QString name;
    QByteArray value;
    ds >> name >> value;
    header.setName(name);
    header.setValue(value);
    return ds;
}

QDataStream &operator<<(QDataStream &ds, const HttpHeader &header)
{
    ds << header.name() << header.value();
    return ds;
}

QList<QByteArray> splitBytes(const QByteArray &bs, char sep, int maxSplit)
{
    const vector<string> parts = QTNG_NAMESPACE::splitBytes(toStdString(bs), sep, maxSplit);
    QList<QByteArray> result;
    for (const string &part : parts) {
        result.append(toQByteArray(part));
    }
    return result;
}

class HeaderSplitterPrivate
{
public:
    HeaderSplitterPrivate(QSharedPointer<SocketLike> connection, const QByteArray &buf, int debugLevel)
        : core(toCoreSocketLike(std::move(connection)), toStdString(buf), debugLevel)
    {
    }
    HeaderSplitterPrivate(QSharedPointer<SocketLike> connection, int debugLevel)
        : core(toCoreSocketLike(std::move(connection)), debugLevel)
    {
    }
    QTNG_NAMESPACE::HeaderSplitter core;
};

class ChunkedBlockReaderPrivate
{
public:
    ChunkedBlockReaderPrivate(QSharedPointer<FileLike> connection, const QByteArray &buf)
        : core(toCoreFileLike(std::move(connection)), toStdString(buf))
    {
    }
    QTNG_NAMESPACE::ChunkedBlockReader core;
};

class PlainBodyFilePrivate
{
public:
    PlainBodyFilePrivate(qint64 contentLength, const QByteArray &partialBody, QSharedPointer<SocketLike> stream)
        : core(contentLength, toStdString(partialBody), toCoreSocketLike(std::move(stream)))
    {
    }
    QTNG_NAMESPACE::PlainBodyFile core;
};

class ChunkedBodyFilePrivate
{
public:
    ChunkedBodyFilePrivate(qint64 maxBodySize, const QByteArray &partialBody, QSharedPointer<FileLike> stream)
        : core(maxBodySize, toStdString(partialBody), toCoreFileLike(std::move(stream)))
    {
    }
    QTNG_NAMESPACE::ChunkedBodyFile core;
};

HeaderSplitter::HeaderSplitter(QSharedPointer<SocketLike> connection, const QByteArray &buf, int debugLevel)
    : d_ptr(new HeaderSplitterPrivate(std::move(connection), buf, debugLevel))
{
}

HeaderSplitter::HeaderSplitter(QSharedPointer<SocketLike> connection, int debugLevel)
    : d_ptr(new HeaderSplitterPrivate(std::move(connection), debugLevel))
{
}

HeaderSplitter::~HeaderSplitter()
{
    delete d_ptr;
}

QByteArray HeaderSplitter::nextLine(Error *error)
{
    QTNG_NAMESPACE::HeaderSplitter::Error coreError = QTNG_NAMESPACE::HeaderSplitter::NoError;
    const string line = d_ptr->core.nextLine(&coreError);
    if (error) {
        *error = static_cast<Error>(coreError);
    }
    return toQByteArray(line);
}

HttpHeader HeaderSplitter::nextHeader(Error *error)
{
    QTNG_NAMESPACE::HeaderSplitter::Error coreError = QTNG_NAMESPACE::HeaderSplitter::NoError;
    const QTNG_NAMESPACE::HttpHeader header = d_ptr->core.nextHeader(&coreError);
    if (error) {
        *error = static_cast<Error>(coreError);
    }
    return toQtHeader(header);
}

QList<HttpHeader> HeaderSplitter::headers(int maxHeaders, Error *error)
{
    QTNG_NAMESPACE::HeaderSplitter::Error coreError = QTNG_NAMESPACE::HeaderSplitter::NoError;
    const vector<QTNG_NAMESPACE::HttpHeader> coreHeaders = d_ptr->core.headers(maxHeaders, &coreError);
    if (error) {
        *error = static_cast<Error>(coreError);
    }
    QList<HttpHeader> result;
    for (const QTNG_NAMESPACE::HttpHeader &header : coreHeaders) {
        result.append(toQtHeader(header));
    }
    return result;
}

QByteArray HeaderSplitter::buf() const
{
    return toQByteArray(d_ptr->core.buf());
}

void HeaderSplitter::setBuf(const QByteArray &buf)
{
    d_ptr->core.setBuf(toStdString(buf));
}

ChunkedBlockReader::ChunkedBlockReader(QSharedPointer<FileLike> connection, const QByteArray &buf)
    : d_ptr(new ChunkedBlockReaderPrivate(std::move(connection), buf))
{
}

ChunkedBlockReader::~ChunkedBlockReader()
{
    delete d_ptr;
}

QByteArray ChunkedBlockReader::nextBlock(qint64 leftBytes, Error *error)
{
    QTNG_NAMESPACE::ChunkedBlockReader::Error coreError = QTNG_NAMESPACE::ChunkedBlockReader::NoError;
    const string block = d_ptr->core.nextBlock(leftBytes, &coreError);
    if (error) {
        *error = static_cast<Error>(coreError);
    }
    return toQByteArray(block);
}

PlainBodyFile::PlainBodyFile(qint64 contentLength, const QByteArray &partialBody, QSharedPointer<SocketLike> stream)
    : d_ptr(new PlainBodyFilePrivate(contentLength, partialBody, std::move(stream)))
{
}

PlainBodyFile::~PlainBodyFile()
{
    delete d_ptr;
}

qint32 PlainBodyFile::read(char *data, qint32 size)
{
    return d_ptr->core.read(data, size);
}

qint64 PlainBodyFile::contentLength() const
{
    return d_ptr->core.contentLength();
}

ChunkedBodyFile::ChunkedBodyFile(qint64 maxBodySize, const QByteArray &partialBody, QSharedPointer<FileLike> stream)
    : d_ptr(new ChunkedBodyFilePrivate(maxBodySize, partialBody, std::move(stream)))
{
}

ChunkedBodyFile::~ChunkedBodyFile()
{
    delete d_ptr;
}

qint32 ChunkedBodyFile::read(char *data, qint32 size)
{
    return d_ptr->core.read(data, size);
}

ChunkedBlockReader::Error ChunkedBodyFile::error() const
{
    return static_cast<ChunkedBlockReader::Error>(d_ptr->core.error());
}

// ChunkedWriter is implemented here instead of wrapping QTNG_NAMESPACE::ChunkedWriter:
// 1. The core destructor writes the terminator chunk "0\r\n\r\n". If we created a
//    temporary core writer on every write() call like the old binding did, the
//    temporary would destruct right away and terminate the stream, degrading to
//    "write is close": one write() emits a single chunk and all later calls fail.
//    (See the BINDING defect note in test_http_utils.cpp "ChunkedWriter 编码输出与
//    close 幂等性".)
// 2. Holding one core instance long-term is viable, but its destructor also writes
//    the terminator chunk, so the Qt destructor must stay empty and rely on the core
//    destructor to finish the stream; any residual close() layer (explicit close +
//    Qt destructor + core destructor) would emit 3 terminator chunks and break the
//    test assertions. A stateless encoder does not warrant a d-pointer for this.
// So the encoding is replicated here: hex length + "\r\n" + data + "\r\n" per chunk,
// chunk size capped at 0xffff, terminated by "0\r\n\r\n". Do not revert to wrapping
// the core with a temporary writer.
// Note: close() is not idempotent; an explicit close() followed by destruction
// duplicates the terminator chunk (original quirk, asserted by the tests).
ChunkedWriter::~ChunkedWriter()
{
    close();
}

qint32 ChunkedWriter::write(const char *data, qint32 size)
{
    if (!data) {
        return -1;
    }
    // the chunked block can not greater than 0xffff!
    qint64 sent = 0;
    while (sent < size) {
        qint32 blockSize = qMin<qint32>(0xffff, size - sent);
        QByteArray buf;
        buf.reserve(blockSize + 8);
        buf.append(QByteArray::number(blockSize, 16));
        buf.append("\r\n", 2);
        buf.append(data + sent, blockSize);
        buf.append("\r\n", 2);

        const qint32 writtenBytes = stream_->write(buf);
        if (writtenBytes != buf.size()) {
            return -1;
        }
        sent += blockSize;
    }
    return size;
}

void ChunkedWriter::close()
{
    stream_->write("0\r\n\r\n", 5);
}

}  // namespace QTNETWORKNG_NAMESPACE
