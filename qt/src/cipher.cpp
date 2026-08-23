#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "cipher.h"
#include "ssl.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class CipherPrivate
{
public:
    Cipher::Algorithm algorithm;
    Cipher::Mode mode;
    std::shared_ptr<qtng_core::Cipher> core;
    explicit CipherPrivate(Cipher::Algorithm alog, Cipher::Mode mode, Cipher::Operation operation)
        : algorithm(alog)
        , mode(mode)
        , core(make_shared<qtng_core::Cipher>(static_cast<qtng_core::Cipher::Algorithm>(alog),
                                               static_cast<qtng_core::Cipher::Mode>(mode),
                                               static_cast<qtng_core::Cipher::Operation>(operation)))
    {
    }

    static const std::shared_ptr<qtng_core::Cipher> &coreOf(const Cipher *cipher)
    {
        return cipher->d_func()->core;
    }
};

Cipher::Cipher(Algorithm alog, Mode mode, Operation operation)
    : d_ptr(new CipherPrivate(alog, mode, operation))
{
}

Cipher::~Cipher() { delete d_ptr; }

Cipher *Cipher::copy(Operation operation)
{
    Q_D(Cipher);
    return new Cipher(d->algorithm, d->mode, operation);
}

bool Cipher::isValid() const { Q_D(const Cipher); return d->core->isValid(); }
bool Cipher::isStream() const { Q_D(const Cipher); return d->core->isStream(); }
bool Cipher::setKey(const QByteArray &key) { Q_D(Cipher); return d->core->setKey(toStdString(key)); }
QByteArray Cipher::key() const { Q_D(const Cipher); return toQByteArray(d->core->key()); }
bool Cipher::setInitialVector(const QByteArray &iv) { Q_D(Cipher); return d->core->setInitialVector(toStdString(iv)); }
QByteArray Cipher::initialVector() const { Q_D(const Cipher); return toQByteArray(d->core->initialVector()); }
bool Cipher::setPassword(const QByteArray &password, const QByteArray &salt, MessageDigest::Algorithm hashAlgo, int i)
{
    Q_D(Cipher);
    return d->core->setPassword(toStdString(password), toStdString(salt),
                                static_cast<qtng_core::MessageDigest::Algorithm>(hashAlgo), i);
}
bool Cipher::setOpensslPassword(const QByteArray &password, const QByteArray &salt, MessageDigest::Algorithm hashAlgo, int i)
{
    Q_D(Cipher);
    return d->core->setOpensslPassword(toStdString(password), toStdString(salt),
                                       static_cast<qtng_core::MessageDigest::Algorithm>(hashAlgo), i);
}
QByteArray Cipher::salt() const { Q_D(const Cipher); return toQByteArray(d->core->salt()); }
QByteArray Cipher::saltHeader() const { Q_D(const Cipher); return toQByteArray(d->core->saltHeader()); }
bool Cipher::setPadding(bool padding) { Q_D(Cipher); return d->core->setPadding(padding); }
bool Cipher::padding() const { Q_D(const Cipher); return d->core->padding(); }
int Cipher::keySize() const { Q_D(const Cipher); return d->core->keySize(); }
int Cipher::ivSize() const { Q_D(const Cipher); return d->core->ivSize(); }
int Cipher::blockSize() const { Q_D(const Cipher); return d->core->blockSize(); }
QByteArray Cipher::addData(const char *data, int len) { Q_D(Cipher); return toQByteArray(d->core->addData(data, len)); }
QByteArray Cipher::finalData() { Q_D(Cipher); return toQByteArray(d->core->finalData()); }

QPair<QByteArray, QByteArray> Cipher::parseSalt(const QByteArray &header)
{
    const auto p = qtng_core::Cipher::parseSalt(toStdString(header));
    return qMakePair(toQByteArray(p.first), toQByteArray(p.second));
}

QSharedPointer<SocketLike> encrypted(QSharedPointer<Cipher> cipher, QSharedPointer<SocketLike> socket)
{
    if (cipher.isNull() || socket.isNull()) {
        return QSharedPointer<SocketLike>();
    }
    return toQtSocketLike(qtng_core::encrypted(CipherPrivate::coreOf(cipher.data()), toCoreSocketLike(socket)));
}

}  // namespace QTNETWORKNG_NAMESPACE
