#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <string>

#include "qtng/certificate.h"
#include "qtng/crypto.h"
#include "qtng/http.h"
#include "qtng/httpd.h"
#include "qtng/pkey.h"
#include "qtng/ssl.h"
#include "qtng/socket_server.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/string_utils.h"

using namespace std;
using namespace qtng;

namespace {

class UploadHandler : public BaseHttpRequestHandler
{
public:
    virtual void doPOST() override
    {
        if (path == "/reject") {
            // A server may refuse the upload without reading the body and answer
            // 4xx immediately. The client must observe that reply instead of
            // blocking forever on the send side.
            sendError(HttpStatus::BadRequest, "refused");
            return;
        }
        if (!readBody()) {
            sendError(HttpStatus::BadRequest, "cannot read body");
            return;
        }
        const string &b = body;
        sendResponse(HttpStatus::OK);
        sendHeader("Content-Type", "text/plain");
        sendHeader("Content-Length", utils::number(static_cast<long long>(b.size())));
        if (!endHeader()) {
            return;
        }
        if (!request->sendall(b)) {
            return;
        }
    }
};

class UploadFixture
{
public:
    UploadFixture()
    {
        PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
        REQUIRE_FALSE(key.isNull());
        multimap<Certificate::SubjectInfo, string> subject;
        subject.insert(make_pair(Certificate::Organization, "qtng test"));
        Certificate cert = Certificate::selfSign(key, MessageDigest::Sha256, 1,
                                                 utils::DateTime::currentDateTimeUtc(),
                                                 utils::DateTime::currentDateTimeUtc().addSecs(3600), subject);
        REQUIRE_FALSE(cert.isNull());
        serverConfig.setLocalCertificate(cert);
        serverConfig.setPrivateKey(key);
        REQUIRE(server.start());
        clientConfig = make_shared<SslConfiguration>();
        clientConfig->addCaCertificate(cert);
        clientConfig->setPeerVerifyMode(Ssl::VerifyNone);
        session = make_shared<HttpSession>();
        session->setSslConfiguration(clientConfig);
        session->setManagingCookies(false);
    }

    string baseUrl() const
    {
        return utils::formatMessage("https://127.0.0.1:%1",
                                    {utils::number(static_cast<uint16_t>(server.serverPort()))});
    }

    SslServer<UploadHandler> server{HostAddress::LocalHost, 0};
    SslConfiguration serverConfig;
    shared_ptr<SslConfiguration> clientConfig;
    shared_ptr<HttpSession> session;
};

}  // namespace

TEST_CASE("HTTPS upload of a body larger than one TLS record", "[ssl][http1][upload]")
{
    UploadFixture fixture;
    const string bigBody(1024 * 1024, 'x');
    HttpResponse response = fixture.session->post(fixture.baseUrl() + "/upload", bigBody);
    REQUIRE(response.isOk());
    REQUIRE(response.body() == bigBody);
}

TEST_CASE("HTTPS upload answered early with 400 before the body is consumed", "[ssl][http1][upload]")
{
    UploadFixture fixture;
    const string bigBody(1024 * 1024, 'x');
    HttpResponse response = fixture.session->post(fixture.baseUrl() + "/reject", bigBody);
    REQUIRE(response.statusCode() == 400);
}

TEST_CASE("plain HTTP upload keeps working with the same interleaved read/write", "[http1][upload]")
{
    TcpServer<UploadHandler> server(HostAddress::LocalHost, 0);
    REQUIRE(server.start());
    const string host =
            utils::formatMessage("http://127.0.0.1:%1",
                                 {utils::number(static_cast<uint16_t>(server.serverPort()))});
    HttpSession session;
    session.setManagingCookies(false);

    const string bigBody(1024 * 1024, 'x');
    HttpResponse ok = session.post(host + "/upload", bigBody);
    REQUIRE(ok.isOk());
    REQUIRE(ok.body() == bigBody);

    HttpResponse rej = session.post(host + "/reject", bigBody);
    REQUIRE(rej.statusCode() == 400);
}
