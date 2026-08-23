#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "pkey.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class PublicKeyPrivate
{
public:
    shared_ptr<qtng_core::PublicKey> core;

    static PublicKey fromCore(shared_ptr<qtng_core::PublicKey> key)
    {
        PublicKey result;
        result.d_ptr->core = std::move(key);
        return result;
    }

    static PrivateKey privateFromCore(shared_ptr<qtng_core::PrivateKey> key)
    {
        PrivateKey result;
        result.d_ptr->core = std::move(key);
        return result;
    }

    static qtng_core::PublicKey &coreOf(PublicKey &key)
    {
        return *key.d_ptr->core;
    }

    static const qtng_core::PublicKey &coreOf(const PublicKey &key)
    {
        return *key.d_ptr->core;
    }

    static qtng_core::PrivateKey &privateCoreOf(PrivateKey &key)
    {
        return static_cast<qtng_core::PrivateKey &>(*key.d_ptr->core);
    }

    static const qtng_core::PrivateKey &privateCoreOf(const PrivateKey &key)
    {
        return static_cast<const qtng_core::PrivateKey &>(*key.d_ptr->core);
    }
};

PublicKey::PublicKey() : d_ptr(new PublicKeyPrivate)
{
    d_ptr->core = make_shared<qtng_core::PublicKey>();
}

PublicKey::~PublicKey() { delete d_ptr; }

PublicKey::PublicKey(const PublicKey &other)
    : d_ptr(new PublicKeyPrivate)
{
    d_ptr->core = make_shared<qtng_core::PublicKey>(*other.d_ptr->core);
}

PublicKey &PublicKey::operator=(const PublicKey &other)
{
    d_ptr->core = make_shared<qtng_core::PublicKey>(*other.d_ptr->core);
    return *this;
}

Qt::HANDLE PublicKey::handle() const { return reinterpret_cast<Qt::HANDLE>(d_ptr->core->handle()); }
bool PublicKey::isNull() const { return d_ptr->core->isNull(); }
bool PublicKey::isValid() const { return d_ptr->core->isValid(); }
PublicKey::Algorithm PublicKey::algorithm() const { return static_cast<Algorithm>(d_ptr->core->algorithm()); }
int PublicKey::bits() const { return d_ptr->core->bits(); }
bool PublicKey::verify(const QByteArray &data, const QByteArray &hash, MessageDigest::Algorithm hashAlgo)
{
    return d_ptr->core->verify(toStdString(data), toStdString(hash), static_cast<qtng_core::MessageDigest::Algorithm>(hashAlgo));
}
QByteArray PublicKey::encrypt(const QByteArray &data) const { return toQByteArray(d_ptr->core->encrypt(toStdString(data))); }
QByteArray PublicKey::digest(MessageDigest::Algorithm algorithm) const
{
    return toQByteArray(d_ptr->core->digest(static_cast<qtng_core::MessageDigest::Algorithm>(algorithm)));
}
QByteArray PublicKey::rsaPublicEncrypt(const QByteArray &data, RsaPadding padding) const
{
    return toQByteArray(d_ptr->core->rsaPublicEncrypt(toStdString(data), static_cast<qtng_core::PublicKey::RsaPadding>(padding)));
}
QByteArray PublicKey::rsaPublicDecrypt(const QByteArray &data, RsaPadding padding) const
{
    return toQByteArray(d_ptr->core->rsaPublicDecrypt(toStdString(data), static_cast<qtng_core::PublicKey::RsaPadding>(padding)));
}
PublicKey PublicKey::load(const QByteArray &data, Ssl::EncodingFormat format)
{
    return PublicKeyPrivate::fromCore(make_shared<qtng_core::PublicKey>(
            qtng_core::PublicKey::load(toStdString(data), static_cast<qtng_core::Ssl::EncodingFormat>(format))));
}
QByteArray PublicKey::save(Ssl::EncodingFormat format) const
{
    return toQByteArray(d_ptr->core->save(static_cast<qtng_core::Ssl::EncodingFormat>(format)));
}
bool PublicKey::operator==(const PublicKey &other) const { return *d_ptr->core == *other.d_ptr->core; }

bool PrivateKey::operator==(const PrivateKey &other) const { return PublicKeyPrivate::privateCoreOf(*this) == PublicKeyPrivate::privateCoreOf(other); }
PublicKey PrivateKey::publicKey() const
{
    return PublicKeyPrivate::fromCore(make_shared<qtng_core::PublicKey>(PublicKeyPrivate::privateCoreOf(*this).publicKey()));
}
QByteArray PrivateKey::sign(const QByteArray &data, MessageDigest::Algorithm hashAlgo)
{
    return toQByteArray(PublicKeyPrivate::privateCoreOf(*this).sign(toStdString(data),
                                                                  static_cast<qtng_core::MessageDigest::Algorithm>(hashAlgo)));
}
QByteArray PrivateKey::decrypt(const QByteArray &data) const { return toQByteArray(PublicKeyPrivate::privateCoreOf(*this).decrypt(toStdString(data))); }
QByteArray PrivateKey::rsaPrivateEncrypt(const QByteArray &data, RsaPadding padding) const
{
    return toQByteArray(PublicKeyPrivate::privateCoreOf(*this).rsaPrivateEncrypt(toStdString(data),
                                                                                 static_cast<qtng_core::PublicKey::RsaPadding>(padding)));
}
QByteArray PrivateKey::rsaPrivateDecrypt(const QByteArray &data, RsaPadding padding) const
{
    return toQByteArray(PublicKeyPrivate::privateCoreOf(*this).rsaPrivateDecrypt(toStdString(data),
                                                                                 static_cast<qtng_core::PublicKey::RsaPadding>(padding)));
}
PrivateKey PrivateKey::generate(Algorithm algo, int bits)
{
    return PublicKeyPrivate::privateFromCore(make_shared<qtng_core::PrivateKey>(
            qtng_core::PrivateKey::generate(static_cast<qtng_core::PublicKey::Algorithm>(algo), bits)));
}
PrivateKey PrivateKey::load(const QByteArray &data, Ssl::EncodingFormat format, const QByteArray &password)
{
    return PublicKeyPrivate::privateFromCore(make_shared<qtng_core::PrivateKey>(qtng_core::PrivateKey::load(
            toStdString(data), static_cast<qtng_core::Ssl::EncodingFormat>(format), toStdString(password))));
}
QByteArray PrivateKey::save(Ssl::EncodingFormat format, const QByteArray &password) const
{
    return toQByteArray(PublicKeyPrivate::privateCoreOf(*this).save(static_cast<qtng_core::Ssl::EncodingFormat>(format),
                                                                    toStdString(password)));
}

