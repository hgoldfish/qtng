#include <cassert>
#include <limits>
#include <locale>
#include <map>
#include <string>
#include <vector>

#include "qtng/io_utils.h"
#include "qtng/utils/platform.h"
#include "qtng/private/qasn1element.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

typedef map<string, string> OidNameMap;
static OidNameMap createOidMap()
{
    OidNameMap oids;
    // used by unit tests
    oids.insert(oids.cend(), make_pair("0.9.2342.19200300.100.1.5", "favouriteDrink"));
    oids.insert(oids.cend(), make_pair("1.2.840.113549.1.9.1", "emailAddress"));
    oids.insert(oids.cend(), make_pair("1.3.6.1.5.5.7.1.1", "authorityInfoAccess"));
    oids.insert(oids.cend(), make_pair("1.3.6.1.5.5.7.48.1", "OCSP"));
    oids.insert(oids.cend(), make_pair("1.3.6.1.5.5.7.48.2", "caIssuers"));
    oids.insert(oids.cend(), make_pair("2.5.29.14", "subjectKeyIdentifier"));
    oids.insert(oids.cend(), make_pair("2.5.29.15", "keyUsage"));
    oids.insert(oids.cend(), make_pair("2.5.29.17", "subjectAltName"));
    oids.insert(oids.cend(), make_pair("2.5.29.19", "basicConstraints"));
    oids.insert(oids.cend(), make_pair("2.5.29.35", "authorityKeyIdentifier"));
    oids.insert(oids.cend(), make_pair("2.5.4.10", "O"));
    oids.insert(oids.cend(), make_pair("2.5.4.11", "OU"));
    oids.insert(oids.cend(), make_pair("2.5.4.12", "title"));
    oids.insert(oids.cend(), make_pair("2.5.4.13", "description"));
    oids.insert(oids.cend(), make_pair("2.5.4.17", "postalCode"));
    oids.insert(oids.cend(), make_pair("2.5.4.3", "CN"));
    oids.insert(oids.cend(), make_pair("2.5.4.4", "SN"));
    oids.insert(oids.cend(), make_pair("2.5.4.41", "name"));
    oids.insert(oids.cend(), make_pair("2.5.4.42", "GN"));
    oids.insert(oids.cend(), make_pair("2.5.4.43", "initials"));
    oids.insert(oids.cend(), make_pair("2.5.4.46", "dnQualifier"));
    oids.insert(oids.cend(), make_pair("2.5.4.5", "serialNumber"));
    oids.insert(oids.cend(), make_pair("2.5.4.6", "C"));
    oids.insert(oids.cend(), make_pair("2.5.4.7", "L"));
    oids.insert(oids.cend(), make_pair("2.5.4.8", "ST"));
    oids.insert(oids.cend(), make_pair("2.5.4.9", "street"));
    return oids;
}
NG_GLOBAL_STATIC_WITH_ARGS(OidNameMap, oidNameMap, (createOidMap()))

static bool stringToNonNegativeInt(const string &asnString, int *val)
{
    // Helper function for toDateTime(), which handles chunking of the original
    // string into smaller sub-components, so we expect the whole 'asnString' to
    // be a valid non-negative number.
    assert(val);

    // We want the C locale, as used by string; however, no leading sign is
    // allowed (which string would accept), so we have to check the data:
    const locale localeC;
    for (char v : asnString) {
        if (!isdigit(v, localeC))
            return false;
    }

    bool ok = false;
    int parsed = utils::parseInt(asnString, &ok);
    if (!ok) {
        return false;
    }
    *val = parsed;
    return true;
}

QAsn1Element::QAsn1Element(uint8_t type, const string &value)
    : mType(type)
    , mValue(value)
{
}

namespace {

// FileLike::read() is allowed to return a short count (0 < n < size) before a
// clean EOF, so loop until the whole buffer has been filled. A 0 return means
// EOF and a negative return is an error; both count as a failure here.
bool readFully(FileLike &stream, char *data, int32_t len)
{
    int32_t total = 0;
    while (total < len) {
        const int32_t bs = stream.read(data + total, len - total);
        if (bs <= 0)
            return false;
        total += bs;
    }
    return true;
}

}  // namespace

