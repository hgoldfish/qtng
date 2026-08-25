#include <QtCore/qdatetime.h>
#include <QtCore/qsharedpointer.h>
#include "bridge/cert_access.h"
#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "bridge/pkey_access.h"
#include "bridge/stream_bridge.h"
#include "certificate.h"
#include "pkey.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class CertificatePrivate : public QSharedData
{
public:
    qtng_core::Certificate core;

    static Certificate fromCore(const qtng_core::Certificate &cert)
    {
        Certificate result;
        result.d->core = cert;
        return result;
    }

    static const qtng_core::Certificate &coreOf(const Certificate &cert)
    {
        return cert.d->core;
    }
};

Certificate::Certificate() : d(new CertificatePrivate) {}
Certificate::Certificate(const Certificate &other) : d(other.d) {}
Certificate::Certificate(Certificate &&other) : d(std::move(other.d)) {}
Certificate::~Certificate() {}
Certificate &Certificate::operator=(const Certificate &other) { d = other.d; return *this; }
bool Certificate::operator==(const Certificate &other) const { return d->core == other.d->core; }
bool Certificate::isBlacklisted() const { return d->core.isBlacklisted(); }
bool Certificate::isNull() const { return d->core.isNull(); }
Qt::HANDLE Certificate::handle() const { return reinterpret_cast<Qt::HANDLE>(d->core.handle()); }
QByteArray Certificate::digest(MessageDigest::Algorithm algorithm) const
{
    return toQByteArray(d->core.digest(static_cast<qtng_core::MessageDigest::Algorithm>(algorithm)));
}
QDateTime Certificate::effectiveDate() const { return toQDateTime(d->core.effectiveDate()); }
QDateTime Certificate::expiryDate() const { return toQDateTime(d->core.expiryDate()); }
PublicKey Certificate::publicKey() const
{
    const qtng_core::PublicKey pk = d->core.publicKey();
    return PublicKey::load(toQByteArray(pk.save(qtng_core::Ssl::Pem)), Ssl::Pem);
}
QByteArray Certificate::serialNumber() const { return toQByteArray(d->core.serialNumber()); }
QMultiMap<Certificate::AlternativeNameEntryType, QString> Certificate::subjectAlternativeNames() const
{
    QMultiMap<AlternativeNameEntryType, QString> result;
    const std::multimap<qtng_core::Certificate::AlternativeNameEntryType, std::string> &names =
            d->core.subjectAlternativeNames();
    for (const auto &entry : names) {
        result.insert(static_cast<AlternativeNameEntryType>(entry.first), toQString(entry.second));
    }
    return result;
}
QStringList Certificate::subjectInfo(SubjectInfo subject) const
{
    return toQList<QString>(d->core.subjectInfo(static_cast<qtng_core::Certificate::SubjectInfo>(subject)), toQString);
}
QStringList Certificate::subjectInfo(const QByteArray &attribute) const
{
    return toQList<QString>(d->core.subjectInfo(toStdString(attribute)), toQString);
}
QList<QByteArray> Certificate::subjectInfoAttributes() const
{
    return toQList<QByteArray>(d->core.subjectInfoAttributes(), toQByteArray);
}
QString Certificate::toString() const { return toQString(d->core.toString()); }
QByteArray Certificate::version() const { return toQByteArray(d->core.version()); }
bool Certificate::isSelfSigned() const { return d->core.isSelfSigned(); }
QStringList Certificate::issuerInfo(SubjectInfo subject) const
{
    return toQList<QString>(d->core.issuerInfo(static_cast<qtng_core::Certificate::SubjectInfo>(subject)), toQString);
}
QStringList Certificate::issuerInfo(const QByteArray &attribute) const
{
    return toQList<QString>(d->core.issuerInfo(toStdString(attribute)), toQString);
}
QList<QByteArray> Certificate::issuerInfoAttributes() const
{
    return toQList<QByteArray>(d->core.issuerInfoAttributes(), toQByteArray);
}
Certificate Certificate::load(const QByteArray &data, Ssl::EncodingFormat format)
{
    Certificate c; c.d->core = qtng_core::Certificate::load(toStdString(data), static_cast<qtng_core::Ssl::EncodingFormat>(format)); return c;
}
Certificate Certificate::generate(const PublicKey &publickey, const PrivateKey &caKey,
                                  MessageDigest::Algorithm signAlgo, long serialNumber,
                                  const QDateTime &effectiveDate, const QDateTime &expiryDate,
                                  const QMultiMap<SubjectInfo, QString> &subjectInfoes)
{
    std::multimap<qtng_core::Certificate::SubjectInfo, std::string> coreSubjectInfoes;
    for (auto it = subjectInfoes.constBegin(); it != subjectInfoes.constEnd(); ++it) {
        coreSubjectInfoes.emplace(static_cast<qtng_core::Certificate::SubjectInfo>(it.key()), toStdString(it.value()));
    }
    return CertificatePrivate::fromCore(qtng_core::Certificate::generate(
            qtng_core::PublicKey::load(toStdString(publickey.save(Ssl::Pem)), qtng_core::Ssl::Pem),
            privateKeyCoreOf(caKey), static_cast<qtng_core::MessageDigest::Algorithm>(signAlgo), serialNumber,
            toCoreDateTime(effectiveDate), toCoreDateTime(expiryDate), coreSubjectInfoes));
}
QByteArray Certificate::save(Ssl::EncodingFormat format) const
{
    return toQByteArray(d->core.save(static_cast<qtng_core::Ssl::EncodingFormat>(format)));
}

uint qHash(const Certificate &key, uint seed)
{
    return qHash(key.digest(), seed);
}

}  // namespace QTNETWORKNG_NAMESPACE

namespace qtng_bridge {

QTNETWORKNG_NAMESPACE::Certificate toQtCertificate(const qtng_core::Certificate &cert)
{
    return QTNETWORKNG_NAMESPACE::CertificatePrivate::fromCore(cert);
}

const qtng_core::Certificate &certificateCoreOf(const QTNETWORKNG_NAMESPACE::Certificate &cert)
{
    return QTNETWORKNG_NAMESPACE::CertificatePrivate::coreOf(cert);
}

}  // namespace qtng_bridge
