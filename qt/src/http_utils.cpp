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
    const bool ok = qtng_core::toMessage(static_cast<qtng_core::HttpStatus>(status),
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
    return toQString(qtng_core::normalizeHeaderName(toStdString(headerName)));
}

QDateTime fromHttpDate(const QByteArray &value)
{
    return toQDateTime(qtng_core::fromHttpDate(toStdString(value)));
}

QByteArray toHttpDate(const QDateTime &dt)
{
    return toQByteArray(qtng_core::toHttpDate(toCoreDateTime(dt)));
}

QString toString(KnownHeader knownHeader)
{
    return toQString(qtng_core::toString(static_cast<qtng_core::KnownHeader>(knownHeader)));
}

QDataStream &operator>>(QDataStream &ds, HttpHeader &header)
{
    ds >> header.name >> header.value;
    return ds;
}

QDataStream &operator<<(QDataStream &ds, const HttpHeader &header)
{
    ds << header.name << header.value;
    return ds;
}

QList<QByteArray> splitBytes(const QByteArray &bs, char sep, int maxSplit)
{
    const vector<string> parts = qtng_core::splitBytes(toStdString(bs), sep, maxSplit);
    QList<QByteArray> result;
    for (const string &part : parts) {
        result.append(toQByteArray(part));
    }
    return result;
}

QByteArray HeaderSplitter::nextLine(Error *error)
{
    qtng_core::HeaderSplitter splitter(toCoreSocketLike(connection), toStdString(buf), debugLevel);
    qtng_core::HeaderSplitter::Error coreError = qtng_core::HeaderSplitter::NoError;
    const string line = splitter.nextLine(&coreError);
    buf = toQByteArray(splitter.buf);
    connection = toQtSocketLike(splitter.connection);
    if (error) {
        *error = static_cast<Error>(coreError);
    }
    return toQByteArray(line);
}

HttpHeader HeaderSplitter::nextHeader(Error *error)
{
    qtng_core::HeaderSplitter splitter(toCoreSocketLike(connection), toStdString(buf), debugLevel);
    qtng_core::HeaderSplitter::Error coreError = qtng_core::HeaderSplitter::NoError;
    const qtng_core::HttpHeader header = splitter.nextHeader(&coreError);
    buf = toQByteArray(splitter.buf);
    connection = toQtSocketLike(splitter.connection);
    if (error) {
        *error = static_cast<Error>(coreError);
    }
    return toQtHeader(header);
}

QList<HttpHeader> HeaderSplitter::headers(int maxHeaders, Error *error)
{
    qtng_core::HeaderSplitter splitter(toCoreSocketLike(connection), toStdString(buf), debugLevel);
    qtng_core::HeaderSplitter::Error coreError = qtng_core::HeaderSplitter::NoError;
    const vector<qtng_core::HttpHeader> coreHeaders = splitter.headers(maxHeaders, &coreError);
    buf = toQByteArray(splitter.buf);
    connection = toQtSocketLike(splitter.connection);
    if (error) {
        *error = static_cast<Error>(coreError);
    }
    QList<HttpHeader> result;
    for (const qtng_core::HttpHeader &header : coreHeaders) {
        result.append(toQtHeader(header));
    }
    return result;
}

QByteArray ChunkedBlockReader::nextBlock(qint64 leftBytes, Error *error)
{
    qtng_core::ChunkedBlockReader reader(toCoreFileLike(connection), toStdString(buf));
    reader.debugLevel = debugLevel;
    qtng_core::ChunkedBlockReader::Error coreError = qtng_core::ChunkedBlockReader::NoError;
    const string block = reader.nextBlock(leftBytes, &coreError);
    buf = toQByteArray(reader.buf);
    connection = toQtFileLike(reader.connection);
    if (error) {
        *error = static_cast<Error>(coreError);
    }
    return toQByteArray(block);
}

PlainBodyFile::PlainBodyFile(qint64 contentLength, const QByteArray &partialBody, QSharedPointer<SocketLike> stream)
    : contentLength(contentLength)
    , stream(std::move(stream))
    , partialBody(partialBody)
    , count(0)
{
}

qint32 PlainBodyFile::read(char *data, qint32 size)
{
    qtng_core::PlainBodyFile file(contentLength, toStdString(partialBody), toCoreSocketLike(stream));
    file.count = count;
    const qint32 readBytes = file.read(data, size);
    partialBody = toQByteArray(file.partialBody);
    count = file.count;
    return readBytes;
}

ChunkedBodyFile::ChunkedBodyFile(qint64 maxBodySize, const QByteArray &partialBody, QSharedPointer<FileLike> stream)
    : reader(stream, partialBody)
    , error(ChunkedBlockReader::NoError)
    , maxBodySize(maxBodySize)
    , count(0)
    , eof(false)
{
}

qint32 ChunkedBodyFile::read(char *data, qint32 size)
{
    qtng_core::ChunkedBodyFile file(maxBodySize, toStdString(reader.buf), toCoreFileLike(reader.connection));
    file.reader = qtng_core::ChunkedBlockReader(toCoreFileLike(reader.connection), toStdString(reader.buf));
    file.reader.debugLevel = reader.debugLevel;
    file.error = static_cast<qtng_core::ChunkedBlockReader::Error>(error);
    file.buf = toStdString(buf);
    file.count = count;
    file.eof = eof;
    const qint32 readBytes = file.read(data, size);
    reader.buf = toQByteArray(file.reader.buf);
    reader.connection = toQtFileLike(file.reader.connection);
    error = static_cast<ChunkedBlockReader::Error>(file.error);
    buf = toQByteArray(file.buf);
    count = file.count;
    eof = file.eof;
    return readBytes;
}

ChunkedWriter::~ChunkedWriter()
{
    close();
}

qint32 ChunkedWriter::write(const char *data, qint32 size)
{
    qtng_core::ChunkedWriter writer(toCoreFileLike(stream));
    return writer.write(data, size);
}

void ChunkedWriter::close()
{
    qtng_core::ChunkedWriter writer(toCoreFileLike(stream));
    writer.close();
}

}  // namespace QTNETWORKNG_NAMESPACE
