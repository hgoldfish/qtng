#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <openssl/dsa.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "qtng/pkey.h"
#include "qtng/io_utils.h"
#include "qtng/private/crypto_p.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.pkey");

namespace qtng {

class PublicKeyPrivate
{
public:
    PublicKeyPrivate();
    PublicKeyPrivate(PublicKeyPrivate *other);
    ~PublicKeyPrivate();

    PublicKey::Algorithm algorithm() const;
    int bits() const;
    string sign(const string &data, MessageDigest::Algorithm hashAlgo) const;
    bool verify(const string &data, const string &hash, MessageDigest::Algorithm hashAlgo) const;
    string encrypt(const string &data) const;
    string decrypt(const string &data) const;
    bool checkValidRsaOperation(const string &data, PublicKey::RsaPadding padding, bool requirePrivate,
                                bool allowOaep) const;
    string rsaPublicEncrypt(const string &data, PublicKey::RsaPadding padding) const;
    string rsaPublicDecrypt(const string &data, PublicKey::RsaPadding padding) const;
    string rsaPrivateEncrypt(const string &data, PublicKey::RsaPadding padding) const;
    string rsaPrivateDecrypt(const string &data, PublicKey::RsaPadding padding) const;
    static PrivateKey generate(PublicKey::Algorithm algo, int bits);
    static bool setPkey(PublicKey *key, EVP_PKEY *pkey, bool hasPrivate);
    static bool adoptRawPkey(PublicKeyPrivate *d, EVP_PKEY *rawPkey, bool hasPrivate);

    EvpPkeyCtxPtr context;
    shared_ptr<EVP_PKEY> pkey;
    bool hasPrivate;
};

PublicKeyPrivate::PublicKeyPrivate()
    : hasPrivate(false)
{
}

PublicKeyPrivate::PublicKeyPrivate(PublicKeyPrivate *other)
    : hasPrivate(false)
{
    if (other->context && other->pkey) {
        context.reset(EVP_PKEY_CTX_dup(other->context.get()));
        pkey = other->pkey;
        hasPrivate = other->hasPrivate;
    }
}

PublicKeyPrivate::~PublicKeyPrivate() = default;

PublicKey::Algorithm PublicKeyPrivate::algorithm() const
{
    if (pkey) {
        int type = EVP_PKEY_base_id(pkey.get());
        switch (type) {
        case EVP_PKEY_RSA:
            return PublicKey::Rsa;
        case EVP_PKEY_DSA:
            return PublicKey::Dsa;
        case EVP_PKEY_EC:
            return PublicKey::Ec;
        default:
            return PublicKey::Opaque;
        }
    } else {
        return PublicKey::Opaque;
    }
}

int PublicKeyPrivate::bits() const
{
    if (pkey) {
        return EVP_PKEY_bits(pkey.get());
    } else {
        return 0;
    }
}

bool openssl_setPkey(PublicKey *key, EVP_PKEY *pkey, bool hasPrivate)
{
    return PublicKeyPrivate::setPkey(key, pkey, hasPrivate);
}

bool PublicKeyPrivate::setPkey(PublicKey *key, EVP_PKEY *pkey, bool hasPrivate)
{
    return adoptRawPkey(key->d_ptr, pkey, hasPrivate);
}

bool PublicKeyPrivate::adoptRawPkey(PublicKeyPrivate *d, EVP_PKEY *rawPkey, bool hasPrivate)
{
    EvpPkeyPtr pkey(rawPkey);
    EvpPkeyCtxPtr context(EVP_PKEY_CTX_new(pkey.get(), nullptr));
    if (!context) {
        return false;
    }
    d->hasPrivate = hasPrivate;
    d->context = std::move(context);
    d->pkey = shareEvpPkey(pkey.release());
    return true;
}

PrivateKey PublicKeyPrivate::generate(PublicKey::Algorithm algo, int bits)
{
    PrivateKey key;

    if (algo == PrivateKey::Rsa) {
        EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
        if (!ctx) {
            return key;
        }
        EVP_PKEY *rawPkey = nullptr;
        if (EVP_PKEY_keygen_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), bits) <= 0
            || EVP_PKEY_keygen(ctx.get(), &rawPkey) <= 0) {
            return key;
        }
        EvpPkeyPtr pkey(rawPkey);
        openssl_setPkey(&key, pkey.release(), true);
    } else if (algo == PrivateKey::Dsa) {
        EvpPkeyCtxPtr pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_DSA, nullptr));
        if (!pctx) {
            return key;
        }
        EVP_PKEY *rawParams = nullptr;
        if (EVP_PKEY_paramgen_init(pctx.get()) <= 0 || EVP_PKEY_CTX_set_dsa_paramgen_bits(pctx.get(), bits) <= 0
            || EVP_PKEY_paramgen(pctx.get(), &rawParams) <= 0) {
            return key;
        }
        EvpPkeyPtr params(rawParams);

        EvpPkeyCtxPtr kctx(EVP_PKEY_CTX_new(params.get(), nullptr));
        if (!kctx) {
            return key;
        }
        EVP_PKEY *rawPkey = nullptr;
        if (EVP_PKEY_keygen_init(kctx.get()) <= 0 || EVP_PKEY_keygen(kctx.get(), &rawPkey) <= 0) {
            return key;
        }
        EvpPkeyPtr pkey(rawPkey);
        openssl_setPkey(&key, pkey.release(), true);
    } else if (algo == PrivateKey::Ec) {
        return key;
    } else {
        NG_UNREACHABLE();
        return key;
    }
    return key;
}

