#ifndef QTNG_NO_CRYPTO
#  include <openssl/rand.h>
#endif

#include "qtng/random.h"
#include "qtng/utils/random.h"

using namespace std;

namespace qtng {

string randomBytes(int numBytes)
{
    if (numBytes <= 0) {
        return string();
    }
    string b;
    b.resize(static_cast<size_t>(numBytes));
#ifndef QTNG_NO_CRYPTO
    RAND_bytes(reinterpret_cast<unsigned char *>(&b[0]), numBytes);
#else
    utils::RandomGenerator::global().generate(&b[0], numBytes);
#endif
    return b;
}

}  // namespace qtng
