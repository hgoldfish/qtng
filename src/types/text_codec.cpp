#include "qtng/types/text_codec.h"

#include "qtng/types/bytes.h"
#include "qtng/types/string.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <iconv.h>
#  include <langinfo.h>
#  include <locale.h>
#else
#  include <dlfcn.h>
#  include <langinfo.h>
#  include <locale.h>
#endif

using namespace std;

namespace qtng {

namespace {

void clearOut(String *out)
{
    if (out) {
        *out = String();
    }
}

void clearOut(ByteArray *out)
{
    if (out) {
        *out = ByteArray();
    }
}

string normalizeEncodingName(const char *encoding)
{
    if (!encoding || !encoding[0]) {
        return string();
    }
    string raw(encoding);
    string lower;
    lower.reserve(raw.size());
    for (char c : raw) {
        lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    }
    string key;
    key.reserve(lower.size());
    for (char c : lower) {
        if (c != '-' && c != '_' && c != ' ') {
            key.push_back(c);
        }
    }

    if (key.empty() || key == "local" || key == "locale" || key == "local8bit" || key == "ansi") {
        return string();
    }
    if (key == "utf8" || key == "utf8mb4") {
        return "UTF-8";
    }
    if (key == "latin1" || key == "iso88591" || key == "cp819" || key == "l1") {
        return "ISO-8859-1";
    }
    if (key == "gbk" || key == "cp936" || key == "windows936" || key == "ms936") {
        return "GBK";
    }
    if (key == "gb18030" || key == "gb180302000") {
        return "GB18030";
    }
    if (key == "gb2312" || key == "euccn") {
        return "GB2312";
    }
    return raw;
}

void appendUtf8Replacement(string &out)
{
    out.push_back(static_cast<char>(0xEF));
    out.push_back(static_cast<char>(0xBF));
    out.push_back(static_cast<char>(0xBD));
}

const char kHexDigits[] = "0123456789abcdef";

void appendHexByte(string &out, unsigned char b)
{
    out.push_back('\\');
    out.push_back('x');
    out.push_back(kHexDigits[b >> 4]);
    out.push_back(kHexDigits[b & 0xF]);
}

void appendUnicodeEscape(string &out, uint32_t cp)
{
    char buf[16];
    int n = 0;
    if (cp <= 0xFFFF) {
        buf[n++] = '\\';
        buf[n++] = 'u';
        for (int i = 12; i >= 0; i -= 4) {
            buf[n++] = kHexDigits[(cp >> i) & 0xF];
        }
    } else {
        buf[n++] = '\\';
        buf[n++] = 'U';
        for (int i = 28; i >= 0; i -= 4) {
            buf[n++] = kHexDigits[(cp >> i) & 0xF];
        }
    }
    out.append(buf, static_cast<size_t>(n));
}

void appendXmlCharRef(string &out, uint32_t cp)
{
    char buf[32];
    const int n = snprintf(buf, sizeof(buf), "&#%u;", static_cast<unsigned>(cp));
    if (n > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
}

bool applyDecodeError(CodecError errors, string &out, unsigned char badByte)
{
    switch (errors) {
    case CodecError::Strict:
        return false;
    case CodecError::Ignore:
        return true;
    case CodecError::Replace:
        appendUtf8Replacement(out);
        return true;
    case CodecError::BackslashReplace:
        appendHexByte(out, badByte);
        return true;
    case CodecError::XmlCharRefReplace:
        appendUtf8Replacement(out);
        return true;
    }
    return false;
}

bool applyEncodeError(CodecError errors, string &out, uint32_t cp)
{
    switch (errors) {
    case CodecError::Strict:
        return false;
    case CodecError::Ignore:
        return true;
    case CodecError::Replace:
        out.push_back('?');
        return true;
    case CodecError::BackslashReplace:
        appendUnicodeEscape(out, cp);
        return true;
    case CodecError::XmlCharRefReplace:
        appendXmlCharRef(out, cp);
        return true;
    }
    return false;
}

uint32_t utf8NextCodePoint(const char *p, size_t len, size_t *consumed)
{
    if (len == 0) {
        *consumed = 0;
        return 0;
    }
    uint32_t cp = 0;
    const uint32_t n = detail::utf8Decode(p, static_cast<uint32_t>(len), &cp);
    *consumed = n;
    return cp;
}

#if defined(_WIN32)

UINT encodingToCodePage(const string &normalized, bool *ok)
{
    *ok = true;
    if (normalized.empty()) {
        return CP_ACP;
    }
    if (normalized == "UTF-8") {
        return CP_UTF8;
    }
    if (normalized == "ISO-8859-1") {
        return 28591;
    }
    if (normalized == "GBK") {
        return 936;
    }
    if (normalized == "GB18030") {
        return 54936;
    }
    if (normalized == "GB2312") {
        return 20936;
    }
    *ok = false;
    return 0;
}

bool winSizeFitsInt(size_t size)
{
    return size <= static_cast<size_t>(INT_MAX);
}

CodecStatus winDecode(String *out, const char *data, size_t size, UINT cp, CodecError errors)
{
    if (size == 0) {
        *out = String();
        return CodecStatus::Ok;
    }
    if (!winSizeFitsInt(size)) {
        clearOut(out);
        return CodecStatus::InvalidInput;
    }

    if (errors == CodecError::Strict) {
        const int wlen = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, data, static_cast<int>(size), nullptr, 0);
        if (wlen <= 0) {
            clearOut(out);
            return CodecStatus::InvalidInput;
        }
        vector<wchar_t> wide(static_cast<size_t>(wlen));
        if (MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, data, static_cast<int>(size), wide.data(), wlen) <= 0) {
            clearOut(out);
            return CodecStatus::InvalidInput;
        }
        const int u8len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) {
            clearOut(out);
            return CodecStatus::InvalidInput;
        }
        string u8(static_cast<size_t>(u8len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, &u8[0], u8len, nullptr, nullptr);
        *out = String(u8);
        return CodecStatus::Ok;
    }

