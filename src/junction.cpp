#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "qtng/junction.h"
#include "qtng/http_cookie.h"
#include "qtng/http_utils.h"
#include "qtng/io_utils.h"
#include "qtng/private/http_p.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/url.h"

using namespace std;

NG_LOGGER("qtng.junction")

QTNG_NAMESPACE_BEGIN

namespace {

const int MAX_MOUNT_DEPTH = 32;
const char *const ROUTE_METHODS[] = {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "QUERY"};

// Route paths keep their trailing slash: /notes and /notes/ are distinct
// registrations, and requesting the other one redirects.
string withLeadingSlash(const string &path)
{
    if (path.empty()) {
        return string("/");
    }
    if (path[0] != '/') {
        return string("/") + path;
    }
    return path;
}

// Mount prefixes drop the trailing slash, so "/api/", "/api" and "api" are the
// same mount. The root prefix becomes an empty string, which matches everything.
string normalizePrefix(const string &prefix)
{
    string p = withLeadingSlash(prefix);
    while (!p.empty() && p[p.size() - 1] == '/') {
        p.erase(p.size() - 1);
    }
    return p;
}

bool prefixMatches(const string &prefix, const string &path)
{
    if (path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    if (path.size() == prefix.size()) {
        return true;
    }
    return path[prefix.size()] == '/';
}

string stripPrefix(const string &path, const string &prefix)
{
    if (path.size() == prefix.size()) {
        return string("/");
    }
    return path.substr(prefix.size());
}

string flipTrailingSlash(const string &path)
{
    if (path.empty() || path == "/") {
        return string();
    }
    if (path[path.size() - 1] == '/') {
        return path.substr(0, path.size() - 1);
    }
    return path + "/";
}

HttpRequest rewritePath(const HttpRequest &request, const string &path)
{
    HttpRequest copy = detail::HttpDeepCopy::request(request);
    utils::Url url = copy.url();
    url.setPath(path);
    copy.setUrl(url);
    return copy;
}

HttpJunction::Handler constantHandler(const HttpResponse &response)
{
    return [response](const HttpRequest &) { return response; };
}

bool drainFile(const shared_ptr<FileLike> &file)
{
    if (!file) {
        return true;
    }
    char buffer[4096];
    while (true) {
        const int32_t n = file->read(buffer, static_cast<int32_t>(sizeof(buffer)));
        if (n == 0) {
            return true;
        }
        if (n < 0) {
            return false;
        }
    }
}

}  // namespace

class HttpJunctionPrivate
{
public:
    typedef std::map<std::string, HttpJunction::Handler> MethodTable;
    typedef std::map<std::string, MethodTable> RouteTable;
public:
    const HttpJunction::Handler *lookupRoute(const std::string &method, const std::string &path, int depth,
                                             std::vector<HttpJunctionPrivate *> *chain, std::string *relativePath,
                                             bool *depthExceeded);
    std::map<std::string, HttpJunction *>::const_iterator longestMount(const std::string &path) const;
    void collectMethods(const std::string &path, int depth, std::set<std::string> *methods) const;
    void collectRoutes(const std::string &prefix, int depth, std::vector<std::string> *routes) const;
    const HttpJunction::Handler *findHandler(const std::vector<HttpJunctionPrivate *> &chain,
                                             HttpJunction::Handler HttpJunctionPrivate::*member) const;
    HttpResponse runThrough(const std::vector<HttpJunctionPrivate *> &chain, const HttpRequest &request,
                            const HttpJunction::Handler &handler);
    HttpResponse dispatch(const HttpRequest &request);
public:
    RouteTable routes;
    std::map<std::string, HttpJunction *> mounts;
    std::vector<HttpJunction::Middleware> middlewares;
    HttpJunction::Handler notFoundHandler;
    HttpJunction::Handler methodNotAllowedHandler;
    HttpJunction::ExceptionHandler exceptionHandler;
};

const HttpJunction::Handler *HttpJunctionPrivate::lookupRoute(const string &method, const string &path, int depth,
                                                              vector<HttpJunctionPrivate *> *chain,
                                                              string *relativePath, bool *depthExceeded)
{
    chain->push_back(this);
    RouteTable::iterator it = routes.find(path);
    if (it != routes.end()) {
        MethodTable::iterator jt = it->second.find(method);
        if (jt == it->second.end() && method == "HEAD") {
            jt = it->second.find("GET");
        }
        if (jt != it->second.end()) {
            *relativePath = path;
            return &jt->second;
        }
    }
    if (depth >= MAX_MOUNT_DEPTH) {
        *depthExceeded = true;
        return nullptr;
    }
    std::map<std::string, HttpJunction *>::const_iterator mount = longestMount(path);
    if (mount != mounts.end()) {
        HttpJunctionPrivate *sub = mount->second->d_func();
        return sub->lookupRoute(method, stripPrefix(path, mount->first), depth + 1, chain, relativePath, depthExceeded);
    }
    return nullptr;
}

std::map<std::string, HttpJunction *>::const_iterator HttpJunctionPrivate::longestMount(const string &path) const
{
    std::map<std::string, HttpJunction *>::const_iterator best = mounts.end();
    for (std::map<std::string, HttpJunction *>::const_iterator it = mounts.begin(); it != mounts.end(); ++it) {
        if (!prefixMatches(it->first, path)) {
            continue;
        }
        if (best == mounts.end() || it->first.size() > best->first.size()) {
            best = it;
        }
    }
    return best;
}

void HttpJunctionPrivate::collectMethods(const string &path, int depth, std::set<string> *methods) const
{
    RouteTable::const_iterator it = routes.find(path);
    if (it != routes.end()) {
        for (MethodTable::const_iterator jt = it->second.begin(); jt != it->second.end(); ++jt) {
            methods->insert(jt->first);
        }
    }
    if (depth >= MAX_MOUNT_DEPTH) {
        return;
    }
    std::map<std::string, HttpJunction *>::const_iterator mount = longestMount(path);
    if (mount != mounts.end()) {
        mount->second->d_func()->collectMethods(stripPrefix(path, mount->first), depth + 1, methods);
    }
    // A HEAD request is served by the GET handler when HEAD is not registered.
    if (methods->find("GET") != methods->end()) {
        methods->insert("HEAD");
    }
}

void HttpJunctionPrivate::collectRoutes(const string &prefix, int depth, vector<string> *result) const
{
    for (RouteTable::const_iterator it = routes.begin(); it != routes.end(); ++it) {
        for (MethodTable::const_iterator jt = it->second.begin(); jt != it->second.end(); ++jt) {
            result->push_back(jt->first + " " + prefix + it->first);
        }
    }
    if (depth >= MAX_MOUNT_DEPTH) {
        return;
    }
    for (std::map<std::string, HttpJunction *>::const_iterator it = mounts.begin(); it != mounts.end(); ++it) {
        it->second->d_func()->collectRoutes(prefix + it->first, depth + 1, result);
    }
}

const HttpJunction::Handler *HttpJunctionPrivate::findHandler(const vector<HttpJunctionPrivate *> &chain,
                                                              HttpJunction::Handler HttpJunctionPrivate::*member) const
{
    for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) {
        const HttpJunction::Handler &handler = chain[static_cast<size_t>(i)]->*member;
        if (handler) {
            return &handler;
        }
    }
    return nullptr;
}

