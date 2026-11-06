#include "qtng/aead.h"

#include <climits>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "qtng/private/crypto_p.h"

using namespace std;

namespace qtng {

namespace {

const EVP_MD *mdFor(MessageDigest::Algorithm hash)
{
    return getOpenSSL_MD(hash);
}

int hashLen(MessageDigest::Algorithm hash)
{
    const EVP_MD *md = mdFor(hash);
    return md ? EVP_MD_size(md) : 0;
}

}  // namespace

class Aead::AeadPrivate
{
public:
    explicit AeadPrivate(Algorithm a)
        : algo(a)
        , valid(false)
    {
        switch (a) {
        case Aes128Gcm:
            cipher = EVP_aes_128_gcm();
            keyLen = 16;
            nonceLen = 12;
            tagLen = 16;
            break;
        case Aes256Gcm:
            cipher = EVP_aes_256_gcm();
            keyLen = 32;
            nonceLen = 12;
            tagLen = 16;
            break;
        case ChaCha20Poly1305:
            cipher = EVP_chacha20_poly1305();
            keyLen = 32;
            nonceLen = 12;
            tagLen = 16;
            break;
        default:
            cipher = nullptr;
            keyLen = nonceLen = tagLen = 0;
            return;
        }
        valid = (cipher != nullptr);
    }

    Algorithm algo;
    const EVP_CIPHER *cipher;
    string key;
    int keyLen;
    int nonceLen;
    int tagLen;
    bool valid;
};

Aead::Aead(Algorithm algo)
    : d(new AeadPrivate(algo))
{
}

Aead::~Aead()
{
    delete d;
}

bool Aead::isValid() const
{
    return d->valid;
}

Aead::Algorithm Aead::algorithm() const
{
    return d->algo;
}

int Aead::keySize() const
{
    return d->keyLen;
}

int Aead::nonceSize() const
{
    return d->nonceLen;
}

int Aead::tagSize() const
{
    return d->tagLen;
}

bool Aead::setKey(const char *key, size_t keySize)
{
    if (!d->valid || !key || keySize != static_cast<size_t>(d->keyLen)) {
        return false;
    }
    d->key.assign(key, keySize);
    return true;
}

bool Aead::seal(const char *nonce, size_t nonceSize, const char *aad, size_t aadSize, const char *plaintext,
                size_t plaintextSize, string *ciphertextAndTag) const
{
    if (!ciphertextAndTag || !d->valid || d->key.empty() || !nonce
        || nonceSize != static_cast<size_t>(d->nonceLen) || aadSize > static_cast<size_t>(INT_MAX)
        || plaintextSize > static_cast<size_t>(INT_MAX) || (aadSize > 0 && !aad)
        || (plaintextSize > 0 && !plaintext)) {
        return false;
    }
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, d->cipher, nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, d->nonceLen, nullptr) != 1) {
            break;
        }
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(d->key.data()),
                               reinterpret_cast<const unsigned char *>(nonce))
            != 1) {
            break;
        }
        int len = 0;
        if (aadSize > 0) {
            if (EVP_EncryptUpdate(ctx, nullptr, &len, reinterpret_cast<const unsigned char *>(aad),
                                  static_cast<int>(aadSize))
                != 1) {
                break;
            }
        }
        string out;
        out.resize(plaintextSize + static_cast<size_t>(d->tagLen));
        unsigned char *outPtr = reinterpret_cast<unsigned char *>(&out[0]);
        int outLen = 0;
        if (plaintextSize > 0) {
            if (EVP_EncryptUpdate(ctx, outPtr, &outLen, reinterpret_cast<const unsigned char *>(plaintext),
                                  static_cast<int>(plaintextSize))
                != 1) {
                break;
            }
        }
        int finalLen = 0;
        if (EVP_EncryptFinal_ex(ctx, outPtr + outLen, &finalLen) != 1) {
            break;
        }
        outLen += finalLen;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, d->tagLen, outPtr + outLen) != 1) {
            break;
        }
        out.resize(static_cast<size_t>(outLen + d->tagLen));
        *ciphertextAndTag = std::move(out);
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool Aead::open(const char *nonce, size_t nonceSize, const char *aad, size_t aadSize, const char *ciphertextAndTag,
                size_t ciphertextAndTagSize, string *plaintext) const
{
    if (!plaintext || !d->valid || d->key.empty() || !nonce || !ciphertextAndTag
        || nonceSize != static_cast<size_t>(d->nonceLen) || ciphertextAndTagSize < static_cast<size_t>(d->tagLen)
        || aadSize > static_cast<size_t>(INT_MAX) || ciphertextAndTagSize > static_cast<size_t>(INT_MAX)
        || (aadSize > 0 && !aad)) {
        return false;
    }
    const size_t ctLen = ciphertextAndTagSize - static_cast<size_t>(d->tagLen);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, d->cipher, nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, d->nonceLen, nullptr) != 1) {
            break;
        }
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(d->key.data()),
                               reinterpret_cast<const unsigned char *>(nonce))
            != 1) {
            break;
        }
        int len = 0;
        if (aadSize > 0) {
            if (EVP_DecryptUpdate(ctx, nullptr, &len, reinterpret_cast<const unsigned char *>(aad),
                                  static_cast<int>(aadSize))
                != 1) {
                break;
            }
        }
        string out;
        out.resize(ctLen);
        unsigned char *outPtr = ctLen ? reinterpret_cast<unsigned char *>(&out[0]) : nullptr;
        int outLen = 0;
        if (ctLen > 0) {
            if (EVP_DecryptUpdate(ctx, outPtr, &outLen, reinterpret_cast<const unsigned char *>(ciphertextAndTag),
                                  static_cast<int>(ctLen))
                != 1) {
                break;
            }
        }
        unsigned char tag[16];
        memcpy(tag, ciphertextAndTag + ctLen, static_cast<size_t>(d->tagLen));
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, d->tagLen, tag) != 1) {
            break;
        }
        int finalLen = 0;
        if (EVP_DecryptFinal_ex(ctx, outPtr ? outPtr + outLen : nullptr, &finalLen) != 1) {
            break;
        }
        out.resize(static_cast<size_t>(outLen + finalLen));
        *plaintext = std::move(out);
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