    string result;
    result.reserve(size);
    size_t i = 0;
    while (i < size) {
        // Guess a max sequence length, then shrink until MultiByteToWideChar accepts it.
        // Do not hard-code GBK ranges: CP_ACP on a Japanese machine is DBCS too.
        int tryLen = 1;
        const size_t remain = size - i;
        if (cp == CP_UTF8 || cp == 54936) {
            tryLen = static_cast<int>(remain > 4 ? 4 : remain);
        } else if (remain > 1 && IsDBCSLeadByteEx(cp, static_cast<BYTE>(data[i]))) {
            tryLen = 2;
        }
        bool converted = false;
        for (; tryLen >= 1; --tryLen) {
            wchar_t wbuf[4];
            const int wlen = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, data + i, tryLen, wbuf, 4);
            if (wlen > 0) {
                char u8buf[16];
                const int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, u8buf, 16, nullptr, nullptr);
                if (u8len > 0) {
                    result.append(u8buf, static_cast<size_t>(u8len));
                    i += static_cast<size_t>(tryLen);
                    converted = true;
                    break;
                }
            }
        }
        if (!converted) {
            if (!applyDecodeError(errors, result, static_cast<unsigned char>(data[i]))) {
                clearOut(out);
                return CodecStatus::InvalidInput;
            }
            ++i;
        }
    }
    *out = String(result);
    return CodecStatus::Ok;
}