bool QAsn1Element::read(FileLike &stream)
{
    // DER is a raw TLV encoding and has nothing to do with MessagePack: the tag
    // and length bytes must be read verbatim. Going through a codec that
    // interprets the first byte as a type header would reject a long-form
    // length such as 0x82 (which looks like a fixmap) and thus break every
    // certificate larger than 127 bytes.
    uint8_t tmpType = 0;
    if (!readFully(stream, reinterpret_cast<char *>(&tmpType), 1))
        return false;
    if (!tmpType)
        return false;

    // length
    uint32_t length = 0;
    uint8_t first = 0;
    if (!readFully(stream, reinterpret_cast<char *>(&first), 1))
        return false;

    if (first & 0x80) {
        // long form: 0x80 | number of length bytes
        const uint8_t bytes = (first & 0x7f);
        // Lengths that do not fit int32_t are rejected up front; accepting more
        // bytes here would overflow the counter and silently wrap to a small
        // (or negative) length.
        if (bytes > 4)
            return false;

        for (uint8_t i = 0; i < bytes; i++) {
            uint8_t b = 0;
            if (!readFully(stream, reinterpret_cast<char *>(&b), 1))
                return false;
            length = (length << 8) | b;
        }
    } else {
        // short form
        length = (first & 0x7f);
    }

    // A length above INT32_MAX cannot be represented by the reads below and,
    // if stored in an int32_t, would look like a negative value and be
    // mistaken for an empty one.
    if (length > static_cast<uint32_t>(numeric_limits<int32_t>::max()))
        return false;

    // value
    string tmpValue;
    if (length > 0) {
        tmpValue.resize(length);
        if (!readFully(stream, &tmpValue[0], static_cast<int32_t>(length)))
            return false;
    }

    mType = tmpType;
    mValue.swap(tmpValue);
    return true;
}

bool QAsn1Element::read(const string &data)
{
    BytesIO stream(data);
    return read(stream);
}

void QAsn1Element::write(FileLike &stream) const
{
    // DER is a raw TLV encoding: write the tag, length and value bytes
    // verbatim, without any MessagePack framing.
    const uint8_t type = mType;
    stream.write(reinterpret_cast<const char *>(&type), 1);

    // length
    int64_t length = static_cast<int64_t>(mValue.size());
    if (length >= 128) {
        // long form
        uint8_t encodedLength = 0x80;
        string ba;
        while (length) {
            ba.insert(0, 1, static_cast<char>(static_cast<uint8_t>(length & 0xff)));
            length >>= 8;
            encodedLength += 1;
        }
        stream.write(reinterpret_cast<const char *>(&encodedLength), 1);
        stream.write(ba.data(), static_cast<int32_t>(ba.size()));
    } else {
        // short form
        const uint8_t shortLen = static_cast<uint8_t>(length);
        stream.write(reinterpret_cast<const char *>(&shortLen), 1);
    }

    // value
    if (!mValue.empty()) {
        stream.write(mValue.data(), static_cast<int32_t>(mValue.size()));
    }
}

QAsn1Element QAsn1Element::fromBool(bool val)
{
    const char negOne = numeric_limits<char>::is_signed ? -1 : 0xff;
    return QAsn1Element(QAsn1Element::BooleanType, string(1, val ? negOne : 0x00));
}

QAsn1Element QAsn1Element::fromInteger(unsigned int val)
{
    const char negOne = numeric_limits<char>::is_signed ? -1 : 0xff;
    QAsn1Element elem(QAsn1Element::IntegerType);
    while (val > 127) {
        elem.mValue.insert(0, 1, static_cast<char>(val & negOne));
        val >>= 8;
    }
    elem.mValue.insert(0, 1, static_cast<char>(val & 0x7f));
    return elem;
}

QAsn1Element QAsn1Element::fromVector(const vector<QAsn1Element> &items)
{
    QAsn1Element seq;
    seq.mType = SequenceType;
    string buffer;
    BytesIO stream(&buffer);
    for (vector<QAsn1Element>::const_iterator it = items.cbegin(), end = items.cend(); it != end; ++it)
        it->write(stream);
    seq.mValue = std::move(buffer);
    return seq;
}

QAsn1Element QAsn1Element::fromObjectId(const string &id)
{
    QAsn1Element elem;
    elem.mType = ObjectIdentifierType;
    const vector<string> bits = utils::split(id, '.');
    assert(bits.size() > 2);
    elem.mValue += static_cast<char>(stoul(bits[0]) * 40 + stoul(bits[1]));
    for (int i = 2; i < static_cast<int>(bits.size()); ++i) {
        char buffer[numeric_limits<unsigned int>::digits / 7 + 2];
        char *pBuffer = buffer + sizeof(buffer);
        *--pBuffer = '\0';
        unsigned int node = static_cast<unsigned int>(stoul(bits[i]));
        *--pBuffer = static_cast<char>(node & 0x7f);
        node >>= 7;
        while (node) {
            *--pBuffer = static_cast<char>((node & 0x7f) | 0x80);
            node >>= 7;
        }
        elem.mValue += pBuffer;
    }
    return elem;
}

