using namespace std;

#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/msgpack.h"
#include "qtng/utils/platform.h"
#include "qtng/utils/logging.h"

NG_LOGGER("qtng.msgpack");
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>


#undef CHECK_STREAM_PRECOND
#ifndef QT_NO_DEBUG
#define CHECK_STREAM_PRECOND(retVal)                  \
    NG_D(MsgPackStream);                               \
    if (!d->dev) {                                    \
        ngWarning() << "msgpack::Stream: No device";       \
        return retVal;                                \
    }                                                 \
    if (d->status != Ok) {                            \
        ngWarning() << "msgpack::Stream: Invalid status."; \
        return retVal;                                \
    }
#else
#define CHECK_STREAM_PRECOND(retVal) \
    NG_D(MsgPackStream);              \
    if (!d->dev) {                   \
        return retVal;               \
    }                                \
    if (d->status != Ok) {           \
        return retVal;               \
    }
#endif

namespace qtng {

namespace {

inline uint16_t _msgpack_load16(const void *p)
{
    return ngFromBigEndian<uint16_t>(p);
}

inline uint32_t _msgpack_load32(const void *p)
{
    return ngFromBigEndian<uint32_t>(p);
}

inline uint64_t _msgpack_load64(const void *p)
{
    return ngFromBigEndian<uint64_t>(p);
}

inline void _msgpack_store16(void *p, uint16_t v)
{
    ngToBigEndian(v, p);
}

inline void _msgpack_store32(void *p, uint32_t v)
{
    ngToBigEndian(v, p);
}

inline void _msgpack_store64(void *p, uint64_t v)
{
    ngToBigEndian(v, p);
}

}  // namespace

static inline utils::DateTime unpackDatetime(const string &bs)
{
    uint64_t seconds = 0;
    uint64_t nanoseconds = 0;
    if (bs.size() == 4) {
        seconds = ngFromBigEndian<uint32_t>(bs.data());
        nanoseconds = 0;
    } else if (bs.size() == 8) {
        uint64_t t = ngFromBigEndian<uint64_t>(bs.data());
        seconds = t & 0x00000003ffffffffULL;
        nanoseconds = t >> 34;
    } else if (bs.size() == 12) {
        nanoseconds = ngFromBigEndian<uint32_t>(bs.data());
        seconds = ngFromBigEndian<int64_t>(bs.data() + 4);
    } else {
        NG_UNREACHABLE();
    }
    return utils::DateTime::fromMSecsSinceEpoch(static_cast<int64_t>(seconds * 1000 + nanoseconds / 1000000));
}

MsgPackExtUserData::~MsgPackExtUserData() { }

class MsgPackStreamPrivate
{
public:
    MsgPackStreamPrivate();
    MsgPackStreamPrivate(FileLike *d);
    MsgPackStreamPrivate(string *a, bool writeMode);
    MsgPackStreamPrivate(const string &a);
    ~MsgPackStreamPrivate();
public:
    bool readBytes(char *data, int64_t len);
    inline bool readBytes(uint8_t *data, int len);
    bool readArrayHeader(uint32_t &len);
    bool readMapHeader(uint32_t &len);
    bool readExtHeader(uint32_t &len, uint8_t &msgpackType);
    bool writeBytes(const char *data, int64_t len);
    inline bool writeBytes(const uint8_t *data, int len);
    bool writeArrayHeader(uint32_t len);
    bool writeMapHeader(uint32_t len);
    bool writeExtHeader(uint32_t len, uint8_t msgpackType);
    bool unpack_longlong(int64_t &i64);
    bool unpack_ulonglong(uint64_t &u64);
    bool unpackString(string &s);
public:
    map<intptr_t, MsgPackExtUserData *> userData;
    FileLike *dev;
    MsgPackStream::Status status;
    uint32_t limit;
    uint32_t pos;
    int version;
    bool owndev;
    bool flushWrites;
};

MsgPackStreamPrivate::MsgPackStreamPrivate()
    : dev(new BytesIO())
    , status(MsgPackStream::Ok)
    , limit(numeric_limits<uint32_t>::max())
    , pos(0)
    , version(0)
    , owndev(true)
    , flushWrites(false)
{
}

MsgPackStreamPrivate::MsgPackStreamPrivate(FileLike *d)
    : dev(d)
    , status(MsgPackStream::Ok)
    , limit(numeric_limits<uint32_t>::max())
    , pos(0)
    , version(0)
    , owndev(false)
    , flushWrites(false)
{
}

MsgPackStreamPrivate::MsgPackStreamPrivate(string *a, bool writeMode)
    : status(MsgPackStream::Ok)
    , limit(numeric_limits<uint32_t>::max())
    , pos(0)
    , version(0)
    , owndev(true)
    , flushWrites(false)
{
    dev = new BytesIO(a);
    if (!writeMode) {
        limit = static_cast<uint32_t>(a->size());
    }
}

MsgPackStreamPrivate::MsgPackStreamPrivate(const string &a)
    : status(MsgPackStream::Ok)
    , limit(a.size())
    , pos(0)
    , version(0)
    , owndev(true)
    , flushWrites(false)
{
    dev = new BytesIO(a);
}

MsgPackStreamPrivate::~MsgPackStreamPrivate()
{
    if (owndev) {
        delete dev;
    }
    for (map<intptr_t, MsgPackExtUserData *>::const_iterator itor = userData.begin(); itor != userData.end();
         ++itor) {
        delete itor->second;
    }
    userData.clear();
}

bool MsgPackStreamPrivate::readBytes(char *data, int64_t len)
{
    if (status != MsgPackStream::Ok) {
        return false;
    }
    if (!dev) {
        status = MsgPackStream::ReadPastEnd;
        return false;
    }
    if (pos + len > limit) {
        status = MsgPackStream::ReadPastEnd;
        return false;
    }
    int64_t total = 0;
    bool metZero = false;
    while (total < len) {
        int64_t bs = dev->read(data, (len - total));
        if (bs < 0) {
            status = MsgPackStream::ReadPastEnd;
            return false;
        } else if (bs == 0) {
            // that is not error if we meet bs == 0 at first time.
            if (!metZero) {
                metZero = true;
            } else {
                status = MsgPackStream::ReadPastEnd;
                return false;
            }
        } else {
            assert(bs > 0);
            data += bs;
            total += bs;
        }
    }
    pos += len;
    return true;
}

bool MsgPackStreamPrivate::readBytes(uint8_t *data, int len)
{
    return readBytes(static_cast<char *>(static_cast<void *>(data)), len);
}

bool MsgPackStreamPrivate::readArrayHeader(uint32_t &len)
{
    uint8_t p[5];
    if (!readBytes(p, 1)) {
        return false;
    }
    if (p[0] >= FirstByte::FIXARRAY && p[0] <= (FirstByte::FIXARRAY + 0xf)) {
        len = p[0] & 0xf;
    } else if (p[0] == FirstByte::ARRAY16) {
        readBytes((char *) p + 1, 2);
        len = _msgpack_load16(p + 1);
    } else if (p[0] == FirstByte::ARRAY32) {
        readBytes((char *) p + 1, 4);
        len = _msgpack_load32(p + 1);
    } else {
        status = MsgPackStream::ReadCorruptData;
        return false;
    }
    return true;
}

bool MsgPackStreamPrivate::readMapHeader(uint32_t &len)
{
    uint8_t p[5];
    if (!readBytes(p, 1)) {
        return false;
    }
    if (p[0] >= FirstByte::FIXMAP && p[0] <= (FirstByte::FIXMAP + 0xf)) {
        len = p[0] & 0xf;
    } else if (p[0] == FirstByte::MAP16) {
        readBytes((char *) p + 1, 2);
        len = _msgpack_load16(p + 1);
    } else if (p[0] == FirstByte::MAP32) {
        readBytes((char *) p + 1, 4);
        len = _msgpack_load32(p + 1);
    } else {
        status = MsgPackStream::ReadCorruptData;
        return false;
    }
    return true;
}

bool MsgPackStreamPrivate::readExtHeader(uint32_t &len, uint8_t &msgpackType)
{
    if (!dev || status != MsgPackStream::Ok) {
        return false;
    }
    uint8_t p[6];
    if (!readBytes(p, 1)) {
        return false;
    }
    if (FirstByte::FIXEXT1 <= p[0] && p[0] <= FirstByte::FIXEX16) {
        len = 1;
        len <<= p[0] - FirstByte::FIXEXT1;
    } else if (p[0] == FirstByte::EXT8) {
        if (!readBytes(p + 1, 1)) {
            return false;
        }
        len = p[1];
    } else if (p[0] == FirstByte::EXT16) {
        if (!readBytes(p + 1, 2)) {
            return false;
        }
        len = _msgpack_load16(p + 1);
    } else if (p[0] == FirstByte::EXT32) {
        if (!readBytes(p + 1, 4)) {
            return false;
        }
        len = _msgpack_load32(p + 1);
    } else {
        status = MsgPackStream::ReadCorruptData;
        return false;
    }
    if (len > limit) {
        ngDebug() << "read length is too large.";
        status = MsgPackStream::ReadCorruptData;
        return false;
    }
    if (!readBytes(p + 5, 1)) {
        return false;
    }
    msgpackType = p[5];
    return true;
}

bool MsgPackStreamPrivate::unpack_longlong(int64_t &i64)
{
    uint8_t p[9];
    if (!readBytes(p, 1)) {
        return false;
    }

    if (p[0] <= FirstByte::POSITIVE_FIXINT) {  // positive fixint 0x00 - 0x7f
        i64 = p[0];
    } else if (p[0] >= FirstByte::NEGATIVE_FIXINT) {  // negative fixint 0xe0 - 0xff
        i64 = static_cast<int8_t>(p[0]);
    } else if (p[0] == FirstByte::UINT8) {
        if (!readBytes(p + 1, 1)) {
            return false;
        }
        i64 = p[1];
    } else if (p[0] == FirstByte::INT8) {
        if (!readBytes(p + 1, 1)) {
            return false;
        }
        i64 = static_cast<int8_t>(p[1]);
    } else if (p[0] == FirstByte::UINT16) {
        if (!readBytes(p + 1, 2)) {
            return false;
        }
        i64 = _msgpack_load16(p + 1);
    } else if (p[0] == FirstByte::INT16) {
        if (!readBytes(p + 1, 2)) {
            return false;
        }
        i64 = static_cast<int16_t>(_msgpack_load16(p + 1));
    } else if (p[0] == FirstByte::UINT32) {
        if (!readBytes(p + 1, 4)) {
            return false;
        }
        i64 = static_cast<int64_t>(_msgpack_load32(p + 1));
    } else if (p[0] == FirstByte::INT32) {
        if (!readBytes(p + 1, 4)) {
            return false;
        }
        i64 = static_cast<int32_t>(_msgpack_load32(p + 1));
    } else if (p[0] == FirstByte::UINT64) {
        if (!readBytes(p + 1, 8)) {
            return false;
        }
        uint64_t u64;
        u64 = _msgpack_load64(p + 1);
        if (u64 > static_cast<uint64_t>(numeric_limits<int64_t>::max())) {
            status = MsgPackStream::ReadCorruptData;
            return false;
        }
        i64 = static_cast<int64_t>(u64);
    } else if (p[0] == FirstByte::INT64) {
        if (!readBytes(p + 1, 8)) {
            return false;
        }
        i64 = static_cast<int64_t>(_msgpack_load64(p + 1));
    } else {
        status = MsgPackStream::ReadCorruptData;
        return false;
    }
    return true;
}

bool MsgPackStreamPrivate::unpack_ulonglong(uint64_t &u64)
{
    uint8_t p[9];
    if (!readBytes(p, 1)) {
        return false;
    }

    if (p[0] <= FirstByte::POSITIVE_FIXINT) {  // positive fixint 0x00 - 0x7f
        u64 = p[0];
    } else if (p[0] >= FirstByte::NEGATIVE_FIXINT) {  // negative fixint 0xe0 - 0xff
        status = MsgPackStream::ReadCorruptData;
        return false;
    } else if (p[0] == FirstByte::UINT8) {
        if (!readBytes(p + 1, 1)) {
            return false;
        }
        u64 = p[1];
    } else if (p[0] == FirstByte::INT8) {
        if (!readBytes(p + 1, 1)) {
            return false;
        }
        int8_t i8 = static_cast<int8_t>(p[1]);
        if (i8 < 0) {
            status = MsgPackStream::ReadCorruptData;
            return false;
        }
        u64 = static_cast<uint64_t>(i8);
    } else if (p[0] == FirstByte::UINT16) {
        if (!readBytes(p + 1, 2)) {
            return false;
        }
        u64 = _msgpack_load16(p + 1);
    } else if (p[0] == FirstByte::INT16) {
        if (!readBytes(p + 1, 2)) {
            return false;
        }
        int16_t i16 = static_cast<int16_t>(_msgpack_load16(p + 1));
        if (i16 < 0) {
            status = MsgPackStream::ReadCorruptData;
            return false;
        }
        u64 = static_cast<uint64_t>(i16);
    } else if (p[0] == FirstByte::UINT32) {
        if (!readBytes(p + 1, 4)) {
            return false;
        }
        u64 = _msgpack_load32(p + 1);
    } else if (p[0] == FirstByte::INT32) {
        if (!readBytes(p + 1, 4)) {
            return false;
        }
        int32_t i32 = static_cast<int32_t>(_msgpack_load32(p + 1));
        if (i32 < 0) {
            status = MsgPackStream::ReadCorruptData;
            return false;
        }
        u64 = static_cast<uint64_t>(i32);
    } else if (p[0] == FirstByte::UINT64) {
        if (!readBytes(p + 1, 8)) {
            return false;
        }
        u64 = _msgpack_load64(p + 1);
    } else if (p[0] == FirstByte::INT64) {
        if (!readBytes(p + 1, 8)) {
            return false;
        }
        u64 = _msgpack_load64(p + 1);
        int64_t i64 = static_cast<int64_t>(u64);
        if (i64 < 0) {
            status = MsgPackStream::ReadCorruptData;
            return false;
        }
    } else {
        status = MsgPackStream::ReadCorruptData;
        return false;
    }
    return true;
}

bool MsgPackStreamPrivate::unpackString(string &s)
{
    uint8_t p[5];
    if (!readBytes(p, 1)) {
        return false;
    }

    uint32_t len = 0;
    if (p[0] >= FirstByte::FIXSTR && p[0] <= (FirstByte::FIXSTR + 0x1f)) {  // fixstr
        len = p[0] - FirstByte::FIXSTR;
    } else if (p[0] == FirstByte::STR8) {
        if (!readBytes(p + 1, 1)) {
            return false;
        }
        len = p[1];
    } else if (p[0] == FirstByte::STR16) {
        if (!readBytes(p + 1, 2)) {
            return false;
        }
        len = _msgpack_load16(p + 1);
    } else if (p[0] == FirstByte::STR32) {
        if (!readBytes(p + 1, 4)) {
            return false;
        }
        len = _msgpack_load32(p + 1);
        if (static_cast<int>(len) < 0) {
            status = MsgPackStream::ReadCorruptData;
            return false;
        }
    } else {
        status = MsgPackStream::ReadCorruptData;
        return false;
    }
    if (len > limit) {
        ngDebug() << "read string length is too large.";
        status = MsgPackStream::ReadCorruptData;
        return false;
    }
    string buf;
    if (len > 0) {
        buf.resize(static_cast<int>(len));
        if (!readBytes(&buf[0], len)) {
            return false;
        }
    }
    s = buf;
    return true;
}

bool MsgPackStreamPrivate::writeBytes(const char *data, int64_t len)
{
    if (status != MsgPackStream::Ok) {
        return false;
    }
    if (!dev) {
        status = MsgPackStream::WriteFailed;
        return false;
    }
    int64_t total = 0;
    while (total < len) {
        int64_t bs = dev->write(data, len - total);
        if (bs < 0) {
            status = MsgPackStream::WriteFailed;
            return false;
        }
        /* Apparently on Windows, the buffer size for named pipes is 0, and
         * any data that is written before the remote end reads it is
         * dropped (!!) without error (see https://bugreports.qt.io/browse/QTBUG-18385).
         * We must be very sure that the data has been written before we try
         * another write. This degrades performance in other cases, so callers
         * must enable this behavior explicitly.
         */
        if (flushWrites) {
            // flushWrites ignored without QIODevice waitForBytesWritten
        }

        /* Increment the write pointer and the total byte count. */
        data += bs;
        total += bs;
    }
    return true;
}

bool MsgPackStreamPrivate::writeBytes(const uint8_t *data, int len)
{
    return writeBytes(static_cast<const char *>(static_cast<const void *>(data)), len);
}

bool MsgPackStreamPrivate::writeArrayHeader(uint32_t len)
{
    uint8_t p[5];
    if (len <= 15) {
        p[0] = FirstByte::FIXARRAY | len;
        writeBytes(p, 1);
    } else if (len <= numeric_limits<uint16_t>::max()) {
        p[0] = FirstByte::ARRAY16;
        _msgpack_store16(p + 1, static_cast<uint16_t>(len));
        writeBytes(p, 3);
    } else {
        p[0] = FirstByte::ARRAY32;
        _msgpack_store32(p + 1, len);
        writeBytes(p, 5);
    }
    return status == MsgPackStream::Ok;
}

bool MsgPackStreamPrivate::writeMapHeader(uint32_t len)
{
    uint8_t p[5];
    if (len <= 15) {
        p[0] = FirstByte::FIXMAP | len;
        writeBytes(p, 1);
    } else if (len <= numeric_limits<uint16_t>::max()) {
        p[0] = FirstByte::MAP16;
        _msgpack_store16(p + 1, static_cast<uint16_t>(len));
        writeBytes(p, 3);
    } else {
        p[0] = FirstByte::MAP32;
        _msgpack_store32(p + 1, len);
        writeBytes(p, 5);
    }
    return status == MsgPackStream::Ok;
}

bool MsgPackStreamPrivate::writeExtHeader(uint32_t len, uint8_t msgpackType)
{
    if (status != MsgPackStream::Ok) {
        return false;
    }
    if (!dev) {
        status = MsgPackStream::WriteFailed;
        return false;
    }
    uint8_t p[6];

    uint8_t sz = 2;
    if (len == 1) {
        p[0] = FirstByte::FIXEXT1;
        p[1] = msgpackType;
    } else if (len == 2) {
        p[0] = FirstByte::FIXEXT2;
        p[1] = msgpackType;
    } else if (len == 4) {
        p[0] = FirstByte::FIXEXT4;
        p[1] = msgpackType;
    } else if (len == 8) {
        p[0] = FirstByte::FIXEXT8;
        p[1] = msgpackType;
    } else if (len == 16) {
        p[0] = FirstByte::FIXEX16;
        p[1] = msgpackType;
    } else if (len <= numeric_limits<uint8_t>::max()) {
        p[0] = FirstByte::EXT8;
        p[1] = static_cast<uint8_t>(len);
        p[2] = msgpackType;
        sz = 3;
    } else if (len <= numeric_limits<uint16_t>::max()) {
        p[0] = FirstByte::EXT16;
        _msgpack_store16(p + 1, static_cast<uint16_t>(len));
        p[3] = msgpackType;
        sz = 4;
    } else {
        p[0] = FirstByte::EXT32;
        _msgpack_store32(p + 1, len);
        p[5] = msgpackType;
        sz = 6;
    }
    if (!writeBytes(p, sz)) {
        return false;
    }
    return true;
}

MsgPackStream::MsgPackStream()
    : d_ptr(new MsgPackStreamPrivate())
{
}

MsgPackStream::MsgPackStream(FileLike *d)
    : d_ptr(new MsgPackStreamPrivate(d))
{
}

MsgPackStream::MsgPackStream(string *a, bool writeMode)
    : d_ptr(new MsgPackStreamPrivate(a, writeMode))
{
}

MsgPackStream::MsgPackStream(const string &a)
    : d_ptr(new MsgPackStreamPrivate(a))
{
}

MsgPackStream::~MsgPackStream()
{
    delete d_ptr;
}

void MsgPackStream::setDevice(FileLike *dev)
{
    NG_D(MsgPackStream);
    if (d->owndev) {
        delete d->dev;
    }
    d->dev = dev;
    d->owndev = false;
}

FileLike *MsgPackStream::device() const
{
    NG_D(const MsgPackStream);
    return d->dev;
}

string MsgPackStream::data() const
{
    NG_D(const MsgPackStream);
    BytesIO *buf = dynamic_cast<BytesIO *>(d->dev);
    if (buf) {
        return buf->data();
    } else {
        return string();
    }
}

bool MsgPackStream::atEnd() const
{
    NG_D(const MsgPackStream);
    return d->dev ? (d->pos >= d->limit) : true;
}

MsgPackStream::Status MsgPackStream::status() const
{
    NG_D(const MsgPackStream);
    return d->status;
}

void MsgPackStream::resetStatus()
{
    NG_D(MsgPackStream);
    d->status = Ok;
}

void MsgPackStream::setStatus(Status status)
{
    NG_D(MsgPackStream);
    d->status = status;
}

void MsgPackStream::setFlushWrites(bool flush)
{
    NG_D(MsgPackStream);
    d->flushWrites = flush;
}

bool MsgPackStream::willFlushWrites()
{
    NG_D(const MsgPackStream);
    return d->flushWrites;
}

void MsgPackStream::setLengthLimit(uint32_t limit)
{
    NG_D(MsgPackStream);
    d->limit = limit;
}

uint32_t MsgPackStream::lengthLimit() const
{
    NG_D(const MsgPackStream);
    return d->limit;
}

void MsgPackStream::setVersion(int version)
{
    NG_D(MsgPackStream);
    d->version = version;
}

int MsgPackStream::version() const
{
    NG_D(const MsgPackStream);
    return d->version;
}

void MsgPackStream::setUserData(intptr_t key, MsgPackExtUserData *userData)
{
    NG_D(MsgPackStream);
    d->userData[key] = userData;
}

MsgPackExtUserData *MsgPackStream::getUserData(intptr_t key) const
{
    NG_D(const MsgPackStream);
    return d->userData.at(key);
}

MsgPackStream &MsgPackStream::operator>>(bool &b)
{
    CHECK_STREAM_PRECOND(*this)
    uint8_t p[1];
    if (!d->readBytes(p, 1)) {
        d->status = ReadPastEnd;
        b = false;
    } else {
        if (p[0] == FirstByte::MTRUE) {
            b = true;
        } else if (p[0] == FirstByte::MFALSE) {
            b = false;
        } else {
            d->status = ReadCorruptData;
        }
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(uint8_t &u8)
{
    CHECK_STREAM_PRECOND(*this);
    uint64_t u64;
    if (!d->unpack_ulonglong(u64)) {
        return *this;
    }
    if (u64 <= numeric_limits<uint8_t>::max()) {
        u8 = static_cast<uint8_t>(u64);
    } else {
        d->status = ReadCorruptData;
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(uint16_t &u16)
{
    CHECK_STREAM_PRECOND(*this);
    uint64_t u64;
    if (!d->unpack_ulonglong(u64)) {
        return *this;
    }
    if (u64 <= numeric_limits<uint16_t>::max()) {
        u16 = static_cast<uint16_t>(u64);
    } else {
        d->status = ReadCorruptData;
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(uint32_t &u32)
{
    CHECK_STREAM_PRECOND(*this);
    uint64_t u64;
    if (!d->unpack_ulonglong(u64)) {
        return *this;
    }
    if (u64 <= numeric_limits<uint32_t>::max()) {
        u32 = static_cast<uint32_t>(u64);
    } else {
        d->status = ReadCorruptData;
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(uint64_t &u64)
{
    CHECK_STREAM_PRECOND(*this);
    d->unpack_ulonglong(u64);
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(int8_t &i8)
{
    CHECK_STREAM_PRECOND(*this);
    int64_t i64;
    if (!d->unpack_longlong(i64))
        return *this;
    if (numeric_limits<int8_t>::min() <= i64 && i64 <= numeric_limits<int8_t>::max()) {
        i8 = static_cast<int8_t>(i64);
    } else {
        d->status = ReadCorruptData;
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(int16_t &i16)
{
    CHECK_STREAM_PRECOND(*this);
    int64_t i64;
    if (!d->unpack_longlong(i64))
        return *this;
    if (numeric_limits<int16_t>::min() <= i64 && i64 <= numeric_limits<int16_t>::max()) {
        i16 = static_cast<int16_t>(i64);
    } else {
        d->status = ReadCorruptData;
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(int32_t &i32)
{
    CHECK_STREAM_PRECOND(*this);
    int64_t i64;
    if (!d->unpack_longlong(i64))
        return *this;
    if (i64 >= numeric_limits<int32_t>::min() && i64 <= numeric_limits<int32_t>::max()) {
        i32 = static_cast<int32_t>(i64);
    } else {
        d->status = ReadCorruptData;
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(int64_t &i64)
{
    CHECK_STREAM_PRECOND(*this);
    d->unpack_longlong(i64);
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(float &f)
{
    CHECK_STREAM_PRECOND(*this);
    uint8_t p[5];
    if (!d->readBytes(p, 1)) {
        return *this;
    }
    if (p[0] != FirstByte::FLOAT32) {
        d->status = ReadCorruptData;
        return *this;
    }
    if (!d->readBytes(p + 1, 4)) {
        return *this;
    }
    uint32_t i32 = _msgpack_load32(p + 1);
    f = *((float *) &i32);
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(double &f)
{
    CHECK_STREAM_PRECOND(*this);
    uint8_t p[9];
    if (!d->readBytes(p, 1)) {
        return *this;
    }
    if (p[0] != FirstByte::FLOAT64) {
        d->status = ReadCorruptData;
        return *this;
    }
    if (!d->readBytes(p + 1, 8)) {
        return *this;
    }
    uint64_t i64 = _msgpack_load64(p + 1);
    f = *((double *) &i64);
    //    strncpy(static_cast<char*>(static_cast<void*>(&f)), static_cast<char*>(static_cast<void*>(&i64)), 8);
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(string &str)
{
    NG_D(MsgPackStream);
    d->unpackString(str);
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(utils::DateTime &dt)
{
    CHECK_STREAM_PRECOND(*this);
    uint32_t len;
    uint8_t msgpackType;
    if (!d->readExtHeader(len, msgpackType) || msgpackType != 0xff) {
        d->status = ReadCorruptData;
        dt = utils::DateTime();
        return *this;
    }
    if (len != 4 && len != 8 && len != 12) {
        ngDebug() << "the datetime require 4/8/12 bytes.";
        d->status = MsgPackStream::ReadCorruptData;
        dt = utils::DateTime();
    }
    uint8_t p[12];
    if (!d->readBytes(p, len)) {
        dt = utils::DateTime();
        return *this;
    }
    dt = unpackDatetime(string(static_cast<char *>(static_cast<void *>(p)), len));
    return *this;
}

MsgPackStream &MsgPackStream::operator>>(MsgPackExtData &ext)
{
    NG_D(MsgPackStream);
    uint32_t len;
    bool success = d->readExtHeader(len, ext.type);
    if (!success) {
        return *this;
    }
    if (static_cast<int>(len) < 0) {
        d->status = ReadCorruptData;
        return *this;
    }
    ext.payload.resize(static_cast<int>(len));
    d->readBytes(&ext.payload[0], len);
    return *this;
}

bool MsgPackStream::readBytes(char *data, int64_t len)
{
    NG_D(MsgPackStream);
    return d->readBytes(data, len);
}

bool MsgPackStream::readArrayHeader(uint32_t &len)
{
    NG_D(MsgPackStream);
    return d->readArrayHeader(len);
}

bool MsgPackStream::readMapHeader(uint32_t &len)
{
    NG_D(MsgPackStream);
    return d->readMapHeader(len);
}

bool MsgPackStream::readExtHeader(uint32_t &len, uint8_t msgpackType)
{
    NG_D(MsgPackStream);
    return d->readExtHeader(len, msgpackType);
}

MsgPackStream &MsgPackStream::operator<<(bool b)
{
    CHECK_STREAM_PRECOND(*this);
    uint8_t p[1];
    p[0] = b ? FirstByte::MTRUE : FirstByte::MFALSE;
    d->writeBytes(p, 1);
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(uint8_t u8)
{
    CHECK_STREAM_PRECOND(*this);
    uint8_t p[2];
    if (u8 <= FirstByte::POSITIVE_FIXINT) {
        _msgpack_store8(p, u8);
        d->writeBytes(p, 1);
    } else {
        p[0] = FirstByte::UINT8;
        _msgpack_store8(p + 1, u8);
        d->writeBytes(p, 2);
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(uint16_t u16)
{
    CHECK_STREAM_PRECOND(*this);
    if (u16 <= numeric_limits<uint8_t>::max()) {
        *this << static_cast<uint8_t>(u16);
    } else {
        uint8_t p[3];
        p[0] = FirstByte::UINT16;
        _msgpack_store16(p + 1, u16);
        d->writeBytes(p, 3);
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(uint32_t u32)
{
    CHECK_STREAM_PRECOND(*this);
    if (u32 <= numeric_limits<uint16_t>::max()) {
        *this << static_cast<uint16_t>(u32);
    } else {
        uint8_t p[5];
        p[0] = FirstByte::UINT32;
        _msgpack_store32(p + 1, u32);
        d->writeBytes(p, 5);
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(uint64_t u64)
{
    CHECK_STREAM_PRECOND(*this);
    if (u64 <= numeric_limits<uint32_t>::max()) {
        *this << static_cast<uint32_t>(u64);
    } else {
        uint8_t p[9];
        p[0] = FirstByte::UINT64;
        _msgpack_store64(p + 1, u64);
        d->writeBytes(p, 9);
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(int8_t i8)
{
    CHECK_STREAM_PRECOND(*this);
    uint8_t p[2];
    if (-32 <= i8) {  //  && i8 <= 127 is always true
        _msgpack_store8(p, i8);
        d->writeBytes(p, 1);
    } else {
        p[0] = i8 > 0 ? FirstByte::UINT8 : FirstByte::INT8;
        _msgpack_store8(p + 1, i8);
        d->writeBytes(p, 2);
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(int16_t i16)
{
    CHECK_STREAM_PRECOND(*this);

    if (numeric_limits<int8_t>::min() <= i16 && i16 <= numeric_limits<int8_t>::max()) {
        *this << static_cast<int8_t>(i16);
    } else if (numeric_limits<int8_t>::max() <= i16 && i16 <= numeric_limits<uint8_t>::max()) {
        uint8_t p[2];
        p[0] = FirstByte::UINT8;
        _msgpack_store8(p + 1, static_cast<uint8_t>(i16));
        d->writeBytes(p, 2);
    } else {
        uint8_t p[3];
        p[0] = i16 > 0 ? FirstByte::UINT16 : FirstByte::INT16;
        _msgpack_store16(p + 1, i16);
        d->writeBytes(p, 3);
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(int32_t i32)
{
    CHECK_STREAM_PRECOND(*this);
    if (numeric_limits<int16_t>::min() <= i32 && i32 <= numeric_limits<int16_t>::max()) {
        *this << static_cast<int16_t>(i32);
    } else if (numeric_limits<int16_t>::max() <= i32 && i32 <= numeric_limits<uint16_t>::max()) {
        uint8_t p[3];
        p[0] = FirstByte::UINT16;
        _msgpack_store16(p + 1, static_cast<uint16_t>(i32));
        d->writeBytes(p, 3);
    } else {
        uint8_t p[5];
        p[0] = i32 > 0 ? FirstByte::UINT32 : FirstByte::INT32;
        _msgpack_store32(p + 1, i32);
        d->writeBytes(p, 5);
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(int64_t i64)
{
    CHECK_STREAM_PRECOND(*this);
    if (numeric_limits<int32_t>::min() <= i64 && i64 <= numeric_limits<int32_t>::max()) {
        *this << static_cast<int32_t>(i64);
    } else if (numeric_limits<int32_t>::max() <= i64 && i64 <= numeric_limits<uint32_t>::max()) {
        uint8_t p[5];
        p[0] = FirstByte::UINT32;
        _msgpack_store32(p + 1, static_cast<uint32_t>(i64));
        d->writeBytes(p, 5);
    } else {
        uint8_t p[9];
        p[0] = i64 > 0 ? FirstByte::UINT64 : FirstByte::INT64;
        _msgpack_store64(p + 1, i64);
        d->writeBytes(p, 9);
    }
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(float f)
{
    CHECK_STREAM_PRECOND(*this);
    uint8_t p[5];
    p[0] = FirstByte::FLOAT32;
    uint32_t u32;
    u32 = *((uint32_t *) &f);
    //    strncpy(static_cast<char*>(static_cast<void*>(&u32)), static_cast<char*>(static_cast<void*>(&f)), 4);
    _msgpack_store32(p + 1, u32);
    d->writeBytes(p, 5);
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(double f)
{
    CHECK_STREAM_PRECOND(*this);
    uint8_t p[9];
    p[0] = FirstByte::FLOAT64;
    uint64_t u64;
    u64 = *((uint64_t *) &f);
    _msgpack_store64(p + 1, u64);
    d->writeBytes(p, 9);
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const string &str)
{
    const string &bytes = str;
    writeString(bytes.data(), bytes.size());
    return *this;
}

static string packDatetime(const utils::DateTime &dt)
{
    if (!dt.isValid()) {
        return string();
    }
    uint64_t msecs = static_cast<uint64_t>(dt.toMSecsSinceEpoch());
    uint64_t t = ((msecs % 1000) * 1000000) << 34 | (msecs / 1000);
    string bs(8, '\0');
    ngToBigEndian(t, &bs[0]);
    return bs;
}

MsgPackStream &MsgPackStream::operator<<(const utils::DateTime &dt)
{
    CHECK_STREAM_PRECOND(*this);
    const string &bs = packDatetime(dt);
    if (bs.empty()) {
        d->status = WriteFailed;
        return *this;
    }
    if (!d->writeExtHeader(static_cast<uint32_t>(bs.size()), 0xff)) {
        return *this;
    }
    d->writeBytes(bs.data(), bs.size());
    return *this;
}

MsgPackStream &MsgPackStream::operator<<(const MsgPackExtData &ext)
{
    CHECK_STREAM_PRECOND(*this);
    bool success = d->writeExtHeader(static_cast<uint32_t>(ext.payload.size()), ext.type);
    if (!success) {
        return *this;
    }
    d->writeBytes(ext.payload.data(), ext.payload.size());
    return *this;
}

bool MsgPackStream::writeBytes(const char *data, int64_t len)
{
    NG_D(MsgPackStream);
    return d->writeBytes(data, len);
}

bool MsgPackStream::writeString(const char *data, uint32_t len)
{
    CHECK_STREAM_PRECOND(false);
    uint8_t p[5];
    int sz;
    if (len <= 31) {
        p[0] = FirstByte::FIXSTR | len;
        sz = 1;
    } else if (len <= numeric_limits<uint8_t>::max()) {
        p[0] = FirstByte::STR8;
        _msgpack_store8(p + 1, static_cast<uint8_t>(len));
        sz = 2;
    } else if (len <= numeric_limits<uint16_t>::max()) {
        p[0] = FirstByte::STR16;
        _msgpack_store16(p + 1, static_cast<uint16_t>(len));
        sz = 3;
    } else {
        p[0] = FirstByte::STR32;
        _msgpack_store32(p + 1, len);
        sz = 5;
    }
    if (!d->writeBytes(p, sz)) {
        return false;
    }
    return d->writeBytes(data, len);
}

bool MsgPackStream::writeArrayHeader(uint32_t len)
{
    NG_D(MsgPackStream);
    return d->writeArrayHeader(len);
}

bool MsgPackStream::writeMapHeader(uint32_t len)
{
    NG_D(MsgPackStream);
    return d->writeMapHeader(len);
}

bool MsgPackStream::writeExtHeader(uint32_t len, uint8_t msgpackType)
{
    NG_D(MsgPackStream);
    return d->writeExtHeader(len, msgpackType);
}

}  // namespace qtng
