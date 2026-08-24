#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "qtng/private/ssh_p.h"
#include "qtng/coroutine.h"
#include "qtng/hostaddress.h"
#include "qtng/pkey.h"
#include "qtng/socket.h"
#include "qtng/ssh.h"

using namespace std;
using namespace qtng;

namespace {

class TestAuthenticator : public SshAuthenticator
{
public:
    string user;
    string password;
    string keyBlob;

    bool checkPassword(const string &u, const string &p) override { return u == user && p == password; }

    bool checkPublicKey(const string &u, const string &blob) override { return u == user && blob == keyBlob; }
};

struct SharedState
{
    bool resized = false;
    bool signalled = false;
    SshTerminalSize size;
    string signalName;
    bool appStarted = false;
    bool appFinished = false;
};

class RecordingCallback : public SshChannelCallback
{
public:
    explicit RecordingCallback(const shared_ptr<SharedState> &state)
        : state(state)
    {
    }

    void onResize(const SshTerminalSize &s) override
    {
        state->size = s;
        state->resized = true;
    }

    void onSignal(const string &name) override
    {
        state->signalName = name;
        state->signalled = true;
    }

    void onClose() override { }

private:
    shared_ptr<SharedState> state;
};

class EchoApplication : public SshApplication
{
public:
    explicit EchoApplication(const shared_ptr<SharedState> &state)
        : state(state)
    {
    }

    void run(SshChannel *channel) override
    {
        state->appStarted = true;
        channel->setCallback(make_shared<RecordingCallback>(state));
        if (channel->send("welcome\n") < 0) {
            return;
        }
        while (true) {
            string data = channel->recv(4096);
            if (data.empty()) {
                break;
            }
            if (channel->send(data) < 0) {
                break;
            }
        }
        state->appFinished = true;
    }

private:
    shared_ptr<SharedState> state;
};

PrivateKey makeRsaKey(int bits)
{
    return PrivateKey::generate(PublicKey::Rsa, bits);
}

}  // namespace

TEST_CASE("SshBuffer round-trips RFC 4251 types", "[ssh]")
{
    SshBuffer buf;
    buf.putByte(0x42);
    buf.putUint32(0xdeadbeefU);
    buf.putString("hello");
    buf.putBoolean(true);
    buf.putBoolean(false);
    buf.putMpint(string("\x7f\x80", 2));
    buf.putNameList({"curve25519-sha256", "aes128-ctr"});
    buf.putBytes("xyz");

    SshBuffer reader(buf.raw());
    uint8_t b = 0;
    REQUIRE(reader.getByte(&b));
    REQUIRE(b == 0x42);
    uint32_t u = 0;
    REQUIRE(reader.getUint32(&u));
    REQUIRE(u == 0xdeadbeefU);
    string s;
    REQUIRE(reader.getString(&s));
    REQUIRE(s == "hello");
    bool flag = false;
    REQUIRE(reader.getBoolean(&flag));
    REQUIRE(flag);
    REQUIRE(reader.getBoolean(&flag));
    REQUIRE_FALSE(flag);
    string mp;
    REQUIRE(reader.getMpint(&mp));
    REQUIRE(mp == string("\x7f\x80", 2));
    vector<string> names;
    REQUIRE(reader.getNameList(&names));
    REQUIRE(names == vector<string>({"curve25519-sha256", "aes128-ctr"}));
    string rest;
    REQUIRE(reader.getBytes(3, &rest));
    REQUIRE(rest == "xyz");
    REQUIRE(reader.isAtEnd());
}

