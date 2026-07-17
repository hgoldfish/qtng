#include "qtng/utils/mime.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {
namespace utils {

string mimeTypeForExtension(const string &extension)
{
    string ext = toLower(extension);
    if (ext == ".html" || ext == ".htm") {
        return "text/html";
    }
    if (ext == ".css") {
        return "text/css";
    }
    if (ext == ".js") {
        return "application/javascript";
    }
    if (ext == ".json") {
        return "application/json";
    }
    if (ext == ".xml") {
        return "application/xml";
    }
    if (ext == ".txt") {
        return "text/plain";
    }
    if (ext == ".png") {
        return "image/png";
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        return "image/jpeg";
    }
    if (ext == ".gif") {
        return "image/gif";
    }
    if (ext == ".svg") {
        return "image/svg+xml";
    }
    if (ext == ".ico") {
        return "image/x-icon";
    }
    if (ext == ".pdf") {
        return "application/pdf";
    }
    if (ext == ".zip") {
        return "application/zip";
    }
    if (ext == ".gz") {
        return "application/gzip";
    }
    if (ext == ".wasm") {
        return "application/wasm";
    }
    return "application/octet-stream";
}

string mimeTypeForFileName(const string &fileName)
{
    size_t pos = fileName.find_last_of('.');
    if (pos == string::npos) {
        return "application/octet-stream";
    }
    return mimeTypeForExtension(fileName.substr(pos));
}

}  // namespace utils
}  // namespace qtng
