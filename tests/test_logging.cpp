using namespace std;

#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <sstream>
#include <streambuf>

#include "qtng/utils/logging.h"

// NG_LOGGER may only be expanded at namespace scope (it emits file-static state
// and inline no-arg overloads). It is placed here in the anonymous namespace so
// the generated ngDebug()/ngInfo()/... bind to "macro.test" for the macro test
// case below. The qtng::utils::* calls are written fully qualified on purpose:
// a `using namespace qtng::utils;` would make the no-arg `ngDebug()` ambiguous,
// since the utils overloads declare a default category argument.
namespace {
NG_LOGGER("macro.test");

class StreamRedirect
{
public:
    explicit StreamRedirect(ostream &stream)
        : target(stream)
        , backup(stream.rdbuf(capture.rdbuf()))
    {
    }

    ~StreamRedirect() { target.rdbuf(backup); }

    string content() const { return capture.str(); }

private:
    ostream &target;
    streambuf *backup;
    ostringstream capture;
};
}  // namespace

TEST_CASE("logMessage formats all levels", "[logging]")
{
    StreamRedirect redirect(cerr);
    qtng::utils::logMessage(qtng::utils::LogLevel::Debug, "test.cat", "debug-msg");
    qtng::utils::logMessage(qtng::utils::LogLevel::Info, "test.cat", "info-msg");
    qtng::utils::logMessage(qtng::utils::LogLevel::Warning, "test.cat", "warn-msg");
    qtng::utils::logMessage(qtng::utils::LogLevel::Critical, "test.cat", "crit-msg");

    const string out = redirect.content();
    REQUIRE(out.find("DEBUG [test.cat] debug-msg") != string::npos);
    REQUIRE(out.find("INFO [test.cat] info-msg") != string::npos);
    REQUIRE(out.find("WARN [test.cat] warn-msg") != string::npos);
    REQUIRE(out.find("CRIT [test.cat] crit-msg") != string::npos);
}

TEST_CASE("logMessage null category falls back", "[logging]")
{
    StreamRedirect redirect(cerr);
    qtng::utils::logMessage(qtng::utils::LogLevel::Info, nullptr, "fallback");
    REQUIRE(redirect.content().find("INFO [qtng] fallback") != string::npos);
}

TEST_CASE("ngWarning with explicit category", "[logging]")
{
    StreamRedirect redirect(cerr);
    qtng::utils::ngWarning("test.cat") << "warn-msg";
    REQUIRE(redirect.content().find("WARN [test.cat] warn-msg") != string::npos);
}

TEST_CASE("LogStream accumulates chained output", "[logging]")
{
    StreamRedirect redirect(cerr);
    qtng::utils::ngDebug("stream.test") << "hello" << ' ' << 42;
    const string out = redirect.content();
    REQUIRE(out.find("DEBUG [stream.test] hello 42") != string::npos);
}

TEST_CASE("LogStream supports string and C string", "[logging]")
{
    StreamRedirect redirect(cerr);
    qtng::utils::ngWarning("stream.test") << string("warn") << " " << static_cast<const char *>(nullptr);
    const string out = redirect.content();
    REQUIRE(out.find("WARN [stream.test] warn ") != string::npos);
}

TEST_CASE("NG_LOGGER binds no-arg overloads to the category", "[logging]")
{
    StreamRedirect redirect(cerr);
    ngDebug() << "dbg";
    ngInfo() << "info";
    const string out = redirect.content();
    REQUIRE(out.find("DEBUG [macro.test] dbg") != string::npos);
    REQUIRE(out.find("INFO [macro.test] info") != string::npos);
}