string PublicKeyPrivate::sign(const string &data, MessageDigest::Algorithm hashAlgo) const
{
    if (!pkey || !hasPrivate) {
        return string();
    }

    int rvalue = 0;
    const EVP_MD *md = getOpenSSL_MD(hashAlgo);
    if (!md) {
        return string();
    }

    EvpMdCtxPtr mctx(EVP_MD_CTX_new());
    if (!mctx) {
        return string();
    }
    rvalue = EVP_DigestSignInit(mctx.get(), nullptr, md, nullptr, pkey.get());
    if (!rvalue) {
        return string();
    }
    rvalue = EVP_DigestSignUpdate(mctx.get(), data.data(), static_cast<unsigned int>(data.size()));
    if (!rvalue) {
        return string();
    }
    size_t siglen;
    rvalue = EVP_DigestSignFinal(mctx.get(), nullptr, &siglen);
    if (!rvalue) {
        return string();
    }

    string result;
    result.resize(static_cast<int>(siglen));
    rvalue = EVP_DigestSignFinal(mctx.get(), reinterpret_cast<unsigned char *>(&result[0]), &siglen);
    if (!rvalue) {
        return string();
    }
    return result;
}

bool PublicKeyPrivate::verify(const string &data, const string &hash, MessageDigest::Algorithm hashAlgo) const
{
    if (!pkey) {
        return false;
    }

    int rvalue = 0;
    const EVP_MD *md = getOpenSSL_MD(hashAlgo);
    if (!md) {
        return false;
    }

    EvpMdCtxPtr mctx(EVP_MD_CTX_new());

    if (!mctx) {
        return false;
    }
    rvalue = EVP_DigestVerifyInit(mctx.get(), nullptr, md, nullptr, pkey.get());
    if (!rvalue) {
        return false;
    }
    rvalue = EVP_DigestVerifyUpdate(mctx.get(), data.data(), static_cast<unsigned int>(data.size()));
    if (!rvalue) {
        return false;
    }
    rvalue = EVP_DigestVerifyFinal(mctx.get(), reinterpret_cast<const unsigned char *>(hash.data()),
                                   static_cast<size_t>(hash.size()));
    return rvalue != 0;
}

string PublicKeyPrivate::encrypt(const string &data) const
{
    if (algorithm() == PublicKey::Rsa) {
        return rsaPublicEncrypt(data, PublicKey::PKCS1_PADDING);
    }
    if (!pkey || !context || data.empty()) {
        return string();
    }

    int rvalue = EVP_PKEY_encrypt_init(context.get());
    if (rvalue) {
        size_t outlen = 0;
        rvalue = EVP_PKEY_encrypt(context.get(), nullptr, &outlen, reinterpret_cast<const unsigned char *>(data.data()),
                                  static_cast<unsigned int>(data.size()));
        if (rvalue && outlen) {
            string result;
            result.resize(static_cast<int>(outlen));
            rvalue = EVP_PKEY_encrypt(context.get(), reinterpret_cast<unsigned char *>(&result[0]), &outlen,
                                      reinterpret_cast<const unsigned char *>(data.data()),
                                      static_cast<unsigned int>(data.size()));
            if (rvalue) {
                result.resize(static_cast<int>(outlen));
                return result;
            }
        }
    }
    ngDebug() << "can not encrypt data.";
    return string();
}

