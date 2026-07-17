#ifndef QTNG_UTILS_DATETIME_H
#define QTNG_UTILS_DATETIME_H

#include <chrono>
#include <cstdint>
#include <string>

namespace qtng {
namespace utils {

class DateTime
{
public:
    DateTime();
    explicit DateTime(std::chrono::system_clock::time_point tp);

    static DateTime currentDateTimeUtc();
    static std::int64_t currentMSecsSinceEpoch();
    static DateTime fromSecsSinceEpoch(std::int64_t secs);
    static DateTime fromMSecsSinceEpoch(std::int64_t msecs);
    static DateTime fromUtc(int year, int month, int day, int hour = 0, int minute = 0, int second = 0,
                            int msec = 0);

    std::int64_t toSecsSinceEpoch() const;
    DateTime addSecs(std::int64_t secs) const;
    std::int64_t secsTo(const DateTime &other) const;
    DateTime toUTC() const { return *this; }
    std::int64_t toMSecsSinceEpoch() const;
    std::string toString(const std::string &format = std::string()) const;
    std::string toHttpDate() const;

    bool isValid() const { return valid; }
    std::chrono::system_clock::time_point timePoint() const { return tp; }

    bool operator==(const DateTime &other) const { return valid == other.valid && tp == other.tp; }
    bool operator!=(const DateTime &other) const { return !(*this == other); }
    bool operator<(const DateTime &other) const { return tp < other.tp; }

private:
    std::chrono::system_clock::time_point tp;
    bool valid;
};

class ElapsedTimer
{
public:
    ElapsedTimer();
    void restart();
    std::int64_t elapsed() const;
    std::int64_t elapsedMicroseconds() const;
private:
    std::chrono::steady_clock::time_point start;
};

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_DATETIME_H
