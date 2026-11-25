#ifndef QTNG_TYPES_JSON_H
#define QTNG_TYPES_JSON_H

// QTNG_NAMESPACE::JsonValue / JsonArray / JsonObject / JsonDocument -- Qt-style JSON.
//
// The shape follows Qt's JSON classes so Qt-facing code ports mechanically:
// JsonValue is the variant, JsonArray/JsonObject are the containers, JsonDocument
// owns a parse result and renders it back to text. The one deliberate widening is
// that a document may hold any JSON value (Qt only allows an array or an object at
// the top level); isArray()/isObject() still answer only for those two types.
//
// Design notes:
//
//   1. **Immutable snapshot**. JsonValue holds a shared, never-mutated value
//      (shared_ptr to a private). Constructing one copies its input into that
//      snapshot, so `JsonObject o; JsonValue v(o); o.insert(...)` cannot change
//      what `v` sees. That is what makes JsonValue safe to copy, compare, read from
//      several threads, and store in the COW containers below.
//   2. **No exceptions**. Anything that can fail (wrong type, bad index, missing
//      key, parse error) returns a default value or an error struct.
//   3. **Numbers**. An integer literal that fits std::int64_t is stored as an
//      integer and printed exactly; everything else is a double. Parsing never
//      consults the process locale (strtod honours LC_NUMERIC, which can turn the
//      decimal point into ','), and a non-finite double serializes as null.
//   4. **Strict RFC 8259**. No comments, no trailing commas, no NaN/Infinity, no
//      unpaired UTF-16 surrogates, no BOM, no bare control characters in strings,
//      and only whitespace after the top-level value. Object keys are ordered and a
//      duplicate key keeps the last value (Qt's behavior).
//
// JSON text is text, so parsing takes a String (or a ByteArray, which is validated
// as UTF-8 first) and serialization returns a String. Both use byteSize()/data(),
// never String::size(): the latter counts code points and is O(n), which would make
// a large document quadratic.

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

#include "qtng/types/containers.h"
#include "qtng/types/string.h"

#include "qtng/config.h"
QTNG_NAMESPACE_BEGIN

class JsonArray;
class JsonObject;
class JsonValue;

namespace detail {
class JsonValuePrivate;
}  // namespace detail

// ---------------------------------------------------------------------------
// JsonParseError
// ---------------------------------------------------------------------------
//
// ParseError mirrors the subset of Qt's enum that this parser can report, plus
// DeepNesting and GarbageAtEnd for its own limits. The trigger conditions are
// documented in references.rst and locked in by tests; IllegalUTF8String covers the
// whole family of string-level violations (bad encoding, raw control characters,
// unpaired surrogates) because the enum has no finer value.
struct JsonParseError
{
    enum ParseError
    {
        NoError = 0,
        UnterminatedObject,
        MissingNameSeparator,
        UnterminatedArray,
        MissingValueSeparator,
        IllegalValue,
        TerminationByNumber,
        IllegalNumber,
        IllegalEscapeSequence,
        IllegalUTF8String,
        UnterminatedString,
        MissingObject,
        DeepNesting,
        DocumentTooLarge,
        GarbageAtEnd,
    };

    ParseError error;
    // Byte offset (0-based, in the String::byteSize() coordinate system) of the
    // offending byte, or -1 when the error has no meaningful position.
    std::int64_t offset;
    // Short English diagnostic, for logs only; do not match on it. error is the
    // stable machine-readable part.
    String message;

    JsonParseError()
        : error(NoError)
        , offset(-1)
    {
    }

    JsonParseError(ParseError e, std::int64_t off, const String &msg = String())
        : error(e)
        , offset(off)
        , message(msg)
    {
    }
};

// ---------------------------------------------------------------------------
// JsonValue
// ---------------------------------------------------------------------------
//
// Follows QJsonValue: implicit construction from the scalar types and from the two
// containers, and accessors that never throw. Type() names Double (not Number) and
// has no Undefined, matching Qt; isInteger() is an extension that tells an exact
// integer apart from a double.
class JsonValue
{
public:
    enum Type
    {
        Null,
        Bool,
        Double,
        String,
        Array,
        Object,
    };

    JsonValue();
    JsonValue(Type type);
    JsonValue(bool b);
    JsonValue(int i) { assignInteger(i); }
    JsonValue(unsigned int i) { assignInteger(i); }
    JsonValue(long i) { assignInteger(i); }
    JsonValue(unsigned long i) { assignInteger(i); }
    JsonValue(long long i) { assignInteger(i); }
    JsonValue(unsigned long long i) { assignIntegerUnsigned(i); }
    JsonValue(double d);
    JsonValue(const char *str);
    JsonValue(const ::QTNG_NAMESPACE::String &str);
    JsonValue(const JsonArray &array);
    JsonValue(const JsonObject &object);
    JsonValue(const JsonValue &other);
    JsonValue &operator=(const JsonValue &other);
    ~JsonValue();

    Type type() const;
    bool isNull() const { return type() == Null; }
    bool isBool() const { return type() == Bool; }
    bool isDouble() const { return type() == Double; }
    bool isString() const { return type() == String; }
    bool isArray() const { return type() == Array; }
    bool isObject() const { return type() == Object; }
    // True when the value was parsed or constructed from an exact 64-bit integer.
    bool isInteger() const;