string PublicKeyPrivate::decrypt(const string &data) const
{
    if (algorithm() == PublicKey::Rsa) {
        return rsaPrivateDecrypt(data, PublicKey::PKCS1_PADDING);
    }
    if (!pkey || !context || data.empty() || !hasPrivate) {
        return string();
    }
    int rvalue;
    rvalue = EVP_PKEY_decrypt_init(context.get());
    if (rvalue) {
        size_t outlen;
        rvalue = EVP_PKEY_decrypt(context.get(), nullptr, &outlen, reinterpret_cast<const unsigned char *>(data.data()),
                                  static_cast<unsigned int>(data.size()));
        if (rvalue && outlen) {
            string result;
            result.resize(static_cast<int>(outlen));
            rvalue = EVP_PKEY_decrypt(context.get(), reinterpret_cast<unsigned char *>(&result[0]), &outlen,
                                      reinterpret_cast<const unsigned char *>(data.data()),
                                      static_cast<unsigned int>(data.size()));
            if (rvalue) {
                result.resize(static_cast<int>(outlen));
                return result;
            }
        }
    }
    ngDebug() << "can not decrypt data.";
    return string();
}

bool PublicKeyPrivate::checkValidRsaOperation(const string &data, PublicKey::RsaPadding padding, bool requirePrivate,
                                              bool allowOaep) const
{
    if (!pkey || !context || data.empty()) {
        ngDebug() << "invalid key or data";
        return false;
    }
    if (requirePrivate && !hasPrivate) {
        ngDebug() << "not a private rsa key.";
        return false;
    }
    if (algorithm() != PublicKey::Rsa) {
        ngDebug() << "not rsa key.";
        return false;
    }
    if (padding != PublicKey::PKCS1_PADDING && padding != PublicKey::NO_PADDING) {
        if (!(allowOaep && padding == PublicKey::PKCS1_OAEP_PADDING)) {
            ngDebug() << "invalid padding:" << padding;
            return false;
        }
    }
    return true;
}

static string rsaEvpCrypt(EVP_PKEY_CTX *ctx, const string &data, PublicKey::RsaPadding padding, bool encrypt)
{
    if (!ctx) {
        return string();
    }

    int rvalue = encrypt ? EVP_PKEY_encrypt_init(ctx) : EVP_PKEY_decrypt_init(ctx);
    if (rvalue <= 0 || EVP_PKEY_CTX_set_rsa_padding(ctx, static_cast<int>(padding)) <= 0) {
        return string();
    }

    const unsigned char *in = reinterpret_cast<const unsigned char *>(data.data());
    const size_t inlen = static_cast<size_t>(data.size());
    size_t outlen = 0;
    rvalue = encrypt ? EVP_PKEY_encrypt(ctx, nullptr, &outlen, in, inlen)
                     : EVP_PKEY_decrypt(ctx, nullptr, &outlen, in, inlen);
    if (rvalue <= 0 || outlen == 0) {
        return string();
    }

    string result;
    result.resize(static_cast<int>(outlen));
    rvalue = encrypt ? EVP_PKEY_encrypt(ctx, reinterpret_cast<unsigned char *>(&result[0]), &outlen, in, inlen)
                     : EVP_PKEY_decrypt(ctx, reinterpret_cast<unsigned char *>(&result[0]), &outlen, in, inlen);
    if (rvalue <= 0) {
        return string();
    }
    result.resize(static_cast<int>(outlen));
    return result;
}

static string rsaEvpSignOrRecover(EVP_PKEY_CTX *ctx, const string &data, PublicKey::RsaPadding padding, bool sign)
{
    if (!ctx) {
        return string();
    }

    int rvalue = sign ? EVP_PKEY_sign_init(ctx) : EVP_PKEY_verify_recover_init(ctx);
    if (rvalue <= 0 || EVP_PKEY_CTX_set_rsa_padding(ctx, static_cast<int>(padding)) <= 0) {
        return string();
    }

    const unsigned char *in = reinterpret_cast<const unsigned char *>(data.data());
    const size_t inlen = static_cast<size_t>(data.size());
    size_t outlen = 0;
    rvalue = sign ? EVP_PKEY_sign(ctx, nullptr, &outlen, in, inlen)
                  : EVP_PKEY_verify_recover(ctx, nullptr, &outlen, in, inlen);
    if (rvalue <= 0 || outlen == 0) {
        return string();
    }

    string result;
    result.resize(static_cast<int>(outlen));
    rvalue = sign ? EVP_PKEY_sign(ctx, reinterpret_cast<unsigned char *>(&result[0]), &outlen, in, inlen)
                  : EVP_PKEY_verify_recover(ctx, reinterpret_cast<unsigned char *>(&result[0]), &outlen, in, inlen);
    if (rvalue <= 0) {
        return string();
    }
    result.resize(static_cast<int>(outlen));
    return result;
}

