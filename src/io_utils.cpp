#include "qtng/utils/platform.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef NG_OS_UNIX
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef NG_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// std::filesystem requires C++17. The header may exist on GCC 8+ even when
// compiling as C++11; only enable it when the language mode actually supports it.
#if defined(__has_include)
#  if __has_include(<filesystem>) && (__cplusplus >= 201703L) && !(defined(__GNUC__) && __GNUC__ < 8)
#    include <filesystem>
#    define QTNG_HAS_STD_FILESYSTEM 1
#  endif
#endif
#if defined(QTNG_HAS_STD_FILESYSTEM)
#  include <system_error>
#endif
#include "qtng/io_utils.h"
#include "qtng/coroutine_utils.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.io_utils");

namespace qtng {

namespace {

#ifdef NG_OS_UNIX
bool setNonBlocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

int openModeint(const string &mode, bool *append)
{
    *append = false;
    if (mode.empty() || mode == "r") {
        return O_RDONLY;
    }
    if (mode == "w") {
        return O_WRONLY | O_CREAT | O_TRUNC;
    }
    if (mode == "a") {
        *append = true;
        return O_WRONLY | O_CREAT | O_APPEND;
    }
    if (mode == "r+") {
        return O_RDWR;
    }
    if (mode == "w+") {
        return O_RDWR | O_CREAT | O_TRUNC;
    }
    if (mode == "a+") {
        *append = true;
        return O_RDWR | O_CREAT | O_APPEND;
    }
    return -1;
}
#endif

}  // namespace

namespace PathFs {

#if defined(QTNG_HAS_STD_FILESYSTEM)

bool isRegularFile(const string &path)
{
    error_code ec;
    return filesystem::is_regular_file(filesystem::path(path), ec);
}

bool isDirectory(const string &path)
{
    error_code ec;
    return filesystem::is_directory(filesystem::path(path), ec);
}

bool isSymlink(const string &path)
{
    error_code ec;
    return filesystem::is_symlink(filesystem::path(path), ec);
}

bool isAbsolute(const string &path)
{
    return filesystem::path(path).is_absolute();
}

bool exists(const string &path)
{
    error_code ec;
    return filesystem::exists(filesystem::path(path), ec);
}

int64_t fileSize(const string &path)
{
    error_code ec;
    auto sz = filesystem::file_size(filesystem::path(path), ec);
    return ec ? -1 : static_cast<int64_t>(sz);
}

string parentPath(const string &path)
{
    return filesystem::path(path).parent_path().string();
}

string filename(const string &path)
{
    return filesystem::path(path).filename().string();
}

string stem(const string &path)
{
    return filesystem::path(path).stem().string();
}

string extension(const string &path)
{
    return filesystem::path(path).extension().string();
}

string absolute(const string &path)
{
    error_code ec;
    auto abs = filesystem::absolute(filesystem::path(path), ec);
    return ec ? path : abs.string();
}

string relative(const string &path, const string &base)
{
    error_code ec;
    auto rel = filesystem::relative(filesystem::path(path), filesystem::path(base), ec);
    return ec ? string() : rel.string();
}

bool isChildOf(const string &path, const string &base)
{
    error_code ec;
    auto rel = filesystem::relative(filesystem::path(path), filesystem::path(base), ec);
    return !ec && !rel.empty() && rel.string()[0] != '.';
}

int64_t lastWriteTimeMsecs(const string &path)
{
    error_code ec;
    auto ft = filesystem::last_write_time(filesystem::path(path), ec);
    if (ec) {
        return -1;
    }
    using chrono::time_point_cast;
    auto sctp = time_point_cast<chrono::system_clock::duration>(
            ft - filesystem::file_time_type::clock::now() + chrono::system_clock::now());
    return chrono::duration_cast<chrono::milliseconds>(sctp.time_since_epoch()).count();
}

vector<string> listDirectory(const string &path)
{
    vector<string> result;
    error_code ec;
    for (const auto &entry : filesystem::directory_iterator(filesystem::path(path), ec)) {
        result.push_back(entry.path().filename().string());
    }
    return result;
}

bool createDirectory(const string &path)
{
    error_code ec;
    filesystem::create_directory(filesystem::path(path), ec);
    return !ec;
}

bool createDirectories(const string &path)
{
    error_code ec;
    filesystem::create_directories(filesystem::path(path), ec);
    return !ec;
}

string currentPath()
{
    error_code ec;
    auto p = filesystem::current_path(ec);
    return ec ? string() : p.string();
}

#else  // !QTNG_HAS_STD_FILESYSTEM

namespace {

string normalizePath(string path)
{
    const bool absolute = !path.empty() && path[0] == '/';
    vector<string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        const size_t j = path.find('/', i);
        const size_t end = (j == string::npos) ? path.size() : j;
        const string seg = path.substr(i, end - i);
        if (!seg.empty() && seg != ".") {
            if (seg == "..") {
                if (!parts.empty() && parts.back() != "..") {
                    parts.pop_back();
                } else if (!absolute) {
                    parts.push_back("..");
                }
            } else {
                parts.push_back(seg);
            }
        }
        if (j == string::npos) {
            break;
        }
        i = j + 1;
    }
    if (parts.empty()) {
        return absolute ? "/" : string();
    }
    string result;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) {
            result += '/';
        }
        result += parts[k];
    }
    if (absolute) {
        result.insert(result.begin(), '/');
    }
    return result;
}

