#ifndef QTNG_ASN1ELEMENT_P_H
#define QTNG_ASN1ELEMENT_P_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API. It exists purely as an
// implementation detail. This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include "qtng/msgpack.h"
#include "qtng/utils/datetime.h"

namespace qtng {

#define RSA_ENCRYPTION_OID "1.2.840.113549.1.1.1"
#define DSA_ENCRYPTION_OID "1.2.840.10040.4.1"
#define EC_ENCRYPTION_OID "1.2.840.10045.2.1"

class QAsn1Element
{
public:
    enum ElementType {
        // universal
        BooleanType = 0x01,
        IntegerType = 0x02,
        BitStringType = 0x03,
        OctetStringType = 0x04,
        NullType = 0x05,
        ObjectIdentifierType = 0x06,
        Utf8StringType = 0x0c,
        PrintableStringType = 0x13,
        TeletexStringType = 0x14,
        UtcTimeType = 0x17,
        GeneralizedTimeType = 0x18,
        SequenceType = 0x30,
        SetType = 0x31,

        // GeneralNameTypes
        Rfc822NameType = 0x81,
        DnsNameType = 0x82,
        UniformResourceIdentifierType = 0x86,

        // context specific
        Context0Type = 0xA0,
        Context1Type = 0xA1,
        Context3Type = 0xA3
    };

    explicit QAsn1Element(std::uint8_t type = 0, const std::string &value = std::string());
    bool read(MsgPackStream &data);
    bool read(const std::string &data);
    void write(MsgPackStream &data) const;

    static QAsn1Element fromBool(bool val);
    static QAsn1Element fromInteger(unsigned int val);
    static QAsn1Element fromVector(const std::vector<QAsn1Element> &items);
    static QAsn1Element fromObjectId(const std::string &id);

    bool toBool(bool *ok = 0) const;
    qtng::utils::DateTime toDateTime() const;
    std::multimap<std::string, std::string> toInfo() const;
    std::int64_t toInteger(bool *ok = 0) const;
    std::vector<QAsn1Element> toVector() const;
    std::string toObjectId() const;
    std::string toObjectName() const;
    std::string toString() const;

    std::uint8_t type() const { return mType; }
    std::string value() const { return mValue; }

    friend inline bool operator==(const QAsn1Element &, const QAsn1Element &);
    friend inline bool operator!=(const QAsn1Element &, const QAsn1Element &);
private:
    std::uint8_t mType;
    std::string mValue;
};

inline bool operator==(const QAsn1Element &e1, const QAsn1Element &e2)
{
    return e1.mType == e2.mType && e1.mValue == e2.mValue;
}

inline bool operator!=(const QAsn1Element &e1, const QAsn1Element &e2)
{
    return e1.mType != e2.mType || e1.mValue != e2.mValue;
}

}  // namespace qtng

#endif  // QTNG_ASN1ELEMENT_P_H
