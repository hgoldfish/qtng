#ifndef QTNG_RPC_SENDDIR_H
#define QTNG_RPC_SENDDIR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qtng/io_utils.h"

#include "qtng/rpc/base.h"
#include "qtng/rpc/value.h"

BEGIN_QTNG_RPC_NAMESPACE

struct RpcDirFileEntry
{
    RpcDirFileEntry();
    std::string path;
    std::uint64_t size;
    qtng::utils::DateTime created;
    qtng::utils::DateTime lastModified;
    qtng::utils::DateTime lastAccess;
    bool isdir;
};

struct CallbackInfo
{
    CallbackInfo(const std::string &filePath, std::int32_t bs, std::uint64_t fileRead, std::uint64_t fileSize,
                 std::uint64_t totalRead, std::uint64_t totalSize);
    std::string filePath;
    std::int32_t currentRead;
    std::uint64_t currentFileRead;
    std::uint64_t currentFileSize;
    std::uint64_t totalRead;
    std::uint64_t totalSize;
};

class RpcDirPrivate;
class RpcDirFileProvider;

// A directory passed as an rpc argument/result. saveState uses datetime fields
// (ctime/mtime/atime) unlike RpcFile (uint64); the content streams per entry
// over the VirtualChannel.
class RpcDir : public UseStream
{
public:
    typedef std::function<bool(const CallbackInfo &)> ProgressCallback;

    explicit RpcDir(const std::string &path);
    RpcDir();
    virtual ~RpcDir() override;

public:
    bool populate();
    bool isValid() const;

    bool writeToPath(const std::string &path, ProgressCallback progressCallback = nullptr);
    bool readFromPath(const std::string &path, ProgressCallback progressCallback = nullptr);
    bool readFromPath(ProgressCallback progressCallback = nullptr);

    bool writeTo(std::shared_ptr<RpcDirFileProvider> provider, ProgressCallback progressCallback = nullptr);
    bool readFrom(std::shared_ptr<RpcDirFileProvider> provider, ProgressCallback progressCallback = nullptr);

public:
    std::string name() const;
    void setName(const std::string &name);
    std::uint64_t size() const;
    void setSize(std::uint64_t size);
    qtng::utils::DateTime lastModified() const;
    void setLastModified(const qtng::utils::DateTime &dt);
    qtng::utils::DateTime created() const;
    void setCreated(const qtng::utils::DateTime &dt);
    qtng::utils::DateTime lastAccess() const;
    void setLastAccess(const qtng::utils::DateTime &dt);
    std::vector<RpcDirFileEntry> entries() const;
    void setEntries(const std::vector<RpcDirFileEntry> &entries);

public:
    static std::string staticLafrpcKey() { return "RpcDir"; }
    virtual std::string lafrpcKey() const override { return "RpcDir"; }
    virtual Value saveState() const override;
    virtual bool restoreState(const Value &state) override;
    virtual std::shared_ptr<Serializable> clone() const override;

private:
    RpcDirPrivate * const d_ptr;
    friend class RpcDirPrivate;
};

// Pluggable file backend used by RpcDir to read/write entry contents.
class RpcDirFileProvider
{
public:
    virtual ~RpcDirFileProvider();
    virtual std::shared_ptr<qtng::FileLike> getFile(const std::string &filePath, bool writeMode) = 0;
    virtual bool createDirectory(const std::string &dirPath);
    virtual bool updateTimes(const std::string &filePath, const qtng::utils::DateTime &created,
                             const qtng::utils::DateTime &lastModified, const qtng::utils::DateTime &lastAccess);
};

class NativeRpcDirFileProvider : public RpcDirFileProvider
{
public:
    explicit NativeRpcDirFileProvider(const std::string &root);
    std::shared_ptr<qtng::FileLike> getFile(const std::string &filePath, bool writeMode) override;
    bool createDirectory(const std::string &dirPath) override;
    bool updateTimes(const std::string &filePath, const qtng::utils::DateTime &created,
                     const qtng::utils::DateTime &lastModified, const qtng::utils::DateTime &lastAccess) override;
    std::string makePath(const std::string &filePath);

private:
    std::string rootDir;
};

END_QTNG_RPC_NAMESPACE

#endif  // QTNG_RPC_SENDDIR_H
