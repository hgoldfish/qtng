#include "qtng/random.h"

#include <chrono>
#include <random>

#ifndef QTNG_NO_CRYPTO
#  include <openssl/rand.h>
#endif

using namespace std;

namespace qtng {

class RandomGeneratorPrivate
{
public:
    RandomGeneratorPrivate();

    std::mt19937_64 engine;
};

RandomGeneratorPrivate::RandomGeneratorPrivate()
    : engine(static_cast<unsigned long long>(
              chrono::steady_clock::now().time_since_epoch().count()))
{
}

RandomGenerator::RandomGenerator()
    : d_ptr(new RandomGeneratorPrivate)
{
}

RandomGenerator::~RandomGenerator()
{
    delete d_ptr;
}

RandomGenerator &RandomGenerator::global()
{
    static RandomGenerator instance;
    return instance;
}

uint32_t RandomGenerator::bounded(uint32_t highest)
{
    return bounded(0, highest);
}

uint32_t RandomGenerator::bounded(uint32_t lowest, uint32_t highest)
{
    if (highest <= lowest) {
        return lowest;
    }
    uniform_int_distribution<uint32_t> dist(lowest, highest - 1);
    return dist(d_ptr->engine);
}

uint32_t RandomGenerator::generate()
{
    return static_cast<uint32_t>(d_ptr->engine());
}

uint64_t RandomGenerator::generate64()
{
    return d_ptr->engine();
}

void RandomGenerator::generate(char *data, int size)
{
    for (int i = 0; i < size; ++i) {
        data[i] = static_cast<char>(bounded(256));
    }
}

string RandomGenerator::generateHex(int byteCount)
{
    static const char hex[] = "0123456789abcdef";
    string result;
    result.reserve(static_cast<size_t>(byteCount) * 2);
    for (int i = 0; i < byteCount; ++i) {
        uint32_t value = bounded(256);
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0x0f]);
    }
    return result;
}

string randomBytes(int numBytes)
{
    if (numBytes <= 0) {
        return string();
    }
    string b;
    b.resize(static_cast<size_t>(numBytes));
#ifndef QTNG_NO_CRYPTO
    // RAND_bytes may fail (e.g. no entropy in a FIPS-mode environment). Fall back
    // rather than returning uninitialized bytes. NOTE: RandomGenerator is a plain
    // PRNG (mt19937_64, not a CSPRNG) -- this path yields weaker randomness than
    // RAND_bytes and must not be relied on for secrets; it exists only as a last
    // resort. The QTNG_NO_CRYPTO build below has no better option.
    if (RAND_bytes(reinterpret_cast<unsigned char *>(&b[0]), numBytes) != 1) {
        RandomGenerator::global().generate(&b[0], numBytes);
    }
#else
    RandomGenerator::global().generate(&b[0], numBytes);
#endif
    return b;
}

}  // namespace qtng
