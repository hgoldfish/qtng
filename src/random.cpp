#ifndef QTNG_NO_CRYPTO
#  include <openssl/rand.h>
#endif

#include "qtng/random.h"
#include "qtng/utils/random.h"
#include "qtng/utils/datetime.h"

using namespace std;

namespace qtng {

#ifndef QTNG_NO_CRYPTO

string randomBytes(int numBytes)
{
    string b;
    b.resize(static_cast<size_t>(numBytes));
    RAND_bytes(reinterpret_cast<unsigned char *>(&b[0]), numBytes);
    return b;
}

#else

string randomBytes(int numBytes)
{
    string b;
    b.reserve(static_cast<size_t>(numBytes));
    utils::RandomGenerator &generator = utils::RandomGenerator::global();
    for (int i = 0; i < numBytes; ++i) {
        b.push_back(static_cast<char>(generator.bounded(0xff))));
    }
    return b;
}

#endif

}  // namespace qtng