class PrivateKeyWriterPrivate
{
public:
    explicit PrivateKeyWriterPrivate(qtng_core::PrivateKeyWriter writer)
        : core(std::move(writer))
    {
    }

    qtng_core::PrivateKeyWriter core;
};

PrivateKeyWriter::PrivateKeyWriter(const PrivateKey &key)
    : d_ptr(new PrivateKeyWriterPrivate(qtng_core::PrivateKeyWriter(PublicKeyPrivate::privateCoreOf(const_cast<PrivateKey &>(key)))))
{
}

PrivateKeyWriter::PrivateKeyWriter(const PublicKey &key)
    : d_ptr(new PrivateKeyWriterPrivate(qtng_core::PrivateKeyWriter(PublicKeyPrivate::coreOf(const_cast<PublicKey &>(key)))))
{
}

PrivateKeyWriter::~PrivateKeyWriter() { delete d_ptr; }
PrivateKeyWriter &PrivateKeyWriter::setCipher(Cipher::Algorithm algo, Cipher::Mode mode)
{
    Q_D(PrivateKeyWriter);
    d->core.setCipher(static_cast<qtng_core::Cipher::Algorithm>(algo), static_cast<qtng_core::Cipher::Mode>(mode));
    return *this;
}
PrivateKeyWriter &PrivateKeyWriter::setPassword(const QByteArray &password)
{
    Q_D(PrivateKeyWriter);
    d->core.setPassword(toStdString(password));
    return *this;
}
PrivateKeyWriter &PrivateKeyWriter::setPublicOnly(bool publicOnly)
{
    Q_D(PrivateKeyWriter);
    d->core.setPublicOnly(publicOnly);
    return *this;
}
QByteArray PrivateKeyWriter::asPem()
{
    Q_D(PrivateKeyWriter);
    return toQByteArray(d->core.asPem());
}
QByteArray PrivateKeyWriter::asDer()
{
    Q_D(PrivateKeyWriter);
    return toQByteArray(d->core.asDer());
}
bool PrivateKeyWriter::save(const QString &filePath)
{
    Q_D(PrivateKeyWriter);
    return d->core.save(toStdString(filePath));
}

class PrivateKeyReaderPrivate
{
public:
    qtng_core::PrivateKeyReader core;
};

PrivateKeyReader::PrivateKeyReader()
    : d_ptr(new PrivateKeyReaderPrivate)
{
}

PrivateKeyReader::~PrivateKeyReader() { delete d_ptr; }
PrivateKeyReader &PrivateKeyReader::setPassword(const QByteArray &password)
{
    Q_D(PrivateKeyReader);
    d->core.setPassword(toStdString(password));
    return *this;
}
PrivateKeyReader &PrivateKeyReader::setFormat(Ssl::EncodingFormat format)
{
    Q_D(PrivateKeyReader);
    d->core.setFormat(static_cast<qtng_core::Ssl::EncodingFormat>(format));
    return *this;
}
PrivateKey PrivateKeyReader::read(const QByteArray &data)
{
    Q_D(PrivateKeyReader);
    return PublicKeyPrivate::privateFromCore(make_shared<qtng_core::PrivateKey>(d->core.read(toStdString(data))));
}
PublicKey PrivateKeyReader::readPublic(const QByteArray &data)
{
    Q_D(PrivateKeyReader);
    return PublicKeyPrivate::fromCore(make_shared<qtng_core::PublicKey>(d->core.readPublic(toStdString(data))));
}
PrivateKey PrivateKeyReader::read(const QString &filePath)
{
    Q_D(PrivateKeyReader);
    return PublicKeyPrivate::privateFromCore(make_shared<qtng_core::PrivateKey>(d->core.read(toStdString(filePath))));
}
PublicKey PrivateKeyReader::readPublic(const QString &filePath)
{
    Q_D(PrivateKeyReader);
    return PublicKeyPrivate::fromCore(make_shared<qtng_core::PublicKey>(d->core.readPublic(toStdString(filePath))));
}

}  // namespace QTNETWORKNG_NAMESPACE

#include "bridge/pkey_access.h"

namespace qtng_bridge {

QTNETWORKNG_NAMESPACE::PrivateKey toQtPrivateKey(const qtng_core::PrivateKey &key)
{
    return QTNETWORKNG_NAMESPACE::PublicKeyPrivate::privateFromCore(make_shared<qtng_core::PrivateKey>(key));
}

const qtng_core::PrivateKey &privateKeyCoreOf(const QTNETWORKNG_NAMESPACE::PrivateKey &key)
{
    return QTNETWORKNG_NAMESPACE::PublicKeyPrivate::privateCoreOf(key);
}

}  // namespace qtng_bridge