    // All accessors return the supplied default, or a null value/container, when the
    // stored type does not match. None of them throws.
    bool toBool(bool defaultValue = false) const;
    double toDouble(double defaultValue = 0.0) const;
    // Exact 64-bit integer view. *ok (when non-null) is set to whether the stored
    // value could be represented exactly: an integer always can, a double only when
    // it is finite, integral and inside the int64 range.
    std::int64_t toInteger(bool *ok = nullptr) const;
    ::QTNG_NAMESPACE::String toString(const ::QTNG_NAMESPACE::String &defaultValue = ::QTNG_NAMESPACE::String()) const;
    // Two overloads rather than a default argument: JsonArray/JsonObject are defined
    // after JsonValue (they hold JsonValue), so a default-constructed argument cannot
    // appear in the declaration.
    JsonArray toArray() const;
    JsonArray toArray(const JsonArray &defaultValue) const;
    JsonObject toObject() const;
    JsonObject toObject(const JsonObject &defaultValue) const;

    // Object member access; a missing key or a non-object yields Null. The array
    // overload returns Null for an out-of-range index.
    JsonValue operator[](const ::QTNG_NAMESPACE::String &key) const;
    JsonValue operator[](int index) const;
    bool contains(const ::QTNG_NAMESPACE::String &key) const;

    bool operator==(const JsonValue &other) const;
    bool operator!=(const JsonValue &other) const { return !(*this == other); }

private:
    void assignInteger(long long value);
    void assignIntegerUnsigned(unsigned long long value);

    std::shared_ptr<const detail::JsonValuePrivate> d;
};

// ---------------------------------------------------------------------------
// JsonArray / JsonObject
// ---------------------------------------------------------------------------
//
// Thin subclasses of the COW containers so the object can offer a Qt-style const
// operator[]. Everything else -- initializer_list construction, size/at/append/
// insert/remove/value/keys, "not found" == npos -- is the container's, including the
// COW rule: const access never detaches, the first write detaches.
class JsonArray : public Vector<JsonValue>
{
    typedef Vector<JsonValue> Base;

public:
    using Base::Base;
};

class JsonObject : public Map<::QTNG_NAMESPACE::String, JsonValue>
{
    typedef Map<::QTNG_NAMESPACE::String, JsonValue> Base;

public:
    using Base::Base;

    // Read-only lookup; a missing key yields a null value instead of Qt's
    // undefined one. Base's non-const operator[] would insert a null member on a
    // mere read, so it is deliberately hidden: write through insert(key, value).
    JsonValue operator[](const ::QTNG_NAMESPACE::String &key) const { return value(key); }
};

// ---------------------------------------------------------------------------
// JsonDocument
// ---------------------------------------------------------------------------
//
// Holds one parsed or constructed value. fromJson() never throws: on failure it
// returns an empty document and fills *error.
class JsonDocument
{
public:
    enum JsonFormat
    {
        Indented,
        Compact,
    };

    // Refuse absurdly large inputs before allocating anything.
    enum
    {
        DefaultMaxDepth = 512,
        DefaultMaxDocumentSize = 64u * 1024u * 1024u,
    };

    JsonDocument();
    JsonDocument(const JsonValue &value);
    JsonDocument(const JsonObject &object);
    JsonDocument(const JsonArray &array);
    JsonDocument(const JsonDocument &other);
    JsonDocument &operator=(const JsonDocument &other);
    ~JsonDocument();

    bool isNull() const;
    // True when the document carries no value at all. Note that this is not Qt's
    // "no array and no object": a document holding a scalar is not empty here.
    bool isEmpty() const;
    bool isArray() const;
    bool isObject() const;

    JsonArray array() const;
    JsonObject object() const;
    JsonValue value() const;

    // Compact JSON with no insignificant whitespace, or 4-space indentation.
    ::QTNG_NAMESPACE::String toJson(JsonFormat format = Compact) const;

    // Strict RFC 8259 parse. The ByteArray overload validates the whole input as
    // UTF-8 first. maxDepth bounds nesting (DeepNesting past it); maxSize bounds
    // the input length (DocumentTooLarge past it).
    static JsonDocument fromJson(const ::QTNG_NAMESPACE::String &json, JsonParseError *error = nullptr,
                                 int maxDepth = DefaultMaxDepth,
                                 std::uint32_t maxSize = DefaultMaxDocumentSize);
    static JsonDocument fromJson(const ByteArray &json, JsonParseError *error = nullptr,
                                 int maxDepth = DefaultMaxDepth,
                                 std::uint32_t maxSize = DefaultMaxDocumentSize);
    // Literal / std::string entry points. Without these, `fromJson("[1]")` is ambiguous:
    // const char* converts implicitly to both String and ByteArray.
    static JsonDocument fromJson(const char *json, JsonParseError *error = nullptr,
                                 int maxDepth = DefaultMaxDepth,
                                 std::uint32_t maxSize = DefaultMaxDocumentSize);
    static JsonDocument fromJson(const std::string &json, JsonParseError *error = nullptr,
                                 int maxDepth = DefaultMaxDepth,
                                 std::uint32_t maxSize = DefaultMaxDocumentSize);

private:
    JsonValue m_value;
};

// Compact JSON on a plain std::ostream, so logging and tests can print a value.
std::ostream &operator<<(std::ostream &os, const JsonValue &value);
std::ostream &operator<<(std::ostream &os, const JsonArray &array);
std::ostream &operator<<(std::ostream &os, const JsonObject &object);
std::ostream &operator<<(std::ostream &os, const JsonDocument &document);

QTNG_NAMESPACE_END

#endif  // QTNG_TYPES_JSON_H