string pathFilename(const string &path)
{
    if (path.empty() || path == "/") {
        return string();
    }
    size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') {
        --end;
    }
    if (end == 0) {
        return string();
    }
    const size_t pos = path.rfind('/', end - 1);
    if (pos == string::npos) {
        return path.substr(0, end);
    }
    return path.substr(pos + 1, end - pos - 1);
}

#ifdef NG_OS_UNIX

bool statPath(const string &path, struct stat *st, bool followLink)
{
    return followLink ? ::stat(path.c_str(), st) == 0 : ::lstat(path.c_str(), st) == 0;
}

#endif

#ifdef NG_OS_WIN

bool win32Attributes(const string &path, DWORD *attr)
{
    *attr = GetFileAttributesA(path.c_str());
    return *attr != INVALID_FILE_ATTRIBUTES;
}

#endif

}  // namespace

bool isRegularFile(const string &path)
{
#ifdef NG_OS_UNIX
    struct stat st;
    return statPath(path, &st, true) && S_ISREG(st.st_mode);
#elif defined(NG_OS_WIN)
    DWORD attr = 0;
    return win32Attributes(path, &attr) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)path;
    return false;
#endif
}

bool isDirectory(const string &path)
{
#ifdef NG_OS_UNIX
    struct stat st;
    return statPath(path, &st, true) && S_ISDIR(st.st_mode);
#elif defined(NG_OS_WIN)
    DWORD attr = 0;
    return win32Attributes(path, &attr) && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)path;
    return false;
#endif
}

bool isSymlink(const string &path)
{
#ifdef NG_OS_UNIX
    struct stat st;
    return statPath(path, &st, false) && S_ISLNK(st.st_mode);
#elif defined(NG_OS_WIN)
    DWORD attr = 0;
    return win32Attributes(path, &attr) && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    (void)path;
    return false;
#endif
}

bool isAbsolute(const string &path)
{
#ifdef NG_OS_WIN
    if (path.size() >= 2 && path[1] == ':') {
        return true;
    }
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
        return true;
    }
#endif
    return !path.empty() && path[0] == '/';
}

bool exists(const string &path)
{
    return isRegularFile(path) || isDirectory(path) || isSymlink(path);
}

int64_t fileSize(const string &path)
{
#ifdef NG_OS_UNIX
    struct stat st;
    if (!statPath(path, &st, true)) {
        return -1;
    }
    return static_cast<int64_t>(st.st_size);
#elif defined(NG_OS_WIN)
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        return -1;
    }
    ULARGE_INTEGER size;
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    return static_cast<int64_t>(size.QuadPart);
#else
    (void)path;
    return -1;
#endif
}

string parentPath(const string &path)
{
    if (path.empty() || path == "/") {
        return path == "/" ? "/" : string();
    }
    size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') {
        --end;
    }
    if (end == 0) {
        return "/";
    }
    const size_t pos = path.rfind('/', end - 1);
    if (pos == string::npos) {
        return string();
    }
    if (pos == 0) {
        return "/";
    }
    return path.substr(0, pos);
}

