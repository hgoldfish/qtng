#include <atomic>

#include <openssl/ssl.h>
#include "qtng/crypto.h"
#include "qtng/utils/platform.h"
#include "qtng/private/crypto_p.h"

using namespace std;

namespace qtng {

struct OpenSSLLib
{
    OpenSSLLib()
        : version(0)
    {
    }
    atomic<int> inited;
    int version;
};

NG_GLOBAL_STATIC(struct OpenSSLLib, lib)

void initOpenSSL()
{
    if (lib().inited.fetch_add(1) > 0) {
        return;
    }
    OPENSSL_add_all_algorithms_noconf();
    SSL_library_init();
    SSL_load_error_strings();
}

void cleanupOpenSSL()
{
    if (lib().inited.fetch_sub(1) > 0) {
        return;
    }
    EVP_cleanup();
    CRYPTO_cleanup_all_ex_data();
    // ERR_remove_state(0);  // deprecated
    ERR_free_strings();
    sk_SSL_COMP_free(SSL_COMP_get_compression_methods());
}

}  // namespace qtng