CodecStatus winEncode(ByteArray *out, const char *utf8, size_t size, UINT cp, CodecError errors)
{
    if (size == 0) {
        *out = ByteArray();
        return CodecStatus::Ok;
    }
    if (!winSizeFitsInt(size)) {
        clearOut(out);
        return CodecStatus::InvalidInput;
    }

    if (errors == CodecError::Strict) {
        const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, static_cast<int>(size), nullptr, 0);
        if (wlen <= 0) {
            clearOut(out);
            return CodecStatus::InvalidInput;
        }
        vector<wchar_t> wide(static_cast<size_t>(wlen));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, static_cast<int>(size), wide.data(), wlen) <= 0) {
            clearOut(out);
            return CodecStatus::InvalidInput;
        }
        // Strict must not silently best-fit (e.g. fancy punctuation → ASCII).
        const DWORD flags = (cp == CP_UTF8) ? 0 : WC_NO_BEST_FIT_CHARS;
        BOOL usedDefault = FALSE;
        BOOL *usedDefaultPtr = (cp == CP_UTF8) ? nullptr : &usedDefault;
        const int mlen = WideCharToMultiByte(cp, flags, wide.data(), wlen, nullptr, 0, nullptr, usedDefaultPtr);
        if (mlen <= 0 || usedDefault) {
            clearOut(out);
            return CodecStatus::InvalidInput;
        }
        string mb(static_cast<size_t>(mlen), '\0');
        usedDefault = FALSE;
        WideCharToMultiByte(cp, flags, wide.data(), wlen, &mb[0], mlen, nullptr, usedDefaultPtr);
        if (usedDefault) {
            clearOut(out);
            return CodecStatus::InvalidInput;
        }
        *out = ByteArray(mb);
        return CodecStatus::Ok;
    }

    string result;
    size_t i = 0;
    while (i < size) {
        size_t consumed = 0;
        const uint32_t cpnt = utf8NextCodePoint(utf8 + i, size - i, &consumed);
        if (consumed == 0) {
            clearOut(out);
            return CodecStatus::InvalidInput;
        }
        char u8tmp[4];
        memcpy(u8tmp, utf8 + i, consumed);
        wchar_t wbuf[2];
        const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8tmp, static_cast<int>(consumed), wbuf, 2);
        if (wlen <= 0) {
            if (!applyEncodeError(errors, result, static_cast<unsigned char>(utf8[i]))) {
                clearOut(out);
                return CodecStatus::InvalidInput;
            }
            i += (consumed > 0) ? consumed : 1;
            continue;
        }
        BOOL usedDefault = FALSE;
        char mbuf[8];
        const int mlen = WideCharToMultiByte(cp, 0, wbuf, wlen, mbuf, 8, nullptr,
                                             (cp == CP_UTF8) ? nullptr : &usedDefault);
        if (mlen <= 0 || usedDefault) {
            if (!applyEncodeError(errors, result, cpnt)) {
                clearOut(out);
                return CodecStatus::InvalidInput;
            }
        } else {
            result.append(mbuf, static_cast<size_t>(mlen));
        }
        i += consumed;
    }
    *out = ByteArray(result);
    return CodecStatus::Ok;
}

#else

typedef void *IconvHandle;

struct IconvFns
{
    IconvHandle (*openFn)(const char *, const char *) = nullptr;
    size_t (*convFn)(IconvHandle, char **, size_t *, char **, size_t *) = nullptr;
    int (*closeFn)(IconvHandle) = nullptr;
    void *library = nullptr;
    bool available = false;
};

IconvFns &iconvFns()
{
    static IconvFns fns;
    return fns;
}

#if defined(__APPLE__)
IconvHandle appleIconvOpen(const char *toCode, const char *fromCode)
{
    return reinterpret_cast<IconvHandle>(iconv_open(toCode, fromCode));
}

size_t appleIconv(IconvHandle cd, char **inbuf, size_t *inLeft, char **outbuf, size_t *outLeft)
{
    return iconv(reinterpret_cast<iconv_t>(cd), inbuf, inLeft, outbuf, outLeft);
}

int appleIconvClose(IconvHandle cd)
{
    return iconv_close(reinterpret_cast<iconv_t>(cd));
}
#endif