string filename(const string &path)
{
    return pathFilename(path);
}

string stem(const string &path)
{
    const string name = pathFilename(path);
    const size_t dot = name.rfind('.');
    if (dot == string::npos || dot == 0) {
        return name;
    }
    return name.substr(0, dot);
}

string extension(const string &path)
{
    const string name = pathFilename(path);
    const size_t dot = name.rfind('.');
    if (dot == string::npos || dot == 0) {
        return string();
    }
    return name.substr(dot);
}

string absolute(const string &path)
{
    if (path.empty()) {
        return string();
    }
    if (isAbsolute(path)) {
        return normalizePath(path);
    }
#ifdef NG_OS_UNIX
    char cwd[PATH_MAX];
    if (!::getcwd(cwd, sizeof(cwd))) {
        return normalizePath(path);
    }
    string result = cwd;
    if (result.back() != '/') {
        result += '/';
    }
    result += path;
    return normalizePath(result);
#elif defined(NG_OS_WIN)
    char buf[MAX_PATH];
    const DWORD len = GetFullPathNameA(path.c_str(), MAX_PATH, buf, nullptr);
    if (len == 0 || len >= MAX_PATH) {
        return normalizePath(path);
    }
    return string(buf);
#else
    return normalizePath(path);
#endif
}

string relative(const string &path, const string &base)
{
    const string absPath = normalizePath(absolute(path));
    const string absBase = normalizePath(absolute(base));
    if (absPath == absBase) {
        return ".";
    }
    vector<string> pathParts = utils::split(absPath, "/");
    vector<string> baseParts = utils::split(absBase, "/");
    if (!absPath.empty() && absPath[0] == '/') {
        if (pathParts.empty() || !pathParts.front().empty()) {
            pathParts.insert(pathParts.begin(), string());
        }
    }
    if (!absBase.empty() && absBase[0] == '/') {
        if (baseParts.empty() || !baseParts.front().empty()) {
            baseParts.insert(baseParts.begin(), string());
        }
    }
    size_t common = 0;
    const size_t minSize = min(pathParts.size(), baseParts.size());
    while (common < minSize && pathParts[common] == baseParts[common]) {
        ++common;
    }
    string result;
    for (size_t i = common; i < baseParts.size(); ++i) {
        if (!baseParts[i].empty()) {
            if (!result.empty()) {
                result += '/';
            }
            result += "..";
        }
    }
    for (size_t i = common; i < pathParts.size(); ++i) {
        if (!pathParts[i].empty()) {
            if (!result.empty()) {
                result += '/';
            }
            result += pathParts[i];
        }
    }
    return result.empty() ? "." : result;
}

bool isChildOf(const string &path, const string &base)
{
    const string rel = relative(path, base);
    return !rel.empty() && rel != "." && rel[0] != '.';
}

int64_t lastWriteTimeMsecs(const string &path)
{
#ifdef NG_OS_UNIX
    struct stat st;
    if (!statPath(path, &st, true)) {
        return -1;
    }
    return static_cast<int64_t>(st.st_mtime) * 1000;
#elif defined(NG_OS_WIN)
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        return -1;
    }
    ULARGE_INTEGER ull;
    ull.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ull.HighPart = data.ftLastWriteTime.dwHighDateTime;
    const int64_t windowsEpochOffset = 11644473600000LL;
    return static_cast<int64_t>(ull.QuadPart / 10000) - windowsEpochOffset;
#else
    (void)path;
    return -1;
#endif
}

vector<string> listDirectory(const string &path)
{
    vector<string> result;
#ifdef NG_OS_UNIX
    DIR *dir = ::opendir(path.c_str());
    if (!dir) {
        return result;
    }
    while (dirent *ent = ::readdir(dir)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        result.push_back(ent->d_name);
    }
    ::closedir(dir);
#elif defined(NG_OS_WIN)
    string pattern = path;
    if (!pattern.empty() && pattern.back() != '/' && pattern.back() != '\\') {
        pattern += '\\';
    }
    pattern += '*';
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return result;
    }
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
            continue;
        }
        result.push_back(data.cFileName);
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
#else
    (void)path;
#endif
    return result;
}