TEST_CASE("SshBuffer rejects truncated reads and leaves offset untouched", "[ssh]")
{
    string payload;
    payload.append(4, '\0');
    ngToBigEndian<uint32_t>(8, &payload[0]);
    payload.append("short");  // string claims 8 bytes but only 5 follow
    SshBuffer buf(payload);
    string s;
    REQUIRE_FALSE(buf.getString(&s));
    REQUIRE(buf.offsetPos() == 0);  // offset unchanged on failure

    // a following read from the same position still sees the string prefix
    uint32_t u = 0;
    REQUIRE(buf.getUint32(&u));
    REQUIRE(u == 8);
    REQUIRE(buf.offsetPos() == 4);

    // truncated mpint and namelist reads fail without advancing
    string payload2;
    payload2.append(4, '\0');
    ngToBigEndian<uint32_t>(10, &payload2[0]);
    payload2.append("12345");  // claims 10 bytes, only 5 present
    SshBuffer buf2(payload2);
    string mp;
    REQUIRE_FALSE(buf2.getMpint(&mp));
    REQUIRE(buf2.offsetPos() == 0);
    vector<string> names;
    REQUIRE_FALSE(buf2.getNameList(&names));
    REQUIRE(buf2.offsetPos() == 0);
}

TEST_CASE("SshBuffer mpint edge cases", "[ssh]")
{
    // zero is a zero-length string
    SshBuffer zero;
    zero.putMpint(string());
    REQUIRE(zero.raw() == string("\x00\x00\x00\x00", 4));

    // positive number with the high bit set gets a 0x00 prefix
    SshBuffer hi;
    hi.putMpint(string("\x80", 1));
    SshBuffer hiReader(hi.raw());
    string s;
    REQUIRE(hiReader.getString(&s));
    REQUIRE(s == string("\x00\x80", 2));

    // leading zero bytes are stripped
    SshBuffer lead;
    lead.putMpint(string("\x00\x00\x01", 3));
    SshBuffer leadReader(lead.raw());
    REQUIRE(leadReader.getString(&s));
    REQUIRE(s == string("\x01", 1));

    // round-trip of a 32-byte curve25519-like shared secret with the high bit
    // set: encoding must add a 0x00 sign byte, decoding keeps it.
    string secret(32, '\x00');
    secret[0] = '\x9f';
    SshBuffer rt;
    rt.putMpint(secret);
    SshBuffer rtReader(rt.raw());
    string out;
    REQUIRE(rtReader.getMpint(&out));
    REQUIRE(out.size() == secret.size() + 1);
    REQUIRE(out[0] == '\0');
    REQUIRE(out.substr(1) == secret);
}

TEST_CASE("RSA public key blob round-trips", "[ssh]")
{
    const PrivateKey key = makeRsaKey(1024);
    REQUIRE(key.isValid());

    const string blob = sshRsaKeyBlob(key.publicKey());
    REQUIRE_FALSE(blob.empty());
    REQUIRE(sshKeyAlgorithmName(blob) == "ssh-rsa");

    PublicKey parsed;
    REQUIRE(sshParseRsaKeyBlob(blob, &parsed));
    REQUIRE_FALSE(parsed.isNull());

    // The parsed key must verify a signature made by the original private key.
    const string data = "exchange-hash-like-data";
    PrivateKey signingKey = key;
    const string signature = signingKey.sign(data, MessageDigest::Sha256);
    REQUIRE_FALSE(signature.empty());
    REQUIRE(parsed.verify(data, signature, MessageDigest::Sha256));

    REQUIRE_FALSE(sshParseRsaKeyBlob(string(), &parsed));
    REQUIRE_FALSE(sshParseRsaKeyBlob(string("\x00\x01\x02", 3), &parsed));
}