HttpResponse HttpJunctionPrivate::runThrough(const vector<HttpJunctionPrivate *> &chain, const HttpRequest &request,
                                             const HttpJunction::Handler &handler)
{
    try {
        HttpJunction::Handler composed = handler;
        for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) {
            const vector<HttpJunction::Middleware> &list = chain[static_cast<size_t>(i)]->middlewares;
            for (int j = static_cast<int>(list.size()) - 1; j >= 0; --j) {
                HttpJunction::Middleware middleware = list[static_cast<size_t>(j)];
                HttpJunction::Handler next = composed;
                composed = [middleware, next](const HttpRequest &r) { return middleware(r, next); };
            }
        }
        return composed(request);
    } catch (...) {
        for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) {
            const HttpJunction::ExceptionHandler &exceptionHandler = chain[static_cast<size_t>(i)]->exceptionHandler;
            if (exceptionHandler) {
                return exceptionHandler(request, std::current_exception());
            }
        }
        throw;
    }
}

HttpResponse HttpJunctionPrivate::dispatch(const HttpRequest &request)
{
    string method = utils::toUpper(request.method());
    string path = request.url().path();
    if (path.empty()) {
        path = "/";
    } else if (path[0] != '/') {
        path.insert(path.begin(), '/');
    }

    // doMethod() is bypassed, so the RFC 10008 check from doQUERY() has to live here.
    if (method == "QUERY" && !request.hasHeader(ContentTypeHeader)) {
        return HttpResponse::plain("QUERY request without Content-Type", HttpStatus::BadRequest);
    }

    bool depthExceeded = false;
    vector<HttpJunctionPrivate *> chain;
    string relativePath;
    const HttpJunction::Handler *route = lookupRoute(method, path, 0, &chain, &relativePath, &depthExceeded);
    if (depthExceeded) {
        ngWarning() << "http junction mount depth exceeded" << MAX_MOUNT_DEPTH;
        return HttpResponse::status(HttpStatus::InternalServerError);
    }
    if (route) {
        if (relativePath == path) {
            return runThrough(chain, request, *route);
        } else {
            return runThrough(chain, rewritePath(request, relativePath), *route);
        }
    }

    const string flipped = flipTrailingSlash(path);
    if (!flipped.empty()) {
        vector<HttpJunctionPrivate *> flipChain;
        string flipRelativePath;
        bool flipDepthExceeded = false;
        if (lookupRoute(method, flipped, 0, &flipChain, &flipRelativePath, &flipDepthExceeded)
            && !flipDepthExceeded) {
            string location = flipped;
            const string &query = request.url().query();
            if (!query.empty()) {
                location += "?";
                location += query;
            }
            return runThrough(flipChain, request,
                              constantHandler(HttpResponse::redirect(location, HttpStatus::MovedPermanently)));
        }
    }

    std::set<string> allowedMethods;
    collectMethods(path, 0, &allowedMethods);
    if (!allowedMethods.empty()) {
        const HttpJunction::Handler *methodNotAllowed =
                findHandler(chain, &HttpJunctionPrivate::methodNotAllowedHandler);
        HttpResponse response = methodNotAllowed ? runThrough(chain, request, *methodNotAllowed)
                                                 : HttpResponse::status(HttpStatus::MethodNotAllowed);
        if (!response.hasHeader(AllowHeader)) {
            vector<string> allowed(allowedMethods.begin(), allowedMethods.end());
            response.setHeader(AllowHeader, utils::join(allowed, ", "));
        }
        return response;
    }

    const HttpJunction::Handler *notFound = findHandler(chain, &HttpJunctionPrivate::notFoundHandler);
    if (notFound) {
        return runThrough(chain, request, *notFound);
    }
    return HttpResponse::status(HttpStatus::NotFound);
}

