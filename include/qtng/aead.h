#ifndef QTNG_AEAD_H
#define QTNG_AEAD_H

#include <cstddef>
#include <string>

#include "qtng/md.h"
#include "qtng/utils/platform.h"

namespace qtng {

// Authenticated encryption with associated data (AES-GCM / ChaCha20-Poly1305).
class Aead
{
public:
    enum Algorithm {
        Aes128Gcm = 1,
        Aes256Gcm = 2,
        ChaCha20Poly1305 = 3,
    };
public:
    explicit Aead(Algorithm algo);
    ~Aead();

    bool isValid() const;
    Algorithm algorithm() const;
    int keySize() const;  // bytes
    int nonceSize() const;  // bytes
    int tagSize() const;  // bytes

    bool setKey(const char *key, size_t keySize);
    inline bool setKey(const std::string &key) { return setKey(key.data(), key.size()); }

    // ciphertextAndTag = ciphertext || tag
    bool seal(const char *nonce, size_t nonceSize, const char *aad, size_t aadSize, const char *plaintext,
              size_t plaintextSize, std::string *ciphertextAndTag) const;
    inline bool seal(const std::string &nonce, const std::string &aad, const std::string &plaintext,
                     std::string *ciphertextAndTag) const
    {
        return seal(nonce.data(), nonce.size(), aad.data(), aad.size(), plaintext.data(), plaintext.size(),
                    ciphertextAndTag);
    }
    bool open(const char *nonce, size_t nonceSize, const char *aad, size_t aadSize, const char *ciphertextAndTag,
              size_t ciphertextAndTagSize, std::string *plaintext) const;
    inline bool open(const std::string &nonce, const std::string &aad, const std::string &ciphertextAndTag,
                     std::string *plaintext) const
    {
        return open(nonce.data(), nonce.size(), aad.data(), aad.size(), ciphertextAndTag.data(),
                    ciphertextAndTag.size(), plaintext);
    }
private:
    NG_DISABLE_COPY(Aead)
    class AeadPrivate;
    AeadPrivate * const d;
};

// RFC 5869
std::string hkdfExtract(MessageDigest::Algorithm hash, const char *salt, size_t saltSize, const char *ikm,
                        size_t ikmSize);
inline std::string hkdfExtract(MessageDigest::Algorithm hash, const std::string &salt, const std::string &ikm)
{
    return hkdfExtract(hash, salt.data(), salt.size(), ikm.data(), ikm.size());
}
std::string hkdfExpand(MessageDigest::Algorithm hash, const char *prk, size_t prkSize, const char *info,
                       size_t infoSize, std::size_t outLen);
inline std::string hkdfExpand(MessageDigest::Algorithm hash, const std::string &prk, const std::string &info,
                              std::size_t outLen)
{
    return hkdfExpand(hash, prk.data(), prk.size(), info.data(), info.size(), outLen);
}
std::string hkdf(MessageDigest::Algorithm hash, const char *ikm, size_t ikmSize, const char *salt, size_t saltSize,
                 const char *info, size_t infoSize, std::size_t outLen);
inline std::string hkdf(MessageDigest::Algorithm hash, const std::string &ikm, const std::string &salt,
                        const std::string &info, std::size_t outLen)
{
    return hkdf(hash, ikm.data(), ikm.size(), salt.data(), salt.size(), info.data(), info.size(), outLen);
}

// TLS 1.3 / QUIC HKDF-Expand-Label (RFC 8446 §7.1). label is without the "tls13 " prefix.
std::string hkdfExpandLabel(MessageDigest::Algorithm hash, const char *secret, size_t secretSize,
                            const std::string &label, const char *context, size_t contextSize, std::size_t outLen);
inline std::string hkdfExpandLabel(MessageDigest::Algorithm hash, const std::string &secret, const std::string &label,
                                   const std::string &context, std::size_t outLen)
{
    return hkdfExpandLabel(hash, secret.data(), secret.size(), label, context.data(), context.size(), outLen);
}

// Single-block AES encrypt (for QUIC header protection mask). key is 16 or 32 bytes.
std::string aesEcbEncryptBlock(const char *key, size_t keySize, const char *block16, size_t blockSize);
inline std::string aesEcbEncryptBlock(const std::string &key, const std::string &block16)
{
    return aesEcbEncryptBlock(key.data(), key.size(), block16.data(), block16.size());
}

}  // namespace qtng

#endif  // QTNG_AEAD_H