bool resolveIconv()
{
    IconvFns &fns = iconvFns();
    static once_flag once;
    call_once(once, [&]() {
#if defined(__APPLE__)
        // Linked against the SDK libiconv; call through tiny wrappers so a possible
        // iconv() macro from <iconv.h> never has to decay to a function pointer.
        fns.openFn = appleIconvOpen;
        fns.convFn = appleIconv;
        fns.closeFn = appleIconvClose;
        fns.available = true;
#else
        auto trySyms = [&](void *handle) -> bool {
            void *o = dlsym(handle, "iconv_open");
            void *c = dlsym(handle, "iconv");
            void *x = dlsym(handle, "iconv_close");
            if (!o || !c || !x) {
                return false;
            }
            fns.openFn = reinterpret_cast<IconvHandle (*)(const char *, const char *)>(o);
            fns.convFn = reinterpret_cast<size_t (*)(IconvHandle, char **, size_t *, char **, size_t *)>(c);
            fns.closeFn = reinterpret_cast<int (*)(IconvHandle)>(x);
            return true;
        };

        if (trySyms(RTLD_DEFAULT)) {
            fns.available = true;
            return;
        }
        static const char *const kLibs[] = {
            "libc.so.6",
            "libc.so",
            "libiconv.so.2",
            "libiconv.so",
            nullptr,
        };
        for (int i = 0; kLibs[i]; ++i) {
            void *h = dlopen(kLibs[i], RTLD_LAZY | RTLD_LOCAL);
            if (!h) {
                continue;
            }
            if (trySyms(h)) {
                fns.library = h;
                fns.available = true;
                return;
            }
            dlclose(h);
        }
        fns.available = false;
#endif
    });
    return fns.available;
}

const char *iconvLocalCharset()
{
    static string name;
    static once_flag once;
    call_once(once, []() {
        // glibc iconv rejects the pseudo-name "char". Use CODESET, but do not clobber an
        // application that already installed a non-default LC_CTYPE.
        const char *cur = setlocale(LC_CTYPE, nullptr);
        if (!cur || strcmp(cur, "C") == 0 || strcmp(cur, "POSIX") == 0) {
            setlocale(LC_CTYPE, "");
        }
        const char *cs = nl_langinfo(CODESET);
        if (cs && cs[0]) {
            name = cs;
        }
        if (name.empty()) {
            name = "UTF-8";
        }
    });
    return name.c_str();
}

const char *iconvCharsetName(const string &normalized)
{
    if (normalized.empty()) {
        return iconvLocalCharset();
    }
    return normalized.c_str();
}

struct IconvGuard
{
    const IconvFns &fns;
    IconvHandle cd;

    IconvGuard(const IconvFns &f, IconvHandle handle)
        : fns(f)
        , cd(handle)
    {
    }
    ~IconvGuard()
    {
        if (cd != reinterpret_cast<IconvHandle>(static_cast<intptr_t>(-1))) {
            fns.closeFn(cd);
        }
    }
    IconvGuard(const IconvGuard &) = delete;
    IconvGuard &operator=(const IconvGuard &) = delete;
};

CodecStatus iconvConvert(string *outBytes, const char *in, size_t inSize, const char *toCode, const char *fromCode,
                         CodecError errors, bool decodingToUtf8)
{
    if (!resolveIconv()) {
        return CodecStatus::Unavailable;
    }
    const IconvFns &fns = iconvFns();
    IconvHandle cd = fns.openFn(toCode, fromCode);
    if (cd == reinterpret_cast<IconvHandle>(static_cast<intptr_t>(-1))) {
        return CodecStatus::UnknownEncoding;
    }
    IconvGuard guard(fns, cd);

    string out;
    out.reserve(inSize + inSize / 2 + 16);
    vector<char> outBuf(inSize > 0 ? (inSize * 4 + 64) : 64);

    char *inPtr = const_cast<char *>(in);
    size_t inLeft = inSize;

    auto resetState = [&]() {
        fns.convFn(cd, nullptr, nullptr, nullptr, nullptr);
    };

    while (inLeft > 0) {
        char *outPtr = outBuf.data();
        size_t outLeft = outBuf.size();
        const size_t n = fns.convFn(cd, &inPtr, &inLeft, &outPtr, &outLeft);
        const int savedErrno = errno;
        const size_t produced = outBuf.size() - outLeft;
        if (produced > 0) {
            out.append(outBuf.data(), produced);
        }
        if (n != static_cast<size_t>(-1)) {
            continue;
        }
        if (savedErrno == E2BIG) {
            outBuf.resize(outBuf.size() * 2);
            continue;
        }
        if (savedErrno != EILSEQ && savedErrno != EINVAL) {
            return CodecStatus::InvalidInput;
        }
        if (inLeft == 0) {
            break;
        }
        const unsigned char bad = static_cast<unsigned char>(*inPtr);
        if (decodingToUtf8) {
            if (!applyDecodeError(errors, out, bad)) {
                return CodecStatus::InvalidInput;
            }
        } else {
            size_t consumed = 0;
            const uint32_t cp = utf8NextCodePoint(inPtr, inLeft, &consumed);
            if (!applyEncodeError(errors, out, consumed ? cp : bad)) {
                return CodecStatus::InvalidInput;
            }
            if (consumed > 0 && consumed <= inLeft) {
                inPtr += consumed;
                inLeft -= consumed;
                resetState();
                continue;
            }
        }
        ++inPtr;
        --inLeft;
        resetState();
    }

    {
        char *outPtr = outBuf.data();
        size_t outLeft = outBuf.size();
        fns.convFn(cd, nullptr, nullptr, &outPtr, &outLeft);
        const size_t produced = outBuf.size() - outLeft;
        if (produced > 0) {
            out.append(outBuf.data(), produced);
        }
    }

    *outBytes = std::move(out);
    return CodecStatus::Ok;
}

