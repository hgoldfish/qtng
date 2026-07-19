#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/asn1.h>
#include "qtng/certificate.h"
#include "qtng/private/qasn1element.h"
#include "qtng/private/crypto_p.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.certificate");

namespace qtng {

static uint qHashBits(const void *ptr, size_t len, uint seed)
{
    auto p = static_cast<const unsigned char *>(ptr);
    for (size_t i = 0; i < len; ++i) {
        seed ^= static_cast<uint>(p[i]) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
}

template<typename K, typename V>
static vector<V> multimapValues(const multimap<K, V> &map, const K &key)
{
    vector<V> values;
    const auto range = map.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        values.push_back(it->second);
    }
    return values;
}

template<typename K, typename V>
static vector<K> multimapUniqueKeys(const multimap<K, V> &map)
{
    vector<K> keys;
    for (const auto &entry : map) {
        if (keys.empty() || keys.back() != entry.first) {
            keys.push_back(entry.first);
        }
    }
    return keys;
}

static bool containsValue(const vector<string> &values, const string &value)
{
    return find(values.begin(), values.end(), value) != values.end();
}

static utils::DateTime getTimeFromASN1(const ASN1_TIME *aTime)
{
    size_t lTimeLength = static_cast<size_t>(aTime->length);
    char *pString = reinterpret_cast<char *>(aTime->data);

    if (aTime->type == V_ASN1_UTCTIME) {

        char lBuffer[24];
        char *pBuffer = lBuffer;

        if ((lTimeLength < 11) || (lTimeLength > 17))
            return utils::DateTime();

        memcpy(pBuffer, pString, 10);
        pBuffer += 10;
        pString += 10;

        int lSecondsFromUCT = 0;
        if (*pString == '-' || *pString == '+') {
            char sign = *(pString++);
            int nhh = 0;
            int nmm = 0;

            if (sscanf(pString, "%2d%2d", &nhh, &nmm) != 2)
                return utils::DateTime();
            lSecondsFromUCT = (nhh * 60 + nmm) * 60;
            if (sign == '-')
                lSecondsFromUCT = -lSecondsFromUCT;
        }

        int year = ((lBuffer[0] - '0') * 10) + (lBuffer[1] - '0');
        if (year < 50)
            year += 2000;
        else
            year += 1900;
        const int month = (((lBuffer[2] - '0') * 10) + (lBuffer[3] - '0'));
        const int day = ((lBuffer[4] - '0') * 10) + (lBuffer[5] - '0');
        const int hour = ((lBuffer[6] - '0') * 10) + (lBuffer[7] - '0');
        const int minute = ((lBuffer[8] - '0') * 10) + (lBuffer[9] - '0');
        const int second = ((lBuffer[10] - '0') * 10) + (lBuffer[11] - '0');

        utils::DateTime result = utils::DateTime::fromUtc(year, month, day, hour, minute, second);
        return result.addSecs(lSecondsFromUCT);

    } else if (aTime->type == V_ASN1_GENERALIZEDTIME) {

        if (lTimeLength < 15)
            return utils::DateTime();

        const int year = ((pString[0] - '0') * 1000) + ((pString[1] - '0') * 100) + ((pString[2] - '0') * 10)
                + (pString[3] - '0');
        const int month = (((pString[4] - '0') * 10) + (pString[5] - '0'));
        const int day = ((pString[6] - '0') * 10) + (pString[7] - '0');
        const int hour = ((pString[8] - '0') * 10) + (pString[9] - '0');
        const int minute = ((pString[10] - '0') * 10) + (pString[11] - '0');
        const int second = ((pString[12] - '0') * 10) + (pString[13] - '0');

        return utils::DateTime::fromUtc(year, month, day, hour, minute, second);

    } else {
        ngWarning() << "unsupported date format detected";
        return utils::DateTime();
    }
}

struct X509Cleaner
{
    void operator()(X509 *x) const
    {
        if (x)
            X509_free(x);
    }
};

class CertificatePrivate 
{
public:
    CertificatePrivate() { }
    CertificatePrivate(const CertificatePrivate &) = default;

    bool isNull() const;
    utils::DateTime effectiveDate() const;
    utils::DateTime expiryDate() const;
    bool isBlacklisted() const;
    PublicKey publicKey() const;
    string serialNumber() const;
    vector<string> subjectInfo(Certificate::SubjectInfo subjec) const;
    vector<string> subjectInfo(const string &attribute) const;
    vector<string> subjectInfoAttributes() const;
    string toString() const;
    string version() const;

    bool isSelfSigned() const;
    vector<string> issuerInfo(Certificate::SubjectInfo subject) const;
    vector<string> issuerInfo(const string &attribute) const;
    vector<string> issuerInfoAttributes() const;

