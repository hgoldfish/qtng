#include <QtCore/qdatetime.h>
#include <QtCore/qsharedpointer.h>
#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
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
QDateTime Certificate::effectiveDate() const { return QDateTime::fromSecsSinceEpoch(d->core.effectiveDate().toSecsSinceEpoch()); }
QDateTime Certificate::expiryDate() const { return QDateTime::fromSecsSinceEpoch(d->core.expiryDate().toSecsSinceEpoch()); }
PublicKey Certificate::publicKey() const
{
    const qtng_core::PublicKey pk = d->core.publicKey();
    return PublicKey::load(toQByteArray(pk.save(qtng_core::Ssl::Pem)), Ssl::Pem);
}
QByteArray Certificate::serialNumber() const { return toQByteArray(d->core.serialNumber()); }
QString Certificate::toString() const { return toQString(d->core.toString()); }
QByteArray Certificate::version() const { return toQByteArray(d->core.version()); }
bool Certificate::isSelfSigned() const { return d->core.isSelfSigned(); }
Certificate Certificate::load(const QByteArray &data, Ssl::EncodingFormat format)
{
    Certificate c; c.d->core = qtng_core::Certificate::load(toStdString(data), static_cast<qtng_core::Ssl::EncodingFormat>(format)); return c;
}
QByteArray Certificate::save(Ssl::EncodingFormat format) const
{
    return toQByteArray(d->core.save(static_cast<qtng_core::Ssl::EncodingFormat>(format)));
}

}  // namespace QTNETWORKNG_NAMESPACE
