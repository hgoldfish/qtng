#ifndef QTNG_UTILS_MIME_H
#define QTNG_UTILS_MIME_H

#include <string>

namespace qtng {
namespace utils {

std::string mimeTypeForFileName(const std::string &fileName);
std::string mimeTypeForExtension(const std::string &extension);

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_MIME_H
