#ifndef QTNG_PKEY_H
#define QTNG_PKEY_H

#include <memory>
#include <string>
#include <utility>

#include "qtng/cipher.h"
#include "qtng/utils/platform.h"

namespace qtng {

class PrivateKey;
class PrivateKeyWriterPrivate;
class PrivateKeyReaderPrivate;
class PublicKeyPrivate;
class PublicKey
{
public:
    enum Algorithm {
        Opaque = 0,
        Rsa = 1,
        Dsa = 2,
        Ec = 3,
    };

    enum RsaPadding {  // copy from openssl header
        PKCS1_PADDING = 1,
        NO_PADDING = 3,
        PKCS1_OAEP_PADDING = 4,
        //        SSLV23_PADDING = 2,
        //        X931_PADDING = 5,
        //        PKCS1_PSS_PADDING = 6,
    };
public:
    PublicKey();
    ~PublicKey();
    PublicKey(const PublicKey &other);
    PublicKey(PublicKey &&other)
        : d_ptr(0)
    {
        std::swap(d_ptr, other.d_ptr);
    }
public:
    void *handle() const;
    bool isNull() const;
    bool isValid() const;
    Algorithm algorithm() const;
    int bits() const;
    bool verify(const char *data, size_t dataSize, const char *hash, size_t hashSize,
                MessageDigest::Algorithm hashAlgo);
    inline bool verify(const std::string &data, const std::string &hash, MessageDigest::Algorithm hashAlgo)
    {
        return verify(data.data(), data.size(), hash.data(), hash.size(), hashAlgo);
    }
    bool verify(const char *, const std::string &, MessageDigest::Algorithm) = delete;
    bool verify(const std::string &, const char *, MessageDigest::Algorithm) = delete;
    std::string encrypt(const char *data, size_t dataSize) const;
    inline std::string encrypt(const std::string &data) const { return encrypt(data.data(), data.size()); }
    std::string encrypt(const char *) const = delete;
    std::string digest(MessageDigest::Algorithm algorithm = MessageDigest::Sha256) const;
public:
    std::string rsaPublicEncrypt(const char *data, size_t dataSize, RsaPadding padding = PKCS1_PADDING) const;
    inline std::string rsaPublicEncrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING) const
    {
        return rsaPublicEncrypt(data.data(), data.size(), padding);
    }
    std::string rsaPublicEncrypt(const char *, RsaPadding) const = delete;
    std::string rsaPublicDecrypt(const char *data, size_t dataSize, RsaPadding padding = PKCS1_PADDING) const;
    inline std::string rsaPublicDecrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING) const
    {
        return rsaPublicDecrypt(data.data(), data.size(), padding);
    }
    std::string rsaPublicDecrypt(const char *, RsaPadding) const = delete;
public:
    static PublicKey load(const std::string &data, Ssl::EncodingFormat format = Ssl::Pem);
    std::string save(Ssl::EncodingFormat format = Ssl::Pem) const;
public:
    PublicKey &operator=(const PublicKey &other);
    bool operator==(const PublicKey &other) const;
    bool operator==(const PrivateKey &) const { return false; }
    bool operator!=(const PublicKey &other) const { return !(*this == other); }
    bool operator!=(const PrivateKey &) const { return true; }
protected:
    PublicKeyPrivate *d_ptr;
    NG_DECLARE_PRIVATE(PublicKey)
    friend class PrivateKeyWriterPrivate;
    friend class PrivateKeyReaderPrivate;
};

