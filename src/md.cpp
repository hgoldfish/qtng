#include "qtng/md.h"
#include "qtng/private/crypto_p.h"

using namespace std;

namespace qtng {

const EVP_MD *getOpenSSL_MD(MessageDigest::Algorithm algo)
{
    const EVP_MD *md = nullptr;
    switch (algo) {
    case MessageDigest::Md4:
#ifndef OPENSSL_NO_MD4
        md = EVP_md4();
#endif
        break;
    case MessageDigest::Md5:
#ifndef OPENSSL_NO_MD5
        md = EVP_md5();
#endif
        break;
    case MessageDigest::Sha1:
#ifndef OPENSSL_NO_SHA
        md = EVP_sha1();
#endif
        break;
    case MessageDigest::Sha224:
#ifndef OPENSSL_NO_SHA256
        md = EVP_sha224();
#endif
        break;
    case MessageDigest::Sha256:
#ifndef OPENSSL_NO_SHA256
        md = EVP_sha256();
#endif
        break;
    case MessageDigest::Sha384:
#ifndef OPENSSL_NO_SHA512
        md = EVP_sha384();
#endif
        break;
    case MessageDigest::Sha512:
#ifndef OPENSSL_NO_SHA512
        md = EVP_sha512();
#endif
        break;
    case MessageDigest::Ripemd160:
#ifndef OPENSSL_NO_RIPEMD
        md = EVP_ripemd160();
#endif
        break;
    case MessageDigest::Whirlpool:
        // LibreSSL 4.0+ removed Whirlpool; OPENSSL_NO_WHIRLPOOL is always defined there.
#ifndef OPENSSL_NO_WHIRLPOOL
        md = EVP_whirlpool();
#endif
        break;
    default:
        NG_UNREACHABLE();
    }
    return md;
}

class MessageDigestPrivate
{
public:
    MessageDigestPrivate(MessageDigest::Algorithm algo);
    ~MessageDigestPrivate();
    void addData(const char *buf, int len);
    string result();
    EVP_MD_CTX *context;
    string finalData;
    MessageDigest::Algorithm algo;
    bool hasError;
};

MessageDigestPrivate::MessageDigestPrivate(MessageDigest::Algorithm algo)
    : context(nullptr)
    , algo(algo)
    , hasError(false)
{
    initOpenSSL();
    const EVP_MD *md = getOpenSSL_MD(algo);

    if (!md) {
        hasError = true;
        return;
    }

    context = EVP_MD_CTX_new();

    if (!context) {
        hasError = true;
        return;
    }
    if (!EVP_DigestInit_ex(context, md, nullptr)) {
        EVP_MD_CTX_free(context);
        context = nullptr;
        hasError = true;
        return;
    }
}

MessageDigestPrivate::~MessageDigestPrivate()
{
    if (context) {
        EVP_MD_CTX_free(context);
    }
    cleanupOpenSSL();
}

void MessageDigestPrivate::addData(const char *buf, int len)
{
    if (hasError)
        return;
    int rvalue = EVP_DigestUpdate(context, buf, static_cast<size_t>(len));
    hasError = !rvalue;
}

string MessageDigestPrivate::result()
{
    if (hasError) {
        return string();
    }
    if (!finalData.empty()) {
        return finalData;
    }
    unsigned int len;
    finalData.resize(EVP_MAX_MD_SIZE);
    int rvalue = EVP_DigestFinal_ex(context, reinterpret_cast<unsigned char *>(&finalData[0]), &len);
    if (!rvalue) {
        hasError = true;
        finalData.clear();
    } else {
        finalData.resize(static_cast<int>(len));
    }

    return finalData;
}

MessageDigest::MessageDigest(MessageDigest::Algorithm algo)
    : d_ptr(new MessageDigestPrivate(algo))
{
}

MessageDigest::~MessageDigest()
{
    delete d_ptr;
}

void MessageDigest::addData(const char *data, int len)
{
    NG_D(MessageDigest);
    d->addData(data, len);
}

string MessageDigest::result()
{
    NG_D(MessageDigest);
    return d->result();
}

string PBKDF2_HMAC(int keylen, const string &password, const string &salt,
                       const MessageDigest::Algorithm hashAlgo, int i)
{
    initOpenSSL();
    const EVP_MD *dgst = getOpenSSL_MD(hashAlgo);

    if (!dgst || salt.empty() || password.empty() || i <= 0) {
        return string();
    }

    string key;
    key.resize(keylen);

    int rvalue =
            PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), reinterpret_cast<const unsigned char *>(salt.data()),
                              salt.size(), i, dgst, keylen, reinterpret_cast<unsigned char *>(&key[0]));
    if (rvalue) {
        return key;
    } else {
        return string();
    }
}

// string scrypt(const string &password, int keylen, const string &salt,
//                   int n, int r, int p)
//{
//     initOpenSSL();
//     EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_SCRYPT, nullptr);

//    if(!pctx || password.empty() || salt.empty()) {
//        return string();
//    }

//    string key;
//    key.resize(keylen);

//    int rvalue = EVP_PKEY_derive_init(pctx);
//    if (rvalue <= 0) {
//        ngDebug() << "can not init scrypt kdf.";
//        return string();
//    }
//    rvalue = EVP_PKEY_CTX_set1_pbe_pass(pctx, password.get(), password.size());
//    if (rvalue <= 0) {
//        ngDebug() << "can not set scrypt password.";
//        return string();
//    }
//    rvalue = EVP_PKEY_CTX_set1_scrypt_salt(pctx, salt.data(), salt.size());
//    if (rvalue <= 0) {
//        ngDebug() << "can not set scrypt salt.";
//        return string();
//    }

//}

}  // namespace qtng
