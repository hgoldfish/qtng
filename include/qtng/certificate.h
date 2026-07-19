#ifndef QTNG_CERTIFICATE_H
#define QTNG_CERTIFICATE_H

#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "qtng/md.h"
#include "qtng/pkey.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/platform.h"

namespace qtng {

// qHash is a friend, but we can't use default arguments for friends (§8.3.6.4)
class Certificate;
uint qHash(const Certificate &key, uint seed = 0);

class CertificatePrivate;
class Certificate
{
public:
    enum SubjectInfo {
        Organization = 0,
        CommonName = 1,
        LocalityName = 2,
        OrganizationalUnitName = 3,
        CountryName = 4,
        StateOrProvinceName = 5,
        DistinguishedNameQualifier = 6,
        SerialNumber = 7,
        EmailAddress = 8,
    };

    enum AlternativeNameEntryType {
        EmailEntry = 0,
        DnsEntry = 1,
    };
public:
    Certificate();
    Certificate(const Certificate &other);
    Certificate(Certificate &&other);
    virtual ~Certificate();
public:
    bool isBlacklisted() const;
    bool isNull() const;
    bool isValid() const { return !isNull() && !isBlacklisted(); }
    void *handle() const;

    std::string digest(MessageDigest::Algorithm algorithm = MessageDigest::Sha256) const;
    qtng::utils::DateTime effectiveDate() const;
    qtng::utils::DateTime expiryDate() const;
    PublicKey publicKey() const;
    std::string serialNumber() const;
    std::multimap<AlternativeNameEntryType, std::string> subjectAlternativeNames() const;
    std::vector<std::string> subjectInfo(SubjectInfo subject) const;
    std::vector<std::string> subjectInfo(const std::string &attribute) const;
    std::vector<std::string> subjectInfoAttributes() const;
    std::string toString() const;
    std::string version() const;

    bool isSelfSigned() const;
    std::vector<std::string> issuerInfo(SubjectInfo subject) const;
    std::vector<std::string> issuerInfo(const std::string &attribute) const;
    std::vector<std::string> issuerInfoAttributes() const;
public:
    inline void swap(Certificate &other) { std::swap(d, other.d); }
    Certificate &operator=(Certificate &&other)
    {
        swap(other);
        return *this;
    }
    Certificate &operator=(const Certificate &other);
    bool operator!=(const Certificate &other) const { return !(*this == other); }
    bool operator==(const Certificate &other) const;
public:
    static Certificate load(const std::string &data, Ssl::EncodingFormat format = Ssl::Pem);
    static Certificate generate(const PublicKey &publickey, const PrivateKey &caKey, MessageDigest::Algorithm signAlgo,
                                long serialNumber, const qtng::utils::DateTime &effectiveDate, const qtng::utils::DateTime &expiryDate,
                                const std::multimap<SubjectInfo, std::string> &subjectInfoes);
    static Certificate selfSign(const PrivateKey &key, MessageDigest::Algorithm signAlgo, long serialNumber,
                                const qtng::utils::DateTime &effectiveDate, const qtng::utils::DateTime &expiryDate,
                                const std::multimap<Certificate::SubjectInfo, std::string> &subjectInfoes)
    {
        return generate(key, key, signAlgo, serialNumber, effectiveDate, expiryDate, subjectInfoes);
    }

    std::string save(Ssl::EncodingFormat format = Ssl::Pem) const;
private:
    std::shared_ptr<CertificatePrivate> d;
    friend class CertificatePrivate;
    friend uint qHash(const Certificate &key, uint seed);
};

class CertificateRequest
{
public:
    Certificate certificate() const;
};

std::ostream &operator<<(std::ostream &debug, const Certificate &certificate);
std::ostream &operator<<(std::ostream &debug, Certificate::SubjectInfo info);

}  // namespace qtng

#endif  // QTNG_CERTIFICATE_H