HttpJunction::HttpJunction()
    : d_ptr(new HttpJunctionPrivate())
{
}

HttpJunction::~HttpJunction()
{
    delete d_ptr;
}

void HttpJunction::use(Middleware middleware)
{
    NG_D(HttpJunction);
    if (!middleware) {
        return;
    }
    d->middlewares.push_back(middleware);
}

void HttpJunction::group(const string &prefix, HttpJunction *sub)
{
    NG_D(HttpJunction);
    if (!sub || sub == this) {
        ngWarning() << "http junction can not mount a null junction or itself";
        return;
    }
    const string normalized = normalizePrefix(prefix);
    if (d->mounts.find(normalized) != d->mounts.end()) {
        ngWarning() << "http junction duplicate mount" << normalized;
    }
    d->mounts[normalized] = sub;
}

void HttpJunction::handle(const string &method, const string &path, Handler handler)
{
    NG_D(HttpJunction);
    if (path.empty() || path[0] != '/') {
        ngWarning() << "http junction route path must start with '/'" << path;
        return;
    }
    if (!handler) {
        ngWarning() << "http junction handler is empty for" << method << path;
        return;
    }
    const string normalizedMethod = utils::toUpper(method);
    if (normalizedMethod.empty()) {
        ngWarning() << "http junction route method is empty for" << path;
        return;
    }
    HttpJunctionPrivate::MethodTable &byMethod = d->routes[path];
    if (byMethod.find(normalizedMethod) != byMethod.end()) {
        ngWarning() << "http junction duplicate route" << normalizedMethod << path;
    }
    byMethod[normalizedMethod] = handler;
}

void HttpJunction::get(const string &path, Handler handler) { handle("GET", path, handler); }