bool createDirectory(const string &path)
{
#ifdef NG_OS_UNIX
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#elif defined(NG_OS_WIN)
    return CreateDirectoryA(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    (void)path;
    return false;
#endif
}

bool createDirectories(const string &path)
{
    if (path.empty()) {
        return false;
    }
    if (isDirectory(path)) {
        return true;
    }
    const string parent = parentPath(path);
    if (!parent.empty() && parent != path && !isDirectory(parent)) {
        if (!createDirectories(parent)) {
            return false;
        }
    }
    return createDirectory(path);
}

string currentPath()
{
#ifdef NG_OS_UNIX
    char buf[PATH_MAX];
    if (!::getcwd(buf, sizeof(buf))) {
        return string();
    }
    return string(buf);
#elif defined(NG_OS_WIN)
    char buf[MAX_PATH];
    const DWORD len = GetCurrentDirectoryA(MAX_PATH, buf);
    if (len == 0 || len >= MAX_PATH) {
        return string();
    }
    return string(buf);
#else
    return string();
#endif
}

#endif  // QTNG_HAS_STD_FILESYSTEM

}  // namespace PathFs

FileLike::~FileLike() { }

string FileLike::readall(bool *ok)
{
    string data;
    int64_t s = size();
    if (s >= static_cast<int64_t>(INT32_MAX)) {
        if (ok) {
            *ok = false;
        }
        return data;
    }
    if (s > 0) {
        data.reserve(static_cast<size_t>(s));
    }
    char buf[1024 * 8];
    while (true) {
        int32_t readBytes = read(buf, static_cast<int32_t>(sizeof(buf)));
        if (readBytes <= 0) {
            if (ok) {
                *ok = (s < 0 || static_cast<int64_t>(data.size()) == s);
            }
            return data;
        }
        data.append(buf, static_cast<size_t>(readBytes));
    }
}

string FileLike::read(int32_t size)
{
    string buf(static_cast<size_t>(size), '\0');
    int32_t readBytes = this->read(&buf[0], size);
    if (readBytes <= 0) {
        return string();
    }
    if (readBytes < size) {
        buf.resize(static_cast<size_t>(readBytes));
    }
    return buf;
}

int32_t FileLike::write(const string &data)
{
    return this->write(data.data(), static_cast<int32_t>(data.size()));
}

class RawFile : public FileLike
{
public:
    explicit RawFile(int fdIn)
        : fd(fdIn)
#ifdef NG_OS_UNIX
        , useFd(true)
#endif
    {
    }
    explicit RawFile(unique_ptr<fstream> streamIn)
        : stream(move(streamIn))
#ifdef NG_OS_UNIX
        , fd(-1)
        , useFd(false)
#endif
    {
    }
    ~RawFile() override { close(); }

    int32_t read(char *data, int32_t size) override;
    int32_t write(const char *data, int32_t size) override;
    void close() override;
    int64_t size() override;

    static shared_ptr<FileLike> open(const string &filepath, const string &mode);

private:
#ifdef NG_OS_UNIX
    int fd;
    bool useFd;
#endif
    unique_ptr<fstream> stream;
};

int32_t RawFile::read(char *data, int32_t size)
{
#ifdef NG_OS_UNIX
    if (useFd) {
        if (fd <= 0) {
            return -1;
        }
        ScopedIoWatcher watcher(EventLoopCoroutine::Read, fd);
        while (true) {
            ssize_t r = 0;
            do {
                r = ::read(fd, data, static_cast<size_t>(size));
            } while (r < 0 && errno == EINTR);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!watcher.start()) {
                        return -1;
                    }
                    continue;
                }
                return -1;
            }
            return static_cast<int32_t>(r);
        }
    }
#endif
    if (!stream || !stream->good()) {
        return -1;
    }
    stream->read(data, size);
    return static_cast<int32_t>(stream->gcount());
}

int32_t RawFile::write(const char *data, int32_t size)
{
#ifdef NG_OS_UNIX
    if (useFd) {
        if (fd <= 0) {
            return -1;
        }
        ScopedIoWatcher watcher(EventLoopCoroutine::Write, fd);
        while (true) {
            ssize_t r = 0;
            do {
                r = ::write(fd, data, static_cast<size_t>(size));
            } while (r < 0 && errno == EINTR);
            if (r <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!watcher.start()) {
                        return -1;
                    }
                    continue;
                }
                return -1;
            }
            return static_cast<int32_t>(r);
        }
    }