    string save(Ssl::EncodingFormat format) const;
    static Certificate load(const string &data, Ssl::EncodingFormat format);
    static Certificate generate(const PublicKey &publickey, const PrivateKey &caKey, MessageDigest::Algorithm signAlgo,
                                long serialNumber, const utils::DateTime &effectiveDate, const utils::DateTime &expiryDate,
                                const multimap<Certificate::SubjectInfo, string> &subjectInfoes);
    static string subjectInfoToString(Certificate::SubjectInfo info);
    static inline bool setX509(Certificate *cert, X509 *x509);

    bool init(X509 *x509);
    bool qtParse();

    shared_ptr<X509> x509;
    multimap<string, string> issuerInfoMap;
    multimap<string, string> subjectInfoMap;
    string versionString;
    utils::DateTime notValidBefore;
    utils::DateTime notValidAfter;
};

static string asn1ObjectId(ASN1_OBJECT *object)
{
    char buf[80];  // The openssl docs a buffer length of 80 should be more than enough
    OBJ_obj2txt(buf, sizeof(buf), object, 1);  // the 1 says always use the oid not the long name

    return string(buf);
}

static string asn1ObjectName(ASN1_OBJECT *object)
{
    int nid = OBJ_obj2nid(object);
    if (nid != NID_undef)
        return string(OBJ_nid2sn(nid));

    return asn1ObjectId(object);
}

static multimap<string, string> _mapFromX509Name(X509_NAME *name)
{
    multimap<string, string> info;
    for (int i = 0; i < X509_NAME_entry_count(name); ++i) {
        X509_NAME_ENTRY *e = X509_NAME_get_entry(name, i);

        string name = asn1ObjectName(X509_NAME_ENTRY_get_object(e));
        unsigned char *data = nullptr;
        int size = ASN1_STRING_to_UTF8(&data, X509_NAME_ENTRY_get_data(e));
        info.insert({name, string(reinterpret_cast<char *>(data), static_cast<size_t>(size))});
#if LIBRESSL_VERSION_NUMBER >= 0x3090000fL
        CRYPTO_free(data, __FILE__, __LINE__);
#elif defined(LIBRESSL_VERSION_NUMBER)
        CRYPTO_free(data);
#else
        CRYPTO_free(data, __FILE__, __LINE__);
#endif
    }

    return info;
}

bool CertificatePrivate::init(X509 *x)
{
    if (!x)
        return false;
    this->x509.reset(x, X509_free);

    int parsed = 0;  // 0 for never parsed, -1 for failed, and 1 for success.
    ASN1_TIME *t = X509_getm_notBefore(x);
    if (t) {
        notValidBefore = getTimeFromASN1(t);
    } else {
        parsed = qtParse() ? 1 : -1;
    }
    t = X509_getm_notAfter(x);
    if (t) {
        notValidAfter = getTimeFromASN1(t);
    } else if (parsed == 0) {
        parsed = qtParse() ? 1 : -1;
    }
    int64_t version = int64_t(X509_get_version(x));
    if (version >= 0) {
        versionString = utils::number(static_cast<long long>(version));
    } else if (parsed == 0) {
        parsed = qtParse() ? 1 : -1;
    }

    issuerInfoMap = _mapFromX509Name(X509_get_issuer_name(x));
    subjectInfoMap = _mapFromX509Name(X509_get_subject_name(x));
    return parsed != -1;
}

bool CertificatePrivate::setX509(Certificate *cert, X509 *x)
{
    if (!x || !cert)
        return false;
    return cert->d->init(X509_dup(x));
}

bool openssl_setCertificate(Certificate *cert, X509 *x509)
{
    return CertificatePrivate::setX509(cert, x509);
}

bool CertificatePrivate::isNull() const
{
    return x509 == nullptr;
}

utils::DateTime CertificatePrivate::effectiveDate() const
{
    return notValidBefore;
}

utils::DateTime CertificatePrivate::expiryDate() const
{
    return notValidAfter;
}

// These certificates are known to be fraudulent and were created during the comodo
// compromise. See http://www.comodo.com/Comodo-Fraud-Incident-2011-03-23.html
static const char * const certificate_blacklist[] = {
    "04:7e:cb:e9:fc:a5:5f:7b:d0:9e:ae:36:e1:0c:ae:1e", "mail.google.com",  // Comodo
    "f5:c8:6a:f3:61:62:f1:3a:64:f5:4f:6d:c9:58:7c:06", "www.google.com",  // Comodo
    "d7:55:8f:da:f5:f1:10:5b:b2:13:28:2b:70:77:29:a3", "login.yahoo.com",  // Comodo
    "39:2a:43:4f:0e:07:df:1f:8a:a3:05:de:34:e0:c2:29", "login.yahoo.com",  // Comodo
    "3e:75:ce:d4:6b:69:30:21:21:88:30:ae:86:a8:2a:71", "login.yahoo.com",  // Comodo
    "e9:02:8b:95:78:e4:15:dc:1a:71:0a:2b:88:15:44:47", "login.skype.com",  // Comodo
    "92:39:d5:34:8f:40:d1:69:5a:74:54:70:e1:f2:3f:43", "addons.mozilla.org",  // Comodo
    "b0:b7:13:3e:d0:96:f9:b5:6f:ae:91:c8:74:bd:3a:c0", "login.live.com",  // Comodo
    "d8:f3:5f:4e:b7:87:2b:2d:ab:06:92:e3:15:38:2f:b0", "global trustee",  // Comodo

    "05:e2:e6:a4:cd:09:ea:54:d6:65:b0:75:fe:22:a2:56", "*.google.com",  // leaf certificate issued by DigiNotar
    "0c:76:da:9c:91:0c:4e:2c:9e:fe:15:d0:58:93:3c:4c", "DigiNotar Root CA",  // DigiNotar root
    "f1:4a:13:f4:87:2b:56:dc:39:df:84:ca:7a:a1:06:49",
    "DigiNotar Services CA",  // DigiNotar intermediate signed by DigiNotar Root
    "36:16:71:55:43:42:1b:9d:e6:cb:a3:64:41:df:24:38",
    "DigiNotar Services 1024 CA",  // DigiNotar intermediate signed by DigiNotar Root
    "0a:82:bd:1e:14:4e:88:14:d7:5b:1a:55:27:be:bf:3e", "DigiNotar Root CA G2",  // other DigiNotar Root CA
    "a4:b6:ce:e3:2e:d3:35:46:26:3c:b3:55:3a:a8:92:21",
    "CertiID Enterprise Certificate Authority",  // DigiNotar intermediate signed by "DigiNotar Root CA G2"
    "5b:d5:60:9c:64:17:68:cf:21:0e:35:fd:fb:05:ad:41",
    "DigiNotar Qualified CA",  // DigiNotar intermediate signed by DigiNotar Root

    "46:9c:2c:b0", "DigiNotar Services 1024 CA",  // DigiNotar intermediate cross-signed by Entrust
    "07:27:10:0d", "DigiNotar Cyber CA",  // DigiNotar intermediate cross-signed by CyberTrust
    "07:27:0f:f9", "DigiNotar Cyber CA",  // DigiNotar intermediate cross-signed by CyberTrust
    "07:27:10:03", "DigiNotar Cyber CA",  // DigiNotar intermediate cross-signed by CyberTrust
    "01:31:69:b0",
    "DigiNotar PKIoverheid CA Overheid en Bedrijven",  // DigiNotar intermediate cross-signed by the Dutch government
    "01:31:34:bf",
    "DigiNotar PKIoverheid CA Organisatie - G2",  // DigiNotar intermediate cross-signed by the Dutch government
    "d6:d0:29:77:f1:49:fd:1a:83:f2:b9:ea:94:8c:5c:b4",
    "DigiNotar Extended Validation CA",  // DigiNotar intermediate signed by DigiNotar EV Root
    "1e:7d:7a:53:3d:45:30:41:96:40:0f:71:48:1f:45:04", "DigiNotar Public CA 2025",  // DigiNotar intermediate
    //    "(has not been seen in the wild so far)", "DigiNotar Public CA - G2", // DigiNotar intermediate
    //    "(has not been seen in the wild so far)", "Koninklijke Notariele Beroepsorganisatie CA", // compromised during
    //    DigiNotar breach
    //    "(has not been seen in the wild so far)", "Stichting TTP Infos CA," // compromised during DigiNotar breach
    "46:9c:2c:af", "DigiNotar Root CA",  // DigiNotar intermediate cross-signed by Entrust
    "46:9c:3c:c9", "DigiNotar Root CA",  // DigiNotar intermediate cross-signed by Entrust

    "07:27:14:a9", "Digisign Server ID (Enrich)",  // (Malaysian) Digicert Sdn. Bhd. cross-signed by Verizon CyberTrust
    "4c:0e:63:6a", "Digisign Server ID - (Enrich)",  // (Malaysian) Digicert Sdn. Bhd. cross-signed by Entrust
    "72:03:21:05:c5:0c:08:57:3d:8e:a5:30:4e:fe:e8:b0", "UTN-USERFirst-Hardware",  // comodogate test certificate
    "41", "MD5 Collisions Inc. (http://www.phreedom.org/md5)",  // http://www.phreedom.org/research/rogue-ca/

    "08:27", "*.EGO.GOV.TR",  // Turktrust mis-issued intermediate certificate
    "08:64", "e-islem.kktcmerkezbankasi.org",  // Turktrust mis-issued intermediate certificate

    "03:1d:a7",
    "AC DG Tr\xC3\xA9sor SSL",  // intermediate certificate linking back to ANSSI French National Security Agency
    "27:83", "NIC Certifying Authority",  // intermediate certificate from NIC India (2007)
    "27:92", "NIC CA 2011",  // intermediate certificate from NIC India (2011)
    "27:b1", "NIC CA 2014",  // intermediate certificate from NIC India (2014)
    nullptr
};

bool CertificatePrivate::isBlacklisted() const
{
    if (!x509)
        return false;
    for (int a = 0; certificate_blacklist[a] != nullptr; a++) {
        string blacklistedCommonName = certificate_blacklist[a + 1];
        if (serialNumber() == certificate_blacklist[a++]
            && (containsValue(subjectInfo(Certificate::CommonName), blacklistedCommonName)
                || containsValue(issuerInfo(Certificate::CommonName), blacklistedCommonName)))
            return true;
    }
    return false;
}

PublicKey CertificatePrivate::publicKey() const
{
    PublicKey key;
    if (x509) {
        EVP_PKEY *pkey = X509_get_pubkey(x509.get());
        if (pkey) {
            openssl_setPkey(&key, pkey, false);
        }
    }
    return key;
}

string CertificatePrivate::serialNumber() const
{
    if (!x509) {
        return string();
    }

    ASN1_INTEGER *serialNumber = X509_get_serialNumber(x509.get());
    if (!serialNumber) {
        return string();
    }

    long value = -1;
    if (serialNumber->length <= static_cast<int>(sizeof(long))
        && serialNumber->type == V_ASN1_INTEGER) {
        uint64_t u64 = 0;
        if (ASN1_INTEGER_get_uint64(&u64, serialNumber) && u64 <= LONG_MAX)
            value = static_cast<long>(u64);
    }

    if (value >= 0) {
        return utils::number(static_cast<long long>(value));
    }

    string result;
    if (serialNumber->type == V_ASN1_NEG_INTEGER) {
        result.append("(Negative) ");
    }

    for (int i = 0; i < serialNumber->length; ++i) {
        if (i > 0)
            result.push_back(':');
        const unsigned int byteValue = static_cast<unsigned int>(serialNumber->data[i]);
        char hex[3] = "00";
        hex[0] = "0123456789abcdef"[byteValue >> 4];
        hex[1] = "0123456789abcdef"[byteValue & 0xf];
        result.append(hex, 2);
    }
    return result;
}

vector<string> CertificatePrivate::subjectInfo(Certificate::SubjectInfo subject) const
{
    return multimapValues(subjectInfoMap, subjectInfoToString(subject));
}

vector<string> CertificatePrivate::subjectInfo(const string &attribute) const
{
    return multimapValues(subjectInfoMap, attribute);
}

vector<string> CertificatePrivate::subjectInfoAttributes() const
{
    return multimapUniqueKeys(subjectInfoMap);
}

string CertificatePrivate::version() const
{
    return versionString;
}

bool CertificatePrivate::isSelfSigned() const
{
    if (!x509)
        return false;
    return (X509_check_issued(x509.get(), x509.get()) == X509_V_OK);
}

string CertificatePrivate::subjectInfoToString(Certificate::SubjectInfo info)
{
    string str;
    switch (info) {
    case Certificate::Organization:
        str = string("O");
        break;
    case Certificate::CommonName:
        str = string("CN");
        break;
    case Certificate::LocalityName:
        str = string("L");
        break;
    case Certificate::OrganizationalUnitName:
        str = string("OU");
        break;
    case Certificate::CountryName:
        str = string("C");
        break;
    case Certificate::StateOrProvinceName:
        str = string("ST");
        break;
    case Certificate::DistinguishedNameQualifier:
        str = string("dnQualifier");
        break;
    case Certificate::SerialNumber:
        str = string("serialNumber");
        break;
    case Certificate::EmailAddress:
        str = string("emailAddress");
        break;
    }
    return str;
}

vector<string> CertificatePrivate::issuerInfo(Certificate::SubjectInfo subject) const
{
    if (!x509)
        return vector<string>();
    return multimapValues(issuerInfoMap, subjectInfoToString(subject));
}

vector<string> CertificatePrivate::issuerInfo(const string &attribute) const
{
    if (!x509)
        return vector<string>();
    return multimapValues(issuerInfoMap, attribute);
}

vector<string> CertificatePrivate::issuerInfoAttributes() const
{
    if (!x509)
        return vector<string>();
    return multimapUniqueKeys(issuerInfoMap);
}

struct BioCleaner
{
    void operator()(BIO *o) const
    {
        if (o)
            BIO_free(o);
    }
};

string CertificatePrivate::toString() const
{
    if (!x509) {
        return string();
    }
    string result(1024 * 64, '\0');
    unique_ptr<BIO, BioCleaner> bio(BIO_new(BIO_s_mem()));
    if (!bio)
        return string();

    // FIXME I have got nothing.
    X509_print(bio.get(), x509.get());

    int count = BIO_read(bio.get(), &result[0], static_cast<int>(result.size()));
    if (count > 0) {
        result.resize(count);
    }
    return result;
}

string CertificatePrivate::save(Ssl::EncodingFormat format) const
{
    if (!x509) {
        return string();
    }

    if (format == Ssl::Pem) {
        shared_ptr<BIO> bio(BIO_new(BIO_s_mem()), BIO_free);
        if (!bio) {
            return string();
        }
        int r = PEM_write_bio_X509(bio.get(), x509.get());
        if (r) {
            char *p = nullptr;
            long size = BIO_get_mem_data(bio.get(), &p);
            if (size > 0 && p != nullptr) {
                return string(p, static_cast<size_t>(size));
            }
        }
    } else if (format == Ssl::Der) {
        unsigned char *buf = nullptr;
        int len = i2d_X509(x509.get(), &buf);
        if (len > 0) {
            return string(static_cast<char *>(static_cast<void *>(buf)), len);
        }
    }
    return string();
}

Certificate CertificatePrivate::load(const string &data, Ssl::EncodingFormat format)
{
    Certificate cert;
    if (data.empty()) {
        return cert;
    }
    if (format == Ssl::Pem) {
        shared_ptr<BIO> bio(BIO_new_mem_buf(data.data(), static_cast<int>(data.size())), BIO_free);
        if (!bio) {
            return cert;
        }
        X509 *x = nullptr;
        PEM_read_bio_X509(bio.get(), &x, nullptr, nullptr);
        if (x) {
            cert.d->init(x);
        }
    } else if (format == Ssl::Der) {
        const unsigned char *buf;
        buf = reinterpret_cast<const unsigned char *>(data.data());
        int len = data.size();
        X509 *x = d2i_X509(nullptr, &buf, len);
        if (x) {
            cert.d->init(x);
        }
        return cert;
    }
    return cert;
}

static bool setIssuerInfos(X509 *x, const multimap<Certificate::SubjectInfo, string> &subjectInfoes)
{
    X509_NAME *name = X509_get_issuer_name(x);
    if (!name) {
        return false;
    }
    map<Certificate::SubjectInfo, string> table = {
        { Certificate::Organization, "O" },
        { Certificate::CommonName, "CN" },
        { Certificate::LocalityName, "L" },
        { Certificate::OrganizationalUnitName, "OU" },
        { Certificate::CountryName, "C" },
        { Certificate::StateOrProvinceName, "ST" },
        { Certificate::DistinguishedNameQualifier, "dnQualifier" },
        { Certificate::SerialNumber, "serialNumber" },
        //        {Certificate::EmailAddress, "emailAddress" },

    };
    bool success = true;
    for (map<Certificate::SubjectInfo, string>::const_iterator itor = table.begin(); itor != table.end();
         ++itor) {
        const vector<string> &sl = multimapValues(subjectInfoes, itor->first);
        for (const string &s : sl) {
            string bs = s;
            success = success
                    && X509_NAME_add_entry_by_txt(name, itor->second.c_str(), MBSTRING_UTF8,
                                                  reinterpret_cast<const unsigned char *>(bs.data()),
                                                  static_cast<int>(bs.size()),
                                                  -1, 0);
        }
    }
    if (!success) {
        return false;
    }
    int r = X509_set_issuer_name(x, name);
    return r;
}

static bool setSubjectInfos(X509 *x, const multimap<Certificate::SubjectInfo, string> &subjectInfoes)
{
    X509_NAME *name = X509_get_subject_name(x);
    if (!name) {
        return false;
    }
    map<Certificate::SubjectInfo, string> table = {
        { Certificate::Organization, "O" },
        { Certificate::CommonName, "CN" },
        { Certificate::LocalityName, "L" },
        { Certificate::OrganizationalUnitName, "OU" },
        { Certificate::CountryName, "C" },
        { Certificate::StateOrProvinceName, "ST" },
        { Certificate::DistinguishedNameQualifier, "dnQualifier" },
        { Certificate::SerialNumber, "serialNumber" },
        //        {Certificate::EmailAddress, "emailAddress" },

    };
    bool success = true;
    for (map<Certificate::SubjectInfo, string>::const_iterator itor = table.begin(); itor != table.end();
         ++itor) {
        const vector<string> &sl = multimapValues(subjectInfoes, itor->first);
        for (const string &s : sl) {
            string bs = s;
            success = success
                    && X509_NAME_add_entry_by_txt(name, itor->second.c_str(), MBSTRING_UTF8,
                                                  reinterpret_cast<const unsigned char *>(bs.data()),
                                                  static_cast<int>(bs.size()),
                                                  -1, 0);
        }
    }
    if (!success) {
        return false;
    }
    int r = X509_set_subject_name(x, name);
    return r;
}

struct Asn1TimeCleaner
{
    void operator()(ASN1_TIME *t) const
    {
        if (t)
            ASN1_STRING_free(t);
    }
};

Certificate CertificatePrivate::generate(const PublicKey &publickey, const PrivateKey &caKey,
                                         MessageDigest::Algorithm signAlgo, long serialNumber,
                                         const utils::DateTime &effectiveDate, const utils::DateTime &expiryDate,
                                         const multimap<Certificate::SubjectInfo, string> &subjectInfoes)
{
    Certificate cert;
    unique_ptr<X509, X509Cleaner> x509(X509_new());
    if (!x509) {
        ngDebug() << "can not allocate X509.";
        return cert;
    }
    int r = X509_set_version(x509.get(), 2);
    ASN1_INTEGER *i = X509_get_serialNumber(x509.get());
    if (!r || !i) {
        ngDebug() << "can not set version and serial number.";
        return cert;
    }
    ASN1_INTEGER_set(i, serialNumber);
    X509_set_pubkey(x509.get(), static_cast<EVP_PKEY *>(publickey.handle()));
    if (!setSubjectInfos(x509.get(), subjectInfoes)) {
        ngDebug() << "can not set subject infos.";
        return cert;
    }
    if (!setIssuerInfos(x509.get(), subjectInfoes)) {
        ngDebug() << "can not set issuer infos.";
        return cert;
    }
    unique_ptr<ASN1_TIME, Asn1TimeCleaner> t(ASN1_TIME_new());
    if (t) {
        if (ASN1_TIME_set(t.get(), static_cast<time_t>(effectiveDate.toUTC().toSecsSinceEpoch()))) {
            r = X509_set1_notBefore(x509.get(), t.get());
            if (!r) {
                ngDebug() << "can not set effective date.";
            }
        } else {
            ngDebug() << "invalid x509 effective date:" << effectiveDate.toString();
        }
        if (ASN1_TIME_set(t.get(), static_cast<time_t>(expiryDate.toUTC().toSecsSinceEpoch()))) {
            r = X509_set1_notAfter(x509.get(), t.get());
            if (!r) {
                ngDebug() << "can not set expiry date";
            }
        } else {
            ngDebug() << "invalid x509 expiry date:" << expiryDate.toString();
        }
    }
    const EVP_MD *md = getOpenSSL_MD(signAlgo);
    if (!md) {
        ngDebug() << "can not find md.";
        return cert;
    }
    r = X509_sign(x509.get(), static_cast<EVP_PKEY *>(caKey.handle()), md);
    if (!r) {
        ngDebug() << "can not sign certificate.";
        return cert;
    }
    cert.d->init(x509.release());
    return cert;
}

inline char toHexLower(uint value)
{
    return "0123456789abcdef"[value & 0xF];
}

string toHex(const string &bs, char separator)
{
    if (bs.empty()) {
        return string();
    }

    const int length = separator ? (bs.size() * 3 - 1) : (bs.size() * 2);
    string hex;
    hex.resize(length);
    char *hexData = &hex[0];
    const uint8_t *data = reinterpret_cast<const uint8_t *>(bs.data());
    for (int i = 0, o = 0; i < bs.size(); ++i) {
        hexData[o++] = toHexLower(data[i] >> 4);
        hexData[o++] = toHexLower(data[i] & 0xf);

        if ((separator) && (o < length))
            hexData[o++] = separator;
    }
    return hex;
}

static string colonSeparatedHex(const string &value)
{
    const int size = value.size();
    int i = 0;
    while (i < size && !value[i])  // skip leading zeros
        ++i;

    return toHex(value.substr(i), ':');
}

bool CertificatePrivate::qtParse()
{
    const string &data = save(Ssl::Der);
    QAsn1Element root;

    MsgPackStream dataStream(data);
    if (!root.read(dataStream) || root.type() != QAsn1Element::SequenceType) {
        return false;
    }

    MsgPackStream rootStream(root.value());
    QAsn1Element cert;
    if (!cert.read(rootStream) || cert.type() != QAsn1Element::SequenceType) {
        return false;
    }

    // version or serial number
    QAsn1Element elem;
    MsgPackStream certStream(cert.value());
    if (!elem.read(certStream)) {
        return false;
    }

    if (elem.type() == QAsn1Element::Context0Type) {
        MsgPackStream versionStream(elem.value());
        if (!elem.read(versionStream) || elem.type() != QAsn1Element::IntegerType) {
            return false;
        }

        versionString = utils::number(elem.value()[0] + 1);
        if (!elem.read(certStream)) {
            return false;
        }
    } else {
        versionString = utils::number(1);
    }

    // serial number
    if (elem.type() != QAsn1Element::IntegerType) {
        return false;
    }
    string serialNumberString = colonSeparatedHex(elem.value());
    (void)(serialNumberString);

    // algorithm ID
    if (!elem.read(certStream) || elem.type() != QAsn1Element::SequenceType) {
        return false;
    }

    // issuer info
    if (!elem.read(certStream) || elem.type() != QAsn1Element::SequenceType) {
        return false;
    }

    // validity period
    if (!elem.read(certStream) || elem.type() != QAsn1Element::SequenceType) {
        return false;
    }

    MsgPackStream validityStream(elem.value());
    if (!elem.read(validityStream)
        || (elem.type() != QAsn1Element::UtcTimeType && elem.type() != QAsn1Element::GeneralizedTimeType)) {
        return false;
    }

    notValidBefore = elem.toDateTime();
    if (!elem.read(validityStream)
        || (elem.type() != QAsn1Element::UtcTimeType && elem.type() != QAsn1Element::GeneralizedTimeType)) {
        return false;
    }

    notValidAfter = elem.toDateTime();

    // we don't care about other informations.
    /*
    // subject name
    if (!elem.read(certStream) || elem.type() != QAsn1Element::SequenceType)
        return false;

    string subjectDer = data.substr(dataStream.device()->pos() - elem.value().size(), elem.value().size());
    subjectInfo = elem.toInfo();
    subjectMatchesIssuer = issuerDer == subjectDer;

    // public key
    int64_t keyStart = certStream.device()->pos();
    if (!elem.read(certStream) || elem.type() != QAsn1Element::SequenceType)
        return false;

    publicKeyDerData.resize(certStream.device()->pos() - keyStart);
    MsgPackStream keyStream(elem.value());
    if (!elem.read(keyStream) || elem.type() != QAsn1Element::SequenceType)
        return false;

    // key algorithm
    if (!elem.read(elem.value()) || elem.type() != QAsn1Element::ObjectIdentifierType)
        return false;

    const string oid = elem.toObjectId();
    if (oid == RSA_ENCRYPTION_OID)
        publicKeyAlgorithm = Ssl::Rsa;
    else if (oid == DSA_ENCRYPTION_OID)
        publicKeyAlgorithm = Ssl::Dsa;
    else if (oid == EC_ENCRYPTION_OID)
        publicKeyAlgorithm = Ssl::Ec;
    else
        publicKeyAlgorithm = Ssl::Opaque;

    certStream.device()->seek(keyStart);
    certStream.readRawData(publicKeyDerData.data(), publicKeyDerData.size());

    // extensions
    while (elem.read(certStream)) {
        if (elem.type() == QAsn1Element::Context3Type) {
            if (elem.read(elem.value()) && elem.type() == QAsn1Element::SequenceType) {
                MsgPackStream extStream(elem.value());
                while (elem.read(extStream) && elem.type() == QAsn1Element::SequenceType) {
                    SslCertificateExtension extension;
                    if (!parseExtension(elem.value(), &extension))
                        return false;
                    extensions << extension;

                    if (extension.oid() == "2.5.29.17") {
                        // subjectAltName
                        QAsn1Element sanElem;
                        if (sanElem.read(extension.value().toByteArray()) && sanElem.type() ==
    QAsn1Element::SequenceType) { MsgPackStream nameStream(sanElem.value()); QAsn1Element nameElem; while
    (nameElem.read(nameStream)) { if (nameElem.type() == QAsn1Element::Rfc822NameType) {
                                    subjectAlternativeNames.insert(Ssl::EmailEntry, nameElem.toString());
                                } else if (nameElem.type() == QAsn1Element::DnsNameType) {
                                    subjectAlternativeNames.insert(Ssl::DnsEntry, nameElem.toString());
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    derData = data.substr(0, dataStream.device()->pos());
    null = false;
    */
    return true;
}

Certificate::Certificate()
    : d(new CertificatePrivate)
{
}

Certificate::Certificate(const Certificate &other)
    : d(other.d)
{
}

Certificate::Certificate(Certificate &&other)
    : d(nullptr)
{
    std::swap(d, other.d);
}

Certificate::~Certificate()
{
}

Certificate &Certificate::operator=(const Certificate &other)
{
    d = other.d;
    return *this;
}

bool Certificate::isNull() const
{
    return d->isNull();
}

string Certificate::digest(MessageDigest::Algorithm algorithm) const
{
    const string &der = save(Ssl::Der);
    if (der.empty()) {
        return string();
    }
    return MessageDigest::hash(der, algorithm);
}

utils::DateTime Certificate::effectiveDate() const
{
    return d->effectiveDate();
}

utils::DateTime Certificate::expiryDate() const
{
    return d->expiryDate();
}

void * Certificate::handle() const
{
    return static_cast<void *>(d->x509.get());
}

bool Certificate::isBlacklisted() const
{
    return d->isBlacklisted();
}

bool Certificate::isSelfSigned() const
{
    return d->isSelfSigned();
}

vector<string> Certificate::issuerInfo(SubjectInfo subject) const
{
    return d->issuerInfo(subject);
}

vector<string> Certificate::issuerInfo(const string &attribute) const
{
    return d->issuerInfo(attribute);
}

vector<string> Certificate::issuerInfoAttributes() const
{
    return d->issuerInfoAttributes();
}

PublicKey Certificate::publicKey() const
{
    return d->publicKey();
}

string Certificate::serialNumber() const
{
    return d->serialNumber();
}

vector<string> Certificate::subjectInfo(SubjectInfo subject) const
{
    return d->subjectInfo(subject);
}

vector<string> Certificate::subjectInfo(const string &attribute) const
{
    return d->subjectInfo(attribute);
}

vector<string> Certificate::subjectInfoAttributes() const
{
    return d->subjectInfoAttributes();
}

string Certificate::toString() const
{
    return d->toString();
}

string Certificate::version() const
{
    return d->version();
}

string Certificate::save(Ssl::EncodingFormat format) const
{
    return d->save(format);
}

Certificate Certificate::load(const string &data, Ssl::EncodingFormat format)
{
    return CertificatePrivate::load(data, format);
}

Certificate Certificate::generate(const PublicKey &publickey, const PrivateKey &caKey,
                                  MessageDigest::Algorithm signAlgo, long serialNumber, const utils::DateTime &effectiveDate,
                                  const utils::DateTime &expiryDate,
                                  const multimap<Certificate::SubjectInfo, string> &subjectInfoes)
{
    return CertificatePrivate::generate(publickey, caKey, signAlgo, serialNumber, effectiveDate, expiryDate,
                                        subjectInfoes);
}

multimap<Certificate::AlternativeNameEntryType, string> Certificate::subjectAlternativeNames() const
{
    return multimap<AlternativeNameEntryType, string>();
}

bool Certificate::operator==(const Certificate &other) const
{
    if (d->x509 && other.d->x509)
        return X509_cmp(d->x509.get(), other.d->x509.get()) == 0;
    return false;
}

uint qHash(const Certificate &key, uint seed)
{
    if (X509 * const x = key.d->x509.get()) {
        const EVP_MD *sha256 = EVP_sha256();
        if (sha256) {
            unsigned int len = 0;
            unsigned char md[EVP_MAX_MD_SIZE];
            X509_digest(x, sha256, md, &len);
            return qHashBits(md, len, seed);
        }
    }
    return seed;
}

ostream &operator<<(ostream &debug, const Certificate &certificate)
{
    debug << "Certificate(" << certificate.version() << ", " << certificate.serialNumber() << ", "
          << utils::bytesToBase64(certificate.digest()) << ", "
          << utils::join(certificate.issuerInfo(Certificate::Organization), ", ") << ", "
          << utils::join(certificate.subjectInfo(Certificate::Organization), ", ") << ", "
          << certificate.effectiveDate().toString() << ", " << certificate.expiryDate().toString() << ')';
    return debug;
}

ostream &operator<<(ostream &debug, Certificate::SubjectInfo info)
{
    switch (info) {
    case Certificate::Organization:
        debug << "Organization";
        break;
    case Certificate::CommonName:
        debug << "CommonName";
        break;
    case Certificate::CountryName:
        debug << "CountryName";
        break;
    case Certificate::LocalityName:
        debug << "LocalityName";
        break;
    case Certificate::OrganizationalUnitName:
        debug << "OrganizationalUnitName";
        break;
    case Certificate::StateOrProvinceName:
        debug << "StateOrProvinceName";
        break;
    case Certificate::DistinguishedNameQualifier:
        debug << "DistinguishedNameQualifier";
        break;
    case Certificate::SerialNumber:
        debug << "SerialNumber";
        break;
    case Certificate::EmailAddress:
        debug << "EmailAddress";
        break;
    }
    return debug;
}

}  // namespace qtng
