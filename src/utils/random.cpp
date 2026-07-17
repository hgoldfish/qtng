#include <chrono>
#include <random>
#include <sstream>

#include "qtng/utils/random.h"

using namespace std;

namespace qtng {
namespace utils {

RandomGenerator::RandomGenerator()
    : engine(static_cast<unsigned long long>(
              chrono::steady_clock::now().time_since_epoch().count()))
{
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
    return dist(engine);
}

uint32_t RandomGenerator::generate()
{
    return static_cast<uint32_t>(engine());
}

uint64_t RandomGenerator::generate64()
{
    return engine();
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

}  // namespace utils
}  // namespace qtng
