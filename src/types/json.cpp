#include "qtng/types/json.h"

#include "qtng/types/bytes.h"
#include "qtng/types/string.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale.h>
#include <ostream>
#include <string>

using namespace std;

QTNG_NAMESPACE_BEGIN

namespace detail {

// A value is exactly one of the six types, so it carries exactly one payload. Empty COW
// handles are not free in this library -- String/Vector/Map each allocate their block in
// the default constructor -- so keeping all three as members made every number in a
// document pay for an empty string, an empty array and an empty object (three mallocs
// per scalar). The payloads are therefore allocated only for the type that owns them.
//
// Invariant: s is non-null iff t == String, and likewise for a/Array and o/Object.
// Constructors below are the only place that allocates a payload, which is what keeps the
// accessors free of null checks.
//
// The pointees are const on purpose: they are pointers rather than members, and a
// non-const pointer would make `d->a->at(i)` bind to Vector's *mutating* at(), which
// detaches and therefore deep-copies the whole container on a mere read (and mutates a
// snapshot that other JsonValues share). const on the pointee forces the read-only
// overloads at every call site, so the immutable snapshot is a compiler-enforced fact
// instead of a convention.
class JsonValuePrivate
{
public:
    JsonValuePrivate()
        : t(JsonValue::Null)
        , boolean(false)
        , integer(false)
        , iv(0)
        , dv(0.0)
    {
    }

    JsonValue::Type t;
    bool boolean;
    bool integer;
    int64_t iv;
    double dv;
    unique_ptr<const ::QTNG_NAMESPACE::String> s;
    unique_ptr<const JsonArray> a;
    unique_ptr<const JsonObject> o;
};

}  // namespace detail

namespace {

const int kIndentWidth = 4;

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// The locale's decimal point is not necessarily '.', and both strtod() and snprintf()
// honour LC_NUMERIC. Rewriting the separator keeps parsing/formatting independent of
// the process locale without touching it (setlocale() would be a data race).
void convertAsciiDecimalPointToLocale(string &text)
{
    const lconv *lc = localeconv();
    const char *dp = lc != nullptr ? lc->decimal_point : ".";
    if (dp == nullptr || dp[0] == '\0' || (dp[0] == '.' && dp[1] == '\0')) {
        return;
    }
    string converted;
    converted.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '.') {
            converted.append(dp);
        } else {
            converted.push_back(text[i]);
        }
    }
    text.swap(converted);
}

// Rewrites the locale's decimal point to '.' in place and returns the resulting length.
// The length has to come back out: a multi-byte separator makes the buffer shorter than
// what snprintf() reported, and callers must not keep slicing the stale count.
size_t convertLocaleDecimalPointToAscii(char *buf)
{
    const lconv *lc = localeconv();
    const char *dp = lc != nullptr ? lc->decimal_point : ".";
    if (dp == nullptr || dp[0] == '\0' || (dp[0] == '.' && dp[1] == '\0')) {
        return strlen(buf);
    }
    char *hit = strstr(buf, dp);
    if (hit == nullptr) {
        return strlen(buf);
    }
    const size_t dplen = strlen(dp);
    *hit = '.';
    if (dplen > 1) {
        memmove(hit + 1, hit + dplen, strlen(hit + dplen) + 1);
    }
    return strlen(buf);
}

double strtodNoLocale(const char *token, size_t len, bool *ok)
{
    string buf(token, len);
    convertAsciiDecimalPointToLocale(buf);
    char *end = nullptr;
    const double v = strtod(buf.c_str(), &end);
    *ok = end != nullptr && *end == '\0' && end != buf.c_str();
    return *ok ? v : 0.0;
}

