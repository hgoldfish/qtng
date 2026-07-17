#ifndef QTNG_IO_UTILS_H
#define QTNG_IO_UTILS_H

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "qtng/utils/platform.h"

namespace qtng {

class FileLike
{
public:
    virtual ~FileLike();
    virtual std::int32_t read(char *data, std::int32_t size) = 0;
    virtual std::int32_t write(const char *data, std::int32_t size) = 0;
    virtual void close() = 0;
    virtual std::int64_t size() = 0;
    virtual std::string readall(bool *ok);
public:
    std::string read(std::int32_t size);
    std::int32_t write(const std::string &data);
public:
    static std::shared_ptr<FileLike> open(const std::string &filepath, const std::string &mode = std::string());
    static std::shared_ptr<FileLike> bytes(const std::string &data);
    static std::shared_ptr<FileLike> bytes(std::string *data);
};

class BytesIOPrivate;
class BytesIO : public FileLike
{
public:
    BytesIO(const std::string &buf, std::int32_t pos = 0);
    BytesIO(std::string *buf, std::int32_t pos = 0);
    BytesIO();
    virtual ~BytesIO() override;
    virtual std::int32_t read(char *data, std::int32_t size) override;
    virtual std::int32_t write(const char *data, std::int32_t size) override;
    virtual void close() override;
    virtual std::int64_t size() override;
    virtual std::string readall(bool *ok) override;
    std::string data();
private:
    BytesIOPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(BytesIO)
};

bool sendfile(std::shared_ptr<FileLike> inputFile, std::shared_ptr<FileLike> outputFile, std::int64_t bytesToCopy = -1,
              int suitableBlockSize = 1024 * 8);

class PipePrivate;
class Pipe : public std::enable_shared_from_this<Pipe>
{
public:
    explicit Pipe(std::int32_t maxBufferSize = 1024 * 64);
public:
    void setDebugLevel(std::int8_t debugLevel);
    void setReadyReadCallback(std::function<void()> callback);
    void setBytesWrittenCallback(std::function<void(std::int64_t)> callback);
    std::shared_ptr<FileLike> fileToRead(bool takePipe = false);
    std::shared_ptr<FileLike> fileToWrite(bool takePipe = false);
private:
    const std::shared_ptr<PipePrivate> d;
};

class PosixPathPrivate;
class PosixPath
{
public:
    PosixPath();
    PosixPath(const std::string &path);
    PosixPath(const PosixPath &other);
    PosixPath(PosixPath &&other) noexcept;
    ~PosixPath();
    PosixPath &operator=(const PosixPath &other);
    PosixPath &operator=(PosixPath &&other) noexcept;
    bool operator==(const PosixPath &other) const;
    inline bool operator!=(const PosixPath &other) const { return !(*this == other); }
public:
    PosixPath operator/(const std::string &path) const;
    PosixPath operator|(const std::string &path) const;
public:
    bool isNull() const;

    bool isFile() const;
    bool isDir() const;
    bool isSymLink() const;
    bool isAbsolute() const;
    bool isExecutable() const;
    bool isReadable() const;
    bool isRelative() const;
    bool isRoot() const;
    bool isWritable() const;
    bool exists() const;
    std::int64_t size() const;

    std::string path() const;
    std::string parentDir() const;
    PosixPath parentPath() const;
    std::string name() const;
    std::string baseName() const;
    std::string suffix() const;
    std::string completeBaseName() const;
    std::string completeSuffix() const;
    std::string toAbsolute() const;
    std::string relativePath(const std::string &other) const;
    std::string relativePath(const PosixPath &other) const;
    bool isChildOf(const PosixPath &other) const;
    bool hasChildOf(const PosixPath &other) const;

    std::int64_t createdMsecsSinceEpoch() const;
    std::int64_t lastModifiedMsecsSinceEpoch() const;
    std::int64_t lastReadMsecsSinceEpoch() const;

    std::vector<std::string> listdir() const;
    std::vector<PosixPath> children() const;

    bool mkdir(bool createParents = false);
    bool touch();
    std::shared_ptr<FileLike> open(const std::string &mode = std::string()) const;
    std::string readall(bool *ok) const;

    static PosixPath cwd();
    static const char point;
    static const char *pointpoint;
    static const char seperator;
private:
    std::shared_ptr<PosixPathPrivate> d;
};

std::pair<std::string, std::string> safeJoinPath(const std::string &parentDir, const std::string &subPath);

}  // namespace qtng

#endif  // QTNG_IO_UTILS_H