#endif
    if (!stream || !stream->good()) {
        return -1;
    }
    stream->write(data, size);
    if (!stream->good()) {
        return -1;
    }
    return size;
}

void RawFile::close()
{
#ifdef NG_OS_UNIX
    if (useFd && fd > 0) {
        ::close(fd);
        fd = -1;
    }
#endif
    if (stream) {
        stream->close();
        stream.reset();
    }
}

int64_t RawFile::size()
{
#ifdef NG_OS_UNIX
    if (useFd && fd > 0) {
        struct stat st;
        if (::fstat(fd, &st) == 0) {
            return st.st_size;
        }
        return -1;
    }
#endif
    if (!stream) {
        return -1;
    }
    auto cur = stream->tellg();
    stream->seekg(0, ios::end);
    int64_t s = static_cast<int64_t>(stream->tellg());
    stream->seekg(cur);
    return s;
}

shared_ptr<FileLike> RawFile::open(const string &filepath, const string &mode)
{
#ifdef NG_OS_UNIX
    bool append = false;
    int flags = openModeint(mode, &append);
    if (flags < 0) {
        ngWarning() << "unknown file mode:" << mode;
        return shared_ptr<FileLike>();
    }
    int fd = ::open(filepath.c_str(), flags, 0644);
    if (fd < 0) {
        return shared_ptr<FileLike>();
    }
    if (!setNonBlocking(fd)) {
        ::close(fd);
        return shared_ptr<FileLike>();
    }
    shared_ptr<RawFile> file(new RawFile(fd));
    if (append) {
        ::lseek(fd, 0, SEEK_END);
    }
    return file;
#else
    ios::openmode fmode = ios::binary;
    if (mode.empty() || mode == "r") {
        fmode |= ios::in;
    } else if (mode == "w") {
        fmode |= ios::out | ios::trunc;
    } else if (mode == "a") {
        fmode |= ios::out | ios::app;
    } else if (mode == "r+") {
        fmode |= ios::in | ios::out;
    } else if (mode == "w+") {
        fmode |= ios::in | ios::out | ios::trunc;
    } else if (mode == "a+") {
        fmode |= ios::in | ios::out | ios::app;
    } else {
        ngWarning() << "unknown file mode:" << mode;
        return shared_ptr<FileLike>();
    }
    auto stream = make_unique<fstream>(filepath, fmode);
    if (!stream->is_open()) {
        return shared_ptr<FileLike>();
    }
    return make_shared<RawFile>(move(stream));
#endif
}

shared_ptr<FileLike> FileLike::open(const string &filepath, const string &mode)
{
    return RawFile::open(filepath, mode);
}

class BytesIOPrivate
{
public:
    explicit BytesIOPrivate(int32_t pos)
        : buf(nullptr)
        , pos(pos)
        , ownbuf(false)
    {
    }
    string *buf;
    string owned;
    int32_t pos;
    bool ownbuf;
};

BytesIO::BytesIO(const string &buf, int32_t pos)
    : d_ptr(new BytesIOPrivate(pos))
{
    NG_D(BytesIO);
    d->owned = buf;
    d->buf = &d->owned;
}

BytesIO::BytesIO(string *buf, int32_t pos)
    : d_ptr(new BytesIOPrivate(pos))
{
    NG_D(BytesIO);
    d->buf = buf;
}

BytesIO::BytesIO()
    : d_ptr(new BytesIOPrivate(0))
{
    NG_D(BytesIO);
    d->ownbuf = true;
    d->owned.clear();
    d->buf = &d->owned;
}

BytesIO::~BytesIO()
{
    delete d_ptr;
}

int32_t BytesIO::read(char *data, int32_t size)
{
    NG_D(BytesIO);
    if (!d->buf) {
        return -1;
    }
    int32_t available = static_cast<int32_t>(d->buf->size()) - d->pos;
    if (available <= 0) {
        return 0;
    }
    int32_t toRead = min(size, available);
    memcpy(data, d->buf->data() + d->pos, static_cast<size_t>(toRead));
    d->pos += toRead;
    return toRead;
}

