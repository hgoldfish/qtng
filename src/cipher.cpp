#include <string>
#include <utility>

#include "qtng/cipher.h"
#include "qtng/private/crypto_p.h"
#include "qtng/utils/string_utils.h"
#include "qtng/random.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.cipher");

namespace qtng {

const EVP_MD *getOpenSSL_MD(MessageDigest::Algorithm algo);

const EVP_CIPHER *getOpenSSL_CIPHER(Cipher::Algorithm algo, Cipher::Mode mode)
{
    const EVP_CIPHER *cipher = nullptr;
    switch (algo) {
    case Cipher::Null:
        cipher = EVP_enc_null();
        break;
    case Cipher::AES128:
        switch (mode) {
        case Cipher::ECB:
            cipher = EVP_aes_128_ecb();
            break;
        case Cipher::CBC:
            cipher = EVP_aes_128_cbc();
            break;
        case Cipher::CFB:
            cipher = EVP_aes_128_cfb128();
            break;
        case Cipher::OFB:
            cipher = EVP_aes_128_ofb();
            break;
        case Cipher::CTR:
            cipher = EVP_aes_128_ctr();
            break;
        default:
            NG_UNREACHABLE();
        }
        break;
    case Cipher::AES192:
        switch (mode) {
        case Cipher::ECB:
            cipher = EVP_aes_192_ecb();
            break;
        case Cipher::CBC:
            cipher = EVP_aes_192_cbc();
            break;
        case Cipher::CFB:
            cipher = EVP_aes_192_cfb128();
            break;
        case Cipher::OFB:
            cipher = EVP_aes_192_ofb();
            break;
        case Cipher::CTR:
            cipher = EVP_aes_192_ctr();
            break;
        default:
            NG_UNREACHABLE();
        }
        break;
    case Cipher::AES256:
        switch (mode) {
        case Cipher::ECB:
            cipher = EVP_aes_256_ecb();
            break;
        case Cipher::CBC:
            cipher = EVP_aes_256_cbc();
            break;
        case Cipher::CFB:
            cipher = EVP_aes_256_cfb128();
            break;
        case Cipher::OFB:
            cipher = EVP_aes_256_ofb();
            break;
        case Cipher::CTR:
            cipher = EVP_aes_256_ctr();
            break;
        default:
            NG_UNREACHABLE();
        }
        break;
    case Cipher::DES:
        switch (mode) {
        case Cipher::ECB:
            cipher = EVP_des_ecb();
            break;
        case Cipher::CBC:
            cipher = EVP_des_cbc();
            break;
        case Cipher::CFB:
            cipher = EVP_des_cfb64();
            break;
        case Cipher::OFB:
            cipher = EVP_des_ofb();
            break;
        case Cipher::CTR:
        default:
            NG_UNREACHABLE();
        }
        break;
    case Cipher::DES2:
        switch (mode) {
        case Cipher::ECB:
            cipher = EVP_des_ede_ecb();
            break;
        case Cipher::CBC:
            cipher = EVP_des_ede_cbc();
            break;
        case Cipher::CFB:
            cipher = EVP_des_ede_cfb64();
            break;
        case Cipher::OFB:
            cipher = EVP_des_ede_ofb();
            break;
        case Cipher::CTR:
        default:
            NG_UNREACHABLE();
        }
        break;
    case Cipher::DES3:
        switch (mode) {
        case Cipher::ECB:
            cipher = EVP_des_ede3_ecb();
            break;
        case Cipher::CBC:
            cipher = EVP_des_ede3_cbc();
            break;
        case Cipher::CFB:
            cipher = EVP_des_ede3_cfb64();
            break;
        case Cipher::OFB:
            cipher = EVP_des_ede3_ofb();
            break;
        case Cipher::CTR:
        default:
            NG_UNREACHABLE();
        }
        break;
    case Cipher::Blowfish:
        switch (mode) {
        case Cipher::ECB:
            cipher = EVP_bf_ecb();
            break;
        case Cipher::CBC:
            cipher = EVP_bf_cbc();
            break;
        case Cipher::CFB:
            cipher = EVP_bf_cfb64();
            break;
        case Cipher::OFB:
            cipher = EVP_bf_ofb();
            break;
        case Cipher::CTR:
        default:
            NG_UNREACHABLE();
        }
        break;
    case Cipher::CAST5:
        switch (mode) {
        case Cipher::ECB:
            cipher = EVP_cast5_ecb();
            break;
        case Cipher::CBC:
            cipher = EVP_cast5_cbc();
            break;
        case Cipher::CFB:
            cipher = EVP_cast5_cfb64();
            break;
        case Cipher::OFB:
            cipher = EVP_cast5_ofb();
            break;
        case Cipher::CTR:
        default:
            NG_UNREACHABLE();
        }
        break;
    case Cipher::Chacha20:
        cipher = EVP_chacha20();
        break;
        //    case Cipher::ChaCha20Poly1305:
        //        cipher = EVP_chacha20_poly1305();
        //        break;
    default:
        break;
    }
    return cipher;
}

class CipherPrivate
{
public:
    CipherPrivate(Cipher::Algorithm algo, Cipher::Mode mode, Cipher::Operation operation);
    ~CipherPrivate();
    string addData(const char *data, int len);
    string finalData();
    pair<string, string> bytesToKey(const string &password, MessageDigest::Algorithm hashAlgo,
                                             const string &salt, int i);
    pair<string, string> PBKDF2_HMAC(const string &password, const string &salt,
                                              MessageDigest::Algorithm hashAlgo, int i);
    bool setPassword(const string &password, const string &salt, const MessageDigest::Algorithm hashAlgo,
                     int i);
    bool setOpensslPassword(const string &password, const string &salt, const MessageDigest::Algorithm hashAlgo,
                            int i);
    bool init();
    bool setPadding(bool padding);