TEST_CASE("Ssh loopback: password auth, echo, pty, resize and signal", "[ssh]")
{
    auto state = make_shared<SharedState>();
    auto auth = make_shared<TestAuthenticator>();
    auth->user = "bob";
    auth->password = "secret";
    auto app = make_shared<EchoApplication>(state);
    const PrivateKey hostKey = makeRsaKey(1024);

    SshServer server(HostAddress::LocalHost, 0);
    server.setHostKey(hostKey);
    server.setAuthenticator(auth);
    server.setApplication(app);
    server.setBanner("Welcome to qtng ssh test.\n");
    server.setMaxAuthTries(3);
    server.setLoginTimeout(10.0f);
    REQUIRE(server.start());
    const uint16_t port = server.serverPort();
    REQUIRE(port != 0);

    {
        SshClient client;
        client.setLoginTimeout(10.0f);
        REQUIRE(client.connect(HostAddress::LocalHost, port));

        // wrong password must be rejected
        REQUIRE_FALSE(client.authenticate("bob", "wrong"));
        REQUIRE(client.authenticate("bob", "secret"));

        shared_ptr<SshChannel> channel = client.openSessionChannel();
        REQUIRE(channel);
        REQUIRE(channel->requestPty("xterm", 100, 40));
        REQUIRE(channel->requestShell());

        const string welcome = channel->recv(4096);
        REQUIRE(welcome == "welcome\n");

        REQUIRE(channel->send("hello") == 5);
        const string echo = channel->recv(4096);
        REQUIRE(echo == "hello");

        REQUIRE(channel->requestWindowChange(120, 50));
        REQUIRE(channel->sendSignal("INT"));

        // allow the server read-loop to deliver the notifications
        for (int i = 0; i < 50 && (!state->resized || !state->signalled); ++i) {
            Coroutine::msleep(20);
        }
        REQUIRE(state->resized);
        REQUIRE(state->size.columns == 120);
        REQUIRE(state->size.rows == 50);
        REQUIRE(state->signalled);
        REQUIRE(state->signalName == "INT");

        channel->close();
        client.close();
    }

    // clean shutdown of the accept loop
    server.stop();
}

TEST_CASE("Ssh loopback: publickey authentication", "[ssh]")
{
    auto auth = make_shared<TestAuthenticator>();
    auth->user = "alice";
    auto app = make_shared<EchoApplication>(make_shared<SharedState>());
    const PrivateKey hostKey = makeRsaKey(1024);
    const PrivateKey userKey = makeRsaKey(1024);

    auth->keyBlob = sshRsaKeyBlob(userKey.publicKey());
    REQUIRE_FALSE(auth->keyBlob.empty());

    SshServer server(HostAddress::LocalHost, 0);
    server.setHostKey(hostKey);
    server.setAuthenticator(auth);
    server.setApplication(app);
    server.setLoginTimeout(10.0f);
    REQUIRE(server.start());
    const uint16_t port = server.serverPort();
    REQUIRE(port != 0);

    {
        SshClient client;
        client.setLoginTimeout(10.0f);
        REQUIRE(client.connect(HostAddress::LocalHost, port));
        REQUIRE(client.authenticateWithPublicKey("alice", userKey));

        shared_ptr<SshChannel> channel = client.openSessionChannel();
        REQUIRE(channel);
        REQUIRE(channel->recv(4096) == "welcome\n");
        channel->close();
        client.close();
    }

    server.stop();
}

TEST_CASE("Ssh loopback: unknown user is rejected", "[ssh]")
{
    auto auth = make_shared<TestAuthenticator>();
    auth->user = "bob";
    auth->password = "secret";
    auto app = make_shared<EchoApplication>(make_shared<SharedState>());
    const PrivateKey hostKey = makeRsaKey(1024);

    SshServer server(HostAddress::LocalHost, 0);
    server.setHostKey(hostKey);
    server.setAuthenticator(auth);
    server.setApplication(app);
    server.setLoginTimeout(10.0f);
    REQUIRE(server.start());
    const uint16_t port = server.serverPort();

    {
        SshClient client;
        client.setLoginTimeout(10.0f);
        REQUIRE(client.connect(HostAddress::LocalHost, port));
        REQUIRE_FALSE(client.authenticate("mallory", "secret"));
        client.close();
    }

    server.stop();
}

#ifndef _WIN32

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

string sshBinaryPath()
{
    const char *p = getenv("QTNG_SSH");
    return p && *p ? string(p) : string("ssh");
}

void setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void drainFd(int fd, string *out)
{
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out->append(buf, static_cast<size_t>(n));
    }
}

