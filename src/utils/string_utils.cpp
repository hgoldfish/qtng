using namespace std;

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "qtng/utils/punycode.h"
#include "qtng/utils/string_utils.h"

namespace qtng {
namespace utils {

namespace {

// UTF-8 -> Unicode code points. Rejects overlong encodings and surrogate code points.
bool utf8ToCodepoints(const string &text, vector<uint32_t> &out)
{
    out.clear();
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp;
        size_t extra;
        if (c <= 0x7F) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (size_t j = 0; j < extra; ++j) {
            unsigned char cc = static_cast<unsigned char>(text[i + 1 + j]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        if (extra == 1 && cp < 0x80) {
            return false;
        }
        if (extra == 2 && cp < 0x800) {
            return false;
        }
        if (extra == 3 && cp < 0x10000) {
            return false;
        }
        out.push_back(cp);
        i += 1 + extra;
    }
    return true;
}

// Unicode code points -> UTF-8.
void codepointsToUtf8(const vector<uint32_t> &cps, string &out)
{
    out.clear();
    out.reserve(cps.size() * 4);
    for (uint32_t cp : cps) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
}

bool isAceLabel(const string &label)
{
    // "xn--" prefix detection (case-insensitive)
    if (label.size() < 4) {
        return false;
    }
    return (label[0] == 'x' || label[0] == 'X') && (label[1] == 'n' || label[1] == 'N')
            && label[2] == '-' && label[3] == '-';
}

}  // namespace

vector<string> split(const string &text, char separator)
{
    vector<string> parts;
    string part;
    for (char ch : text) {
        if (ch == separator) {
            parts.push_back(part);
            part.clear();
        } else {
            part.push_back(ch);
        }
    }
    parts.push_back(part);
    return parts;
}

vector<string> split(const string &text, const string &separator)
{
    vector<string> parts;
    if (separator.empty()) {
        parts.push_back(text);
        return parts;
    }
    size_t start = 0;
    while (true) {
        size_t pos = text.find(separator, start);
        if (pos == string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + separator.size();
    }
    return parts;
}

string trimmed(const string &text)
{
    size_t begin = 0;
    while (begin < text.size() && isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

string toLower(const string &text)
{
    string result = text;
    transform(result.begin(), result.end(), result.begin(),
              [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
    return result;
}

string toUpper(const string &text)
{
    string result = text;
    transform(result.begin(), result.end(), result.begin(),
              [](unsigned char ch) { return static_cast<char>(toupper(ch)); });
    return result;
}

bool startsWith(const string &text, const string &prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const string &text, const string &suffix)
{
    return text.size() >= suffix.size()
            && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

string number(int value)
{
    return to_string(value);
}

string number(long long value)
{
    return to_string(value);
}

string number(double value, int precision)
{
    ostringstream oss;
    oss.setf(ios::fixed, ios::floatfield);
    oss.precision(precision);
    oss << value;
    return oss.str();
}

int parseInt(const string &text, bool *ok)
{
    try {
        size_t consumed = 0;
        int value = stoi(text, &consumed);
        bool parsed = consumed == text.size();
        if (ok) {
            *ok = parsed;
        }
        return parsed ? value : 0;
    } catch (...) {
        if (ok) {
            *ok = false;
        }
        return 0;
    }
}

long long parseLongLong(const string &text, bool *ok)
{
    try {
        size_t consumed = 0;
        long long value = stoll(text, &consumed);
        bool parsed = consumed == text.size();
        if (ok) {
            *ok = parsed;
        }
        return parsed ? value : 0;
    } catch (...) {
        if (ok) {
            *ok = false;
        }
        return 0;
    }
}

float parseFloat(const string &text, bool *ok)
{
    try {
        size_t consumed = 0;
        float value = stof(text, &consumed);
        bool parsed = consumed == text.size();
        if (ok) {
            *ok = parsed;
        }
        return parsed ? value : 0.0f;
    } catch (...) {
        if (ok) {
            *ok = false;
        }
        return 0.0f;
    }
}

double parseDouble(const string &text, bool *ok)
{
    try {
        size_t consumed = 0;
        double value = stod(text, &consumed);
        bool parsed = consumed == text.size();
        if (ok) {
            *ok = parsed;
        }
        return parsed ? value : 0.0;
    } catch (...) {
        if (ok) {
            *ok = false;
        }
        return 0.0;
    }
}

string fromLatin1(const char *data, size_t size)
{
    if (!data || size == 0) {
        return string();
    }
    return string(data, size);
}

string join(const vector<string> &parts, const string &separator)
{
    if (parts.empty()) {
        return string();
    }
    ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            oss << separator;
        }
        oss << parts[i];
    }
    return oss.str();
}

// Minimal IDNA shell: split by '.', keep pure-ASCII labels as-is;
// Punycode-encode non-ASCII labels and prepend the "xn--" prefix.
// No Nameprep (NFKC normalization / case folding / Bidi / Joining checks) --
// callers must handle ASCII case normalization themselves. Returns empty on
// invalid UTF-8.
string toAce(const string &domain)
{
    if (domain.empty()) {
        return string();
    }
    vector<string> labels = split(domain, '.');
    string result;
    result.reserve(domain.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            result.push_back('.');
        }
        const string &label = labels[i];
        if (label.empty()) {
            continue;  // keep empty labels (e.g. trailing dot) as-is
        }
        bool pureAscii = true;
        for (char ch : label) {
            if (static_cast<unsigned char>(ch) >= 0x80) {
                pureAscii = false;
                break;
            }
        }
        if (pureAscii) {
            result += label;  // pass ASCII labels (including existing xn-- ACE) through
            continue;
        }
        vector<uint32_t> cps;
        if (!utf8ToCodepoints(label, cps)) {
            return string();  // invalid UTF-8
        }
        string encoded;
        if (!punycodeEncode(cps, encoded)) {
            return string();
        }
        result += "xn--";
        result += encoded;
    }
    return result;
}

// Minimal IDNA shell: split by '.', Punycode-decode labels with the "xn--"
// prefix, keep other labels as-is. Returns empty on invalid ACE.
string fromAce(const string &domain)
{
    if (domain.empty()) {
        return string();
    }
    vector<string> labels = split(domain, '.');
    string result;
    result.reserve(domain.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            result.push_back('.');
        }
        const string &label = labels[i];
        if (label.empty()) {
            continue;
        }
        if (!isAceLabel(label)) {
            result += label;
            continue;
        }
        vector<uint32_t> cps;
        if (!punycodeDecode(label.substr(4), cps)) {
            return string();  // invalid ACE
        }
        string utf8;
        codepointsToUtf8(cps, utf8);
        result += utf8;
    }
    return result;
}

string bytesToHex(const string &data)
{
    static const char hex[] = "0123456789abcdef";
    string result;
    result.reserve(data.size() * 2);
    for (unsigned char ch : data) {
        result.push_back(hex[ch >> 4]);
        result.push_back(hex[ch & 0x0f]);
    }
    return result;
}

string bytesToBase64(const string &data)
{
    static const char table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string result;
    result.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        unsigned int n = (static_cast<unsigned char>(data[i]) << 16)
                | (static_cast<unsigned char>(data[i + 1]) << 8)
                | static_cast<unsigned char>(data[i + 2]);
        result.push_back(table[(n >> 18) & 63]);
        result.push_back(table[(n >> 12) & 63]);
        result.push_back(table[(n >> 6) & 63]);
        result.push_back(table[n & 63]);
        i += 3;
    }
    if (i < data.size()) {
        unsigned int n = static_cast<unsigned char>(data[i]) << 16;
        if (i + 1 < data.size()) {
            n |= static_cast<unsigned char>(data[i + 1]) << 8;
        }
        result.push_back(table[(n >> 18) & 63]);
        result.push_back(table[(n >> 12) & 63]);
        if (i + 1 < data.size()) {
            result.push_back(table[(n >> 6) & 63]);
            result.push_back('=');
        } else {
            result.push_back('=');
            result.push_back('=');
        }
    }
    return result;
}

string join(const vector<string> &parts, char separator)
{
    return join(parts, string(1, separator));
}

bool equalsIgnoreCase(const string &a, const string &b)
{
    return toLower(a) == toLower(b);
}

string htmlEscape(const string &text)
{
    string result;
    result.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result.push_back(ch);
            break;
        }
    }
    return result;
}

string formatMessage(const string &pattern, const vector<string> &args)
{
    string result;
    result.reserve(pattern.size());
    size_t argIndex = 0;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.size()) {
            char marker = pattern[++i];
            if (marker >= '1' && marker <= '9') {
                size_t idx = static_cast<size_t>(marker - '1');
                if (idx < args.size()) {
                    result += args[idx];
                }
                continue;
            }
            if (marker == '%') {
                result.push_back('%');
                continue;
            }
            if ((marker == '1' || marker == 'd' || marker == 's') && argIndex < args.size()) {
                result += args[argIndex++];
                continue;
            }
            result.push_back('%');
            result.push_back(marker);
        } else {
            result.push_back(pattern[i]);
        }
    }
    return result;
}

}  // namespace utils
}  // namespace qtng