string PublicKeyPrivate::rsaPublicEncrypt(const string &data, PublicKey::RsaPadding padding) const
{
    if (!checkValidRsaOperation(data, padding, false, true)) {
        return string();
    }
    const string &result = rsaEvpCrypt(context.get(), data, padding, true);
    if (result.empty()) {
        ngDebug() << "can not public encrypt data.";
    }
    return result;
}

string PublicKeyPrivate::rsaPublicDecrypt(const string &data, PublicKey::RsaPadding padding) const
{
    // RSA_public_decrypt equivalent: recover data encrypted/signed with the private key.
    if (!checkValidRsaOperation(data, padding, false, false)) {
        return string();
    }
    const string &result = rsaEvpSignOrRecover(context.get(), data, padding, false);
    if (result.empty()) {
        ngDebug() << "can not public decrypt data.";
    }
    return result;
}

string PublicKeyPrivate::rsaPrivateEncrypt(const string &data, PrivateKey::RsaPadding padding) const
{
    // RSA_private_encrypt equivalent: raw private-key operation (sign without digest).
    if (!checkValidRsaOperation(data, padding, true, false)) {
        return string();
    }
    const string &result = rsaEvpSignOrRecover(context.get(), data, padding, true);
    if (result.empty()) {
        ngDebug() << "can not private encrypt data.";
    }
    return result;
}

string PublicKeyPrivate::rsaPrivateDecrypt(const string &data, PrivateKey::RsaPadding padding) const
{
    if (!checkValidRsaOperation(data, padding, true, true)) {
        return string();
    }
    const string &result = rsaEvpCrypt(context.get(), data, padding, false);
    if (result.empty()) {
        ngDebug() << "can not private decrypt data.";
    }
    return result;
}

struct SimplePasswordCallback : public PasswordCallback
{
public:
    SimplePasswordCallback(const string &password)
        : password(password)
    {
    }
    virtual ~SimplePasswordCallback() override;
    virtual string get(bool writing) override
    {
        (void)(writing);
        return password;
    }
    string password;
};
SimplePasswordCallback::~SimplePasswordCallback() { }

static int pem_password_cb(char *buf, int size, int rwflag, void *userdata)
{
    if (userdata == nullptr) {
        return 0;
    }

    PasswordCallback *callback = static_cast<PasswordCallback *>(userdata);
    const string &password = callback->get(rwflag == 1);
    int move = min<int>(size - 1, password.size());
    if (move <= 0) {
        return 0;
    }
#if defined(Q_CC_MSVC)
    strncpy_s(buf, size, password.data(), static_cast<size_t>(move));
#else
    strncpy(buf, password.data(), static_cast<size_t>(move));
    // buf[move - 1] = '\0';
#endif
    return move;
}

class PrivateKeyWriterPrivate
{
public:
    PrivateKeyWriterPrivate(const PublicKey &key)
        : key(key)
        , algo(Cipher::Null)
        , mode(Cipher::CBC)
        , publicOnly(true)
    {
    }
    PrivateKeyWriterPrivate(const PrivateKey &key)
        : key(key)
        , algo(Cipher::Null)
        , mode(Cipher::CBC)
        , publicOnly(false)
    {
    }
    string asPem();
    string asDer();
    string save(Ssl::EncodingFormat format);

    const PublicKey &key;
    Cipher::Algorithm algo;
    Cipher::Mode mode;
    shared_ptr<PasswordCallback> callback;
    string password;
    bool publicOnly;
};

