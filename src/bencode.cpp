#include "qtng/bencode.h"

#include <cctype>
#include <limits>
#include <utility>

using namespace std;

#undef CHECK_STREAM_PRECOND
#ifndef QT_NO_DEBUG
#define CHECK_STREAM_PRECOND(retVal) \
    NG_D(BencodeStream);             \
    if (!d->dev) {                   \
        return retVal;               \
    }                                \
    if (d->status != BencodeStream::Ok) { \
        return retVal;               \
    }
#else
#define CHECK_STREAM_PRECOND(retVal) \
    NG_D(BencodeStream);             \
    if (!d->dev) {                   \
        return retVal;               \
    }                                \
    if (d->status != BencodeStream::Ok) { \
        return retVal;               \
    }
#endif

namespace qtng {

class BencodeStreamPrivate
{
public:
    BencodeStreamPrivate();
    BencodeStreamPrivate(FileLike *d);
    BencodeStreamPrivate(string *a, bool writeMode);
    BencodeStreamPrivate(const string &a);
    ~BencodeStreamPrivate();

    bool readBytes(char *data, int64_t len);
    bool writeBytes(const char *data, int64_t len);
    bool peekByte(char *c) const;
    bool ensurePeekCached();
    bool consumeByte(char expected);
    bool account(size_t n);

    bool readInteger(int64_t &i);
    bool writeInteger(int64_t i);
    bool readString(string &s);
    bool writeString(const string &s);
    bool readValue(Bencode &v);
    bool writeValue(const Bencode &v);