    EVP_CIPHER_CTX *context;
    const EVP_CIPHER *cipher;
    string key;
    string iv;
    string salt;
    Cipher::Algorithm algo;
    Cipher::Mode mode;
    Cipher::Operation operation;
    bool hasError;
    bool inited;
    bool padding;
};

CipherPrivate::CipherPrivate(Cipher::Algorithm algo, Cipher::Mode mode, Cipher::Operation operation)
    : context(nullptr)
    , cipher(nullptr)
    , algo(algo)
    , mode(mode)
    , operation(operation)
    , hasError(false)
    , inited(false)
    , padding(true)
{
    initOpenSSL();
    cipher = getOpenSSL_CIPHER(algo, mode);
    if (!cipher) {
        hasError = true;
        ngWarning() << "cipher is not supported.";
        return;
    }

    context = EVP_CIPHER_CTX_new();
    if (!context) {
        hasError = true;
        return;
    }
    setPadding(true);
}

CipherPrivate::~CipherPrivate()
{
    if (context) {
        EVP_CIPHER_CTX_free(context);
    }
    cleanupOpenSSL();
}

bool CipherPrivate::init()
{
    if (inited || !context || !cipher || key.empty() || iv.empty() || hasError) {
        return false;
    }
    int rvalue = EVP_CipherInit_ex(context, cipher, nullptr, reinterpret_cast<unsigned char *>(&key[0]),
                                   reinterpret_cast<unsigned char *>(&iv[0]), operation == Cipher::Decrypt ? 0 : 1);
    if (rvalue) {
        inited = true;
        return true;
    } else {
        return false;
    }
}

pair<string, string> CipherPrivate::bytesToKey(const string &password, MessageDigest::Algorithm hashAlgo,
                                                        const string &salt, int i)
{
    const EVP_MD *dgst = getOpenSSL_MD(hashAlgo);
    unsigned char key[EVP_MAX_KEY_LENGTH], iv[EVP_MAX_IV_LENGTH];

    if (hasError || !context || !cipher || !dgst || (!salt.empty() && salt.size() != 8) || password.empty()
        || i <= 0) {
        return make_pair(string(), string());
    }
    const unsigned char *saltPtr = nullptr;
    if (!salt.empty()) {
        saltPtr = reinterpret_cast<const unsigned char *>(salt.data());
    }
    int rvalue = EVP_BytesToKey(cipher, dgst, saltPtr, reinterpret_cast<const unsigned char *>(password.data()),
                                static_cast<int>(password.size()), i, key, iv);
    if (rvalue) {
        int keylen = EVP_CIPHER_key_length(cipher);
        int ivlen = EVP_CIPHER_iv_length(cipher);
        if (keylen > 0 && ivlen >= 0) {
            return make_pair(string(reinterpret_cast<const char *>(key), keylen),
                             string(reinterpret_cast<const char *>(iv), ivlen));
        }
    }
    return make_pair(string(), string());
}

pair<string, string> CipherPrivate::PBKDF2_HMAC(const string &password, const string &salt,
                                                         MessageDigest::Algorithm hashAlgo, int i)
{
    const EVP_MD *dgst = getOpenSSL_MD(hashAlgo);
    unsigned char key[EVP_MAX_KEY_LENGTH], iv[EVP_MAX_IV_LENGTH];

    if (hasError || !context || !cipher || !dgst || salt.empty() || password.empty() || i <= 0) {
        return make_pair(string(), string());
    }
    int keylen = EVP_CIPHER_key_length(cipher);
    int ivlen = EVP_CIPHER_iv_length(cipher);
    if (keylen > 0 && ivlen > 0) {
        int rvalue = PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                                       reinterpret_cast<const unsigned char *>(salt.data()), salt.size(), i, dgst,
                                       keylen, key);
        if (rvalue) {
            rvalue = PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), key, keylen, i, dgst, ivlen, iv);
            if (rvalue) {
                return make_pair(string(reinterpret_cast<const char *>(key), keylen),
                                 string(reinterpret_cast<const char *>(iv), ivlen));
            }
        }
    }
    return make_pair(string(), string());
}