int32_t BytesIO::write(const char *data, int32_t size)
{
    NG_D(BytesIO);
    if (!d->buf) {
        return -1;
    }
    if (d->pos > static_cast<int32_t>(d->buf->size())) {
        d->buf->resize(static_cast<size_t>(d->pos));
    }
    if (d->pos + size > static_cast<int32_t>(d->buf->size())) {
        d->buf->resize(static_cast<size_t>(d->pos + size));
    }
    memcpy(&(*d->buf)[static_cast<size_t>(d->pos)], data, static_cast<size_t>(size));
    d->pos += size;
    return size;
}

void BytesIO::close()
{
    NG_D(BytesIO);
    d->buf = nullptr;
}

int64_t BytesIO::size()
{
    NG_D(BytesIO);
    if (!d->buf) {
        return -1;
    }
    return static_cast<int64_t>(d->buf->size());
}

string BytesIO::readall(bool *ok)
{
    NG_D(BytesIO);
    if (!d->buf) {
        if (ok) {
            *ok = false;
        }
        return string();
    }
    if (ok) {
        *ok = true;
    }
    if (d->pos >= static_cast<int32_t>(d->buf->size())) {
        return string();
    }
    return d->buf->substr(static_cast<size_t>(d->pos));
}

string BytesIO::data()
{
    NG_D(BytesIO);
    return d->buf ? *d->buf : string();
}

shared_ptr<FileLike> FileLike::bytes(const string &data)
{
    return make_shared<BytesIO>(data);
}

shared_ptr<FileLike> FileLike::bytes(string *data)
{
    return make_shared<BytesIO>(data);
}

bool sendfile(shared_ptr<FileLike> inputFile, shared_ptr<FileLike> outputFile, int64_t bytesToCopy, int suitableBlockSize)
{
    if (!inputFile || !outputFile) {
        return false;
    }
    if (suitableBlockSize <= 0) {
        suitableBlockSize = 1024 * 8;
    }
    vector<char> buf(static_cast<size_t>(suitableBlockSize));
    int64_t copied = 0;
    while (bytesToCopy < 0 || copied < bytesToCopy) {
        int32_t toRead = suitableBlockSize;
        if (bytesToCopy >= 0) {
            toRead = static_cast<int32_t>(min<int64_t>(suitableBlockSize, bytesToCopy - copied));
        }
        int32_t readBytes = inputFile->read(&buf[0], toRead);
        if (readBytes <= 0) {
            break;
        }
        int32_t written = outputFile->write(buf.data(), readBytes);
        if (written != readBytes) {
            return false;
        }
        copied += readBytes;
    }
    return true;
}

class PipePrivate : public enable_shared_from_this<PipePrivate>
{
public:
    explicit PipePrivate(int32_t maxBufferSize)
        : queue(static_cast<uint32_t>(maxBufferSize))
        , closed(false)
        , debugLevel(0)
    {
    }

    Queue<string> queue;
    bool closed;
    int8_t debugLevel;
    function<void()> readyReadCallback;
    function<void(int64_t)> bytesWrittenCallback;
};

Pipe::Pipe(int32_t maxBufferSize)
    : d(make_shared<PipePrivate>(maxBufferSize))
{
}

void Pipe::setDebugLevel(int8_t debugLevel)
{
    d->debugLevel = debugLevel;
}

void Pipe::setReadyReadCallback(function<void()> callback)
{
    d->readyReadCallback = move(callback);
}

void Pipe::setBytesWrittenCallback(function<void(int64_t)> callback)
{
    d->bytesWrittenCallback = move(callback);
}

class FileToRead : public FileLike
{
public:
    explicit FileToRead(shared_ptr<PipePrivate> pp)
        : pp(move(pp))
    {
    }

    int32_t read(char *data, int32_t size) override
    {
        string chunk = pp->queue.get();
        if (chunk.empty()) {
            return 0;
        }
        int32_t toCopy = min(size, static_cast<int32_t>(chunk.size()));
        memcpy(data, chunk.data(), static_cast<size_t>(toCopy));
        if (toCopy < static_cast<int32_t>(chunk.size())) {
            pp->queue.returns(chunk.substr(static_cast<size_t>(toCopy)));
        }
        if (pp->bytesWrittenCallback) {
            pp->bytesWrittenCallback(toCopy);
        }
        return toCopy;
    }

