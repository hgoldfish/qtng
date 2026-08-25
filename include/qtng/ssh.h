#ifndef QTNG_SSH_H
#define QTNG_SSH_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/pkey.h"
#include "qtng/socket_server.h"
#include "qtng/socket_utils.h"

namespace qtng {

class SshChannelPrivate;
class SshServerPrivate;
class SshClientPrivate;
class SshConnectionPrivate;

// Terminal size of the SSH session channel (updated by window-change requests).
struct SshTerminalSize
{
public:
    SshTerminalSize() = default;

    std::uint32_t columns() const { return columns_; }
    void setColumns(std::uint32_t columns) { columns_ = columns; }
    std::uint32_t rows() const { return rows_; }
    void setRows(std::uint32_t rows) { rows_ = rows; }

private:
    std::uint32_t columns_ = 80;
    std::uint32_t rows_ = 24;
};

// Notifications delivered from the connection's read-loop coroutine.
// Implement on a subclass and set it via SshChannel::setCallback().
class SshChannelCallback
{
public:
    virtual ~SshChannelCallback() = default;
    virtual void onResize(const SshTerminalSize &size) = 0;
    virtual void onSignal(const std::string &name) = 0;
    virtual void onClose() = 0;
};

// A single SSH "session" channel. On the server side the application receives
// it from SshServer and drives the TUI through send()/recv(). On the client
// side SshClient::openSessionChannel() returns one.
class SshChannel
{
public:
    virtual ~SshChannel();

    // Send data to the peer (blocking, coroutine-safe). Returns bytes sent or
    // -1 on failure. Empty data returns 0.
    std::int32_t send(const std::string &data);
    // Receive data from the peer (blocking). Returns an empty string on EOF or
    // when the channel/connection is closed. At most maxSize bytes are returned.
    std::string recv(std::int32_t maxSize);
    // Send EOF then close the channel.
    void close();
    bool isClosed() const;
    SshTerminalSize terminalSize() const;
    // Set the callback for resize/signal/close notifications (server side).
    void setCallback(const std::shared_ptr<SshChannelCallback> &callback);

    // Client-side channel requests.
    bool requestPty(const std::string &term, std::uint32_t cols, std::uint32_t rows);
    bool requestShell();
    bool requestWindowChange(std::uint32_t cols, std::uint32_t rows);
    bool sendSignal(const std::string &signalName);
    // Server-side: report the session exit status to the peer (RFC 4254 §6.10).
    bool sendExitStatus(std::uint32_t status);
private:
    friend class SshConnectionPrivate;
    friend class SshClientPrivate;
    friend class SshClient;
    explicit SshChannel(const std::shared_ptr<SshChannelPrivate> &d);
    std::shared_ptr<SshChannelPrivate> d;
    NG_DISABLE_COPY(SshChannel)
};

// Server-side interactive application. Implement run() to drive the TUI:
// blocking channel->recv()/send() are allowed inside. Returning closes the
// channel. run() executes in a spawned coroutine per session channel.
class SshApplication
{
public:
    virtual ~SshApplication() = default;
    virtual void run(SshChannel *channel) = 0;
};

// Authentication callback for SshServer. Either method may be rejected for a
// given user; both are consulted when enabled.
class SshAuthenticator
{
public:
    virtual ~SshAuthenticator() = default;
    virtual bool checkPassword(const std::string &user, const std::string &password) = 0;
    // keyBlob is the raw SSH public key blob (e.g. "ssh-rsa" || e || n).
    virtual bool checkPublicKey(const std::string &user, const std::string &keyBlob) = 0;
};

// Callback invoked with the server host key blob (raw "ssh-rsa" blob) so a
// client can implement known_hosts-style verification. Return false to abort.
class SshHostKeyVerifier
{
public:
    virtual ~SshHostKeyVerifier() = default;
    virtual bool verify(const std::string &host, const std::string &keyBlob) = 0;
};

// An SSH server that hosts interactive applications over "session" channels.
// Unlike a full sshd it does not fork/exec shells; the SshApplication callback
// receives the terminal byte stream plus resize/signal notifications.
class SshServer : public BaseStreamServer
{
public:
    SshServer(const HostAddress &serverAddress, std::uint16_t serverPort);
    SshServer(std::uint16_t serverPort);
    virtual ~SshServer() override;
public:
    void setHostKey(const PrivateKey &key);
    void setAuthenticator(const std::shared_ptr<SshAuthenticator> &authenticator);
    void setApplication(const std::shared_ptr<SshApplication> &application);
    // Shown as SSH_MSG_USERAUTH_BANNER during authentication.
    void setBanner(const std::string &banner);
    void setMaxAuthTries(int tries);
    // Timeout for the version exchange + KEX + authentication phase, in seconds.
    void setLoginTimeout(float seconds);
protected:
    virtual std::shared_ptr<SocketLike> serverCreate() override;
    virtual void processRequest(std::shared_ptr<SocketLike> request) override;
private:
    SshServerPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(SshServer)
    NG_DISABLE_COPY(SshServer)
};

// An SSH protocol client (not a TUI; see examples/ssh-bbs for a server-side TUI).
class SshClient
{
public:
    SshClient();
    virtual ~SshClient();
public:
    bool connect(const std::string &host, std::uint16_t port,
                 std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>());
    bool connect(const HostAddress &addr, std::uint16_t port);
    void setHostKeyVerifier(const std::shared_ptr<SshHostKeyVerifier> &verifier);
    void setLoginTimeout(float seconds);
    bool authenticate(const std::string &user, const std::string &password);
    bool authenticateWithPublicKey(const std::string &user, const PrivateKey &key);
    // Open a "session" channel; returns null on failure. The returned channel
    // is valid until close() or the connection drops.
    std::shared_ptr<SshChannel> openSessionChannel();
    void close();
    bool isConnected() const;
    Socket::SocketError error() const;
    std::string errorString() const;
private:
    SshClientPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(SshClient)
    NG_DISABLE_COPY(SshClient)
};

}  // namespace qtng

#endif  // QTNG_SSH_H
