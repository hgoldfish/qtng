using namespace std;

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>

#include "qtng/gzip.h"
#include "qtng/utils/logging.h"
extern "C" {
#include <zlib.h>
}

NG_LOGGER("qtng.gzip");

namespace qtng {

class GzipFilePrivate
{
public:
    GzipFilePrivate(shared_ptr<FileLike> backend, GzipFile::IOMode mode, int level)
        : backend(backend)
        , mode(mode)
        , processedBytes(0)
        , level(max(-1, min(9, level)))
        , hasError(false)
        , inited(false)
        , triedRawDeflate(false)
        , eof(false)
    {
    }
public:
    bool initZStream(bool asRawDeflate);
public:
    shared_ptr<FileLike> backend;
    GzipFile::IOMode mode;
    int64_t processedBytes;
    string buf;
    z_stream zstream;
    int level;
    bool hasError;
    bool inited;
    bool triedRawDeflate;
    bool eof;
};

bool GzipFilePrivate::initZStream(bool asRawDeflate)
{
    if (inited) {
        deflateEnd(&zstream);
    }
    memset(&zstream, 0, sizeof(zstream));
    int ret;
    if (asRawDeflate) {
        triedRawDeflate = true;
        if (mode == GzipFile::Deflate || mode == GzipFile::Compress) {
            ret = deflateInit2(&zstream, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        } else {
            assert(mode == GzipFile::Inflate || mode == GzipFile::Decompress);
            ret = inflateInit2(&zstream, -MAX_WBITS);
        }
    } else {
        assert(!triedRawDeflate);
        assert(mode != GzipFile::Deflate && mode != GzipFile::Inflate);
        // GZIP_WINDOWS_BIT = MAX_WBITS + 32
        if (mode == GzipFile::Compress) {
            ret = deflateInit2(&zstream, level, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
        } else {
            assert(mode == GzipFile::Decompress);
            ret = inflateInit2(&zstream, MAX_WBITS + 32);
        }
    }

    inited = (ret == Z_OK);
    return inited;
}

GzipFile::GzipFile(shared_ptr<FileLike> backend, GzipFile::IOMode mode, int level)
    : d_ptr(new GzipFilePrivate(backend, mode, level))
{
    d_ptr->initZStream(mode == GzipFile::Inflate || mode == GzipFile::Deflate);
}

GzipFile::~GzipFile()
{
    NG_D(GzipFile);
    close();
    if (d->inited) {
        deflateEnd(&d->zstream);
    }
    delete d_ptr;
}

int32_t GzipFile::read(char *data, int32_t size)
{
    NG_D(GzipFile);
    if (d->hasError || !d->inited || ( d->mode != Decompress && d->mode != Inflate)) {
        return -1;
    }
    const int OutputBufferSize = 1024 * 48;
    const int InputBufferSize = 1024 * 8;
    string inBuf(InputBufferSize, '\0');
    string outBuf(OutputBufferSize, '\0');

    while (d->buf.size() < size && !d->eof) {
        int32_t readBytes = d->backend->read(&inBuf[0], static_cast<int32_t>(inBuf.size()));
        if (readBytes <= 0) {
            // the gzip stream have an eof mark. we expect it!
            // before the gzip steam eof, readBytes <= 0 is always error.
            if (d->buf.empty()) {
                return -1;
            }
            break;
        }
        d->processedBytes += readBytes;
        d->zstream.next_in = reinterpret_cast<Bytef *>(&inBuf[0]);
        d->zstream.avail_in = static_cast<uint>(readBytes);
        do {
            d->zstream.next_out = reinterpret_cast<Bytef *>(&outBuf[0]);
            d->zstream.avail_out = static_cast<uint>(outBuf.size());
            int ret = inflate(&d->zstream, readBytes > 0 ? Z_NO_FLUSH : Z_FINISH);
            if (ret == Z_DATA_ERROR && !d->triedRawDeflate) {
                if (!d->initZStream(true)) {
                    return -1;
                }
                d->zstream.next_in = reinterpret_cast<Bytef *>(&inBuf[0]);
                d->zstream.avail_in = static_cast<uint>(readBytes);
                continue;
            } else if (ret < 0 || ret == Z_NEED_DICT) {
                ngWarning() << "gzip report need dict?! why this happened?";
                d->hasError = true;
                return -1;
            }
            assert(ret == Z_OK || ret == Z_STREAM_END);
            if ((d->zstream.avail_out > static_cast<uint>(outBuf.size()))) {  // is this possible?
                ngWarning() << "gzip report avail_out > outBuf.size() at reading, this is impossible!";
                d->hasError = true;
                return -1;
            }
            d->triedRawDeflate = true;
            int have = outBuf.size() - static_cast<int>(d->zstream.avail_out);
            if (have > 0) {
                d->buf.append(outBuf.data(), have);
            }
            if (ret == Z_STREAM_END) {
                d->eof = true;
                break;
            }
            if (d->zstream.avail_in == 0) {
                // all readBytes is consumed, we must read more.
                break;
            }
        } while (d->zstream.avail_out == 0);
    }
    if (d->buf.empty()) {
        if (d->eof) {
            return 0;
        }
        // the server closed the connection prematurely, and the data was not sent completely
        return -1;
    }
    int32_t bytesToRead = static_cast<int32_t>(min(static_cast<size_t>(size), d->buf.size()));
    memcpy(data, d->buf.data(), bytesToRead);
    d->buf.erase(0, static_cast<size_t>(bytesToRead));
    return bytesToRead;
}

int32_t GzipFile::write(const char *data, int32_t size)
{
    NG_D(GzipFile);
    if (d->hasError || !d->inited || (d->mode != Compress && d->mode != Deflate)) {
        return -1;
    }

    const int OutputBufferSize = 1024 * 32;
    string outBuf(OutputBufferSize, '\0');

    // data can be nullptr and size can be 0 while closing.
    d->zstream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
    d->zstream.avail_in = static_cast<uint>(size);
    do {
        d->zstream.next_out = reinterpret_cast<Bytef *>(&outBuf[0]);
        d->zstream.avail_out = static_cast<uint>(outBuf.size());
        int ret = deflate(&d->zstream, size > 0 ? Z_NO_FLUSH : Z_FINISH);
        if (ret < 0 || ret == Z_NEED_DICT) {
            if ((ret == Z_NEED_DICT)) {
                ngWarning() << "gzip report need dict?! why this happened?";
            }
            d->hasError = true;
            return -1;
        }
        if ((d->zstream.avail_out > static_cast<uint>(outBuf.size()))) {  // is this possible?
            ngWarning() << "gzip report avail_out > outBuf.size() at writing, this is impossible!";
            d->hasError = true;
            return -1;
        }
        int have = outBuf.size() - static_cast<int>(d->zstream.avail_out);
        if (have > 0) {
            d->buf.append(outBuf.data(), static_cast<int32_t>(have));
        }
    } while (d->zstream.avail_out == 0 || d->zstream.avail_in > 0);

    if (d->buf.empty()) {
        return size;
    }

    int32_t bytesWritten = d->backend->write(d->buf);
    d->processedBytes += bytesWritten;
    bool success = (bytesWritten == d->buf.size());
    d->buf.clear();
    return success ? size : -1;
}

void GzipFile::close()
{
    write(nullptr, 0);
}

int64_t GzipFile::processedBytes() const
{
    NG_D(const GzipFile);
    return d->processedBytes;
}

bool qGzipCompress(shared_ptr<FileLike> input, shared_ptr<FileLike> output, int level, int blockSize)
{
    if (!input || !output) {
        return false;
    }
    shared_ptr<GzipFile> gzip(new GzipFile(output, GzipFile::Compress, level));
    return sendfile(input, gzip, input->size(), blockSize);
}

bool qGzipDecompress(shared_ptr<FileLike> input, shared_ptr<FileLike> output, int blockSize)
{
    if (!input || !output) {
        return false;
    }
    shared_ptr<GzipFile> gzip(new GzipFile(input, GzipFile::Decompress));
    return sendfile(gzip, output, gzip->size(), blockSize);
}

}  // namespace qtng
