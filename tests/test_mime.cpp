using namespace std;

#include <catch2/catch_test_macros.hpp>

#include "qtng/utils/mime.h"

using namespace qtng::utils;


TEST_CASE("mimeTypeForExtension known types", "[mime]")
{
    REQUIRE(mimeTypeForExtension(".html") == "text/html");
    REQUIRE(mimeTypeForExtension(".htm") == "text/html");
    REQUIRE(mimeTypeForExtension(".css") == "text/css");
    REQUIRE(mimeTypeForExtension(".js") == "application/javascript");
    REQUIRE(mimeTypeForExtension(".json") == "application/json");
    REQUIRE(mimeTypeForExtension(".xml") == "application/xml");
    REQUIRE(mimeTypeForExtension(".txt") == "text/plain");
    REQUIRE(mimeTypeForExtension(".png") == "image/png");
    REQUIRE(mimeTypeForExtension(".jpg") == "image/jpeg");
    REQUIRE(mimeTypeForExtension(".jpeg") == "image/jpeg");
    REQUIRE(mimeTypeForExtension(".gif") == "image/gif");
    REQUIRE(mimeTypeForExtension(".svg") == "image/svg+xml");
    REQUIRE(mimeTypeForExtension(".ico") == "image/x-icon");
    REQUIRE(mimeTypeForExtension(".pdf") == "application/pdf");
    REQUIRE(mimeTypeForExtension(".zip") == "application/zip");
    REQUIRE(mimeTypeForExtension(".gz") == "application/gzip");
    REQUIRE(mimeTypeForExtension(".wasm") == "application/wasm");
}

TEST_CASE("mimeTypeForExtension case insensitive", "[mime]")
{
    REQUIRE(mimeTypeForExtension(".HTML") == "text/html");
    REQUIRE(mimeTypeForExtension(".Js") == "application/javascript");
}

TEST_CASE("mimeTypeForExtension unknown", "[mime]")
{
    REQUIRE(mimeTypeForExtension(".unknown") == "application/octet-stream");
    REQUIRE(mimeTypeForExtension("") == "application/octet-stream");
}

TEST_CASE("mimeTypeForFileName", "[mime]")
{
    REQUIRE(mimeTypeForFileName("index.html") == "text/html");
    REQUIRE(mimeTypeForFileName("/path/to/data.JSON") == "application/json");
    REQUIRE(mimeTypeForFileName("README") == "application/octet-stream");
    REQUIRE(mimeTypeForFileName("archive.tar.gz") == "application/gzip");
}

TEST_CASE("mimeTypeForExtension without dot", "[mime]")
{
    REQUIRE(mimeTypeForExtension("html") == "application/octet-stream");
}

TEST_CASE("mimeTypeForFileName no extension", "[mime]")
{
    REQUIRE(mimeTypeForFileName("Makefile") == "application/octet-stream");
    REQUIRE(mimeTypeForFileName(".hidden") == "application/octet-stream");
}
