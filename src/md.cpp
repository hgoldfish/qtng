#include "qtng/md.h"
#include "qtng/private/crypto_p.h"

#include <openssl/hmac.h>

using namespace std;

namespace qtng {

const EVP_MD *getOpenSSL_MD(MessageDigest::Algorithm algo)
{
    const char *name = nullptr;
    switch (algo) {
    case MessageDigest::Md5:
        name = "md5";
        break;
    case MessageDigest::Sha1:
        name = "sha1";
        break;
    case MessageDigest::Sha224:
        name = "sha224";
        break;
    case MessageDigest::Sha256:
        name = "sha256";
        break;
    case MessageDigest::Sha384:
        name = "sha384";
        break;
    case MessageDigest::Sha512:
        name = "sha512";
        break;
    case MessageDigest::Sha3_224:
        name = "sha3-224";
        break;
    case MessageDigest::Sha3_256:
        name = "sha3-256";
        break;
    case MessageDigest::Sha3_384:
        name = "sha3-384";
        break;
    case MessageDigest::Sha3_512:
        name = "sha3-512";
        break;
    case MessageDigest::Ripemd160:
        name = "ripemd160";
        break;
    case MessageDigest::Sha512_224:
        name = "sha512-224";
        break;
    case MessageDigest::Sha512_256:
        name = "sha512-256";
        break;
    case MessageDigest::Blake2s_256:
        name = "blake2s256";
        break;
    case MessageDigest::Blake2b_512:
        name = "blake2b512";
        break;
    case MessageDigest::Sm3:
        name = "sm3";
        break;
    }
    return name ? EVP_get_digestbyname(name) : nullptr;
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

string hmac(const MessageDigest::Algorithm hashAlgo, const string &key, const string &data)
{
    const EVP_MD *dgst = getOpenSSL_MD(hashAlgo);
    if (!dgst || key.empty()) {
        return string();
    }
    unsigned int len = 0;
    unsigned char out[EVP_MAX_MD_SIZE];
    if (!HMAC(dgst, key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char *>(data.data()), data.size(), out, &len)) {
        return string();
    }
    return string(reinterpret_cast<char *>(out), len);
}

string PBKDF2_HMAC(int keylen, const string &password, const string &salt,
                       const MessageDigest::Algorithm hashAlgo, int i)
{
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

}  // namespace qtng