// Shortest decimal that reads back as the same double. C++11 has no std::to_chars, so
// try increasing precision and keep the first that round-trips bit-exactly.
string formatDouble(double v)
{
    char buf[64];
    // Negative zero needs an explicit fractional form: "-0" would parse back as the
    // integer 0 and lose the sign.
    if (v == 0.0 && signbit(v)) {
        return "-0.0";
    }
    for (int precision = 1; precision <= 17; ++precision) {
        const int n = snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
            continue;
        }
        const size_t len = convertLocaleDecimalPointToAscii(buf);
        bool ok = false;
        const double back = strtodNoLocale(buf, len, &ok);
        if (ok && memcmp(&back, &v, sizeof(double)) == 0) {
            return string(buf, len);
        }
    }
    const int n = snprintf(buf, sizeof(buf), "%.17g", v);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
        // Unreachable: 17 significant digits of a finite double need at most ~24 bytes.
        // Emitting "null" is the file's convention for a double it cannot represent.
        return "null";
    }
    return string(buf, convertLocaleDecimalPointToAscii(buf));
}

string formatInteger(int64_t v)
{
    char buf[32];
    const int n = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
        // Unreachable: int64 needs at most 20 bytes. Never emit a truncated number or an
        // empty token that would make the whole document unparsable.
        return "null";
    }
    return string(buf, static_cast<size_t>(n));
}

void appendIndent(string &out, int depth)
{
    out.append(static_cast<size_t>(depth) * kIndentWidth, ' ');
}