// Write stdinData to the child, then wait (polling so the event loop keeps
// serving the SSH connection). Returns the child exit status.
int runSsh(const vector<string> &args, const string &stdinData, string *stdoutData, string *stderrData)
{
    int inPipe[2] = { -1, -1 };
    int outPipe[2] = { -1, -1 };
    int errPipe[2] = { -1, -1 };
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(inPipe[0], 0);
        dup2(outPipe[1], 1);
        dup2(errPipe[1], 2);
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        vector<char *> argv;
        argv.push_back(const_cast<char *>("ssh"));
        for (const string &a : args) {
            argv.push_back(const_cast<char *>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (pid < 0) {
        return -1;
    }

    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    setNonBlocking(inPipe[1]);
    setNonBlocking(outPipe[0]);
    setNonBlocking(errPipe[0]);

    size_t written = 0;
    const int timeoutMs = 60000;
    for (int elapsed = 0; elapsed < timeoutMs;) {
        if (written < stdinData.size()) {
            ssize_t n = write(inPipe[1], stdinData.data() + written, stdinData.size() - written);
            if (n > 0) {
                written += static_cast<size_t>(n);
            }
        }
        if (written == stdinData.size()) {
            close(inPipe[1]);
            inPipe[1] = -1;
        }
        drainFd(outPipe[0], stdoutData);
        drainFd(errPipe[0], stderrData);

        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (inPipe[1] >= 0) {
                close(inPipe[1]);
            }
            drainFd(outPipe[0], stdoutData);
            drainFd(errPipe[0], stderrData);
            close(outPipe[0]);
            close(errPipe[0]);
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            return -1;
        }
        Coroutine::msleep(20);
        elapsed += 20;
    }

    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    close(inPipe[1]);
    close(outPipe[0]);
    close(errPipe[0]);
    return -1;
}

}  // namespace

TEST_CASE("OpenSSH client interoperates over a session channel", "[ssh][interop]")
{
    if (sshBinaryPath() == "ssh" && access("/usr/bin/ssh", X_OK) != 0 && access("/bin/ssh", X_OK) != 0) {
        SKIP("ssh client not found");
    }

    auto auth = make_shared<TestAuthenticator>();
    auth->user = "bob";
    auto app = make_shared<EchoApplication>(make_shared<SharedState>());
    const PrivateKey hostKey = makeRsaKey(2048);
    const PrivateKey userKey = makeRsaKey(2048);
    auth->keyBlob = sshRsaKeyBlob(userKey.publicKey());
    REQUIRE_FALSE(auth->keyBlob.empty());

    // write the user key to a temp file for ssh -i
    string keyFile = "/tmp/qtng_ssh_test_key_XXXXXX";
    vector<char> keyPath(keyFile.begin(), keyFile.end());
    keyPath.push_back('\0');
    const int fd = mkstemp(keyPath.data());
    REQUIRE(fd >= 0);
    keyFile.assign(keyPath.data());
    {
        const string pem = userKey.save(Ssl::Pem);
        REQUIRE_FALSE(pem.empty());
        const ssize_t w = write(fd, pem.data(), pem.size());
        REQUIRE(w == static_cast<ssize_t>(pem.size()));
        close(fd);
    }

    SshServer server(HostAddress::LocalHost, 0);
    server.setHostKey(hostKey);
    server.setAuthenticator(auth);
    server.setApplication(app);
    server.setLoginTimeout(10.0f);
    REQUIRE(server.start());
    const uint16_t port = server.serverPort();
    REQUIRE(port != 0);

    string stdoutData;
    string stderrData;
    vector<string> args;
    args.push_back("-v");
    args.push_back("-T");
    args.push_back("-o");
    args.push_back("StrictHostKeyChecking=no");
    args.push_back("-o");
    args.push_back("UserKnownHostsFile=/dev/null");
    args.push_back("-o");
    args.push_back("BatchMode=yes");
    args.push_back("-o");
    args.push_back("IdentitiesOnly=yes");
    args.push_back("-o");
    args.push_back("PasswordAuthentication=no");
    args.push_back("-i");
    args.push_back(keyFile);
    args.push_back("-p");
    args.push_back(to_string(port));
    args.push_back("bob@127.0.0.1");

    const int exitCode = runSsh(args, "ping\n", &stdoutData, &stderrData);

    unlink(keyFile.c_str());
    server.stop();

    CAPTURE(exitCode, stdoutData, stderrData);
    REQUIRE(exitCode == 0);
    REQUIRE(stdoutData.find("ping") != string::npos);
}

#endif  // !_WIN32
