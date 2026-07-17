#ifndef QTNG_GZIP_H
#define QTNG_GZIP_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtng/io_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

class GzipFilePrivate;
class GzipFile : public FileLike
{
public:
    enum IOMode
    {
        Decompress = 0, Compress = 1,
        Inflate = 2, Deflate = 3
    };
public:
    GzipFile(std::shared_ptr<FileLike> backend, IOMode mode, int level = -1);
    virtual ~GzipFile() override;
public:
    virtual std::int32_t read(char *data, std::int32_t size) override;
    virtual std::int32_t write(const char *data, std::int32_t size) override;
    virtual void close() override;
    virtual std::int64_t size() override { return -1; }
public:
    std::int64_t processedBytes() const;
private:
    GzipFilePrivate * const d_ptr;
    NG_DECLARE_PRIVATE(GzipFile);
};

bool qGzipCompress(std::shared_ptr<FileLike> input, std::shared_ptr<FileLike> output, int level = -1, int blockSize = 1024 * 8);
bool qGzipDecompress(std::shared_ptr<FileLike> input, std::shared_ptr<FileLike> output, int blockSize = 1024 * 8);

}  // namespace qtng

#endif  // QTNG_GZIP_H
