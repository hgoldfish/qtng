#ifndef QTNG_UTILS_MIME_H
#define QTNG_UTILS_MIME_H

#include <string>

#include "qtng/config.h"
QTNG_NAMESPACE_BEGIN
namespace utils {

std::string mimeTypeForFileName(const std::string &fileName);
std::string mimeTypeForExtension(const std::string &extension);

}  // namespace utils
QTNG_NAMESPACE_END
#endif  // QTNG_UTILS_MIME_H
