#ifndef QTNG_TYPES_TEXT_CODEC_H
#define QTNG_TYPES_TEXT_CODEC_H

namespace qtng {

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

}  // namespace qtng

#endif  // QTNG_TYPES_TEXT_CODEC_H