string hkdfExtract(MessageDigest::Algorithm hash, const char *salt, size_t saltSize, const char *ikm, size_t ikmSize)
{
    const EVP_MD *md = mdFor(hash);
    if (!md || saltSize > static_cast<size_t>(INT_MAX) || (saltSize > 0 && !salt) || (ikmSize > 0 && !ikm)) {
        return string();
    }
    const int hlen = EVP_MD_size(md);
    string realSalt;
    if (saltSize == 0) {
        realSalt.assign(static_cast<size_t>(hlen), '\0');
        salt = realSalt.data();
        saltSize = realSalt.size();
    }
    unsigned int prkLen = 0;
    unsigned char prk[EVP_MAX_MD_SIZE];
    if (!HMAC(md, salt, static_cast<int>(saltSize), reinterpret_cast<const unsigned char *>(ikm), ikmSize, prk,
              &prkLen)) {
        return string();
    }
    return string(reinterpret_cast<char *>(prk), prkLen);
}

string hkdfExpand(MessageDigest::Algorithm hash, const char *prk, size_t prkSize, const char *info, size_t infoSize,
                  size_t outLen)
{
    const EVP_MD *md = mdFor(hash);
    if (!md || !prk || prkSize == 0 || prkSize > static_cast<size_t>(INT_MAX) || outLen == 0
        || (infoSize > 0 && !info)) {
        return string();
    }
    string out;
    out.reserve(outLen);
    string t;
    unsigned char counter = 1;
    while (out.size() < outLen) {
        string blockIn = t;
        if (infoSize > 0) {
            blockIn.append(info, infoSize);
        }
        blockIn.push_back(static_cast<char>(counter++));
        unsigned int mdLen = 0;
        unsigned char block[EVP_MAX_MD_SIZE];
        if (!HMAC(md, prk, static_cast<int>(prkSize), reinterpret_cast<const unsigned char *>(blockIn.data()),
                  blockIn.size(), block, &mdLen)) {
            return string();
        }
        t.assign(reinterpret_cast<char *>(block), mdLen);
        out.append(t);
    }
    out.resize(outLen);
    return out;
}

string hkdf(MessageDigest::Algorithm hash, const char *ikm, size_t ikmSize, const char *salt, size_t saltSize,
            const char *info, size_t infoSize, size_t outLen)
{
    const string prk = hkdfExtract(hash, salt, saltSize, ikm, ikmSize);
    return hkdfExpand(hash, prk.data(), prk.size(), info, infoSize, outLen);
}

string hkdfExpandLabel(MessageDigest::Algorithm hash, const char *secret, size_t secretSize, const string &label,
                       const char *context, size_t contextSize, size_t outLen)
{
    if (!secret || (contextSize > 0 && !context)) {
        return string();
    }
    string fullLabel = "tls13 ";
    fullLabel.append(label);
    if (fullLabel.size() < 7 || fullLabel.size() > 255 || contextSize > 255 || outLen > 65535) {
        return string();
    }
    string info;
    info.push_back(static_cast<char>((outLen >> 8) & 0xff));
    info.push_back(static_cast<char>(outLen & 0xff));
    info.push_back(static_cast<char>(fullLabel.size()));
    info.append(fullLabel);
    info.push_back(static_cast<char>(contextSize));
    if (contextSize > 0) {
        info.append(context, contextSize);
    }
    return hkdfExpand(hash, secret, secretSize, info.data(), info.size(), outLen);
}

string aesEcbEncryptBlock(const char *key, size_t keySize, const char *block16, size_t blockSize)
{
    if (!key || !block16 || (keySize != 16 && keySize != 32) || blockSize != 16) {
        return string();
    }
    const EVP_CIPHER *cipher = (keySize == 16) ? EVP_aes_128_ecb() : EVP_aes_256_ecb();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return string();
    }
    string out(16, '\0');
    int outLen = 0;
    bool ok = EVP_EncryptInit_ex(ctx, cipher, nullptr, reinterpret_cast<const unsigned char *>(key), nullptr) == 1
            && EVP_CIPHER_CTX_set_padding(ctx, 0) == 1
            && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(&out[0]), &outLen,
                                reinterpret_cast<const unsigned char *>(block16), 16)
                    == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok || outLen != 16) {
        return string();
    }
    return out;
}

}  // namespace qtng
