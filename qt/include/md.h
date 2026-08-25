#ifndef QTNG_MD_H
#define QTNG_MD_H

#include <QtCore/qbytearray.h>
#include "crypto.h"

QTNETWORKNG_NAMESPACE_BEGIN

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
    inline void addData(const QByteArray &data) { addData(data.constData(), data.size()); }
    void addData(const char *data, int len);
    QByteArray result();
public:
    inline void update(const QByteArray &data) { addData(data.constData(), data.size()); }
    inline void update(const char *data, int len) { addData(data, len); }
    inline QByteArray digest() { return result(); }
    inline QByteArray hexDigest() { return result().toHex(); }
public:
    static QByteArray hash(const QByteArray &data, Algorithm algo);
    static QByteArray digest(const QByteArray &data, Algorithm algo);
private:
    MessageDigestPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(MessageDigest)
    Q_DISABLE_COPY(MessageDigest)
    MessageDigest(MessageDigest &&) = delete;
    MessageDigest &operator=(MessageDigest &&) = delete;
};

inline QByteArray MessageDigest::hash(const QByteArray &data, Algorithm algo)
{
    MessageDigest m(algo);
    m.addData(data);
    return m.result().toHex();
}

inline QByteArray MessageDigest::digest(const QByteArray &data, Algorithm algo)
{
    MessageDigest m(algo);
    m.addData(data);
    return m.result();
}

QByteArray PBKDF2_HMAC(int keylen, const QByteArray &password, const QByteArray &salt,
                       const MessageDigest::Algorithm hashAlgo = MessageDigest::Sha256, int i = 10000);

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_MD_H
