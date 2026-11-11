#ifndef QTNG_CRYPTO_P_H
#define QTNG_CRYPTO_P_H

extern "C" {
#include <openssl/x509.h>
#include <openssl/err.h>
}
#include "qtng/private/openssl_raii.h"
#include "qtng/md.h"
#include "qtng/cipher.h"
#include "qtng/pkey.h"
#include "qtng/certificate.h"

#include "qtng/config.h"
QTNG_NAMESPACE_BEGIN

const EVP_MD *getOpenSSL_MD(MessageDigest::Algorithm algo);
const EVP_CIPHER *getOpenSSL_CIPHER(Cipher::Algorithm algo, Cipher::Mode mode);
bool openssl_setPkey(PublicKey *key, EVP_PKEY *pkey, bool hasPrivate);
bool openssl_setCertificate(Certificate *cert, X509 *x509);

QTNG_NAMESPACE_END
#endif  // QTNG_CRYPTO_P_H
