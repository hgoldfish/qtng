#ifndef QTNG_CIPHER_H
#define QTNG_CIPHER_H

#include <string>
#include <utility>

#include "qtng/md.h"
#include "qtng/utils/platform.h"

namespace qtng {

class CipherPrivate;
class Cipher
{
public:
    enum Algorithm {
        Null = 1,
        AES128 = 2,
        AES192 = 3,
        AES256 = 4,
        DES = 5,
        DES2 = 6,
        DES3 = 7,
        Blowfish = 8,
        CAST5 = 9,
        Chacha20 = 10,
        ChaCha20Poly1305 = 11
    };
    enum Mode {
        ECB = 1,
        CBC = 2,
        CFB = 3,
        // PGP = 4,
        OFB = 5,
        CTR = 6,
        OPENPGP = 7,
    };
    enum Operation {
        Encrypt = 1,
        Decrypt = 2,
    };
public:
    Cipher(Algorithm alog, Mode mode, Operation operation);
    virtual ~Cipher();
    Cipher *copy(Operation operation);
public:
    bool isValid() const;
    bool isStream() const;
    bool isBlock() const { return !isStream(); }
    bool setKey(const std::string &key);
    std::string key() const;
    bool setInitialVector(const std::string &iv);
    std::string initialVector() const;
    inline std::string iv() const { return initialVector(); }
    bool setPassword(const std::string &password, const std::string &salt,
                     const MessageDigest::Algorithm hashAlgo = MessageDigest::Sha256,
                     int i = 100000 /* same as django PBKDF2*/);
    bool setOpensslPassword(const std::string &password, const std::string &salt,
                            const MessageDigest::Algorithm hashAlgo = MessageDigest::Md5,
                            int i = 1);  // same as openssl command line.
    std::string salt() const;
    std::string saltHeader() const;  // `openssl enc` generate a header contains salt
    bool setPadding(bool padding);
    bool padding() const;
    int keySize() const;  // in bytes.
    int ivSize() const;  // in bytes.
    int blockSize() const;  // in bytes.
public:
    std::string addData(const std::string &data) { return addData(data.data(), data.size()); }
    std::string addData(const char *data, int len);
    std::string finalData();
public:
    std::string update(const std::string &data) { return addData(data.data(), data.size()); }
    std::string update(const char *data, int len) { return addData(data, len); }
    std::string final() { return finalData(); }
public:
    static std::pair<std::string, std::string> parseSalt(const std::string &header);  // parse salt from `openssl enc` header
private:
    CipherPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(Cipher)
};

}  // namespace qtng

#endif  // QTNG_CIPHER_H