bool QAsn1Element::toBool(bool *ok) const
{
    if (*this == fromBool(true)) {
        if (ok)
            *ok = true;
        return true;
    } else if (*this == fromBool(false)) {
        if (ok)
            *ok = true;
        return false;
    } else {
        if (ok)
            *ok = false;
        return false;
    }
}

utils::DateTime QAsn1Element::toDateTime() const
{
    if (utils::endsWith(mValue, "Z")) {
        if (mType == UtcTimeType && mValue.size() == 13) {
            int year = 0;
            if (!stringToNonNegativeInt(mValue.substr(0, 2), &year))
                return utils::DateTime();
            year = year < 50 ? 2000 + year : 1900 + year;
            return utils::DateTime::fromUtc(year,
                    stoi(mValue.substr(2, 2)),
                    stoi(mValue.substr(4, 2)),
                    stoi(mValue.substr(6, 2)),
                    stoi(mValue.substr(8, 2)),
                    stoi(mValue.substr(10, 2)));
        } else if (mType == GeneralizedTimeType && mValue.size() == 15) {
            return utils::DateTime::fromUtc(
                    stoi(mValue.substr(0, 4)),
                    stoi(mValue.substr(4, 2)),
                    stoi(mValue.substr(6, 2)),
                    stoi(mValue.substr(8, 2)),
                    stoi(mValue.substr(10, 2)),
                    stoi(mValue.substr(12, 2)));
        }
    }
    return utils::DateTime();
}

multimap<string, string> QAsn1Element::toInfo() const
{
    multimap<string, string> info;
    QAsn1Element elem;
    BytesIO issuerStream(mValue);
    while (elem.read(issuerStream) && elem.mType == QAsn1Element::SetType) {
        QAsn1Element issuerElem;
        BytesIO setStream(elem.mValue);
        if (issuerElem.read(setStream) && issuerElem.mType == QAsn1Element::SequenceType) {
            vector<QAsn1Element> elems = issuerElem.toVector();
            if (elems.size() == 2) {
                const string key = elems.front().toObjectName();
                if (!key.empty())
                    info.emplace(key, elems.back().toString());
            }
        }
    }
    return info;
}

int64_t QAsn1Element::toInteger(bool *ok) const
{
    if (mType != QAsn1Element::IntegerType || mValue.empty()) {
        if (ok)
            *ok = false;
        return 0;
    }

    // NOTE: negative numbers are not handled
    if (mValue[0] & 0x80) {
        if (ok)
            *ok = false;
        return 0;
    }

    int64_t value = mValue[0] & 0x7f;
    for (int i = 1; i < mValue.size(); ++i)
        value = (value << 8) | static_cast<uint8_t>(mValue[i]);

    if (ok)
        *ok = true;
    return value;
}

vector<QAsn1Element> QAsn1Element::toVector() const
{
    vector<QAsn1Element> items;
    if (mType == SequenceType) {
        QAsn1Element elem;
        BytesIO stream(mValue);
        while (elem.read(stream))
            items.push_back(elem);
    }
    return items;
}

string QAsn1Element::toObjectId() const
{
    string key;
    if (mType == ObjectIdentifierType && !mValue.empty()) {
        uint8_t b = mValue[0];
        key += to_string(b / 40) + '.' + to_string(b % 40);
        unsigned int val = 0;
        for (int i = 1; i < static_cast<int>(mValue.size()); ++i) {
            b = static_cast<uint8_t>(mValue[i]);
            val = (val << 7) | (b & 0x7f);
            if (!(b & 0x80)) {
                key += '.' + to_string(val);
                val = 0;
            }
        }
    }
    return key;
}

string QAsn1Element::toObjectName() const
{
    string key = toObjectId();
    const OidNameMap &map = oidNameMap();
    OidNameMap::const_iterator it = map.find(key);
    return it != map.end() ? it->second : key;
}

string QAsn1Element::toString() const
{
    // Detect embedded NULs and reject
    if (mValue.find('\0') != string::npos)
        return string();

    if (mType == PrintableStringType || mType == TeletexStringType || mType == Rfc822NameType || mType == DnsNameType
        || mType == UniformResourceIdentifierType)
        return mValue;
    if (mType == Utf8StringType)
        return mValue;

    return string();
}

}  // namespace qtng
