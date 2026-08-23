#ifndef QTNG_RANDOM_H
#define QTNG_RANDOM_H

#include <cstdint>
#include <string>

#include "qtng/utils/platform.h"

namespace qtng {

std::string randomBytes(int numBytes);

class RandomGeneratorPrivate;
class RandomGenerator
{
public:
    static RandomGenerator &global();
    ~RandomGenerator();
    std::uint32_t bounded(std::uint32_t highest);
    // Returns a value in [lowest, highest). If highest <= lowest, returns lowest.
    std::uint32_t bounded(std::uint32_t lowest, std::uint32_t highest);
    std::uint32_t generate();
    std::uint32_t generate32() { return generate(); }
    std::uint64_t generate64();
    void generate(char *data, int size);
    std::string generateHex(int byteCount);

private:
    RandomGenerator();
    RandomGeneratorPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(RandomGenerator)
    NG_DISABLE_COPY(RandomGenerator)
};

}  // namespace qtng

#endif  // QTNG_RANDOM_H