string PrivateKeyWriterPrivate::asPem()
{
    if (!key.isValid()) {
        return string();
    }

    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return string();
    }
    const EVP_CIPHER *cipher = nullptr;
    if (algo != Cipher::Null && (!password.empty() || callback)) {
        cipher = getOpenSSL_CIPHER(algo, mode);
        if (!cipher) {
            BIO_free(bio);
            return string();
        }
    }
    int rvalue;
    if (key.d_ptr->hasPrivate && !publicOnly) {
        // we don't use PEM_write_bio_RSAPrivateKey() & PEM_write_bio_DSAPrivateKey
        if (callback) {
            // q_PEM_write_bio_PrivateKey
            rvalue = PEM_write_bio_PKCS8PrivateKey(bio, key.d_ptr->pkey.get(), cipher, nullptr, 0, pem_password_cb,
                                                   callback.get());
        } else if (!password.empty()) {
            rvalue = PEM_write_bio_PKCS8PrivateKey(bio, key.d_ptr->pkey.get(), cipher,
                                                   const_cast<char *>(password.data()),
                                                   static_cast<int>(password.size()), nullptr, nullptr);
        } else {
            rvalue = PEM_write_bio_PKCS8PrivateKey(bio, key.d_ptr->pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);
        }
    } else {
        rvalue = PEM_write_bio_PUBKEY(bio, key.d_ptr->pkey.get());
    }
    if (rvalue) {
        char *p = nullptr;
        long size = BIO_get_mem_data(bio, &p);
        if (size > 0 && p != nullptr) {
            string result(p, static_cast<int>(size));
            BIO_free(bio);
            return result;
        }
    }
    BIO_free(bio);
    return string();
}

string PrivateKeyWriterPrivate::asDer()
{
    // FIXME
    return string();
}

string PrivateKeyWriterPrivate::save(Ssl::EncodingFormat format)
{
    if (format == Ssl::Pem) {
        return asPem();
    } else if (format == Ssl::Der) {
        return asDer();
    } else {
        return string();
    }
}

class PrivateKeyReaderPrivate
{
public:
    PrivateKey read(const string &data);
    PrivateKey readFile(const string &filePath);
    PublicKey readPublic(const string &data);
    PublicKey readPublicFile(const string &filePath);

    shared_ptr<PasswordCallback> callback;
    string password;
    Ssl::EncodingFormat format;
};

PrivateKey PrivateKeyReaderPrivate::read(const string &data)
{
    PrivateKey key;
    if (format != Ssl::Pem) {
        return key;
    }

    BIO *bio = BIO_new_mem_buf(data.data(), data.size());
    EVP_PKEY *rawPkey = nullptr;
    if (!password.empty()) {
        shared_ptr<SimplePasswordCallback> cb = make_shared<SimplePasswordCallback>(password);
        PEM_read_bio_PrivateKey(bio, &rawPkey, pem_password_cb, cb.get());
    } else if (callback) {
        PEM_read_bio_PrivateKey(bio, &rawPkey, pem_password_cb, callback.get());
    } else {
        PEM_read_bio_PrivateKey(bio, &rawPkey, nullptr, nullptr);
    }
    if (!rawPkey) {
        BIO_free(bio);
        return key;
    }
    if (!PublicKeyPrivate::adoptRawPkey(key.d_ptr, rawPkey, true)) {
        BIO_free(bio);
        return key;
    }
    BIO_free(bio);
    return key;
}

PrivateKey PrivateKeyReaderPrivate::readFile(const string &filePath)
{
    string data = PosixPath(filePath).readall(nullptr);
    return read(data);
}

PublicKey PrivateKeyReaderPrivate::readPublic(const string &data)
{
    PublicKey key;
    BIO *bio = BIO_new_mem_buf(data.data(), data.size());
    EVP_PKEY *rawPkey = nullptr;
    if (format == Ssl::Pem) {
        if (!password.empty()) {
            shared_ptr<SimplePasswordCallback> cb = make_shared<SimplePasswordCallback>(password);
            PEM_read_bio_PUBKEY(bio, &rawPkey, pem_password_cb, cb.get());
        } else if (callback) {
            PEM_read_bio_PUBKEY(bio, &rawPkey, pem_password_cb, callback.get());
        } else {
            PEM_read_bio_PUBKEY(bio, &rawPkey, nullptr, nullptr);
        }
    } else if (format == Ssl::Der) {
        d2i_PUBKEY_bio(bio, &rawPkey);
    } else {
        BIO_free(bio);
        return key;
    }
    if (!rawPkey) {
        BIO_free(bio);
        return key;
    }

    if (!PublicKeyPrivate::adoptRawPkey(key.d_ptr, rawPkey, false)) {
        BIO_free(bio);
        return key;
    }
    BIO_free(bio);
    return key;
}

