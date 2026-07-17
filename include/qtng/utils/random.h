#ifndef QTNG_UTILS_RANDOM_H
#define QTNG_UTILS_RANDOM_H

#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include "qtng/utils/platform.h"

namespace qtng {
namespace utils {

class RandomGenerator
{
public:
    static RandomGenerator &global();

    std::uint32_t bounded(std::uint32_t highest);
    std::uint32_t generate();
    std::uint32_t generate32() { return generate(); }
    std::uint64_t generate64();
    void generate(char *data, int size);
    std::string generateHex(int byteCount);

private:
    RandomGenerator();
    std::mt19937_64 engine;
};

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_RANDOM_H
