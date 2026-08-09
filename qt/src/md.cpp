#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "md.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class MessageDigestPrivate
{
public:
    explicit MessageDigestPrivate(MessageDigest::Algorithm algo)
        : core(static_cast<qtng_core::MessageDigest::Algorithm>(algo))
    {
    }

    qtng_core::MessageDigest core;
};

MessageDigest::MessageDigest(Algorithm algo)
    : d_ptr(new MessageDigestPrivate(algo))
{
}

MessageDigest::~MessageDigest()
{
    delete d_ptr;
}

void MessageDigest::addData(const char *data, int len)
{
    Q_D(MessageDigest);
    d->core.addData(data, len);
}

QByteArray MessageDigest::result()
{
    Q_D(MessageDigest);
    return toQByteArray(d->core.result());
}

QByteArray PBKDF2_HMAC(int keylen, const QByteArray &password, const QByteArray &salt,
                       MessageDigest::Algorithm hashAlgo, int i)
{
    return toQByteArray(qtng_core::PBKDF2_HMAC(keylen, toStdString(password), toStdString(salt),
                                              static_cast<qtng_core::MessageDigest::Algorithm>(hashAlgo), i));
}

QByteArray scrypt(int keylen, const QByteArray &password, const QByteArray &salt, int n, int r, int p)
{
    return toQByteArray(qtng_core::scrypt(keylen, toStdString(password), toStdString(salt), n, r, p));
}

}  // namespace QTNETWORKNG_NAMESPACE
