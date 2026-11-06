#ifndef QTNG_MD_H
#define QTNG_MD_H

#include <cstddef>
#include <string>

#include "qtng/crypto.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

class MessageDigestPrivate;
class MessageDigest
{
public:
    enum Algorithm {
        Md5 = 1,  // checksums and legacy protocols; not collision-resistant
        Sha1 = 2,  // WebSocket, BitTorrent, and Kademlia
        Sha224 = 3,
        Sha256 = 4,
        Sha384 = 5,
        Sha512 = 6,
        Sha3_224 = 7,
        Sha3_256 = 8,
        Sha3_384 = 9,
        Sha3_512 = 10,
        Ripemd160 = 11,  // inner hash of Bitcoin HASH160
        Sha512_224 = 13,
        Sha512_256 = 14,
        Blake2s_256 = 15,
        Blake2b_512 = 16,
        Sm3 = 17
    };
public:
    explicit MessageDigest(Algorithm algo);
    virtual ~MessageDigest();
public:
    inline void addData(const std::string &data) { addData(data.data(), data.size()); }
    void addData(const char *data, size_t len);
    std::string result();
public:
    inline void update(const std::string &data) { addData(data.data(), data.size()); }
    inline void update(const char *data, size_t len) { addData(data, len); }
    inline std::string digest() { return result(); }
    inline std::string hexDigest() { return qtng::utils::bytesToHex(result()); }
public:
    static std::string hash(const char *data, size_t dataSize, Algorithm algo);
    inline static std::string hash(const std::string &data, Algorithm algo)
    {
        return hash(data.data(), data.size(), algo);
    }
    static std::string hash(const char *, Algorithm) = delete;
    static std::string digest(const char *data, size_t dataSize, Algorithm algo);
    inline static std::string digest(const std::string &data, Algorithm algo)
    {
        return digest(data.data(), data.size(), algo);
    }
    static std::string digest(const char *, Algorithm) = delete;
private:
    MessageDigestPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(MessageDigest)
    NG_DISABLE_COPY_MOVE(MessageDigest)
};

inline std::string MessageDigest::hash(const char *data, size_t dataSize, Algorithm algo)
{
    MessageDigest m(algo);
    m.addData(data, dataSize);
    return qtng::utils::bytesToHex(m.result());
}

inline std::string MessageDigest::digest(const char *data, size_t dataSize, Algorithm algo)
{
    MessageDigest m(algo);
    m.addData(data, dataSize);
    return m.result();
}

// HMAC (RFC 2104) using the given digest. Returns empty string on failure
// (unsupported digest or empty key).
std::string hmac(const MessageDigest::Algorithm hashAlgo, const char *key, size_t keySize, const char *data,
                 size_t dataSize);
inline std::string hmac(const MessageDigest::Algorithm hashAlgo, const std::string &key, const std::string &data)
{
    return hmac(hashAlgo, key.data(), key.size(), data.data(), data.size());
}
std::string hmac(const MessageDigest::Algorithm, const char *, const char *) = delete;

std::string PBKDF2_HMAC(int keylen, const std::string &password, const std::string &salt,
                       const MessageDigest::Algorithm hashAlgo = MessageDigest::Sha256, int i = 10000);

}  // namespace qtng

#endif  // QTNG_MD_H
