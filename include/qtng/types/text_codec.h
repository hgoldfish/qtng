#ifndef QTNG_TYPES_TEXT_CODEC_H
#define QTNG_TYPES_TEXT_CODEC_H

#include <ostream>

#include "qtng/config.h"
QTNG_NAMESPACE_BEGIN

class String;
class ByteArray;

enum class CodecError
{
    Strict,
    Ignore,
    Replace,
    BackslashReplace,
    XmlCharRefReplace,
};

enum class CodecStatus
{
    Ok = 0,
    Unavailable,
    UnknownEncoding,
    InvalidInput,
};

CodecStatus decode(String *out, const ByteArray &data, const char *encoding,
                   CodecError errors = CodecError::Strict);
CodecStatus encode(ByteArray *out, const String &text, const char *encoding,
                   CodecError errors = CodecError::Strict);

inline std::ostream &operator<<(std::ostream &os, CodecError error)
{
    switch (error) {
    case CodecError::Strict:
        return os << "Strict";
    case CodecError::Ignore:
        return os << "Ignore";
    case CodecError::Replace:
        return os << "Replace";
    case CodecError::BackslashReplace:
        return os << "BackslashReplace";
    case CodecError::XmlCharRefReplace:
        return os << "XmlCharRefReplace";
    default:
        return os << "Unknown";
    }
}

inline std::ostream &operator<<(std::ostream &os, CodecStatus status)
{
    switch (status) {
    case CodecStatus::Ok:
        return os << "Ok";
    case CodecStatus::Unavailable:
        return os << "Unavailable";
    case CodecStatus::UnknownEncoding:
        return os << "UnknownEncoding";
    case CodecStatus::InvalidInput:
        return os << "InvalidInput";
    default:
        return os << "Unknown";
    }
}

QTNG_NAMESPACE_END
#endif  // QTNG_TYPES_TEXT_CODEC_H