void HttpJunction::head(const string &path, Handler handler) { handle("HEAD", path, handler); }

void HttpJunction::post(const string &path, Handler handler) { handle("POST", path, handler); }

void HttpJunction::put(const string &path, Handler handler) { handle("PUT", path, handler); }

void HttpJunction::patch(const string &path, Handler handler) { handle("PATCH", path, handler); }

void HttpJunction::delete_(const string &path, Handler handler) { handle("DELETE", path, handler); }

void HttpJunction::options(const string &path, Handler handler) { handle("OPTIONS", path, handler); }

void HttpJunction::query(const string &path, Handler handler) { handle("QUERY", path, handler); }

void HttpJunction::all(const string &path, Handler handler)
{
    for (const char *method : ROUTE_METHODS) {
        handle(method, path, handler);
    }
}

void HttpJunction::setNotFoundHandler(Handler handler)
{
    NG_D(HttpJunction);
    d->notFoundHandler = handler;
}

void HttpJunction::setMethodNotAllowedHandler(Handler handler)
{
    NG_D(HttpJunction);
    d->methodNotAllowedHandler = handler;
}

void HttpJunction::setExceptionHandler(ExceptionHandler handler)
{
    NG_D(HttpJunction);
    d->exceptionHandler = handler;
}

HttpResponse HttpJunction::dispatch(const HttpRequest &request)
{
    NG_D(HttpJunction);
    try {
        return d->dispatch(request);
    } catch (...) {
        ngWarning() << "unhandled exception escaped the http junction";
        return HttpResponse::plain("internal server error", HttpStatus::InternalServerError);
    }
}

vector<string> HttpJunction::allowedMethods(const string &path) const
{
    NG_D(const HttpJunction);
    std::set<string> methods;
    d->collectMethods(withLeadingSlash(path), 0, &methods);
    return vector<string>(methods.begin(), methods.end());
}

vector<string> HttpJunction::routes() const
{
    NG_D(const HttpJunction);
    vector<string> result;
    d->collectRoutes(string(), 0, &result);
    std::sort(result.begin(), result.end());
    return result;
}

JunctionRequestHandler::JunctionRequestHandler()
{
}

JunctionRequestHandler::~JunctionRequestHandler()
{
}

HttpJunction *JunctionRequestHandler::junction() const
{
    if (!server) {
        return nullptr;
    }
    return userData<HttpJunction>();
}

void JunctionRequestHandler::doMethod()
{
    HttpJunction *j = junction();
    if (!j) {
        sendError(HttpStatus::InternalServerError, "http junction is not configured");
        return;
    }

    utils::Url url(path);
    // Not named `request`: BaseRequestHandler already has a `request` member
    // pointing at the socket, and shadowing it here would invite bugs.
    HttpRequest httpRequest;
    httpRequest.setMethod(method);
    httpRequest.setUrl(url);
    httpRequest.setQuery(url.queryItems());
    httpRequest.setVersion(version);
    const string &cookieHeader = header(CookieHeader);
    if (!cookieHeader.empty()) {
        httpRequest.setCookies(HttpCookie::parseCookies(cookieHeader));
    }
    shared_ptr<FileLike> bodyFile = bodyAsFile();
    if (!bodyFile) {
        // bodyAsFile() has already sent the error response for an oversized or
        // badly encoded body.
        closeConnection = Yes;
        return;
    }
    // Copy the headers after bodyAsFile(): it strips the encodings it has
    // already decoded, so the handler never sees a stale Content-Encoding.
    httpRequest.setHeaders(allHeaders());
    httpRequest.setBody(bodyFile);

    HttpResponse response = j->dispatch(httpRequest);
    if (!writeResponse(response)) {
        // Same convention as BaseHttpProxyRequestHandler: a failed write leaves
        // the connection unusable, so do not try to reuse it.
        closeConnection = Yes;
        return;
    }
    // The rest of the body belongs to this request, not to the next one. Skip
    // it when the connection is already closing, and close when it cannot be
    // framed correctly anymore.
    if (closeConnection != Yes && !drainFile(bodyFile)) {
        closeConnection = Yes;
    }
}

bool JunctionRequestHandler::writeResponse(const HttpResponse &response)
{
    return writeServerResponse(response);
}

QTNG_NAMESPACE_END
