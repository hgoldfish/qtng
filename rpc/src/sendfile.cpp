#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "qtng/coroutine_utils.h"
#include "qtng/io_utils.h"
#include "qtng/md.h"
#include "qtng/random.h"
#include "qtng/utils/string_utils.h"

#include "qtng/rpc/sendfile.h"

using namespace std;

namespace qtng {
namespace rpc {

namespace {
const std::int64_t BLOCK_SIZE = 1024 * 32;

std::int64_t fileTimeToMSecs(const std::filesystem::file_time_type &t)
{
    // On Linux/glibc and Android the file clock shares the system epoch, so
    // the duration since epoch maps directly to unix milliseconds.
    const std::int64_t secs =
            std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    return secs * 1000;
}

std::string computeFileHash(const std::string &filePath)
{
    std::shared_ptr<qtng::FileLike> f = qtng::FileLike::open(filePath, "r");
    if (!f) {
        return std::string();
    }
    qtng::MessageDigest hasher(qtng::MessageDigest::Sha256);
    std::string buf(1024 * 32, '\0');
    while (true) {
        const std::int32_t n = f->read(&buf[0], static_cast<std::int32_t>(buf.size()));
        if (n < 0) {
            return std::string();
        }
        if (n == 0) {
            break;
        }
        hasher.addData(buf.data(), n);
    }
    return hasher.result();
}

}  // namespace

class RpcFilePrivate
{
public:
    RpcFilePrivate(RpcFile *q);
    bool sendfileViaChannel(std::shared_ptr<qtng::FileLike> f, RpcFile::ProgressCallback progressCallback);
    bool recvfileViaChannel(std::shared_ptr<qtng::FileLike> f, RpcFile::ProgressCallback progressCallback,
                            const std::string &header);
    bool sendfileViaRawSocket(std::shared_ptr<qtng::FileLike> f, RpcFile::ProgressCallback progressCallback);
    bool recvfileViaRawSocket(std::shared_ptr<qtng::FileLike> f, RpcFile::ProgressCallback progressCallback,
                              const std::string &header);

public:
    std::string filePath;
    std::string name;
    std::uint64_t size;
    std::uint64_t atime;
    std::uint64_t mtime;
    std::uint64_t ctime;
    std::string hash;
    RpcFile * const q_ptr;
};

RpcFilePrivate::RpcFilePrivate(RpcFile *q)
    : size(0)
    , atime(0)
    , mtime(0)
    , ctime(0)
    , q_ptr(q)
{
}

bool RpcFilePrivate::sendfileViaChannel(std::shared_ptr<qtng::FileLike> f, RpcFile::ProgressCallback progressCallback)
{
    RpcFile *q = q_ptr;
    if (!q->channel) {
        return false;
    }
    if (size == 0) {
        if (progressCallback) {
            return progressCallback(0, 0, 0);
        }
        return true;
    }
    q->channel->setCapacity(8 * 1024 * 1024);

    std::uint64_t count = 0;
    std::string buf(static_cast<std::size_t>(BLOCK_SIZE), '\0');
    while (count < size) {
        const std::int32_t toRead = static_cast<std::int32_t>(
                std::min<std::int64_t>(BLOCK_SIZE, static_cast<std::int64_t>(size - count)));
        const std::int32_t readBytes = f->read(&buf[0], toRead);
        if (readBytes <= 0) {
            if (progressCallback) {
                progressCallback(-1, count, size);
            }
            return false;
        }
        if (!q->channel->sendPacket(buf.substr(0, static_cast<std::size_t>(readBytes)))) {
            if (progressCallback) {
                progressCallback(-1, count, size);
            }
            return false;
        }
        count += static_cast<std::uint64_t>(readBytes);
        if (progressCallback && !progressCallback(readBytes, count, size)) {
            return false;
        }
    }
    // Wait for the peer to finish reading (the next packet is the rpc response).
    q->channel->recvPacket();
    return true;
}

bool RpcFilePrivate::recvfileViaChannel(std::shared_ptr<qtng::FileLike> f, RpcFile::ProgressCallback progressCallback,
                                        const std::string &header)
{
    RpcFile *q = q_ptr;
    if (!q->channel) {
        return false;
    }
    if (size == 0) {
        if (progressCallback) {
            return progressCallback(0, 0, 0);
        }
        return true;
    }
    q->channel->setCapacity(8 * 1024 * 1024);

    std::uint64_t count = static_cast<std::uint64_t>(header.size());
    std::unique_ptr<qtng::MessageDigest> hasher;
    const bool doHash = !hash.empty();
    if (doHash) {
        hasher.reset(new qtng::MessageDigest(qtng::MessageDigest::Sha256));
    }
    while (count < size) {
        std::string buf = q->channel->recvPacket();
        if (buf.empty()) {
            if (progressCallback) {
                progressCallback(-1, count, size);
            }
            return false;
        }
        if (f->write(buf) != static_cast<std::int32_t>(buf.size())) {
            if (progressCallback) {
                progressCallback(-1, count, size);
            }
            return false;
        }
        count += static_cast<std::uint64_t>(buf.size());
        if (doHash) {
            hasher->addData(buf);
        }
        if (progressCallback && !progressCallback(static_cast<std::int64_t>(buf.size()), count, size)) {
            return false;
        }
    }
    if (doHash && hasher->result() != hash) {
        return false;
    }
    return true;
}

bool RpcFilePrivate::sendfileViaRawSocket(std::shared_ptr<qtng::FileLike> f,
                                          RpcFile::ProgressCallback progressCallback)
{
    RpcFile *q = q_ptr;
    if (!q->rawSocket) {
        return false;
    }
    if (size == 0) {
        if (progressCallback) {
            return progressCallback(-1, 0, 0);
        }
        return false;
    }
    std::uint64_t count = 0;
    std::string buf(static_cast<std::size_t>(BLOCK_SIZE), '\0');
    while (count < size) {
        const std::int32_t toRead = static_cast<std::int32_t>(
                std::min<std::int64_t>(BLOCK_SIZE, static_cast<std::int64_t>(size - count)));
        const std::int32_t readBytes = f->read(&buf[0], toRead);
        if (readBytes <= 0) {
            if (progressCallback) {
                progressCallback(-1, count, size);
            }
            return false;
        }
        if (q->rawSocket->sendall(buf.data(), readBytes) != readBytes) {
            if (progressCallback) {
                progressCallback(-1, count, size);
            }
            return false;
        }
        count += static_cast<std::uint64_t>(readBytes);
        if (progressCallback && !progressCallback(readBytes, count, size)) {
            return false;
        }
    }
    q->rawSocket->recv(1);
    return true;
}

bool RpcFilePrivate::recvfileViaRawSocket(std::shared_ptr<qtng::FileLike> f,
                                          RpcFile::ProgressCallback progressCallback, const std::string &header)
{
    RpcFile *q = q_ptr;
    if (!q->rawSocket) {
        return false;
    }
    if (size == 0) {
        if (progressCallback) {
            return progressCallback(0, 0, 0);
        }
        return true;
    }
    std::uint64_t count = static_cast<std::uint64_t>(header.size());
    std::unique_ptr<qtng::MessageDigest> hasher;
    const bool doHash = !hash.empty();
    if (doHash) {
        hasher.reset(new qtng::MessageDigest(qtng::MessageDigest::Sha256));
    }
    while (count < size) {
        std::string buf = q->rawSocket->recv(1024);
        if (buf.empty()) {
            if (progressCallback) {
                progressCallback(-1, count, size);
            }
            return false;
        }
        if (f->write(buf) != static_cast<std::int32_t>(buf.size())) {
            if (progressCallback) {
                progressCallback(-1, count, size);
            }
            return false;
        }
        count += static_cast<std::uint64_t>(buf.size());
        if (doHash) {
            hasher->addData(buf);
        }
        if (progressCallback && !progressCallback(static_cast<std::int64_t>(buf.size()), count, size)) {
            return false;
        }
    }
    if (doHash && hasher->result() != hash) {
        return false;
    }
    return true;
}

RpcFile::RpcFile(const std::string &filePath, bool withHash)
    : d_ptr(new RpcFilePrivate(this))
{
    RpcFilePrivate *d = d_ptr;
    d->filePath = filePath;
    std::error_code ec;
    std::filesystem::path p(filePath);
    d->name = p.filename().string();
    if (std::filesystem::exists(p, ec)) {
        d->size = static_cast<std::uint64_t>(std::filesystem::file_size(p, ec));
        const std::int64_t msecs = fileTimeToMSecs(std::filesystem::last_write_time(p, ec));
        d->ctime = msecs;
        d->mtime = msecs;
        d->atime = msecs;
        if (withHash) {
            calculateHash();
        }
    }
}

RpcFile::RpcFile()
    : d_ptr(new RpcFilePrivate(this))
{
}

RpcFile::~RpcFile()
{
    delete d_ptr;
}

std::shared_ptr<RpcFile> RpcFile::prepareToSend(std::int64_t size)
{
    std::shared_ptr<RpcFile> rpcFile = std::make_shared<RpcFile>();
    rpcFile->setName(qtng::utils::bytesToHex(qtng::randomBytes(8)));
    rpcFile->setSize(static_cast<std::uint64_t>(size));
    const qtng::utils::DateTime now = qtng::utils::DateTime::currentDateTimeUtc();
    rpcFile->setCreated(now);
    rpcFile->setModified(now);
    rpcFile->setLastAccess(now);
    return rpcFile;
}

bool RpcFile::calculateHash()
{
    RpcFilePrivate *d = d_ptr;
    if (d->filePath.empty()) {
        return false;
    }
    const std::string path = d->filePath;
    const std::string hash = qtng::callInThread<std::string>([path] { return computeFileHash(path); });
    if (hash.empty()) {
        return false;
    }
    d->hash = hash;
    return true;
}

bool RpcFile::isValid() const
{
    return !d_ptr->name.empty();
}

bool RpcFile::writeToPath(const std::string &path, ProgressCallback progressCallback)
{
    std::shared_ptr<qtng::FileLike> f = qtng::FileLike::open(path, "w");
    if (!f) {
        if (progressCallback) {
            progressCallback(-1, 0, d_ptr->size);
        }
        return false;
    }
    return writeTo(f, std::move(progressCallback));
}

bool RpcFile::readFromPath(const std::string &path, ProgressCallback progressCallback)
{
    std::shared_ptr<qtng::FileLike> f = qtng::FileLike::open(path, "r");
    if (!f) {
        if (progressCallback) {
            progressCallback(-1, 0, d_ptr->size);
        }
        return false;
    }
    return readFrom(f, std::move(progressCallback));
}

bool RpcFile::readFromPath(ProgressCallback progressCallback)
{
    if (d_ptr->filePath.empty()) {
        if (progressCallback) {
            progressCallback(-1, 0, d_ptr->size);
        }
        return false;
    }
    return readFromPath(d_ptr->filePath, std::move(progressCallback));
}

bool RpcFile::writeTo(std::shared_ptr<qtng::FileLike> f, ProgressCallback progressCallback)
{
    if (!ready.tryWait()) {
        return false;
    }
    if (rawSocket) {
        return d_ptr->recvfileViaRawSocket(f, std::move(progressCallback), std::string());
    }
    return d_ptr->recvfileViaChannel(f, std::move(progressCallback), std::string());
}

bool RpcFile::readFrom(std::shared_ptr<qtng::FileLike> f, ProgressCallback progressCallback)
{
    if (!ready.tryWait()) {
        return false;
    }
    if (rawSocket) {
        return d_ptr->sendfileViaRawSocket(f, std::move(progressCallback));
    }
    return d_ptr->sendfileViaChannel(f, std::move(progressCallback));
}

bool RpcFile::sendall(const std::string &data, ProgressCallback progressCallback)
{
    if (!ready.tryWait()) {
        return false;
    }
    std::shared_ptr<qtng::FileLike> f = qtng::FileLike::bytes(data);
    if (rawSocket) {
        return d_ptr->sendfileViaRawSocket(f, std::move(progressCallback));
    }
    return d_ptr->sendfileViaChannel(f, std::move(progressCallback));
}

bool RpcFile::recvall(std::string &data, ProgressCallback progressCallback)
{
    if (!ready.tryWait()) {
        return false;
    }
    std::shared_ptr<qtng::FileLike> f = qtng::FileLike::bytes(&data);
    if (rawSocket) {
        return d_ptr->recvfileViaRawSocket(f, std::move(progressCallback), std::string());
    }
    return d_ptr->recvfileViaChannel(f, std::move(progressCallback), std::string());
}

std::string RpcFile::name() const
{
    return d_ptr->name;
}

void RpcFile::setName(const std::string &name)
{
    d_ptr->name = name;
}

std::uint64_t RpcFile::size() const
{
    return d_ptr->size;
}

void RpcFile::setSize(std::uint64_t size)
{
    d_ptr->size = size;
}

qtng::utils::DateTime RpcFile::modified() const
{
    return qtng::utils::DateTime::fromMSecsSinceEpoch(static_cast<std::int64_t>(d_ptr->mtime));
}

void RpcFile::setModified(const qtng::utils::DateTime &dt)
{
    d_ptr->mtime = static_cast<std::uint64_t>(dt.toMSecsSinceEpoch());
}

qtng::utils::DateTime RpcFile::created() const
{
    return qtng::utils::DateTime::fromMSecsSinceEpoch(static_cast<std::int64_t>(d_ptr->ctime));
}

void RpcFile::setCreated(const qtng::utils::DateTime &dt)
{
    d_ptr->ctime = static_cast<std::uint64_t>(dt.toMSecsSinceEpoch());
}

qtng::utils::DateTime RpcFile::lastAccess() const
{
    return qtng::utils::DateTime::fromMSecsSinceEpoch(static_cast<std::int64_t>(d_ptr->atime));
}

void RpcFile::setLastAccess(const qtng::utils::DateTime &dt)
{
    d_ptr->atime = static_cast<std::uint64_t>(dt.toMSecsSinceEpoch());
}

std::string RpcFile::hash() const
{
    return d_ptr->hash;
}

void RpcFile::setHash(const std::string &hash)
{
    d_ptr->hash = hash;
}

Value RpcFile::saveState() const
{
    ValueMap m;
    m["name"] = Value::str(d_ptr->name);
    m["size"] = Value(d_ptr->size);
    m["mtime"] = Value(d_ptr->mtime);
    m["ctime"] = Value(d_ptr->ctime);
    m["atime"] = Value(d_ptr->atime);
    if (!d_ptr->hash.empty()) {
        m["hash"] = Value::bin(d_ptr->hash);
    }
    return Value(std::move(m));
}

bool RpcFile::restoreState(const Value &state)
{
    if (state.isNull() || state.type() != Value::Type::Map) {
        return false;
    }
    try {
        const Value *name = state.find("name");
        if (!name || name->type() != Value::Type::Str) {
            return false;
        }
        d_ptr->name = name->asStr();
        const Value *size = state.find("size");
        if (!size) {
            return false;
        }
        d_ptr->size = size->asUint();
        const Value *atime = state.find("atime");
        if (!atime) {
            return false;
        }
        d_ptr->atime = atime->asUint();
        const Value *ctime = state.find("ctime");
        if (!ctime) {
            return false;
        }
        d_ptr->ctime = ctime->asUint();
        const Value *mtime = state.find("mtime");
        if (!mtime) {
            return false;
        }
        d_ptr->mtime = mtime->asUint();
        const Value *hash = state.find("hash");
        if (hash) {
            d_ptr->hash = hash->asBin();
        }
    } catch (...) {
        return false;
    }
    return true;
}

std::shared_ptr<Serializable> RpcFile::clone() const
{
    std::shared_ptr<RpcFile> f = std::make_shared<RpcFile>();
    f->setName(name());
    f->setSize(size());
    f->setCreated(created());
    f->setModified(modified());
    f->setLastAccess(lastAccess());
    f->setHash(hash());
    return f;
}

}  // namespace rpc
}  // namespace qtng