    int32_t write(const char *, int32_t) override { return -1; }
    void close() override { pp->closed = true; }
    int64_t size() override { return -1; }

    shared_ptr<PipePrivate> pp;
};

class FileToWrite : public FileLike
{
public:
    explicit FileToWrite(shared_ptr<PipePrivate> pp)
        : pp(move(pp))
    {
    }

    int32_t read(char *, int32_t) override { return -1; }
    int32_t write(const char *data, int32_t size) override
    {
        if (pp->closed) {
            return -1;
        }
        string chunk(data, static_cast<size_t>(size));
        if (!pp->queue.put(chunk)) {
            return -1;
        }
        if (pp->readyReadCallback) {
            pp->readyReadCallback();
        }
        return size;
    }
    void close() override { pp->closed = true; }
    int64_t size() override { return -1; }

    shared_ptr<PipePrivate> pp;
};

shared_ptr<FileLike> Pipe::fileToRead(bool)
{
    return make_shared<FileToRead>(d);
}

shared_ptr<FileLike> Pipe::fileToWrite(bool)
{
    return make_shared<FileToWrite>(d);
}

struct PosixPathPrivate
{
    explicit PosixPathPrivate(string pathIn)
        : path(move(pathIn))
    {
        while (!path.empty() && path.back() == '/') {
            path.pop_back();
        }
        if (path.empty()) {
            path = "/";
        }
        parts = utils::split(path, "/");
        for (string &part : parts) {
            if (utils::trimmed(part) == ".") {
                part = ".";
            } else if (utils::trimmed(part) == "..") {
                part = "..";
            }
        }
    }

    string path;
    vector<string> parts;
};

const char PosixPath::point = '.';
const char *PosixPath::pointpoint = "..";
const char PosixPath::seperator = '/';

PosixPath::PosixPath()
    : d(nullptr)
{
}

PosixPath::PosixPath(const string &path)
    : d(make_shared<PosixPathPrivate>(path))
{
}

PosixPath::PosixPath(const PosixPath &other) = default;

PosixPath::PosixPath(PosixPath &&other) noexcept
    : d(move(other.d))
{
}

PosixPath::~PosixPath() = default;

PosixPath &PosixPath::operator=(const PosixPath &other) = default;

PosixPath &PosixPath::operator=(PosixPath &&other) noexcept
{
    d = move(other.d);
    return *this;
}

bool PosixPath::operator==(const PosixPath &other) const
{
    if (!d && !other.d) {
        return true;
    }
    if (!d || !other.d) {
        return false;
    }
    return d->path == other.d->path;
}

PosixPath PosixPath::operator/(const string &sub) const
{
    if (isNull()) {
        return PosixPath();
    }
    if (!sub.empty() && utils::startsWith(sub, string(1, seperator))) {
        return PosixPath(sub);
    }
    string t = d->path;
    if (!t.empty() && t.back() != seperator) {
        t += seperator;
    }
    t += sub;
    return PosixPath(t);
}

PosixPath PosixPath::operator|(const string &sub) const
{
    return *this / sub;
}

bool PosixPath::isNull() const
{
    return !d || d->path.empty();
}

bool PosixPath::isFile() const
{
    return !isNull() && PathFs::isRegularFile(d->path);
}

bool PosixPath::isDir() const
{
    return !isNull() && PathFs::isDirectory(d->path);
}

bool PosixPath::isSymLink() const
{
    return !isNull() && PathFs::isSymlink(d->path);
}

bool PosixPath::isAbsolute() const
{
    return !isNull() && PathFs::isAbsolute(d->path);
}

bool PosixPath::isExecutable() const
{
#ifdef NG_OS_UNIX
    return !isNull() && ::access(d->path.c_str(), X_OK) == 0;
#else
    return false;
#endif
}

bool PosixPath::isReadable() const
{
#ifdef NG_OS_UNIX
    return !isNull() && ::access(d->path.c_str(), R_OK) == 0;
#else
    return !isNull() && isFile();
#endif
}

bool PosixPath::isRelative() const
{
    return !isNull() && !isAbsolute();
}

