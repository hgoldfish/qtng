#ifndef QTNG_MD_H
#define QTNG_MD_H

#include <string>

#include "qtng/crypto.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

class MessageDigestPrivate;
class MessageDigest
{
public:
    enum Algorithm {
        Md4 = 0,
        Md5 = 1,
        Sha1 = 2,
        Sha224 = 3,
        Sha256 = 4,
        Sha384 = 5,
        Sha512 = 6,
        Ripemd160 = 11,
        Whirlpool = 12
    };
public:
    explicit MessageDigest(Algorithm algo);
    virtual ~MessageDigest();
public:
    inline void addData(const std::string &data) { addData(data.data(), data.size()); }
    void addData(const char *data, int len);
    std::string result();
public:
    inline void update(const std::string &data) { addData(data.data(), data.size()); }
    inline void update(const char *data, int len) { addData(data, len); }
    inline std::string digest() { return result(); }
    inline std::string hexDigest() { return qtng::utils::bytesToHex(result()); }
public:
    static std::string hash(const std::string &data, Algorithm algo);
    static std::string digest(const std::string &data, Algorithm algo);
private:
    MessageDigestPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(MessageDigest)
};

inline std::string MessageDigest::hash(const std::string &data, Algorithm algo)
{
    MessageDigest m(algo);
    m.addData(data);
    return qtng::utils::bytesToHex(m.result());
}

inline std::string MessageDigest::digest(const std::string &data, Algorithm algo)
{
    MessageDigest m(algo);
    m.addData(data);
    return m.result();
}

std::string PBKDF2_HMAC(int keylen, const std::string &password, const std::string &salt,
                       const MessageDigest::Algorithm hashAlgo = MessageDigest::Sha256, int i = 10000);

std::string scrypt(int keylen, const std::string &password, const std::string &salt, int n = 1048576, int r = 8,
                  int p = 1);

}  // namespace qtng

#endif  // QTNG_MD_H
