#ifndef QTNG_RPC_SENDFILE_H
#define QTNG_RPC_SENDFILE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "qtng/io_utils.h"

#include "qtng/rpc/base.h"
#include "qtng/rpc/value.h"

BEGIN_QTNG_RPC_NAMESPACE

class RpcFilePrivate;

// A file passed as an rpc argument/result. On the wire it serializes to a map
// {name, size, mtime, ctime, atime, hash?} tagged with sid "RpcFile"; the file
// content then streams over a VirtualChannel (or a raw socket when
// preferRawSocket is set). Times are encoded as uint64 milliseconds, matching
// the legacy lafrpc layout.
class RpcFile : public UseStream
{
public:
    typedef std::function<bool(std::int64_t bs, std::uint64_t count, std::uint64_t total)> ProgressCallback;

    explicit RpcFile(const std::string &filePath, bool withHash = false);
    RpcFile();
    virtual ~RpcFile() override;

public:
    static std::shared_ptr<RpcFile> prepareToSend(std::int64_t size);
    bool calculateHash();
    bool isValid() const;

    bool writeToPath(const std::string &path, ProgressCallback progressCallback = nullptr);
    bool readFromPath(const std::string &path, ProgressCallback progressCallback = nullptr);
    bool readFromPath(ProgressCallback progressCallback = nullptr);
    bool writeTo(std::shared_ptr<qtng::FileLike> f, ProgressCallback progressCallback = nullptr);
    bool readFrom(std::shared_ptr<qtng::FileLike> f, ProgressCallback progressCallback = nullptr);

    bool sendall(const std::string &data, ProgressCallback progressCallback = nullptr);
    bool recvall(std::string &data, ProgressCallback progressCallback = nullptr);

public:
    std::string name() const;
    void setName(const std::string &name);
    std::uint64_t size() const;
    void setSize(std::uint64_t size);
    qtng::utils::DateTime modified() const;
    void setModified(const qtng::utils::DateTime &dt);
    qtng::utils::DateTime created() const;
    void setCreated(const qtng::utils::DateTime &dt);
    qtng::utils::DateTime lastAccess() const;
    void setLastAccess(const qtng::utils::DateTime &dt);
    std::string hash() const;  // binary sha256
    void setHash(const std::string &hash);

public:
    static std::string staticLafrpcKey() { return "RpcFile"; }
    virtual std::string lafrpcKey() const override { return "RpcFile"; }
    virtual Value saveState() const override;
    virtual bool restoreState(const Value &state) override;
    virtual std::shared_ptr<Serializable> clone() const override;

private:
    RpcFilePrivate * const d_ptr;
    friend class RpcFilePrivate;
};

END_QTNG_RPC_NAMESPACE

#endif  // QTNG_RPC_SENDFILE_H