class PrivateKey : public PublicKey
{
public:
    PrivateKey()
        : PublicKey()
    {
    }
    PrivateKey(const PrivateKey &other)
        : PublicKey(other)
    {
    }
    PrivateKey(PrivateKey &&other)
        : PublicKey(other)
    {
    }
    PrivateKey &operator=(const PublicKey &other)
    {
        PublicKey::operator=(other);
        return *this;
    }
    PrivateKey &operator=(const PrivateKey &other)
    {
        PublicKey::operator=(other);
        return *this;
    }
    bool operator==(const PrivateKey &other) const;
    bool operator==(const PublicKey &) const { return false; }
    bool operator!=(const PrivateKey &other) const { return !(*this == other); }
    bool operator!=(const PublicKey &) const { return true; }
public:
    PublicKey publicKey() const;
    std::string sign(const char *data, size_t dataSize, MessageDigest::Algorithm hashAlgo);
    inline std::string sign(const std::string &data, MessageDigest::Algorithm hashAlgo)
    {
        return sign(data.data(), data.size(), hashAlgo);
    }
    std::string sign(const char *, MessageDigest::Algorithm) = delete;
    std::string decrypt(const char *data, size_t dataSize) const;
    inline std::string decrypt(const std::string &data) const { return decrypt(data.data(), data.size()); }
    std::string decrypt(const char *) const = delete;
public:
    std::string rsaPrivateEncrypt(const char *data, size_t dataSize, RsaPadding padding = PKCS1_PADDING) const;
    inline std::string rsaPrivateEncrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING) const
    {
        return rsaPrivateEncrypt(data.data(), data.size(), padding);
    }
    std::string rsaPrivateEncrypt(const char *, RsaPadding) const = delete;
    std::string rsaPrivateDecrypt(const char *data, size_t dataSize, RsaPadding padding = PKCS1_PADDING) const;
    inline std::string rsaPrivateDecrypt(const std::string &data, RsaPadding padding = PKCS1_PADDING) const
    {
        return rsaPrivateDecrypt(data.data(), data.size(), padding);
    }
    std::string rsaPrivateDecrypt(const char *, RsaPadding) const = delete;
public:
    static PrivateKey generate(Algorithm algo, int bits);
    static PrivateKey load(const std::string &data, Ssl::EncodingFormat format = Ssl::Pem,
                           const std::string &password = std::string());
    std::string save(Ssl::EncodingFormat format = Ssl::Pem, const std::string &password = std::string()) const;
    std::string savePublic(Ssl::EncodingFormat format = Ssl::Pem) const { return PublicKey::save(format); }
private:
    friend class PublicKeyPrivate;
    friend class PrivateKeyReaderPrivate;
    friend class PrivateKeyWriterPrivate;
};

class PasswordCallback
{
public:
    virtual ~PasswordCallback() = default;
    virtual std::string get(bool writing) = 0;
};

class PrivateKeyWriter
{
public:
    explicit PrivateKeyWriter(const PrivateKey &key);
    explicit PrivateKeyWriter(const PublicKey &key);
    ~PrivateKeyWriter();
public:
    PrivateKeyWriter &setCipher(Cipher::Algorithm algo, Cipher::Mode mode);
    PrivateKeyWriter &setPassword(const std::string &password);
    PrivateKeyWriter &setPassword(std::shared_ptr<PasswordCallback> callback);
    PrivateKeyWriter &setPublicOnly(bool publicOnly);
    std::string asPem();
    std::string asDer();
    bool save(const std::string &filePath);
private:
    NG_DECLARE_PRIVATE(PrivateKeyWriter)
    NG_DISABLE_COPY_MOVE(PrivateKeyWriter)
    PrivateKeyWriterPrivate * const d_ptr;
};

class PrivateKeyReader
{
public:
    PrivateKeyReader();
    ~PrivateKeyReader();
public:
    PrivateKeyReader &setPassword(const std::string &password);
    PrivateKeyReader &setPassword(std::shared_ptr<PasswordCallback> callback);
    PrivateKeyReader &setFormat(Ssl::EncodingFormat format);
    PrivateKey read(const char *data, size_t dataSize);
    inline PrivateKey read(const std::string &data) { return read(data.data(), data.size()); }
    PublicKey readPublic(const char *data, size_t dataSize);
    inline PublicKey readPublic(const std::string &data) { return readPublic(data.data(), data.size()); }
    PrivateKey readFile(const std::string &filePath);
    PublicKey readPublicFile(const std::string &filePath);
private:
    NG_DECLARE_PRIVATE(PrivateKeyReader)
    NG_DISABLE_COPY_MOVE(PrivateKeyReader)
    PrivateKeyReaderPrivate * const d_ptr;
};

}  // namespace qtng

#endif  // QTNG_PKEY_H
