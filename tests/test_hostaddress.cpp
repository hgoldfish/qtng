#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "qtng/hostaddress.h"

using namespace std;

using namespace qtng;

TEST_CASE("HostAddress special addresses", "[hostaddress]")
{
    HostAddress nullAddr;
    REQUIRE(nullAddr == HostAddress::Null);
    REQUIRE(nullAddr.isNull());

    HostAddress localhost(HostAddress::LocalHost);
    REQUIRE(localhost == HostAddress::LocalHost);
    REQUIRE(localhost.isLoopback());
    REQUIRE(localhost.isIPv4());
    REQUIRE(localhost.toString() == "127.0.0.1");

    HostAddress any(HostAddress::Any);
    REQUIRE(any == HostAddress::Any);

    HostAddress any4(HostAddress::AnyIPv4);
    REQUIRE(any4.toString() == "0.0.0.0");

    HostAddress broadcast(HostAddress::Broadcast);
    REQUIRE(broadcast.isBroadcast());
    REQUIRE(broadcast.toString() == "255.255.255.255");

    HostAddress localhost6(HostAddress::LocalHostIPv6);
    REQUIRE(localhost6.protocol() == HostAddress::IPv6Protocol);
    REQUIRE(localhost6.isLoopback());
    REQUIRE(localhost6.toString() == "::1");

    HostAddress any6(HostAddress::AnyIPv6);
    REQUIRE(any6.protocol() == HostAddress::IPv6Protocol);
    REQUIRE(any6.toString() == "::");
}

TEST_CASE("HostAddress IPv4 parsing", "[hostaddress]")
{
    HostAddress addr("192.168.1.10");
    REQUIRE(addr.protocol() == HostAddress::IPv4Protocol);
    REQUIRE(addr.isIPv4());
    REQUIRE(addr.toString() == "192.168.1.10");

    HostAddress fromUint(0x7f000001);
    REQUIRE(fromUint.toString() == "127.0.0.1");

    HostAddress invalid("999.999.999.999");
    REQUIRE_FALSE(invalid.setAddress("999.999.999.999"));
}

TEST_CASE("HostAddress IPv6 parsing", "[hostaddress]")
{
    HostAddress addr;
    REQUIRE(addr.setAddress("2001:db8::1"));
    REQUIRE(addr.protocol() == HostAddress::IPv6Protocol);
    REQUIRE(addr.toString() == "2001:db8::1");

    HostAddress compressed;
    REQUIRE(compressed.setAddress("::ffff:192.0.2.1"));
    REQUIRE(compressed.protocol() == HostAddress::IPv6Protocol);
}

TEST_CASE("HostAddress copy and assignment", "[hostaddress]")
{
    HostAddress a("10.0.0.1");
    HostAddress b = a;
    REQUIRE(a == b);

    HostAddress c;
    c = a;
    REQUIRE(c == a);

    HostAddress d(HostAddress::LocalHost);
    c = HostAddress::LocalHost;
    REQUIRE(c == d);
}

TEST_CASE("HostAddress isEqual with conversion modes", "[hostaddress]")
{
    HostAddress v4mapped;
    REQUIRE(v4mapped.setAddress("::ffff:127.0.0.1"));
    HostAddress v4("127.0.0.1");

    REQUIRE(v4mapped.isEqual(v4, HostAddress::TolerantConversion));
    REQUIRE_FALSE(v4mapped.isEqual(v4, HostAddress::StrictConversion));
}

TEST_CASE("HostAddress subnet", "[hostaddress]")
{
    auto subnet = HostAddress::parseSubnet("192.168.0.0/24");
    REQUIRE(subnet.second == 24);
    REQUIRE(subnet.first.toString() == "192.168.0.0");

    HostAddress host("192.168.0.42");
    REQUIRE(host.isInSubnet(subnet));
    REQUIRE_FALSE(host.isInSubnet(HostAddress::parseSubnet("192.168.1.0/24")));
}

TEST_CASE("HostAddress scope id", "[hostaddress]")
{
    HostAddress addr;
    REQUIRE(addr.setAddress("fe80::1"));
    addr.setScopeId("eth0");
    REQUIRE(addr.scopeId() == "eth0");
}

TEST_CASE("HostAddress clear", "[hostaddress]")
{
    HostAddress addr("8.8.8.8");
    addr.clear();
    REQUIRE(addr.isNull());
}

TEST_CASE("HostAddress from IPv6 bytes", "[hostaddress]")
{
    uint8_t bytes[16] = {};
    bytes[15] = 1;
    HostAddress addr(bytes);
    REQUIRE(addr.toString() == "::1");
}

TEST_CASE("HostAddress operator equality", "[hostaddress]")
{
    HostAddress a("10.1.2.3");
    HostAddress b("10.1.2.3");
    HostAddress c("10.1.2.4");
    REQUIRE(a == b);
    REQUIRE(a != c);
    REQUIRE_FALSE(a == HostAddress::LocalHost);
    REQUIRE_FALSE(a == HostAddress::Broadcast);
}

TEST_CASE("HostAddress invalid strings", "[hostaddress]")
{
    HostAddress invalid;
    REQUIRE_FALSE(invalid.setAddress("not-an-ip"));
    REQUIRE_FALSE(invalid.setAddress("gggg::1"));

    HostAddress shorthand;
    REQUIRE(shorthand.setAddress("1.2.3"));
    REQUIRE(shorthand.toString() == "1.2.0.3");
}

TEST_CASE("HostAddress IPv4 leading zeros", "[hostaddress]")
{
    HostAddress addr;
    REQUIRE(addr.setAddress("127.000.000.001"));
    REQUIRE(addr.toString() == "127.0.0.1");
}

TEST_CASE("HostAddress isInSubnet negative", "[hostaddress]")
{
    HostAddress host("10.0.0.1");
    REQUIRE_FALSE(host.isInSubnet(make_pair(HostAddress(), -1)));
}