bool PosixPath::isRoot() const
{
    return !isNull() && d->path == "/";
}

bool PosixPath::isWritable() const
{
#ifdef NG_OS_UNIX
    return !isNull() && ::access(d->path.c_str(), W_OK) == 0;
#else
    return false;
#endif
}

bool PosixPath::exists() const
{
    return !isNull() && PathFs::exists(d->path);
}

int64_t PosixPath::size() const
{
    if (isNull() || !exists()) {
        return -1;
    }
    return PathFs::fileSize(d->path);
}

string PosixPath::path() const
{
    return d ? d->path : string();
}

string PosixPath::parentDir() const
{
    return parentPath().path();
}

PosixPath PosixPath::parentPath() const
{
    if (isNull()) {
        return PosixPath();
    }
    return PosixPath(PathFs::parentPath(d->path));
}

string PosixPath::name() const
{
    return isNull() ? string() : PathFs::filename(d->path);
}

string PosixPath::baseName() const
{
    return isNull() ? string() : PathFs::stem(d->path);
}

string PosixPath::suffix() const
{
    return isNull() ? string() : PathFs::extension(d->path);
}

string PosixPath::completeBaseName() const
{
    return baseName();
}

string PosixPath::completeSuffix() const
{
    return suffix();
}

string PosixPath::toAbsolute() const
{
    if (isNull()) {
        return string();
    }
    return PathFs::absolute(d->path);
}

string PosixPath::relativePath(const string &other) const
{
    return relativePath(PosixPath(other));
}

string PosixPath::relativePath(const PosixPath &other) const
{
    if (isNull() || other.isNull()) {
        return string();
    }
    return PathFs::relative(d->path, other.d->path);
}

bool PosixPath::isChildOf(const PosixPath &other) const
{
    if (isNull() || other.isNull()) {
        return false;
    }
    return PathFs::isChildOf(d->path, other.d->path);
}

bool PosixPath::hasChildOf(const PosixPath &other) const
{
    return other.isChildOf(*this);
}

int64_t PosixPath::createdMsecsSinceEpoch() const
{
    if (isNull() || !exists()) {
        return -1;
    }
    return PathFs::lastWriteTimeMsecs(d->path);
}

int64_t PosixPath::lastModifiedMsecsSinceEpoch() const
{
    return createdMsecsSinceEpoch();
}

int64_t PosixPath::lastReadMsecsSinceEpoch() const
{
    return createdMsecsSinceEpoch();
}

vector<string> PosixPath::listdir() const
{
    vector<string> result;
    if (isNull() || !isDir()) {
        return result;
    }
    return PathFs::listDirectory(d->path);
}

vector<PosixPath> PosixPath::children() const
{
    vector<PosixPath> result;
    for (const string &name : listdir()) {
        result.push_back(*this / name);
    }
    return result;
}

bool PosixPath::mkdir(bool createParents)
{
    if (isNull()) {
        return false;
    }
    if (createParents) {
        return PathFs::createDirectories(d->path);
    }
    return PathFs::createDirectory(d->path);
}

bool PosixPath::touch()
{
    if (isNull()) {
        return false;
    }
    ofstream f(d->path, ios::app);
    return f.good();
}

shared_ptr<FileLike> PosixPath::open(const string &mode) const
{
    if (isNull()) {
        return shared_ptr<FileLike>();
    }
    return FileLike::open(d->path, mode);
}

string PosixPath::readall(bool *ok) const
{
    auto f = open("r");
    if (!f) {
        if (ok) {
            *ok = false;
        }
        return string();
    }
    return f->readall(ok);
}

PosixPath PosixPath::cwd()
{
    const string p = PathFs::currentPath();
    return p.empty() ? PosixPath() : PosixPath(p);
}

pair<string, string> safeJoinPath(const string &parentDir, const string &subPath)
{
    if (subPath.empty()) {
        return make_pair(parentDir, string());
    }
    if (subPath.find("..") != string::npos) {
        return make_pair(string(), string());
    }
    PosixPath parent(parentDir);
    PosixPath child = parent / subPath;
    if (!child.path().compare(0, parent.path().size(), parent.path())) {
        return make_pair(child.path(), string());
    }
    return make_pair(string(), string());
}

}  // namespace qtng