PublicKey PrivateKeyReaderPrivate::readPublicFile(const string &filePath)
{
    string data = PosixPath(filePath).readall(nullptr);
    return readPublic(data);
}

PublicKey::PublicKey()
    : d_ptr(new PublicKeyPrivate)
{
}

PublicKey::PublicKey(const PublicKey &other)
    : d_ptr(new PublicKeyPrivate(other.d_ptr))
{
}

PublicKey &PublicKey::operator=(const PublicKey &other)
{
    delete d_ptr;
    d_ptr = new PublicKeyPrivate(other.d_ptr);
    return *this;
}

bool comparePublicKey(const PublicKeyPrivate *d1, const PublicKeyPrivate *d2)
{
    if (d1->pkey == d2->pkey) {
        return true;
    }
    if (!d1->pkey || !d2->pkey) {
        return false;
    }
    // OpenSSL 3.0 deprecated EVP_PKEY_cmp() in favor of EVP_PKEY_eq().
    // LibreSSL and OpenSSL < 3.0 still only provide EVP_PKEY_cmp().
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
    return EVP_PKEY_eq(d1->pkey.get(), d2->pkey.get()) == 1;
#else
    return EVP_PKEY_cmp(d1->pkey.get(), d2->pkey.get()) == 1;
#endif
}

bool PublicKey::operator==(const PublicKey &other) const
{
    NG_D(const PublicKey);
    return comparePublicKey(d, other.d_ptr);
}

PublicKey::~PublicKey()
{
    if (d_ptr) {
        delete d_ptr;
    }
}

PublicKey PublicKey::load(const string &data, Ssl::EncodingFormat format)
{
    PrivateKeyReader reader;
    return reader.setFormat(format).readPublic(data);
}

string PublicKey::save(Ssl::EncodingFormat format) const
{
    PrivateKeyWriter writer(*this);
    if (format == Ssl::Pem) {
        return writer.asPem();
    } else if (format == Ssl::Der) {
        return writer.asDer();
    } else {
        NG_UNREACHABLE();
        return string();
    }
}

bool PublicKey::isNull() const
{
    NG_D(const PublicKey);
    return d->context == nullptr;
}

bool PublicKey::isValid() const
{
    NG_D(const PublicKey);
    return d->context != nullptr;
}

void * PublicKey::handle() const
{
    NG_D(const PublicKey);
    return static_cast<void *>(d->pkey.get());
}

PublicKey::Algorithm PublicKey::algorithm() const
{
    NG_D(const PublicKey);
    return d->algorithm();
}

int PublicKey::bits() const
{
    NG_D(const PublicKey);
    return d->bits();
}

bool PublicKey::verify(const string &data, const string &hash, MessageDigest::Algorithm hashAlgo)
{
    NG_D(PublicKey);
    return d->verify(data, hash, hashAlgo);
}

string PublicKey::encrypt(const string &data) const
{
    NG_D(const PublicKey);
    return d->encrypt(data);
}

string PublicKey::digest(MessageDigest::Algorithm algorithm) const
{
    const string &bs = save(Ssl::Pem);
    if (bs.empty()) {
        return string();
    } else {
        return MessageDigest::hash(bs, algorithm);
    }
}

string PublicKey::rsaPublicEncrypt(const string &data, PrivateKey::RsaPadding padding) const
{
    NG_D(const PublicKey);
    return d->rsaPublicEncrypt(data, padding);
}

string PublicKey::rsaPublicDecrypt(const string &data, PrivateKey::RsaPadding padding) const
{
    NG_D(const PublicKey);
    return d->rsaPublicDecrypt(data, padding);
}

bool PrivateKey::operator==(const PrivateKey &other) const
{
    NG_D(const PublicKey);
    return comparePublicKey(d, other.d_ptr);
}

PrivateKey PrivateKey::load(const string &data, Ssl::EncodingFormat format, const string &password)
{
    PrivateKeyReader reader;
    if (!password.empty()) {
        reader.setPassword(password);
    }
    return reader.setFormat(format).read(data);
}

PrivateKey PrivateKey::generate(PrivateKey::Algorithm algo, int bits)
{
    return PublicKeyPrivate::generate(algo, bits);
}

