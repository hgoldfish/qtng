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

    bool setKey(const std::string &key);

    // ciphertextAndTag = ciphertext || tag
    bool seal(const std::string &nonce, const std::string &aad, const std::string &plaintext,
              std::string *ciphertextAndTag) const;
    bool open(const std::string &nonce, const std::string &aad, const std::string &ciphertextAndTag,
              std::string *plaintext) const;
private:
    NG_DISABLE_COPY(Aead)
    class AeadPrivate;
    AeadPrivate * const d;
};

// RFC 5869
std::string hkdfExtract(MessageDigest::Algorithm hash, const std::string &salt, const std::string &ikm);
std::string hkdfExpand(MessageDigest::Algorithm hash, const std::string &prk, const std::string &info,
                       std::size_t outLen);
std::string hkdf(MessageDigest::Algorithm hash, const std::string &ikm, const std::string &salt,
                 const std::string &info, std::size_t outLen);

// TLS 1.3 / QUIC HKDF-Expand-Label (RFC 8446 §7.1). label is without the "tls13 " prefix.
std::string hkdfExpandLabel(MessageDigest::Algorithm hash, const std::string &secret, const std::string &label,
                            const std::string &context, std::size_t outLen);

// Single-block AES encrypt (for QUIC header protection mask). key is 16 or 32 bytes.
std::string aesEcbEncryptBlock(const std::string &key, const std::string &block16);

}  // namespace qtng

#endif  // QTNG_AEAD_H