#endif

}  // namespace

CodecStatus decode(String *out, const ByteArray &data, const char *encoding, CodecError errors)
{
    if (!out) {
        return CodecStatus::InvalidInput;
    }
    const string normalized = normalizeEncodingName(encoding);
#if defined(_WIN32)
    bool ok = false;
    const UINT cp = encodingToCodePage(normalized, &ok);
    if (!ok) {
        clearOut(out);
        return CodecStatus::UnknownEncoding;
    }
    return winDecode(out, data.data(), data.size(), cp, errors);
#else
    const char *fromCode = iconvCharsetName(normalized);
    string bytes;
    const CodecStatus st = iconvConvert(&bytes, data.data(), data.size(), "UTF-8", fromCode, errors, true);
    if (st != CodecStatus::Ok) {
        clearOut(out);
        return st;
    }
    *out = String(bytes);
    return CodecStatus::Ok;
#endif
}

CodecStatus encode(ByteArray *out, const String &text, const char *encoding, CodecError errors)
{
    if (!out) {
        return CodecStatus::InvalidInput;
    }
    const string normalized = normalizeEncodingName(encoding);
#if defined(_WIN32)
    bool ok = false;
    const UINT cp = encodingToCodePage(normalized, &ok);
    if (!ok) {
        clearOut(out);
        return CodecStatus::UnknownEncoding;
    }
    return winEncode(out, text.data(), text.byteSize(), cp, errors);
#else
    const char *toCode = iconvCharsetName(normalized);
    string bytes;
    const CodecStatus st =
            iconvConvert(&bytes, text.data(), text.byteSize(), toCode, "UTF-8", errors, false);
    if (st != CodecStatus::Ok) {
        clearOut(out);
        return st;
    }
    *out = ByteArray(bytes);
    return CodecStatus::Ok;
#endif
}

CodecStatus String::fromEncoded(String *out, const ByteArray &data, const char *encoding, CodecError errors)
{
    return decode(out, data, encoding, errors);
}

CodecStatus String::toEncoded(ByteArray *out, const char *encoding, CodecError errors) const
{
    return encode(out, *this, encoding, errors);
}

CodecStatus String::fromGbk(String *out, const ByteArray &data, CodecError errors)
{
    return decode(out, data, "GBK", errors);
}

CodecStatus String::toGbk(ByteArray *out, CodecError errors) const
{
    return encode(out, *this, "GBK", errors);
}

CodecStatus String::fromLatin1(String *out, const ByteArray &data, CodecError errors)
{
    return decode(out, data, "ISO-8859-1", errors);
}

CodecStatus String::toLatin1(ByteArray *out, CodecError errors) const
{
    return encode(out, *this, "ISO-8859-1", errors);
}

CodecStatus String::fromLocal8Bit(String *out, const ByteArray &data, CodecError errors)
{
    return decode(out, data, "", errors);
}

CodecStatus String::toLocal8Bit(ByteArray *out, CodecError errors) const
{
    return encode(out, *this, "", errors);
}

}  // namespace qtng