void appendStringLiteral(string &out, const ::QTNG_NAMESPACE::String &s)
{
    out.push_back('"');
    const char *p = s.data();
    const uint32_t n = s.byteSize();
    for (uint32_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
                out += esc;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

void appendValue(string &out, const JsonValue &value, bool pretty, int depth);

void appendArray(string &out, const JsonArray &array, bool pretty, int depth)
{
    if (array.isEmpty()) {
        out += "[]";
        return;
    }
    out.push_back('[');
    const JsonArray::SizeType n = array.size();
    for (JsonArray::SizeType i = 0; i < n; ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        if (pretty) {
            out.push_back('\n');
            appendIndent(out, depth + 1);
        }
        appendValue(out, array.at(i), pretty, depth + 1);
    }
    if (pretty) {
        out.push_back('\n');
        appendIndent(out, depth);
    }
    out.push_back(']');
}

void appendObject(string &out, const JsonObject &object, bool pretty, int depth)
{
    if (object.isEmpty()) {
        out += "{}";
        return;
    }
    out.push_back('{');
    bool first = true;
    const JsonObject::StlType &c = object.get();
    for (JsonObject::ConstIterator it = c.begin(); it != c.end(); ++it) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        if (pretty) {
            out.push_back('\n');
            appendIndent(out, depth + 1);
        }
        appendStringLiteral(out, it->first);
        out.push_back(':');
        if (pretty) {
            out.push_back(' ');
        }
        appendValue(out, it->second, pretty, depth + 1);
    }
    if (pretty) {
        out.push_back('\n');
        appendIndent(out, depth);
    }
    out.push_back('}');
}

void appendValue(string &out, const JsonValue &value, bool pretty, int depth)
{
    switch (value.type()) {
    case JsonValue::Null:
        out += "null";
        break;
    case JsonValue::Bool:
        out += value.toBool() ? "true" : "false";
        break;
    case JsonValue::Double: {
        if (value.isInteger()) {
            out += formatInteger(value.toInteger());
        } else {
            const double d = value.toDouble();
            if (!isfinite(d)) {
                out += "null";
            } else {
                out += formatDouble(d);
            }
        }
        break;
    }
    case JsonValue::String:
        appendStringLiteral(out, value.toString());
        break;
    case JsonValue::Array:
        appendArray(out, value.toArray(), pretty, depth);
        break;
    case JsonValue::Object:
        appendObject(out, value.toObject(), pretty, depth);
        break;
    }
}

string serialize(const JsonValue &value, bool pretty)
{
    string out;
    appendValue(out, value, pretty, 0);
    return out;
}

class JsonParser
{
public:
    JsonParser(const char *data, uint32_t size, int maxDepth)
        : data_(data)
        , size_(size)
        , pos_(0)
        , maxDepth_(maxDepth)
    {
    }

    JsonValue parse()
    {
        JsonValue value = parseValue(0);
        if (bad()) {
            return JsonValue();
        }
        skipWhitespace();
        if (pos_ != size_) {
            fail(JsonParseError::GarbageAtEnd, pos_, "trailing characters after the JSON value");
            return JsonValue();
        }
        return value;
    }

    const JsonParseError &error() const { return error_; }

private:
    bool bad() const { return error_.error != JsonParseError::NoError; }

    void fail(JsonParseError::ParseError e, size_t offset, const char *message)
    {
        if (bad()) {
            return;
        }
        error_ = JsonParseError(e, static_cast<int64_t>(offset), ::QTNG_NAMESPACE::String(message));
    }

    void skipWhitespace()
    {
        while (pos_ < size_) {
            const char c = data_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos_;
        }
    }

    // Container loops end for exactly one reason: the input ran out. These two helpers
    // exist so the grammar at the call sites stays readable, and so the error code is
    // named rather than computed from an argument.
    bool moreInObject()
    {
        skipWhitespace();
        if (pos_ < size_) {
            return true;
        }
        fail(JsonParseError::UnterminatedObject, pos_, "unexpected end of input inside an object");
        return false;
    }

    bool moreInArray()
    {
        skipWhitespace();
        if (pos_ < size_) {
            return true;
        }
        fail(JsonParseError::UnterminatedArray, pos_, "unexpected end of input inside an array");
        return false;
    }

    JsonValue parseValue(int depth)
    {
        skipWhitespace();
        if (pos_ >= size_) {
            fail(JsonParseError::IllegalValue, pos_, "unexpected end of input");
            return JsonValue();
        }
        const char c = data_[pos_];
        switch (c) {
        case '{':
            return parseObject(depth + 1);
        case '[':
            return parseArray(depth + 1);
        case '"': {
            ::QTNG_NAMESPACE::String str;
            if (!parseString(&str)) {
                return JsonValue();
            }
            return JsonValue(str);
        }
        case 't':
            return parseLiteral("true", JsonValue(true));
        case 'f':
            return parseLiteral("false", JsonValue(false));
        case 'n':
            return parseLiteral("null", JsonValue());
        default:
            if (c == '-' || isDigit(c)) {
                return parseNumber();
            }
            fail(JsonParseError::IllegalValue, pos_, "unexpected character where a value was expected");
            return JsonValue();
        }
    }

    JsonValue parseLiteral(const char *literal, const JsonValue &value)
    {
        const size_t len = strlen(literal);
        if (static_cast<size_t>(size_ - pos_) < len
            || memcmp(data_ + pos_, literal, len) != 0) {
            fail(JsonParseError::IllegalValue, pos_, "ill-formed literal");
            return JsonValue();
        }
        pos_ += static_cast<uint32_t>(len);
        return value;
    }

    JsonValue parseObject(int depth)
    {
        if (depth > maxDepth_) {
            fail(JsonParseError::DeepNesting, pos_, "object nesting exceeds the depth limit");
            return JsonValue();
        }
        ++pos_;  // '{'
        JsonObject object;
        if (!moreInObject()) {
            return JsonValue();
        }
        if (data_[pos_] == '}') {
            ++pos_;
            return JsonValue(object);
        }
        for (;;) {
            if (data_[pos_] != '"') {
                fail(JsonParseError::MissingObject, pos_, "object key must be a string");
                return JsonValue();
            }
            ::QTNG_NAMESPACE::String key;
            if (!parseString(&key)) {
                return JsonValue();
            }
            if (!moreInObject()) {
                return JsonValue();
            }
            if (data_[pos_] != ':') {
                fail(JsonParseError::MissingNameSeparator, pos_, "':' expected after an object key");
                return JsonValue();
            }
            ++pos_;
            const JsonValue member = parseValue(depth);
            if (bad()) {
                return JsonValue();
            }
            object.insert(key, member);
            if (!moreInObject()) {
                return JsonValue();
            }
            if (data_[pos_] == '}') {
                ++pos_;
                return JsonValue(object);
            }
            if (data_[pos_] != ',') {
                fail(JsonParseError::MissingValueSeparator, pos_, "',' or '}' expected");
                return JsonValue();
            }
            ++pos_;
            if (!moreInObject()) {
                return JsonValue();
            }
        }
    }

    JsonValue parseArray(int depth)
    {
        if (depth > maxDepth_) {
            fail(JsonParseError::DeepNesting, pos_, "array nesting exceeds the depth limit");
            return JsonValue();
        }
        ++pos_;  // '['
        JsonArray array;
        if (!moreInArray()) {
            return JsonValue();
        }
        if (data_[pos_] == ']') {
            ++pos_;
            return JsonValue(array);
        }
        for (;;) {
            const JsonValue item = parseValue(depth);
            if (bad()) {
                return JsonValue();
            }
            array.append(item);
            if (!moreInArray()) {
                return JsonValue();
            }
            if (data_[pos_] == ']') {
                ++pos_;
                return JsonValue(array);
            }
            if (data_[pos_] != ',') {
                fail(JsonParseError::MissingValueSeparator, pos_, "',' or ']' expected");
                return JsonValue();
            }
            ++pos_;
            if (!moreInArray()) {
                return JsonValue();
            }
        }
    }

    bool parseString(::QTNG_NAMESPACE::String *out)
    {
        ++pos_;  // '"'
        string buffer;
        while (pos_ < size_) {
            const unsigned char c = static_cast<unsigned char>(data_[pos_]);
            if (c == '"') {
                ++pos_;
                *out = ::QTNG_NAMESPACE::String(buffer);
                return true;
            }
            if (c == '\\') {
                if (!parseEscape(&buffer)) {
                    return false;
                }
                continue;
            }
            if (c < 0x20) {
                fail(JsonParseError::IllegalUTF8String, pos_, "unescaped control character in a string");
                return false;
            }
            if (c < 0x80) {
                buffer.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            uint32_t cp = 0;
            const uint32_t consumed = detail::utf8DecodeStrict(data_ + pos_, size_ - pos_, &cp);
            if (consumed == 0) {
                fail(JsonParseError::IllegalUTF8String, pos_, "invalid UTF-8 in a string");
                return false;
            }
            buffer.append(data_ + pos_, consumed);
            pos_ += consumed;
        }
        fail(JsonParseError::UnterminatedString, pos_, "unexpected end of input inside a string");
        return false;
    }

    bool parseEscape(string *buffer)
    {
        const size_t escapeOffset = pos_;
        ++pos_;  // '\\'
        if (pos_ >= size_) {
            fail(JsonParseError::UnterminatedString, pos_, "unexpected end of input inside a string");
            return false;
        }
        const char e = data_[pos_++];
        switch (e) {
        case '"':
            buffer->push_back('"');
            return true;
        case '\\':
            buffer->push_back('\\');
            return true;
        case '/':
            buffer->push_back('/');
            return true;
        case 'b':
            buffer->push_back('\b');
            return true;
        case 'f':
            buffer->push_back('\f');
            return true;
        case 'n':
            buffer->push_back('\n');
            return true;
        case 'r':
            buffer->push_back('\r');
            return true;
        case 't':
            buffer->push_back('\t');
            return true;
        case 'u':
            break;
        default:
            fail(JsonParseError::IllegalEscapeSequence, escapeOffset, "invalid escape sequence");
            return false;
        }

        uint32_t cp = 0;
        if (!parseHex4(&cp)) {
            return false;
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
            fail(JsonParseError::IllegalUTF8String, escapeOffset, "unpaired UTF-16 low surrogate");
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos_ + 1 >= size_ || data_[pos_] != '\\' || data_[pos_ + 1] != 'u') {
                fail(JsonParseError::IllegalUTF8String, escapeOffset, "unpaired UTF-16 high surrogate");
                return false;
            }
            pos_ += 2;
            uint32_t low = 0;
            if (!parseHex4(&low)) {
                return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                fail(JsonParseError::IllegalUTF8String, escapeOffset, "invalid UTF-16 surrogate pair");
                return false;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        detail::utf8Encode(cp, *buffer);
        return true;
    }

    bool parseHex4(uint32_t *out)
    {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= size_) {
                fail(JsonParseError::UnterminatedString, pos_, "truncated \\u escape at the end of input");
                return false;
            }
            const int digit = hexValue(data_[pos_]);
            if (digit < 0) {
                fail(JsonParseError::IllegalEscapeSequence, pos_, "invalid \\u escape digit");
                return false;
            }
            value = (value << 4) | static_cast<uint32_t>(digit);
            ++pos_;
        }
        *out = value;
        return true;
    }

    JsonValue parseNumber()
    {
        const size_t start = pos_;
        bool negative = false;
        if (data_[pos_] == '-') {
            negative = true;
            ++pos_;
        }
        if (pos_ >= size_) {
            fail(JsonParseError::IllegalNumber, pos_, "no digits in a number");
            return JsonValue();
        }
        if (data_[pos_] == '0') {
            ++pos_;
            if (pos_ < size_ && isDigit(data_[pos_])) {
                fail(JsonParseError::IllegalNumber, pos_, "leading zero in a number");
                return JsonValue();
            }
        } else if (data_[pos_] >= '1' && data_[pos_] <= '9') {
            while (pos_ < size_ && isDigit(data_[pos_])) {
                ++pos_;
            }
        } else {
            fail(JsonParseError::IllegalNumber, pos_, "no digits in a number");
            return JsonValue();
        }

        bool isInteger = true;
        if (pos_ < size_ && data_[pos_] == '.') {
            isInteger = false;
            ++pos_;
            if (pos_ >= size_ || !isDigit(data_[pos_])) {
                fail(JsonParseError::IllegalNumber, pos_, "digits expected after the decimal point");
                return JsonValue();
            }
            while (pos_ < size_ && isDigit(data_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < size_ && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
            isInteger = false;
            ++pos_;
            if (pos_ < size_ && (data_[pos_] == '+' || data_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= size_ || !isDigit(data_[pos_])) {
                fail(JsonParseError::IllegalNumber, pos_, "digits expected in the exponent");
                return JsonValue();
            }
            while (pos_ < size_ && isDigit(data_[pos_])) {
                ++pos_;
            }
        }

        if (pos_ < size_) {
            const char next = data_[pos_];
            if (isDigit(next) || (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')
                || next == '.' || next == '+' || next == '-') {
                fail(JsonParseError::TerminationByNumber, pos_, "unexpected character after a number");
                return JsonValue();
            }
        }

        const uint32_t len = static_cast<uint32_t>(pos_ - start);
        if (isInteger) {
            const char *p = data_ + start + (negative ? 1 : 0);
            const char *const end = data_ + pos_;
            unsigned long long magnitude = 0;
            bool overflow = false;
            for (; p < end; ++p) {
                const unsigned int digit = static_cast<unsigned int>(*p - '0');
                if (magnitude > (ULLONG_MAX - digit) / 10ULL) {
                    overflow = true;
                    break;
                }
                magnitude = magnitude * 10ULL + digit;
            }
            const unsigned long long limit =
                    negative ? (1ULL << 63) : static_cast<unsigned long long>(INT64_MAX);
            if (!overflow && magnitude <= limit) {
                if (negative) {
                    const int64_t v = magnitude == (1ULL << 63)
                            ? INT64_MIN
                            : -static_cast<int64_t>(magnitude);
                    return JsonValue(static_cast<long long>(v));
                }
                return JsonValue(static_cast<long long>(magnitude));
            }
        }

        bool ok = false;
        const double value = strtodNoLocale(data_ + start, len, &ok);
        if (!ok || !isfinite(value)) {
            fail(JsonParseError::IllegalNumber, start, "number is out of range");
            return JsonValue();
        }
        return JsonValue(value);
    }

private:
    const char *data_;
    uint32_t size_;
    uint32_t pos_;
    int maxDepth_;
    JsonParseError error_;
};

// Byte offset of the first byte that breaks UTF-8, or -1 when the whole buffer is valid.
int64_t firstInvalidUtf8Offset(const char *data, uint32_t size)
{
    uint32_t i = 0;
    while (i < size) {
        uint32_t cp = 0;
        const uint32_t consumed = detail::utf8DecodeStrict(data + i, size - i, &cp);
        if (consumed == 0) {
            return static_cast<int64_t>(i);
        }
        i += consumed;
    }
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// JsonValue
// ---------------------------------------------------------------------------

namespace {

// The one place that allocates a value's private. Every constructor below fills in the
// payload it cares about instead of repeating "make_shared + assign the type tag", which
// is the step that is easy to forget when adding a constructor.
shared_ptr<detail::JsonValuePrivate> newValue(JsonValue::Type type)
{
    shared_ptr<detail::JsonValuePrivate> p = make_shared<detail::JsonValuePrivate>();
    p->t = type;
    return p;
}

// Both fromJson overloads reject an oversized document; the two must agree on the error
// code, the offset and the wording.
JsonDocument rejectTooLarge(JsonParseError *error)
{
    if (error != nullptr) {
        *error = JsonParseError(JsonParseError::DocumentTooLarge, 0,
                                ::QTNG_NAMESPACE::String("document exceeds the size limit"));
    }
    return JsonDocument();
}

// Exact conversion of a double to int64: the single definition of "this double is an
// integer that fits". toInteger() and operator==() must agree on it, otherwise a value
// could compare equal to something toString()/toJson() cannot reproduce.
bool doubleToInteger(double d, int64_t *out)
{
    if (!isfinite(d) || d < -9223372036854775808.0 || d >= 9223372036854775808.0) {
        return false;
    }
    if (floor(d) != d) {
        return false;
    }
    *out = static_cast<int64_t>(d);
    return true;
}

bool integerEqualsDouble(int64_t i, double d)
{
    int64_t converted = 0;
    return doubleToInteger(d, &converted) && converted == i;
}

}  // namespace

JsonValue::JsonValue()
{
}

JsonValue::JsonValue(Type type)
{
    if (type == Null) {
        return;
    }
    shared_ptr<detail::JsonValuePrivate> p = newValue(type);
    switch (type) {
    case String:
        p->s.reset(new ::QTNG_NAMESPACE::String());
        break;
    case Array:
        p->a.reset(new JsonArray());
        break;
    case Object:
        p->o.reset(new JsonObject());
        break;
    default:
        break;
    }
    d = p;
}

JsonValue::JsonValue(bool b)
{
    shared_ptr<detail::JsonValuePrivate> p = newValue(Bool);
    p->boolean = b;
    d = p;
}

JsonValue::JsonValue(double v)
{
    shared_ptr<detail::JsonValuePrivate> p = newValue(Double);
    p->dv = v;
    d = p;
}

JsonValue::JsonValue(const char *str)
{
    shared_ptr<detail::JsonValuePrivate> p = newValue(String);
    p->s.reset(new ::QTNG_NAMESPACE::String(str != nullptr ? str : ""));
    d = p;
}

JsonValue::JsonValue(const ::QTNG_NAMESPACE::String &str)
{
    shared_ptr<detail::JsonValuePrivate> p = newValue(String);
    p->s.reset(new ::QTNG_NAMESPACE::String(str));
    d = p;
}

JsonValue::JsonValue(const JsonArray &array)
{
    shared_ptr<detail::JsonValuePrivate> p = newValue(Array);
    p->a.reset(new JsonArray(array));
    d = p;
}

JsonValue::JsonValue(const JsonObject &object)
{
    shared_ptr<detail::JsonValuePrivate> p = newValue(Object);
    p->o.reset(new JsonObject(object));
    d = p;
}

JsonValue::JsonValue(const JsonValue &other)
    : d(other.d)
{
}

JsonValue &JsonValue::operator=(const JsonValue &other)
{
    d = other.d;
    return *this;
}

JsonValue::~JsonValue()
{
}

void JsonValue::assignInteger(long long value)
{
    shared_ptr<detail::JsonValuePrivate> p = newValue(Double);
    p->integer = true;
    p->iv = static_cast<int64_t>(value);
    p->dv = static_cast<double>(value);
    d = p;
}

void JsonValue::assignIntegerUnsigned(unsigned long long value)
{
    if (value <= static_cast<unsigned long long>(INT64_MAX)) {
        assignInteger(static_cast<long long>(value));
        return;
    }
    shared_ptr<detail::JsonValuePrivate> p = newValue(Double);
    p->dv = static_cast<double>(value);
    d = p;
}

JsonValue::Type JsonValue::type() const
{
    return d ? d->t : Null;
}

bool JsonValue::isInteger() const
{
    return d && d->t == Double && d->integer;
}

bool JsonValue::toBool(bool defaultValue) const
{
    return d && d->t == Bool ? d->boolean : defaultValue;
}

double JsonValue::toDouble(double defaultValue) const
{
    return d && d->t == Double ? d->dv : defaultValue;
}

int64_t JsonValue::toInteger(bool *ok) const
{
    if (d != nullptr && d->t == Double) {
        if (d->integer) {
            if (ok != nullptr) {
                *ok = true;
            }
            return d->iv;
        }
        int64_t converted = 0;
        if (doubleToInteger(d->dv, &converted)) {
            if (ok != nullptr) {
                *ok = true;
            }
            return converted;
        }
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return 0;
}

::QTNG_NAMESPACE::String JsonValue::toString(const ::QTNG_NAMESPACE::String &defaultValue) const
{
    return d && d->t == String ? *d->s : defaultValue;
}

JsonArray JsonValue::toArray() const
{
    return d && d->t == Array ? *d->a : JsonArray();
}

JsonArray JsonValue::toArray(const JsonArray &defaultValue) const
{
    return d && d->t == Array ? *d->a : defaultValue;
}

JsonObject JsonValue::toObject() const
{
    return d && d->t == Object ? *d->o : JsonObject();
}

JsonObject JsonValue::toObject(const JsonObject &defaultValue) const
{
    return d && d->t == Object ? *d->o : defaultValue;
}

JsonValue JsonValue::operator[](const ::QTNG_NAMESPACE::String &key) const
{
    if (d == nullptr || d->t != Object) {
        return JsonValue();
    }
    const JsonObject::StlType &c = d->o->get();
    const JsonObject::ConstIterator it = c.find(key);
    return it == c.end() ? JsonValue() : it->second;
}

JsonValue JsonValue::operator[](int index) const
{
    if (d == nullptr || d->t != Array || index < 0) {
        return JsonValue();
    }
    const JsonArray::SizeType i = static_cast<JsonArray::SizeType>(index);
    return i < d->a->size() ? d->a->at(i) : JsonValue();
}

bool JsonValue::contains(const ::QTNG_NAMESPACE::String &key) const
{
    return d != nullptr && d->t == Object && d->o->contains(key);
}

// Integer/double equality is decided in the integer domain: going through double loses
// the low bits above 2^53 and would call two different numbers equal.
bool JsonValue::operator==(const JsonValue &other) const
{
    const Type a = type();
    const Type b = other.type();
    if (a != b) {
        return false;
    }
    switch (a) {
    case Null:
        return true;
    case Bool:
        return d->boolean == other.d->boolean;
    case Double:
        if (d->integer && other.d->integer) {
            return d->iv == other.d->iv;
        }
        if (d->integer) {
            return integerEqualsDouble(d->iv, other.d->dv);
        }
        if (other.d->integer) {
            return integerEqualsDouble(other.d->iv, d->dv);
        }
        return d->dv == other.d->dv;
    case String:
        return *d->s == *other.d->s;
    case Array:
        return *d->a == *other.d->a;
    case Object:
        return *d->o == *other.d->o;
    }
    return false;
}

// ---------------------------------------------------------------------------
// JsonDocument
// ---------------------------------------------------------------------------

JsonDocument::JsonDocument()
{
}

JsonDocument::JsonDocument(const JsonValue &value)
    : m_value(value)
{
}

JsonDocument::JsonDocument(const JsonObject &object)
    : m_value(object)
{
}

JsonDocument::JsonDocument(const JsonArray &array)
    : m_value(array)
{
}

JsonDocument::JsonDocument(const JsonDocument &other)
    : m_value(other.m_value)
{
}

JsonDocument &JsonDocument::operator=(const JsonDocument &other)
{
    m_value = other.m_value;
    return *this;
}

JsonDocument::~JsonDocument()
{
}

// Every observation below is the corresponding JsonValue query: the document holds a
// value, so re-deriving the type tag here would be a second copy of the same dispatch.

bool JsonDocument::isNull() const
{
    return m_value.isNull();
}

bool JsonDocument::isEmpty() const
{
    return m_value.isNull();
}

bool JsonDocument::isArray() const
{
    return m_value.isArray();
}

bool JsonDocument::isObject() const
{
    return m_value.isObject();
}

JsonArray JsonDocument::array() const
{
    return m_value.toArray();
}

JsonObject JsonDocument::object() const
{
    return m_value.toObject();
}

JsonValue JsonDocument::value() const
{
    return m_value;
}

::QTNG_NAMESPACE::String JsonDocument::toJson(JsonFormat format) const
{
    const bool pretty = format == Indented;
    return ::QTNG_NAMESPACE::String(serialize(m_value, pretty));
}

JsonDocument JsonDocument::fromJson(const ::QTNG_NAMESPACE::String &json, JsonParseError *error, int maxDepth,
                                   uint32_t maxSize)
{
    if (json.byteSize() > maxSize) {
        return rejectTooLarge(error);
    }
    JsonParser parser(json.data(), json.byteSize(), maxDepth);
    const JsonValue value = parser.parse();
    const JsonParseError parseError = parser.error();
    if (error != nullptr) {
        *error = parseError;
    }
    if (parseError.error != JsonParseError::NoError) {
        return JsonDocument();
    }
    return JsonDocument(value);
}

JsonDocument JsonDocument::fromJson(const ByteArray &json, JsonParseError *error, int maxDepth,
                                   uint32_t maxSize)
{
    // Size first: scanning a too-large document for UTF-8 would be wasted work, and the
    // two overloads must report the same error for the same input.
    if (json.size() > maxSize) {
        return rejectTooLarge(error);
    }
    const int64_t badOffset = firstInvalidUtf8Offset(json.data(), json.size());
    if (badOffset >= 0) {
        if (error != nullptr) {
            *error = JsonParseError(JsonParseError::IllegalUTF8String, badOffset,
                                    ::QTNG_NAMESPACE::String("document is not valid UTF-8"));
        }
        return JsonDocument();
    }
    return fromJson(::QTNG_NAMESPACE::String::fromUtf8(json), error, maxDepth, maxSize);
}

JsonDocument JsonDocument::fromJson(const char *json, JsonParseError *error, int maxDepth, uint32_t maxSize)
{
    return fromJson(::QTNG_NAMESPACE::String(json != nullptr ? json : ""), error, maxDepth, maxSize);
}

JsonDocument JsonDocument::fromJson(const string &json, JsonParseError *error, int maxDepth,
                                   uint32_t maxSize)
{
    return fromJson(::QTNG_NAMESPACE::String(json), error, maxDepth, maxSize);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

ostream &operator<<(ostream &os, const JsonValue &value)
{
    const string text = serialize(value, false);
    return os.write(text.data(), static_cast<streamsize>(text.size()));
}

ostream &operator<<(ostream &os, const JsonArray &array)
{
    return os << JsonValue(array);
}

ostream &operator<<(ostream &os, const JsonObject &object)
{
    return os << JsonValue(object);
}

ostream &operator<<(ostream &os, const JsonDocument &document)
{
    return os << document.value();
}

QTNG_NAMESPACE_END
