#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "qtng/coroutine_utils.h"
#include "qtng/io_utils.h"

#include "qtng/rpc/senddir.h"

using namespace std;

namespace qtng {
namespace rpc {

RpcDirFileEntry::RpcDirFileEntry()
    : size(0)
    , isdir(false)
{
}

CallbackInfo::CallbackInfo(const std::string &filePath, std::int32_t bs, std::uint64_t fileRead,
                           std::uint64_t fileSize, std::uint64_t totalRead, std::uint64_t totalSize)
    : filePath(filePath)
    , currentRead(bs)
    , currentFileRead(fileRead)
    , currentFileSize(fileSize)
    , totalRead(totalRead)
    , totalSize(totalSize)
{
}

RpcDirFileProvider::~RpcDirFileProvider() = default;

bool RpcDirFileProvider::createDirectory(const std::string &)
{
    return false;
}

bool RpcDirFileProvider::updateTimes(const std::string &, const qtng::utils::DateTime &,
                                     const qtng::utils::DateTime &, const qtng::utils::DateTime &)
{
    return false;
}

NativeRpcDirFileProvider::NativeRpcDirFileProvider(const std::string &root)
    : rootDir(std::filesystem::absolute(root).string())
{
}

std::string NativeRpcDirFileProvider::makePath(const std::string &filePath)
{
    if (filePath.empty() || filePath[0] == '/') {
        return std::string();
    }
    std::error_code ec;
    std::filesystem::path full = std::filesystem::weakly_canonical(
            std::filesystem::path(rootDir) / filePath, ec);
    if (ec) {
        return std::string();
    }
    const std::string fullStr = full.string();
    // reject paths that escape rootDir.
    if (fullStr.size() < rootDir.size() || fullStr.compare(0, rootDir.size(), rootDir) != 0) {
        return std::string();
    }
    return fullStr;
}

std::shared_ptr<qtng::FileLike> NativeRpcDirFileProvider::getFile(const std::string &filePath, bool writeMode)
{
    const std::string fullFilePath = makePath(filePath);
    if (fullFilePath.empty()) {
        return std::shared_ptr<qtng::FileLike>();
    }
    return qtng::FileLike::open(fullFilePath, writeMode ? "w" : "r");
}

bool NativeRpcDirFileProvider::createDirectory(const std::string &dirPath)
{
    const std::string fullDirPath = makePath(dirPath);
    if (fullDirPath.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::create_directories(fullDirPath, ec);
}

bool NativeRpcDirFileProvider::updateTimes(const std::string &filePath, const qtng::utils::DateTime &,
                                           const qtng::utils::DateTime &, const qtng::utils::DateTime &)
{
    const std::string fullFilePath = makePath(filePath);
    if (fullFilePath.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(fullFilePath, ec);
}

class RpcDirPrivate
{
public:
    RpcDirPrivate(RpcDir *q);
    bool writeTo(std::shared_ptr<RpcDirFileProvider> provider, RpcDir::ProgressCallback progressCallback);
    bool readFrom(std::shared_ptr<RpcDirFileProvider> provider, RpcDir::ProgressCallback progressCallback);
    void failAndAbort(RpcDir::ProgressCallback progressCallback, const CallbackInfo &info);

public:
    std::string name;
    std::string dirPath;
    std::uint64_t size;
    qtng::utils::DateTime created;
    qtng::utils::DateTime lastModified;
    qtng::utils::DateTime lastAccess;
    std::vector<RpcDirFileEntry> entries;
    RpcDir * const q_ptr;
};

RpcDirPrivate::RpcDirPrivate(RpcDir *q)
    : size(0)
    , q_ptr(q)
{
}

void RpcDirPrivate::failAndAbort(RpcDir::ProgressCallback progressCallback, const CallbackInfo &info)
{
    RpcDir *q = q_ptr;
    if (progressCallback) {
        progressCallback(info);
    }
    if (q->channel) {
        q->channel->abort();
    }
}

bool RpcDirPrivate::writeTo(std::shared_ptr<RpcDirFileProvider> provider, RpcDir::ProgressCallback progressCallback)
{
    RpcDir *q = q_ptr;
    if (!q->ready.tryWait()) {
        return false;
    }
    if (!q->channel) {
        return false;
    }
    q->channel->setCapacity(8 * 1024 * 1024);
    std::uint64_t totalWritten = 0;
    for (const RpcDirFileEntry &entry : entries) {
        if (entry.isdir) {
            if (!provider->createDirectory(entry.path)) {
                failAndAbort(progressCallback, CallbackInfo(entry.path, -1, 0, 0, totalWritten, size));
                return false;
            }
            if (progressCallback && !progressCallback(CallbackInfo(entry.path, 0, 0, 0, totalWritten, size))) {
                q->channel->abort();
                return true;
            }
            continue;
        }
        if (entry.size == 0) {
            std::shared_ptr<qtng::FileLike> file = provider->getFile(entry.path, true);
            if (!file) {
                failAndAbort(progressCallback, CallbackInfo(entry.path, -1, 0, 0, totalWritten, size));
                return false;
            }
            if (progressCallback && !progressCallback(CallbackInfo(entry.path, 0, 0, 0, totalWritten, size))) {
                q->channel->abort();
                return true;
            }
            continue;
        }

        std::shared_ptr<qtng::FileLike> file = provider->getFile(entry.path, true);
        if (!file) {
            failAndAbort(progressCallback, CallbackInfo(entry.path, -1, 0, entry.size, totalWritten, size));
            return false;
        }

        std::uint64_t fileWritten = 0;
        while (fileWritten < entry.size) {
            std::string buf = q->channel->recvPacket();
            if (buf.empty()) {
                failAndAbort(progressCallback,
                             CallbackInfo(entry.path, -1, fileWritten, entry.size, totalWritten, size));
                return false;
            }
            if (fileWritten + buf.size() > entry.size) {
                failAndAbort(progressCallback,
                             CallbackInfo(entry.path, -1, fileWritten, entry.size, totalWritten, size));
                return false;
            }
            if (file->write(buf) != static_cast<std::int32_t>(buf.size())) {
                failAndAbort(progressCallback,
                             CallbackInfo(entry.path, -1, fileWritten, entry.size, totalWritten, size));
                return false;
            }
            fileWritten += buf.size();
            totalWritten += buf.size();
            if (progressCallback
                && !progressCallback(CallbackInfo(entry.path, static_cast<std::int32_t>(buf.size()), fileWritten,
                                                  entry.size, totalWritten, size))) {
                q->channel->abort();
                return true;
            }
        }
        if (!provider->updateTimes(entry.path, entry.created, entry.lastModified, entry.lastAccess)) {
            failAndAbort(progressCallback,
                         CallbackInfo(entry.path, -1, fileWritten, entry.size, totalWritten, size));
            return false;
        }
    }
    return true;
}

bool RpcDirPrivate::readFrom(std::shared_ptr<RpcDirFileProvider> provider, RpcDir::ProgressCallback progressCallback)
{
    RpcDir *q = q_ptr;
    if (!q->ready.tryWait()) {
        return false;
    }
    if (!q->channel) {
        return false;
    }
    q->channel->setCapacity(8 * 1024 * 1024);
    std::uint64_t totalRead = 0;
    std::string buf(1024 * 64, '\0');
    for (const RpcDirFileEntry &entry : entries) {
        if (entry.isdir || entry.size == 0) {
            if (progressCallback && !progressCallback(CallbackInfo(entry.path, 0, 0, 0, totalRead, size))) {
                q->channel->abort();
                return true;
            }
            continue;
        }

        std::shared_ptr<qtng::FileLike> file = provider->getFile(entry.path, false);
        if (!file) {
            failAndAbort(progressCallback, CallbackInfo(entry.path, -1, 0, entry.size, totalRead, size));
            return false;
        }

        std::uint64_t fileRead = 0;
        std::int32_t blockSize = static_cast<std::int32_t>(q->channel->payloadSizeHint());
        if (blockSize <= 0) {
            blockSize = 1024 * 32;
        }
        if (buf.size() < static_cast<std::size_t>(blockSize)) {
            buf.resize(static_cast<std::size_t>(blockSize));
        }
        while (fileRead < entry.size) {
            const std::int64_t remaining = static_cast<std::int64_t>(entry.size - fileRead);
            const std::int32_t toRead = static_cast<std::int32_t>(std::min<std::int64_t>(blockSize, remaining));
            const std::int32_t bs = file->read(&buf[0], toRead);
            if (bs <= 0) {
                failAndAbort(progressCallback,
                             CallbackInfo(entry.path, -1, fileRead, entry.size, totalRead, size));
                return false;
            }
            if (!q->channel->sendPacket(buf.substr(0, static_cast<std::size_t>(bs)))) {
                failAndAbort(progressCallback,
                             CallbackInfo(entry.path, -1, fileRead, entry.size, totalRead, size));
                return false;
            }
            fileRead += static_cast<std::uint64_t>(bs);
            totalRead += static_cast<std::uint64_t>(bs);
            if (progressCallback
                && !progressCallback(CallbackInfo(entry.path, bs, fileRead, entry.size, totalRead, size))) {
                q->channel->abort();
                return true;
            }
        }
    }
    // Ensure all data drained / peer finished writeTo.
    q->channel->recvPacket();
    return true;
}

namespace {

std::int64_t fileTimeToMSecs(const std::filesystem::file_time_type &t)
{
    const std::int64_t secs =
            std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    return secs * 1000;
}

struct PopulateResult
{
    PopulateResult()
        : totalSize(0)
    {
    }
    std::vector<RpcDirFileEntry> entries;
    std::uint64_t totalSize;
};

void populateDir(const std::filesystem::path &dir, const std::string &relativePath, PopulateResult &result)
{
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> dirEntries;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        dirEntries.push_back(*it);
        if (ec) {
            break;
        }
    }
    std::sort(dirEntries.begin(), dirEntries.end(), [](const std::filesystem::directory_entry &a,
                                                       const std::filesystem::directory_entry &b) {
        return a.path().filename().string() < b.path().filename().string();
    });
    auto makeEntry = [&relativePath](const std::filesystem::directory_entry &de) {
        RpcDirFileEntry entry;
        const std::string name = de.path().filename().string();
        entry.path = relativePath.empty() ? name : relativePath + "/" + name;
        entry.isdir = de.is_directory();
        entry.size = entry.isdir ? 0 : static_cast<std::uint64_t>(de.file_size());
        const std::int64_t msecs = fileTimeToMSecs(de.last_write_time());
        entry.created = qtng::utils::DateTime::fromMSecsSinceEpoch(msecs);
        entry.lastModified = entry.created;
        entry.lastAccess = entry.created;
        return entry;
    };
    // directories first, then files.
    for (const std::filesystem::directory_entry &de : dirEntries) {
        if (!de.is_directory()) {
            continue;
        }
        result.entries.push_back(makeEntry(de));
        populateDir(de.path(), result.entries.back().path, result);
    }
    for (const std::filesystem::directory_entry &de : dirEntries) {
        if (de.is_directory()) {
            continue;
        }
        RpcDirFileEntry entry = makeEntry(de);
        result.totalSize += entry.size;
        result.entries.push_back(std::move(entry));
    }
}

PopulateResult populateDirectory(const std::string &dirPath)
{
    PopulateResult result;
    std::error_code ec;
    if (std::filesystem::exists(dirPath, ec) && std::filesystem::is_directory(dirPath, ec)) {
        populateDir(dirPath, std::string(), result);
    }
    return result;
}

}  // namespace

bool RpcDir::populate()
{
    RpcDirPrivate *d = d_ptr;
    if (d->dirPath.empty()) {
        return false;
    }
    const std::string dirPath = d->dirPath;
    const PopulateResult result = qtng::callInThread<PopulateResult>([dirPath] { return populateDirectory(dirPath); });
    d->entries = result.entries;
    d->size = result.totalSize;
    return true;
}

bool RpcDir::isValid() const
{
    return !d_ptr->name.empty();
}

bool RpcDir::writeToPath(const std::string &path, ProgressCallback progressCallback)
{
    return d_ptr->writeTo(std::make_shared<NativeRpcDirFileProvider>(path), std::move(progressCallback));
}

bool RpcDir::readFromPath(const std::string &path, ProgressCallback progressCallback)
{
    return d_ptr->readFrom(std::make_shared<NativeRpcDirFileProvider>(path), std::move(progressCallback));
}

bool RpcDir::readFromPath(ProgressCallback progressCallback)
{
    if (d_ptr->dirPath.empty()) {
        return false;
    }
    return d_ptr->readFrom(std::make_shared<NativeRpcDirFileProvider>(d_ptr->dirPath),
                           std::move(progressCallback));
}

bool RpcDir::writeTo(std::shared_ptr<RpcDirFileProvider> provider, ProgressCallback progressCallback)
{
    return d_ptr->writeTo(std::move(provider), std::move(progressCallback));
}

bool RpcDir::readFrom(std::shared_ptr<RpcDirFileProvider> provider, ProgressCallback progressCallback)
{
    return d_ptr->readFrom(std::move(provider), std::move(progressCallback));
}

std::string RpcDir::name() const
{
    return d_ptr->name;
}

void RpcDir::setName(const std::string &name)
{
    d_ptr->name = name;
}

std::uint64_t RpcDir::size() const
{
    return d_ptr->size;
}

void RpcDir::setSize(std::uint64_t size)
{
    d_ptr->size = size;
}

qtng::utils::DateTime RpcDir::lastModified() const
{
    return d_ptr->lastModified;
}

void RpcDir::setLastModified(const qtng::utils::DateTime &dt)
{
    d_ptr->lastModified = dt;
}

qtng::utils::DateTime RpcDir::created() const
{
    return d_ptr->created;
}

void RpcDir::setCreated(const qtng::utils::DateTime &dt)
{
    d_ptr->created = dt;
}

qtng::utils::DateTime RpcDir::lastAccess() const
{
    return d_ptr->lastAccess;
}

void RpcDir::setLastAccess(const qtng::utils::DateTime &dt)
{
    d_ptr->lastAccess = dt;
}

std::vector<RpcDirFileEntry> RpcDir::entries() const
{
    return d_ptr->entries;
}

void RpcDir::setEntries(const std::vector<RpcDirFileEntry> &entries)
{
    d_ptr->entries = entries;
}

Value RpcDir::saveState() const
{
    ValueMap m;
    m["name"] = Value::str(d_ptr->name);
    m["size"] = Value(d_ptr->size);
    if (d_ptr->created.isValid()) {
        m["ctime"] = Value(d_ptr->created);
    }
    if (d_ptr->lastModified.isValid()) {
        m["mtime"] = Value(d_ptr->lastModified);
    }
    if (d_ptr->lastAccess.isValid()) {
        m["atime"] = Value(d_ptr->lastAccess);
    }
    std::vector<Value> entryList;
    for (const RpcDirFileEntry &entry : d_ptr->entries) {
        ValueMap entryObj;
        entryObj["path"] = Value::str(entry.path);
        entryObj["size"] = Value(entry.size);
        if (entry.created.isValid()) {
            entryObj["ctime"] = Value(entry.created);
        }
        if (entry.lastModified.isValid()) {
            entryObj["mtime"] = Value(entry.lastModified);
        }
        if (entry.lastAccess.isValid()) {
            entryObj["atime"] = Value(entry.lastAccess);
        }
        entryObj["isdir"] = Value(entry.isdir);
        entryList.emplace_back(std::move(entryObj));
    }
    m["entries"] = Value(std::move(entryList));
    return Value(std::move(m));
}

bool RpcDir::restoreState(const Value &state)
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
        const Value *ctime = state.find("ctime");
        d_ptr->created = (ctime && ctime->type() == Value::Type::DateTime) ? ctime->asDateTime() : qtng::utils::DateTime();
        const Value *mtime = state.find("mtime");
        d_ptr->lastModified =
                (mtime && mtime->type() == Value::Type::DateTime) ? mtime->asDateTime() : qtng::utils::DateTime();
        const Value *atime = state.find("atime");
        d_ptr->lastAccess =
                (atime && atime->type() == Value::Type::DateTime) ? atime->asDateTime() : qtng::utils::DateTime();

        d_ptr->entries.clear();
        const Value *entries = state.find("entries");
        if (!entries || entries->type() != Value::Type::Array) {
            return false;
        }
        for (const Value &t : entries->asArray()) {
            if (t.type() != Value::Type::Map) {
                return false;
            }
            const ValueMap &entryObj = t.asMap();
            RpcDirFileEntry entry;
            ValueMap::const_iterator pit = entryObj.find("path");
            if (pit == entryObj.end() || pit->second.type() != Value::Type::Str) {
                return false;
            }
            entry.path = pit->second.asStr();
            ValueMap::const_iterator sit = entryObj.find("size");
            if (sit == entryObj.end()) {
                return false;
            }
            entry.size = sit->second.asUint();
            ValueMap::const_iterator dit = entryObj.find("isdir");
            entry.isdir = dit != entryObj.end() ? dit->second.asBool() : false;
            ValueMap::const_iterator it;
            it = entryObj.find("ctime");
            entry.created = (it != entryObj.end() && it->second.type() == Value::Type::DateTime)
                    ? it->second.asDateTime()
                    : qtng::utils::DateTime();
            it = entryObj.find("mtime");
            entry.lastModified = (it != entryObj.end() && it->second.type() == Value::Type::DateTime)
                    ? it->second.asDateTime()
                    : qtng::utils::DateTime();
            it = entryObj.find("atime");
            entry.lastAccess = (it != entryObj.end() && it->second.type() == Value::Type::DateTime)
                    ? it->second.asDateTime()
                    : qtng::utils::DateTime();
            d_ptr->entries.push_back(std::move(entry));
        }
    } catch (...) {
        return false;
    }
    return true;
}

std::shared_ptr<Serializable> RpcDir::clone() const
{
    std::shared_ptr<RpcDir> d = std::make_shared<RpcDir>();
    d->setName(name());
    d->setSize(size());
    d->setCreated(created());
    d->setLastModified(lastModified());
    d->setLastAccess(lastAccess());
    d->setEntries(entries());
    return d;
}

RpcDir::RpcDir(const std::string &path)
    : d_ptr(new RpcDirPrivate(this))
{
    RpcDirPrivate *d = d_ptr;
    std::error_code ec;
    std::filesystem::path p(path);
    d->dirPath = path;
    d->name = p.filename().string();
    if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
        const std::int64_t msecs = fileTimeToMSecs(std::filesystem::last_write_time(p, ec));
        d->created = qtng::utils::DateTime::fromMSecsSinceEpoch(msecs);
        d->lastModified = d->created;
        d->lastAccess = d->created;
        populate();
    }
}

RpcDir::RpcDir()
    : d_ptr(new RpcDirPrivate(this))
{
}

RpcDir::~RpcDir()
{
    delete d_ptr;
}

}  // namespace rpc
}  // namespace qtng