string CipherPrivate::addData(const char *data, int len)
{
    if (!context || !inited || hasError) {
        return string();
    }
    string out;
    out.resize(len + EVP_MAX_BLOCK_LENGTH);
    int outl = 0;
    int rvalue = EVP_CipherUpdate(context, reinterpret_cast<unsigned char *>(&out[0]), &outl,
                                  reinterpret_cast<const unsigned char *>(data), len);
    if (rvalue) {
        out.resize(outl);
        return out;
    } else {
        hasError = true;
        return string();
    }
}

string CipherPrivate::finalData()
{
    if (!context || !inited || hasError) {
        return string();
    }
    string out;
    out.resize(1024 + EVP_MAX_BLOCK_LENGTH);
    int outl = 0;
    int rvalue = EVP_CipherFinal_ex(context, reinterpret_cast<unsigned char *>(&out[0]), &outl);
    if (rvalue) {
        out.resize(outl);
        return out;
    } else {
        hasError = true;
        return string();
    }
}

bool CipherPrivate::setPassword(const string &password, const string &salt,
                                const MessageDigest::Algorithm hashAlgo, int i)
{
    string s;
    if (salt.empty()) {
        s = randomBytes(32);
    } else {
        s = salt;
    }
    this->salt = s;
    const pair<string, string> &t = PBKDF2_HMAC(password, s, hashAlgo, i);
    key = t.first;
    iv = t.second;
    if (key.empty()) {
        return false;
    }
    return init();
}

bool CipherPrivate::setOpensslPassword(const string &password, const string &salt,
                                       const MessageDigest::Algorithm hashAlgo, int i)
{
    string s;
    if (salt.empty()) {
        s = randomBytes(8);
    } else {
        if (salt.size() == 8) {
            s = salt;
        } else {
            ngWarning() << "setOpensslPassword() require the length of salt is 8.";
            return false;
        }
    }
    this->salt = s;
    const pair<string, string> &t = bytesToKey(password, hashAlgo, s, i);
    key = t.first;
    iv = t.second;
    if (key.empty()) {
        return false;
    }
    return init();
}

