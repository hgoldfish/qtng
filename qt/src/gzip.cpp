#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "gzip.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class GzipFilePrivate
{
public:
    shared_ptr<qtng_core::GzipFile> core;
};

GzipFile::GzipFile(QSharedPointer<FileLike> backend, IOMode mode, int level)
    : d_ptr(new GzipFilePrivate)
{
    Q_D(GzipFile);
    d->core = make_shared<qtng_core::GzipFile>(toCoreFileLike(backend),
                                               static_cast<qtng_core::GzipFile::IOMode>(mode), level);
}

GzipFile::~GzipFile()
{
    delete d_ptr;
}

qint32 GzipFile::read(char *data, qint32 size)
{
    Q_D(GzipFile);
    return d->core->read(data, size);
}

qint32 GzipFile::write(const char *data, qint32 size)
{
    Q_D(GzipFile);
    return d->core->write(data, size);
}

void GzipFile::close()
{
    Q_D(GzipFile);
    d->core->close();
}

qint64 GzipFile::processedBytes() const
{
    Q_D(const GzipFile);
    return d->core->processedBytes();
}

bool qGzipCompress(QSharedPointer<FileLike> input, QSharedPointer<FileLike> output, int level, int blockSize)
{
    return qtng_core::qGzipCompress(toCoreFileLike(input), toCoreFileLike(output), level, blockSize);
}

bool qGzipDecompress(QSharedPointer<FileLike> input, QSharedPointer<FileLike> output, int blockSize)
{
    return qtng_core::qGzipDecompress(toCoreFileLike(input), toCoreFileLike(output), blockSize);
}

}  // namespace QTNETWORKNG_NAMESPACE