    FileLike *dev;
    BencodeStream::Status status;
    uint32_t limit;
    uint32_t allocLimit;
    uint32_t pos;
    bool owndev;
    size_t bytesAllocated;
    mutable bool hasPeekByte;
    mutable char peekByteCache;
};

BencodeStreamPrivate::BencodeStreamPrivate()
    : dev(new BytesIO())
    , status(BencodeStream::Ok)
    , limit(numeric_limits<uint32_t>::max())
    , allocLimit(16 * 1024 * 1024)
    , pos(0)
    , owndev(true)
    , bytesAllocated(0)
    , hasPeekByte(false)
    , peekByteCache(0)
{
}

BencodeStreamPrivate::BencodeStreamPrivate(FileLike *d)
    : dev(d)
    , status(BencodeStream::Ok)
    , limit(numeric_limits<uint32_t>::max())
    , allocLimit(16 * 1024 * 1024)
    , pos(0)
    , owndev(false)
    , bytesAllocated(0)
    , hasPeekByte(false)
    , peekByteCache(0)
{
}

BencodeStreamPrivate::BencodeStreamPrivate(string *a, bool writeMode)
    : status(BencodeStream::Ok)
    , limit(numeric_limits<uint32_t>::max())
    , allocLimit(16 * 1024 * 1024)
    , pos(0)
    , owndev(true)
    , bytesAllocated(0)
    , hasPeekByte(false)
    , peekByteCache(0)
{
    dev = new BytesIO(a);
    if (!writeMode) {
        limit = static_cast<uint32_t>(a->size());
    }
}

BencodeStreamPrivate::BencodeStreamPrivate(const string &a)
    : status(BencodeStream::Ok)
    , limit(static_cast<uint32_t>(a.size()))
    , allocLimit(16 * 1024 * 1024)
    , pos(0)
    , owndev(true)
    , bytesAllocated(0)
    , hasPeekByte(false)
    , peekByteCache(0)
{
    dev = new BytesIO(a);
}

BencodeStreamPrivate::~BencodeStreamPrivate()
{
    if (owndev) {
        delete dev;
    }
}

bool BencodeStreamPrivate::readBytes(char *data, int64_t len)
{
    if (status != BencodeStream::Ok) {
        return false;
    }
    if (!dev) {
        status = BencodeStream::ReadPastEnd;
        return false;
    }
    if (pos + len > limit) {
        status = BencodeStream::ReadPastEnd;
        return false;
    }
    int64_t total = 0;
    if (hasPeekByte && len > 0) {
        data[0] = peekByteCache;
        hasPeekByte = false;
        ++total;
    }
    bool metZero = false;
    while (total < len) {
        int64_t bs = dev->read(data + total, static_cast<int32_t>(len - total));
        if (bs < 0) {
            status = BencodeStream::ReadPastEnd;
            return false;
        } else if (bs == 0) {
            if (!metZero) {
                metZero = true;
            } else {
                status = BencodeStream::ReadPastEnd;
                return false;
            }
        } else {
            total += bs;
        }
    }
    pos += static_cast<uint32_t>(len);
    return true;
}

bool BencodeStreamPrivate::writeBytes(const char *data, int64_t len)
{
    if (status != BencodeStream::Ok) {
        return false;
    }
    if (!dev) {
        status = BencodeStream::WriteFailed;
        return false;
    }
    int64_t total = 0;
    while (total < len) {
        int64_t bs = dev->write(data, static_cast<int32_t>(len - total));
        if (bs < 0) {
            status = BencodeStream::WriteFailed;
            return false;
        }
        data += bs;
        total += bs;
    }
    pos += static_cast<uint32_t>(len);
    return true;
}

bool BencodeStreamPrivate::ensurePeekCached()
{
    if (hasPeekByte) {
        return true;
    }
    if (status != BencodeStream::Ok || !dev) {
        return false;
    }
    if (pos >= limit) {
        return false;
    }
    char byte = 0;
    bool metZero = false;
    while (true) {
        int64_t bs = dev->read(&byte, 1);
        if (bs < 0) {
            status = BencodeStream::ReadPastEnd;
            return false;
        } else if (bs == 0) {
            if (!metZero) {
                metZero = true;
            } else {
                status = BencodeStream::ReadPastEnd;
                return false;
            }
        } else {
            peekByteCache = byte;
            hasPeekByte = true;
            return true;
        }
    }
}

bool BencodeStreamPrivate::peekByte(char *c) const
{
    BencodeStreamPrivate *self = const_cast<BencodeStreamPrivate *>(this);
    if (!self->ensurePeekCached()) {
        return false;
    }
    *c = peekByteCache;
    return true;
}

bool BencodeStreamPrivate::consumeByte(char expected)
{
    char c;
    if (!readBytes(&c, 1)) {
        return false;
    }
    if (c != expected) {
        status = BencodeStream::ReadCorruptData;
        return false;
    }
    return true;
}

bool BencodeStreamPrivate::account(size_t n)
{
    if (bytesAllocated > allocLimit || n > allocLimit - bytesAllocated) {
        status = BencodeStream::ReadCorruptData;
        return false;
    }
    bytesAllocated += n;
    return true;
}

bool BencodeStreamPrivate::readInteger(int64_t &i)
{
    if (!consumeByte('i')) {
        return false;
    }
    string digits;
    char c;
    if (!readBytes(&c, 1)) {
        return false;
    }
    if (c == '-') {
        digits.push_back(c);
        if (!readBytes(&c, 1)) {
            return false;
        }
    }
    if (!isdigit(static_cast<unsigned char>(c))) {
        status = BencodeStream::ReadCorruptData;
        return false;
    }
    if (c == '0') {
        digits.push_back(c);
        if (!readBytes(&c, 1)) {
            return false;
        }
        if (c != 'e') {
            status = BencodeStream::ReadCorruptData;
            return false;
        }
        i = 0;
        return true;
    }
    digits.push_back(c);
    while (true) {
        if (!readBytes(&c, 1)) {
            return false;
        }
        if (c == 'e') {
            break;
        }
        if (!isdigit(static_cast<unsigned char>(c))) {
            status = BencodeStream::ReadCorruptData;
            return false;
        }
        digits.push_back(c);
        if (digits.size() > 20) {
            status = BencodeStream::ReadCorruptData;
            return false;
        }
    }
    if (digits == "-0") {
        status = BencodeStream::ReadCorruptData;
        return false;
    }
    char *end = nullptr;
    long long v = strtoll(digits.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        status = BencodeStream::ReadCorruptData;
        return false;
    }
    i = static_cast<int64_t>(v);
    return true;
}

bool BencodeStreamPrivate::writeInteger(int64_t i)
{
    string out;
    out.push_back('i');
    out.append(to_string(i));
    out.push_back('e');
    return writeBytes(out.data(), static_cast<int64_t>(out.size()));
}

bool BencodeStreamPrivate::readString(string &s)
{
    char c;
    if (!peekByte(&c)) {
        status = BencodeStream::ReadPastEnd;
        return false;
    }
    if (!isdigit(static_cast<unsigned char>(c))) {
        status = BencodeStream::ReadCorruptData;
        return false;
    }
    string lenDigits;
    while (peekByte(&c) && isdigit(static_cast<unsigned char>(c))) {
        if (!readBytes(&c, 1)) {
            return false;
        }
        lenDigits.push_back(c);
        if (lenDigits.size() > 10) {
            status = BencodeStream::ReadCorruptData;
            return false;
        }
    }
    if (lenDigits.empty() || (lenDigits.size() > 1 && lenDigits[0] == '0')) {
        status = BencodeStream::ReadCorruptData;
        return false;
    }
    if (!consumeByte(':')) {
        return false;
    }
    char *end = nullptr;
    unsigned long long len = strtoull(lenDigits.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || len > allocLimit) {
        status = BencodeStream::ReadCorruptData;
        return false;
    }
    if (!account(static_cast<size_t>(len))) {
        return false;
    }
    s.resize(static_cast<size_t>(len));
    if (len > 0 && !readBytes(&s[0], static_cast<int64_t>(len))) {
        return false;
    }
    return true;
}

bool BencodeStreamPrivate::writeString(const string &s)
{
    string out = to_string(s.size());
    out.push_back(':');
    out.append(s);
    return writeBytes(out.data(), static_cast<int64_t>(out.size()));
}

bool BencodeStreamPrivate::readValue(Bencode &v)
{
    char c;
    if (!peekByte(&c)) {
        status = BencodeStream::ReadPastEnd;
        return false;
    }
    if (c == 'i') {
        int64_t i = 0;
        if (!readInteger(i)) {
            return false;
        }
        v = Bencode();
        v.m_type = Bencode::Integer;
        v.m_integer = i;
        return true;
    }
    if (c == 'l') {
        if (!consumeByte('l')) {
            return false;
        }
        v = Bencode::list();
        while (true) {
            if (!peekByte(&c)) {
                status = BencodeStream::ReadPastEnd;
                return false;
            }
            if (c == 'e') {
                return consumeByte('e');
            }
            Bencode item;
            if (!readValue(item)) {
                return false;
            }
            if (!account(1)) {
                return false;
            }
            v.m_list.push_back(item);
        }
    }
    if (c == 'd') {
        if (!consumeByte('d')) {
            return false;
        }
        v = Bencode::dict();
        while (true) {
            if (!peekByte(&c)) {
                status = BencodeStream::ReadPastEnd;
                return false;
            }
            if (c == 'e') {
                return consumeByte('e');
            }
            string key;
            if (!readString(key)) {
                return false;
            }
            Bencode value;
            if (!readValue(value)) {
                return false;
            }
            if (!account(1)) {
                return false;
            }
            v.m_dict[key] = value;
        }
    }
    if (isdigit(static_cast<unsigned char>(c))) {
        string s;
        if (!readString(s)) {
            return false;
        }
        v = Bencode();
        v.m_type = Bencode::String;
        v.m_string = s;
        return true;
    }
    status = BencodeStream::ReadCorruptData;
    return false;
}

bool BencodeStreamPrivate::writeValue(const Bencode &v)
{
    switch (v.m_type) {
    case Bencode::Integer:
        return writeInteger(v.m_integer);
    case Bencode::String:
        return writeString(v.m_string);
    case Bencode::List:
        if (!writeBytes("l", 1)) {
            return false;
        }
        for (size_t i = 0; i < v.m_list.size(); ++i) {
            if (!writeValue(v.m_list[i])) {
                return false;
            }
        }
        return writeBytes("e", 1);
    case Bencode::Dict:
        if (!writeBytes("d", 1)) {
            return false;
        }
        for (map<string, Bencode>::const_iterator it = v.m_dict.begin(); it != v.m_dict.end(); ++it) {
            if (!writeString(it->first)) {
                return false;
            }
            if (!writeValue(it->second)) {
                return false;
            }
        }
        return writeBytes("e", 1);
    case Bencode::Invalid:
    default:
        status = BencodeStream::WriteFailed;
        return false;
    }
}

BencodeStream::BencodeStream()
    : d_ptr(new BencodeStreamPrivate())
{
}

BencodeStream::BencodeStream(FileLike *d)
    : d_ptr(new BencodeStreamPrivate(d))
{
}

BencodeStream::BencodeStream(string *a, bool writeMode)
    : d_ptr(new BencodeStreamPrivate(a, writeMode))
{
}

BencodeStream::BencodeStream(const string &a)
    : d_ptr(new BencodeStreamPrivate(a))
{
}

BencodeStream::~BencodeStream()
{
    delete d_ptr;
}

void BencodeStream::setDevice(FileLike *dev)
{
    NG_D(BencodeStream);
    if (d->owndev) {
        delete d->dev;
    }
    d->dev = dev;
    d->owndev = false;
    d->pos = 0;
    d->bytesAllocated = 0;
    d->hasPeekByte = false;
}

FileLike *BencodeStream::device() const
{
    NG_D(const BencodeStream);
    return d->dev;
}

string BencodeStream::data() const
{
    NG_D(const BencodeStream);
    BytesIO *buf = dynamic_cast<BytesIO *>(d->dev);
    if (buf) {
        return buf->data();
    }
    return string();
}

bool BencodeStream::atEnd() const
{
    NG_D(const BencodeStream);
    return d->dev ? (d->pos >= d->limit) : true;
}

BencodeStream::Status BencodeStream::status() const
{
    NG_D(const BencodeStream);
    return d->status;
}

void BencodeStream::resetStatus()
{
    NG_D(BencodeStream);
    d->status = Ok;
}

void BencodeStream::setStatus(Status status)
{
    NG_D(BencodeStream);
    d->status = status;
}

void BencodeStream::setLengthLimit(uint32_t limit)
{
    NG_D(BencodeStream);
    d->allocLimit = limit;
}

uint32_t BencodeStream::lengthLimit() const
{
    NG_D(const BencodeStream);
    return d->allocLimit;
}

BencodeStream &BencodeStream::operator>>(int64_t &i)
{
    CHECK_STREAM_PRECOND(*this);
    if (!d->readInteger(i)) {
        if (d->status == Ok) {
            d->status = ReadCorruptData;
        }
    }
    return *this;
}

BencodeStream &BencodeStream::operator>>(string &str)
{
    CHECK_STREAM_PRECOND(*this);
    if (!d->readString(str)) {
        if (d->status == Ok) {
            d->status = ReadCorruptData;
        }
    }
    return *this;
}

BencodeStream &BencodeStream::operator>>(Bencode &v)
{
    CHECK_STREAM_PRECOND(*this);
    if (!d->readValue(v)) {
        if (d->status == Ok) {
            d->status = ReadCorruptData;
        }
    }
    return *this;
}

BencodeStream &BencodeStream::operator<<(int64_t i)
{
    CHECK_STREAM_PRECOND(*this);
    if (!d->writeInteger(i)) {
        if (d->status == Ok) {
            d->status = WriteFailed;
        }
    }
    return *this;
}

BencodeStream &BencodeStream::operator<<(const string &str)
{
    CHECK_STREAM_PRECOND(*this);
    if (!d->writeString(str)) {
        if (d->status == Ok) {
            d->status = WriteFailed;
        }
    }
    return *this;
}

BencodeStream &BencodeStream::operator<<(const Bencode &v)
{
    CHECK_STREAM_PRECOND(*this);
    if (!d->writeValue(v)) {
        if (d->status == Ok) {
            d->status = WriteFailed;
        }
    }
    return *this;
}

bool BencodeStream::readBytes(char *data, int64_t len)
{
    NG_D(BencodeStream);
    return d->readBytes(data, len);
}

bool BencodeStream::peekByte(uint8_t *b) const
{
    NG_D(const BencodeStream);
    char c = 0;
    if (!d->peekByte(&c)) {
        return false;
    }
    *b = static_cast<uint8_t>(c);
    return true;
}

bool BencodeStream::readArrayHeader(uint32_t &len)
{
    CHECK_STREAM_PRECOND(false);
    if (!d->consumeByte('l')) {
        return false;
    }
    // Bencode has no length prefix; UINT32_MAX means indefinite (use peekContainerEnd).
    len = numeric_limits<uint32_t>::max();
    return true;
}

bool BencodeStream::readMapHeader(uint32_t &len)
{
    CHECK_STREAM_PRECOND(false);
    if (!d->consumeByte('d')) {
        return false;
    }
    len = numeric_limits<uint32_t>::max();
    return true;
}

bool BencodeStream::readArrayEnd()
{
    CHECK_STREAM_PRECOND(false);
    return d->consumeByte('e');
}

bool BencodeStream::readMapEnd()
{
    CHECK_STREAM_PRECOND(false);
    return d->consumeByte('e');
}

bool BencodeStream::peekContainerEnd() const
{
    NG_D(const BencodeStream);
    if (!d->dev || d->status != Ok) {
        return true;
    }
    char c = 0;
    if (!d->peekByte(&c)) {
        return true;
    }
    return c == 'e';
}

bool BencodeStream::writeBytes(const char *data, int64_t len)
{
    NG_D(BencodeStream);
    return d->writeBytes(data, len);
}

bool BencodeStream::writeArrayHeader(uint32_t len)
{
    (void) len;
    CHECK_STREAM_PRECOND(false);
    return d->writeBytes("l", 1);
}

bool BencodeStream::writeMapHeader(uint32_t len)
{
    (void) len;
    CHECK_STREAM_PRECOND(false);
    return d->writeBytes("d", 1);
}

bool BencodeStream::writeArrayEnd()
{
    CHECK_STREAM_PRECOND(false);
    return d->writeBytes("e", 1);
}

bool BencodeStream::writeMapEnd()
{
    CHECK_STREAM_PRECOND(false);
    return d->writeBytes("e", 1);
}

Bencode::Bencode()
    : m_type(Invalid)
    , m_integer(0)
{
}

Bencode::Bencode(int64_t i)
    : m_type(Integer)
    , m_integer(i)
{
}

Bencode::Bencode(const string &s)
    : m_type(String)
    , m_integer(0)
    , m_string(s)
{
}

Bencode::Bencode(const char *s)
    : m_type(String)
    , m_integer(0)
    , m_string(s ? s : "")
{
}

Bencode::Bencode(const vector<Bencode> &list)
    : m_type(List)
    , m_integer(0)
    , m_list(list)
{
}

Bencode::Bencode(vector<Bencode> &&list)
    : m_type(List)
    , m_integer(0)
    , m_list(std::move(list))
{
}

Bencode::Bencode(const map<string, Bencode> &dict)
    : m_type(Dict)
    , m_integer(0)
    , m_dict(dict)
{
}

Bencode::Bencode(map<string, Bencode> &&dict)
    : m_type(Dict)
    , m_integer(0)
    , m_dict(std::move(dict))
{
}

Bencode::Bencode(const Bencode &other)
    : m_type(other.m_type)
    , m_integer(other.m_integer)
    , m_string(other.m_string)
    , m_list(other.m_list)
    , m_dict(other.m_dict)
{
}

Bencode &Bencode::operator=(const Bencode &other)
{
    if (this != &other) {
        m_type = other.m_type;
        m_integer = other.m_integer;
        m_string = other.m_string;
        m_list = other.m_list;
        m_dict = other.m_dict;
    }
    return *this;
}

Bencode::~Bencode() { }

Bencode Bencode::dict()
{
    return Bencode(map<string, Bencode>());
}

Bencode Bencode::list()
{
    return Bencode(vector<Bencode>());
}

int64_t Bencode::toInteger(int64_t defaultValue) const
{
    return m_type == Integer ? m_integer : defaultValue;
}

string Bencode::toString() const
{
    return m_type == String ? m_string : string();
}

const vector<Bencode> &Bencode::toList() const
{
    static const vector<Bencode> empty;
    return m_type == List ? m_list : empty;
}

const map<string, Bencode> &Bencode::toMap() const
{
    static const map<string, Bencode> empty;
    return m_type == Dict ? m_dict : empty;
}

string Bencode::encode() const
{
    BencodeStream stream;
    stream << *this;
    return stream.data();
}

Bencode Bencode::decode(const string &data, string *error, uint32_t lengthLimit)
{
    BencodeStream stream(data);
    stream.setLengthLimit(lengthLimit);
    Bencode value;
    stream >> value;
    if (!stream.isOk()) {
        if (error) {
            *error = "invalid bencode input";
        }
        return Bencode();
    }
    if (!stream.atEnd()) {
        if (error) {
            *error = "trailing data after bencode value";
        }
        return Bencode();
    }
    return value;
}

Bencode Bencode::decode(FileLike *device, string *error, uint32_t lengthLimit)
{
    if (!device) {
        if (error) {
            *error = "null FileLike";
        }
        return Bencode();
    }
    bool ok = false;
    string data = device->readall(&ok);
    if (!ok) {
        if (error) {
            *error = "failed to read FileLike";
        }
        return Bencode();
    }
    return decode(data, error, lengthLimit);
}

bool Bencode::operator==(const Bencode &other) const
{
    if (m_type != other.m_type) {
        return false;
    }
    switch (m_type) {
    case Integer:
        return m_integer == other.m_integer;
    case String:
        return m_string == other.m_string;
    case List:
        return m_list == other.m_list;
    case Dict:
        return m_dict == other.m_dict;
    case Invalid:
    default:
        return true;
    }
}

}  // namespace qtng