bool CipherPrivate::setPadding(bool padding)
{
    if (!context) {
        return false;
    }
    int rvalue = EVP_CIPHER_CTX_set_padding(context, padding ? 1 : 0);
    if (rvalue == 1) {
        this->padding = padding;
        return true;
    } else {
        return false;
    }
}

Cipher::Cipher(Cipher::Algorithm alog, Cipher::Mode mode, Cipher::Operation operation)
    : d_ptr(new CipherPrivate(alog, mode, operation))
{
}

Cipher::~Cipher()
{
    delete d_ptr;
}

Cipher *Cipher::copy(Cipher::Operation operation)
{
    NG_D(Cipher);
    if (!isValid()) {
        return nullptr;
    }
    Cipher *newOne = new Cipher(d->algo, d->mode, operation);
    newOne->setKey(d->key);
    newOne->setInitialVector(d->iv);
    if (!d->padding) {
        newOne->setPadding(d->padding);
    }
    return newOne;
}

bool Cipher::isValid() const
{
    NG_D(const Cipher);
    return d->cipher && d->context && !d->hasError && d->inited;
}

bool Cipher::isStream() const
{
    NG_D(const Cipher);
    switch (d->algo) {
    case AES128:
    case AES192:
    case AES256:
    case DES:
    case DES2:
    case DES3:
    case Blowfish:
        switch (d->mode) {
        case ECB:
        case CBC:
            return false;
        case CFB:
        case OFB:
        case CTR:
        case OPENPGP:
            return true;
        }
    case Null:
    case CAST5:
    case Chacha20:
    case ChaCha20Poly1305:
        return true;
    }
    return false;
}

string Cipher::addData(const char *data, int len)
{
    NG_D(Cipher);
    return d->addData(data, len);
}

string Cipher::finalData()
{
    NG_D(Cipher);
    return d->finalData();
}

bool Cipher::setInitialVector(const string &iv)
{
    NG_D(Cipher);
    d->iv = iv;
    return d->init();
}

bool Cipher::setKey(const string &key)
{
    NG_D(Cipher);
    d->key = key;
    return d->init();
}

bool Cipher::setPassword(const string &password, const string &salt, const MessageDigest::Algorithm hashAlgo,
                         int i)
{
    NG_D(Cipher);
    return d->setPassword(password, salt, hashAlgo, i);
}

bool Cipher::setOpensslPassword(const string &password, const string &salt,
                                const MessageDigest::Algorithm hashAlgo, int i)
{
    NG_D(Cipher);
    return d->setOpensslPassword(password, salt, hashAlgo, i);
}

string Cipher::saltHeader() const
{
    NG_D(const Cipher);
    if (d->salt.empty()) {
        return string();
    } else {
        const string &saltHeader = string("Salted__") + d->salt;
        return saltHeader;
    }
}

string Cipher::salt() const
{
    NG_D(const Cipher);
    return d->salt;
}

pair<string, string> Cipher::parseSalt(const string &header)
{
    if (utils::startsWith(header, "Salted_") && header.size() >= 15) {
        const string &salt = header.substr(7, 8);
        const string &other = header.substr(15);
        return make_pair(salt, other);
    } else {
        return make_pair(string(), string());
    }
}

bool Cipher::setPadding(bool padding)
{
    NG_D(Cipher);
    return d->setPadding(padding);
}

bool Cipher::padding() const
{
    NG_D(const Cipher);
    return d->padding;
}

string Cipher::key() const
{
    NG_D(const Cipher);
    return d->key;
}

string Cipher::initialVector() const
{
    NG_D(const Cipher);
    return d->iv;
}

int Cipher::keySize() const
{
    NG_D(const Cipher);
    int keylen = EVP_CIPHER_key_length(d->cipher);
    return keylen;
}

int Cipher::ivSize() const
{
    NG_D(const Cipher);
    int ivlen = EVP_CIPHER_iv_length(d->cipher);
    return ivlen;
}

int Cipher::blockSize() const
{
    NG_D(const Cipher);
    int blockSize = EVP_CIPHER_block_size(d->cipher);
    return blockSize;
}

}  // namespace qtng
