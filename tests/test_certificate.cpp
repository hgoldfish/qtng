#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include "qtng/certificate.h"
#include "qtng/md.h"
#include "qtng/pkey.h"
#include "qtng/utils/datetime.h"

using namespace std;
using namespace qtng;

TEST_CASE("Certificate selfSign, roundtrip, and repeated fields", "[certificate]")
{
    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    REQUIRE_FALSE(key.isNull());
    const utils::DateTime now = utils::DateTime::currentDateTimeUtc();
    const Certificate cert = Certificate::selfSign(
            key, MessageDigest::Sha256, 42, now, now.addSecs(365 * 24 * 3600),
            {
                    {Certificate::CommonName, "qtng.test"},
                    {Certificate::Organization, "qtng"},
                    {Certificate::CountryName, "CN"},
                    {Certificate::OrganizationalUnitName, "core"},
                    {Certificate::OrganizationalUnitName, "crypto"},
            });
    REQUIRE_FALSE(cert.isNull());
    REQUIRE(cert.isSelfSigned());
    REQUIRE(cert.serialNumber() == "42");
    REQUIRE(cert.subjectInfo(Certificate::CommonName) == vector<string>{"qtng.test"});
    REQUIRE(cert.issuerInfo(Certificate::CommonName) == vector<string>{"qtng.test"});
    REQUIRE(cert.subjectInfo(Certificate::Organization) == vector<string>{"qtng"});
    REQUIRE(cert.issuerInfo("O") == vector<string>{"qtng"});
    REQUIRE(cert.subjectInfo("C") == vector<string>{"CN"});
    REQUIRE(cert.subjectInfo(Certificate::OrganizationalUnitName) == vector<string>{"core", "crypto"});

    const string pem = cert.save(Ssl::Pem);
    REQUIRE_FALSE(pem.empty());
    const Certificate fromPem = Certificate::load(pem, Ssl::Pem);
    REQUIRE_FALSE(fromPem.isNull());
    REQUIRE(fromPem.subjectInfo(Certificate::CommonName) == cert.subjectInfo(Certificate::CommonName));
    REQUIRE(fromPem.serialNumber() == cert.serialNumber());

    const string der = cert.save(Ssl::Der);
    REQUIRE_FALSE(der.empty());
    const Certificate fromDer = Certificate::load(der, Ssl::Der);
    REQUIRE_FALSE(fromDer.isNull());
    REQUIRE(fromDer.digest() == cert.digest());
}
