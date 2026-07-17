#ifndef QTNG_UTILS_STRING_UTILS_H
#define QTNG_UTILS_STRING_UTILS_H

#include <string>
#include <vector>

namespace qtng {
namespace utils {

std::vector<std::string> split(const std::string &text, char separator);
std::vector<std::string> split(const std::string &text, const std::string &separator);
std::string trimmed(const std::string &text);
std::string toLower(const std::string &text);
std::string toUpper(const std::string &text);
bool startsWith(const std::string &text, const std::string &prefix);
bool endsWith(const std::string &text, const std::string &suffix);
std::string number(int value);
std::string number(long long value);
std::string number(double value, int precision = 6);
int parseInt(const std::string &text, bool *ok = nullptr);
long long parseLongLong(const std::string &text, bool *ok = nullptr);
float parseFloat(const std::string &text, bool *ok = nullptr);
double parseDouble(const std::string &text, bool *ok = nullptr);
std::string fromLatin1(const char *data, std::size_t size);
std::string join(const std::vector<std::string> &parts, const std::string &separator);
bool equalsIgnoreCase(const std::string &a, const std::string &b);
std::string htmlEscape(const std::string &text);
std::string formatMessage(const std::string &pattern, const std::vector<std::string> &args);
std::string toAce(const std::string &domain);
std::string fromAce(const std::string &domain);
std::string bytesToHex(const std::string &data);
std::string bytesToBase64(const std::string &data);
std::string join(const std::vector<std::string> &parts, char separator);

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_STRING_UTILS_H
