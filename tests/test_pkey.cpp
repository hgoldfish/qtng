#include <catch2/catch_test_macros.hpp>

#ifndef OPENSSL_SUPPRESS_DEPRECATED
#define OPENSSL_SUPPRESS_DEPRECATED
#endif
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <string>

#include "qtng/pkey.h"

using namespace std;
using namespace qtng;

namespace {

string opensslRsaLegacy(EVP_PKEY *pkey, const string &data, int padding,
                        int (*op)(int, const unsigned char *, unsigned char *, RSA *, int))
{
    RSA *rsa = EVP_PKEY_get1_RSA(pkey);
    if (!rsa) {
        return string();
    }
    string out(static_cast<size_t>(RSA_size(rsa)), '\0');
    const int n = op(static_cast<int>(data.size()), reinterpret_cast<const unsigned char *>(data.data()),
                     reinterpret_cast<unsigned char *>(&out[0]), rsa, padding);
    RSA_free(rsa);
    if (n <= 0) {
        return string();
    }
    out.resize(static_cast<size_t>(n));
    return out;
}

EVP_PKEY *pkeyOf(const PublicKey &key)
{
    return static_cast<EVP_PKEY *>(key.handle());
}

}  // namespace

TEST_CASE("rsaPublicDecrypt matches legacy RSA_public_decrypt (PKCS1 type 1)", "[pkey][rsa]")
{
    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    REQUIRE_FALSE(key.isNull());
    const PublicKey pub = key.publicKey();
    REQUIRE_FALSE(pub.isNull());

    const string plain = string("qtng") + string(1, '\0') + "rsa-public-decrypt";
    EVP_PKEY *legacyKey = pkeyOf(key);
    REQUIRE(legacyKey != nullptr);

    const string qtngCipher = key.rsaPrivateEncrypt(plain, PublicKey::PKCS1_PADDING);
    const string opensslCipher = opensslRsaLegacy(legacyKey, plain, RSA_PKCS1_PADDING, RSA_private_encrypt);
    REQUIRE_FALSE(qtngCipher.empty());
    REQUIRE_FALSE(opensslCipher.empty());
    // PKCS#1 v1.5 type 1 padding is deterministic (00 01 FF..00 || data).
    REQUIRE(qtngCipher == opensslCipher);

    REQUIRE(pub.rsaPublicDecrypt(qtngCipher, PublicKey::PKCS1_PADDING) == plain);
    REQUIRE(opensslRsaLegacy(legacyKey, qtngCipher, RSA_PKCS1_PADDING, RSA_public_decrypt) == plain);
    REQUIRE(pub.rsaPublicDecrypt(opensslCipher, PublicKey::PKCS1_PADDING) == plain);
    REQUIRE(opensslRsaLegacy(pkeyOf(pub), opensslCipher, RSA_PKCS1_PADDING, RSA_public_decrypt) == plain);
}

TEST_CASE("rsaPublicDecrypt matches legacy RSA_public_decrypt (NO_PADDING)", "[pkey][rsa]")
{
    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    REQUIRE_FALSE(key.isNull());
    const PublicKey pub = key.publicKey();
    EVP_PKEY *legacyKey = pkeyOf(key);
    REQUIRE(legacyKey != nullptr);

    const int k = EVP_PKEY_size(legacyKey);
    REQUIRE(k > 16);
    string block(static_cast<size_t>(k), '\x11');
    block[0] = '\0';  // keep integer < modulus

    const string qtngCipher = key.rsaPrivateEncrypt(block, PublicKey::NO_PADDING);
    const string opensslCipher = opensslRsaLegacy(legacyKey, block, RSA_NO_PADDING, RSA_private_encrypt);
    REQUIRE_FALSE(qtngCipher.empty());
    REQUIRE_FALSE(opensslCipher.empty());
    REQUIRE(qtngCipher == opensslCipher);

    REQUIRE(pub.rsaPublicDecrypt(qtngCipher, PublicKey::NO_PADDING) == block);
    REQUIRE(opensslRsaLegacy(pkeyOf(pub), qtngCipher, RSA_NO_PADDING, RSA_public_decrypt) == block);
    REQUIRE(pub.rsaPublicDecrypt(opensslCipher, PublicKey::NO_PADDING) == block);
}

TEST_CASE("rsaPublicDecrypt rejects OAEP like RSA_public_decrypt", "[pkey][rsa]")
{
    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    REQUIRE_FALSE(key.isNull());
    const string cipher = key.rsaPrivateEncrypt("payload", PublicKey::PKCS1_PADDING);
    REQUIRE_FALSE(cipher.empty());
    REQUIRE(key.publicKey().rsaPublicDecrypt(cipher, PublicKey::PKCS1_OAEP_PADDING).empty());
    REQUIRE(opensslRsaLegacy(pkeyOf(key), cipher, RSA_PKCS1_OAEP_PADDING, RSA_public_decrypt).empty());
}

TEST_CASE("encrypt stays PKCS1 after rsaPublicEncrypt OAEP on shared context", "[pkey][rsa]")
{
    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    REQUIRE_FALSE(key.isNull());
    const PublicKey pub = key.publicKey();

    const string oaep = pub.rsaPublicEncrypt("oaep-plain", PublicKey::PKCS1_OAEP_PADDING);
    REQUIRE_FALSE(oaep.empty());
    const string pkcs1 = pub.encrypt("pkcs1-plain");
    REQUIRE_FALSE(pkcs1.empty());

    REQUIRE(key.decrypt(pkcs1) == "pkcs1-plain");
    REQUIRE(key.rsaPrivateDecrypt(oaep, PublicKey::PKCS1_OAEP_PADDING) == "oaep-plain");
}

TEST_CASE("rsaPublicDecrypt does not recover RSA_public_encrypt ciphertext", "[pkey][rsa]")
{
    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    REQUIRE_FALSE(key.isNull());
    const string cipher = key.publicKey().rsaPublicEncrypt("hello", PublicKey::PKCS1_PADDING);
    REQUIRE_FALSE(cipher.empty());
    // Type 2 encryption padding is not type 1 signature padding.
    REQUIRE(key.publicKey().rsaPublicDecrypt(cipher, PublicKey::PKCS1_PADDING).empty());
    REQUIRE(opensslRsaLegacy(pkeyOf(key), cipher, RSA_PKCS1_PADDING, RSA_public_decrypt).empty());
}
