// ssh-bbs: a minimal BBS-style SSH application server.
//
// Login with any OpenSSH client:
//
//   ssh user@localhost -p 2222
//
// The session channel carries a plain ANSI terminal stream (no PTY, no
// fork/exec). The server answers pty-req/window-change/signal so that
// terminal resizes and Ctrl+C arrive as notifications.
//
// Run:
//
//   ./ssh-bbs --port 2222 --user user --password pass123

#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "qtng/hostaddress.h"
#include "qtng/pkey.h"
#include "qtng/ssh.h"

using namespace std;
using namespace qtng;

namespace {

// ---- command line helpers ----

string argValue(int argc, char **argv, const string &name, const string &fallback)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return argv[i + 1];
        }
    }
    return fallback;
}

bool hasArg(int argc, char **argv, const string &name)
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == name) {
            return true;
        }
    }
    return false;
}

PrivateKey loadOrCreateHostKey(const string &path)
{
    ifstream in(path.c_str());
    if (in) {
        string pem((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
        const PrivateKey key = PrivateKey::load(pem, Ssl::Pem);
        if (key.isValid()) {
            return key;
        }
    }
    const PrivateKey key = PrivateKey::generate(PublicKey::Rsa, 2048);
    ofstream out(path.c_str());
    if (out) {
        out << key.save(Ssl::Pem);
    }
    return key;
}

class DemoAuthenticator : public SshAuthenticator
{
public:
    string user;
    string password;

    bool checkPassword(const string &u, const string &p) override { return u == user && p == password; }

    bool checkPublicKey(const string &, const string &) override { return false; }
};

// ---- ANSI terminal screen over one SSH session channel ----

class BbsScreen
{
public:
    explicit BbsScreen(SshChannel *channel)
        : channel(channel)
    {
    }

    void clear() { channel->send("\x1b[2J\x1b[H"); }
    void showCursor() { channel->send("\x1b[?25h"); }
    void hideCursor() { channel->send("\x1b[?25l"); }
    void print(const string &text) { channel->send(text); }

    // Echo a single line; returns "" on EOF. Ctrl+C marks interrupted().
    string readLine()
    {
        string line;
        while (true) {
            const string chunk = channel->recv(1);
            if (chunk.empty()) {
                break;
            }
            const char c = chunk[0];
            if (c == '\r' || c == '\n') {
                print("\r\n");
                break;
            }
            if (c == 0x03) {  // ^C delivered as SIGINT on the pty
                interrupted = true;
                print("^C\r\n");
                break;
            }
            if (c == 0x7f || c == 0x08) {  // backspace
                if (!line.empty()) {
                    line.pop_back();
                    print("\b \b");
                }
                continue;
            }
            if (c >= 32 && c < 127) {
                line.push_back(c);
                print(string(1, c));
            }
        }
        return line;
    }

    bool interrupted = false;

private:
    SshChannel *channel;
};

// ---- the BBS application ----

class BbsApplication : public SshApplication
{
public:
    void run(SshChannel *channel) override
    {
        channel->setCallback(shared_ptr<SshChannelCallback>(new BbsCallback(this)));
        BbsScreen screen(channel);
        screen.hideCursor();

        while (true) {
            const SshTerminalSize size = channel->terminalSize();
            screen.clear();
            screen.print(header(size));
            screen.print(" 1. System information\r\n");
            screen.print(" 2. Echo (party line)\r\n");
            screen.print(" 3. Quit\r\n");
            screen.print("\r\n> ");
            const string choice = screen.readLine();
            if (screen.interrupted || choice == "3" || choice == "q") {
                break;
            } else if (choice == "1") {
                showSystemInfo(channel, screen, size);
            } else if (choice == "2") {
                echoMode(channel, screen);
            } else if (!choice.empty()) {
                screen.print("\r\nUnknown choice. Press Enter to continue.");
                screen.readLine();
            }
        }

        screen.clear();
        screen.showCursor();
        screen.print("Thanks for visiting. Bye.\r\n");
        channel->close();
    }

private:
    struct BbsCallback : public SshChannelCallback
    {
        explicit BbsCallback(BbsApplication *app)
            : app(app)
        {
        }
        void onResize(const SshTerminalSize &size) override
        {
            app->lastResize = size;
            app->resized = true;
        }
        void onSignal(const string &name) override
        {
            app->lastSignal = name;
            app->signalled = true;
        }
        void onClose() override { app->closed = true; }
        BbsApplication *app;
    };

    string header(const SshTerminalSize &size)
    {
        string out = "\x1b[1;36m";
        out += "  ================== qtng SSH BBS ==================\r\n";
        out += "\x1b[0m\r\n";
        out += "  terminal " + to_string(size.columns) + "x" + to_string(size.rows) + "\r\n";
        if (resized) {
            out += "  last resize: " + to_string(lastResize.columns) + "x" + to_string(lastResize.rows) + "\r\n";
        }
        if (signalled) {
            out += "  last signal: " + lastSignal + "\r\n";
        }
        out += "\r\n";
        return out;
    }

    static void showSystemInfo(SshChannel *channel, BbsScreen &screen, const SshTerminalSize &size)
    {
        screen.print("\x1b[1;33m");
        screen.print("\r\n  System information\r\n");
        screen.print("\x1b[0m");
        screen.print("\r\n");
        screen.print("  Server     : qtng ssh-bbs example\r\n");
        screen.print("  Terminal   : " + to_string(size.columns) + "x" + to_string(size.rows) + "\r\n");
        screen.print("  Channel    : session (no pty/exec)\r\n");
        screen.print("\r\n  Press Enter to return to the menu.");
        screen.readLine();
    }

    static void echoMode(SshChannel *channel, BbsScreen &screen)
    {
        screen.print("\x1b[1;33m\r\n  Echo party. Type a line, or a blank line to return.\r\n");
        screen.print("\x1b[0m\r\n");
        while (true) {
            screen.print("  echo> ");
            const string line = screen.readLine();
            if (screen.interrupted || line.empty()) {
                break;
            }
            screen.print("       " + line + "\r\n");
        }
    }

    bool resized = false;
    SshTerminalSize lastResize;
    bool signalled = false;
    string lastSignal;
    bool closed = false;
};

}  // namespace

int main(int argc, char **argv)
{
    if (hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h")) {
        cerr << "usage: ssh-bbs [--port PORT] [--user USER] [--password PASS]\n"
                "                [--hostkey FILE] [--banner TEXT]\n";
        return 0;
    }

    const uint16_t port = static_cast<uint16_t>(atoi(argValue(argc, argv, "--port", "2222").c_str()));
    const string user = argValue(argc, argv, "--user", "user");
    const string password = argValue(argc, argv, "--password", "pass123");
    const string hostKeyPath = argValue(argc, argv, "--hostkey", ".ssh_bbs_host_key");
    const string banner = argValue(argc, argv, "--banner", "Welcome to the qtng SSH BBS.\r\n");

    auto auth = make_shared<DemoAuthenticator>();
    auth->user = user;
    auth->password = password;

    const PrivateKey hostKey = loadOrCreateHostKey(hostKeyPath);
    if (!hostKey.isValid()) {
        cerr << "failed to load or create host key\n";
        return 1;
    }

    SshServer server(HostAddress::Any, port);
    server.setHostKey(hostKey);
    server.setAuthenticator(auth);
    server.setApplication(make_shared<BbsApplication>());
    server.setBanner(banner);
    server.setMaxAuthTries(3);
    server.setLoginTimeout(60.0f);

    cout << "ssh-bbs listening on port " << port << " (login: " << user << "/" << password << ")\n";
    cout << "connect with: ssh " << user << "@localhost -p " << port << "\n";
    return server.serveForever() ? 0 : 1;
}
