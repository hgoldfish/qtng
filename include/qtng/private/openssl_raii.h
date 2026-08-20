#ifndef QTNG_OPENSSL_RAII_H
#define QTNG_OPENSSL_RAII_H

#include <memory>

extern "C" {
#include <openssl/evp.h>
}

namespace qtng {

struct EvpPkeyDeleter
{
    void operator()(EVP_PKEY *pkey) const { EVP_PKEY_free(pkey); }
};

struct EvpPkeyCtxDeleter
{
    void operator()(EVP_PKEY_CTX *ctx) const { EVP_PKEY_CTX_free(ctx); }
};

struct EvpMdCtxDeleter
{
    void operator()(EVP_MD_CTX *ctx) const { EVP_MD_CTX_free(ctx); }
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

inline std::shared_ptr<EVP_PKEY> shareEvpPkey(EVP_PKEY *pkey)
{
    return std::shared_ptr<EVP_PKEY>(pkey, EvpPkeyDeleter{});
}

}  // namespace qtng

#endif  // QTNG_OPENSSL_RAII_H