string PrivateKey::save(Ssl::EncodingFormat format, const string &password) const
{
    PrivateKeyWriter writer(*this);
    if (!password.empty()) {
        writer.setPassword(password);
    }
    if (format == Ssl::Pem) {
        return writer.asPem();
    } else if (format == Ssl::Der) {
        return writer.asDer();
    } else {
        NG_UNREACHABLE();
        return string();
    }
}

PublicKey PrivateKey::publicKey() const
{
    return *this;
}

string PrivateKey::sign(const string &data, MessageDigest::Algorithm hashAlgo)
{
    NG_D(PublicKey);
    return d->sign(data, hashAlgo);
}

string PrivateKey::decrypt(const string &data) const
{
    NG_D(const PublicKey);
    return d->decrypt(data);
}

string PrivateKey::rsaPrivateEncrypt(const string &data, PrivateKey::RsaPadding padding) const
{
    NG_D(const PublicKey);
    return d->rsaPrivateEncrypt(data, padding);
}

string PrivateKey::rsaPrivateDecrypt(const string &data, PrivateKey::RsaPadding padding) const
{
    NG_D(const PublicKey);
    return d->rsaPrivateDecrypt(data, padding);
}

PrivateKeyWriter::PrivateKeyWriter(const PrivateKey &key)
    : d_ptr(new PrivateKeyWriterPrivate(key))
{
}

PrivateKeyWriter::PrivateKeyWriter(const PublicKey &key)
    : d_ptr(new PrivateKeyWriterPrivate(key))
{
}

PrivateKeyWriter::~PrivateKeyWriter()
{
    delete d_ptr;
}

PrivateKeyWriter &PrivateKeyWriter::setCipher(Cipher::Algorithm algo, Cipher::Mode mode)
{
    NG_D(PrivateKeyWriter);
    d->algo = algo;
    d->mode = mode;
    return *this;
}

PrivateKeyWriter &PrivateKeyWriter::setPassword(const string &password)
{
    NG_D(PrivateKeyWriter);
    if (d->algo == Cipher::Null) {
        ngDebug() << "no cipher specified.";
    }
    d->password = password;
    d->callback.reset();
    return *this;
}

PrivateKeyWriter &PrivateKeyWriter::setPassword(shared_ptr<PasswordCallback> callback)
{
    NG_D(PrivateKeyWriter);
    if (d->algo == Cipher::Null) {
        ngDebug() << "no cipher specified.";
    }
    d->callback = callback;
    d->password.clear();
    return *this;
}

PrivateKeyWriter &PrivateKeyWriter::setPublicOnly(bool publicOnly)
{
    NG_D(PrivateKeyWriter);
    d->publicOnly = publicOnly;
    return *this;
}

string PrivateKeyWriter::asPem()
{
    NG_D(PrivateKeyWriter);
    return d->asPem();
}

string PrivateKeyWriter::asDer()
{
    return string();
}

bool PrivateKeyWriter::save(const string &filePath)
{
    shared_ptr<FileLike> f = PosixPath(filePath).open("w");
    if (f) {
        f->write(asPem());
        f->close();
        return true;
    }
    return false;
}

PrivateKeyReader::PrivateKeyReader()
    : d_ptr(new PrivateKeyReaderPrivate())
{
}

PrivateKeyReader::~PrivateKeyReader()
{
    delete d_ptr;
}

PrivateKeyReader &PrivateKeyReader::setPassword(const string &password)
{
    NG_D(PrivateKeyReader);
    d->password = password;
    d->callback.reset();
    return *this;
}

PrivateKeyReader &PrivateKeyReader::setPassword(shared_ptr<PasswordCallback> callback)
{
    NG_D(PrivateKeyReader);
    d->password.clear();
    d->callback = callback;
    return *this;
}

PrivateKeyReader &PrivateKeyReader::setFormat(Ssl::EncodingFormat format)
{
    NG_D(PrivateKeyReader);
    d->format = format;
    return *this;
}

PrivateKey PrivateKeyReader::read(const string &data)
{
    NG_D(PrivateKeyReader);
    return d->read(data);
}

PublicKey PrivateKeyReader::readPublic(const string &data)
{
    NG_D(PrivateKeyReader);
    return d->readPublic(data);
}

PrivateKey PrivateKeyReader::readFile(const string &filePath)
{
    NG_D(PrivateKeyReader);
    return d->readFile(filePath);
}

PublicKey PrivateKeyReader::readPublicFile(const string &filePath)
{
    NG_D(PrivateKeyReader);
    return d->readPublicFile(filePath);
}

}  // namespace qtng
