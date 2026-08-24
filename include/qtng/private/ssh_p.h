#ifndef QTNG_SSH_P_H
#define QTNG_SSH_P_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/cipher.h"
#include "qtng/eventloop.h"
#include "qtng/locks.h"
#include "qtng/md.h"
#include "qtng/noise.h"
#include "qtng/pkey.h"
#include "qtng/random.h"
#include "qtng/socket.h"
#include "qtng/socket_utils.h"
#include "qtng/ssh.h"
#include "qtng/utils/platform.h"

namespace qtng {

// RFC 4251 data type codec.
class SshBuffer
{
public:
    SshBuffer() = default;
    explicit SshBuffer(const std::string &data)
        : data(data)
        , offset(0)
    {
    }
    explicit SshBuffer(std::string &&data)
        : data(std::move(data))
        , offset(0)
    {
    }

    // Encoders.
    void putByte(std::uint8_t b) { data.push_back(static_cast<char>(b)); }
    void putUint32(std::uint32_t v)
    {
        const std::size_t pos = data.size();
        data.append(4, '\0');
        ngToBigEndian(v, &data[pos]);
    }
    void putString(const std::string &s);
    void putMpint(const std::string &bytes);  // big-endian magnitude, sign bit handled
    void putBoolean(bool b) { data.push_back(b ? 1 : 0); }
    void putNameList(const std::vector<std::string> &names);
    void putBytes(const std::string &s) { data.append(s); }

    // Decoders; false on short input (offset left unchanged in that case).
    bool getByte(std::uint8_t *b);
    bool getUint32(std::uint32_t *v);
    bool getString(std::string *s);
    bool getMpint(std::string *bytes);
    bool getBoolean(bool *b);
    bool getNameList(std::vector<std::string> *names);
    bool getBytes(std::size_t len, std::string *out);
    std::string takeRest();

    bool isAtEnd() const { return offset >= data.size(); }
    std::size_t size() const { return data.size(); }
    std::size_t offsetPos() const { return offset; }
    const std::string &raw() const { return data; }

private:
    std::string data;
    std::size_t offset = 0;
};

// SSH message type numbers (RFC 4253/4252/4254).
enum SshMessageType {
    SSH_MSG_DISCONNECT = 1,
    SSH_MSG_IGNORE = 2,
    SSH_MSG_UNIMPLEMENTED = 3,
    SSH_MSG_DEBUG = 4,
    SSH_MSG_SERVICE_REQUEST = 5,
    SSH_MSG_SERVICE_ACCEPT = 6,
    SSH_MSG_KEXINIT = 20,
    SSH_MSG_NEWKEYS = 21,
    SSH_MSG_KEX_ECDH_INIT = 30,   // curve25519-sha256 uses ECDH message numbers
    SSH_MSG_KEX_ECDH_REPLY = 31,
    SSH_MSG_USERAUTH_REQUEST = 50,
    SSH_MSG_USERAUTH_FAILURE = 51,
    SSH_MSG_USERAUTH_SUCCESS = 52,
    SSH_MSG_USERAUTH_BANNER = 53,
    SSH_MSG_USERAUTH_PK_OK = 60,
    SSH_MSG_GLOBAL_REQUEST = 80,
    SSH_MSG_CHANNEL_OPEN = 90,
    SSH_MSG_CHANNEL_OPEN_CONFIRMATION = 91,
    SSH_MSG_CHANNEL_OPEN_FAILURE = 92,
    SSH_MSG_CHANNEL_WINDOW_ADJUST = 93,
    SSH_MSG_CHANNEL_DATA = 94,
    SSH_MSG_CHANNEL_EXTENDED_DATA = 95,
    SSH_MSG_CHANNEL_EOF = 96,
    SSH_MSG_CHANNEL_CLOSE = 97,
    SSH_MSG_CHANNEL_REQUEST = 98,
    SSH_MSG_CHANNEL_SUCCESS = 99,
    SSH_MSG_CHANNEL_FAILURE = 100,
};

// OpenSSH public key blob helpers.
std::string sshRsaKeyBlob(const PublicKey &key);
std::string sshKeyAlgorithmName(const std::string &blob);  // "ssh-rsa", ...
bool sshParseRsaKeyBlob(const std::string &blob, PublicKey *key);

class SshConnectionPrivate;

class SshChannelPrivate
{
public:
    SshChannelPrivate(SshConnectionPrivate *conn, std::uint32_t localId, std::uint32_t peerId,
                      std::uint32_t remoteWindow, std::uint32_t localWindow, std::uint32_t maxPacketSize);

    std::string recv(std::int32_t maxSize);
    bool send(const std::string &data);
    void closeChannel();
    void notifyEof();
    void notifyRemoteClose();

    SshConnectionPrivate * const conn;
    std::uint32_t localId;
    std::uint32_t peerId;
    std::uint32_t remoteWindow;  // peer-granted; how much we may send
    std::uint32_t localWindow;   // what we grant the peer
    std::uint32_t initialWindow;
    std::uint32_t maxPacketSize;
    Queue<std::string> incoming;
    Event windowAdjust;
    bool eofSent;
    bool eofReceived;
    bool closed;  // our side initiated close
    bool remoteClosed;
    SshTerminalSize termSize;
    std::shared_ptr<SshChannelCallback> callback;
};

class SshConnectionPrivate : public std::enable_shared_from_this<SshConnectionPrivate>
{
public:
    SshConnectionPrivate();
    ~SshConnectionPrivate();

    // Server: run the whole session in the current coroutine.
    void runServer(const std::shared_ptr<SocketLike> &socket, const PrivateKey &hostKey,
                   const std::shared_ptr<SshAuthenticator> &auth, const std::shared_ptr<SshApplication> &app,
                   const std::string &banner, int maxAuthTries, float loginTimeout);
    // Client: connect + start background read-loop coroutine.
    bool startClient(const std::shared_ptr<SocketLike> &socket, float loginTimeout);
    void stopClient();

    void readLoop(bool untilAuthenticated);

    // Transport.
    bool readVersion();
    bool sendKexInit();
    bool handleKexInitMessage(const std::string &payload);
    bool handleKexEcdhInit(const std::string &payload);   // server
    bool handleKexEcdhReply(const std::string &payload);  // client
    bool handleNewKeysMessage();
    void maybeStartKex();
    bool sendKexEcdhInit();
    bool negotiateAlgorithms();
    bool deriveKeys();
    std::string deriveKey(char label, std::size_t len);
    bool sendNewKeys();
    bool sendPacket(const std::string &payload);
    bool recvPacket(std::string *payload);
    std::string readLine();
    void computeExchangeHash(const std::string &serverHostKeyBlob);
    bool verifyPublicKeySignature(const std::string &user, const std::string &algo, const std::string &blob,
                                  const std::string &signature);

    // Connection layer.
    std::shared_ptr<SshChannelPrivate> createChannel(std::uint32_t peerId, std::uint32_t remoteWindow,
                                                     std::uint32_t maxPacketSize, bool *ok);
    void handleChannelOpen(const std::string &payload);
    void handleChannelOpenConfirmation(const std::string &payload);
    void handleChannelOpenFailure(const std::string &payload);
    void handleChannelData(const std::string &payload);
    void handleChannelWindowAdjust(const std::string &payload);
    void handleChannelEof(const std::string &payload);
    void handleChannelClose(const std::string &payload);
    void handleChannelRequest(const std::string &payload);
    void handleChannelRequestReply(const std::string &payload, bool success);
    bool sendChannelData(SshChannelPrivate *ch, const std::string &data);
    bool sendChannelEof(SshChannelPrivate *ch);
    bool sendChannelClose(SshChannelPrivate *ch);
    bool sendWindowAdjust(SshChannelPrivate *ch, std::uint32_t n);
    bool sendChannelRequest(SshChannelPrivate *ch, const std::string &type, const SshBuffer &extra, bool wantReply);
    bool sendOpenChannel();
    bool sendChannelSuccess(std::uint32_t peerChannelId);
    bool sendChannelFailure(std::uint32_t peerChannelId);

    // Auth (server).
    void handleAuthRequest(const std::string &payload);
    void handleServiceRequest(const std::string &payload);
    bool sendAuthFailure();
    bool sendAuthSuccess();
    bool sendPkOk(const std::string &algo, const std::string &blob);

    // Client helpers.
    bool startClientAuth(const std::string &user, const std::string &password);
    bool startClientPublicKeyAuth(const std::string &user, const PrivateKey &key);
    bool sendServiceAcceptRequest();
    bool sendUserauthService();
    bool waitAuthReply();
    bool waitChannelOpen();
    bool waitRequestReply();

    void setError(const std::string &message, Socket::SocketError err = Socket::RemoteHostClosedError);
    bool sendUnimplemented(std::uint32_t seq);
    bool sendDisconnect(std::uint32_t reason, const std::string &description);
    void notifyChannelsClosed();

    // Config / role.
    std::shared_ptr<SocketLike> socket;
    bool serverSide;
    bool running;
    Socket::SocketError error;
    std::string errorString;

    // Version strings.
    std::string localVersion;
    std::string peerVersion;

    // KEX state.
    bool kexStarted;
    bool kexDone;
    std::string myKexInitPayload;
    std::string peerKexInitPayload;
    std::string kexAlgo;
    std::string hostKeyAlgo;
    std::string cipherAlgo;
    std::string macAlgo;
    std::string sessionId;
    std::string exchangeHash;
    std::string clientEphPriv;
    std::string clientEphPub;
    std::string serverEphPriv;
    std::string serverEphPub;
    std::string sharedSecret;  // raw 32-byte X25519 shared secret
    PrivateKey hostKey;

    // Derived transport keys / ciphers.
    std::string ivC2S;
    std::string ivS2C;
    std::string keyC2S;
    std::string keyS2C;
    std::string macC2S;
    std::string macS2C;
    std::unique_ptr<Cipher> sendCipher;
    std::unique_ptr<Cipher> recvCipher;
    std::string sendMacKey;
    std::string recvMacKey;
    std::uint32_t sendSeq;
    std::uint32_t recvSeq;
    int blockSize;
    int macLength;
    bool sendEncrypted;
    bool recvEncrypted;

    // Serialization.
    Lock sendLock;

    // Server auth state.
    std::shared_ptr<SshAuthenticator> auth;
    int maxAuthTries;
    int authTries;
    bool authenticated;
    std::string authUser;
    std::string banner;

    // Server application.
    std::shared_ptr<SshApplication> app;
    std::vector<std::shared_ptr<Coroutine>> appCoroutines;

    // Channels: local id -> private state.
    std::map<std::uint32_t, std::shared_ptr<SshChannelPrivate>> channels;
    std::uint32_t nextLocalChannelId;

    // Client sync.
    Event authEvent;
    bool authOk;
    bool authFinished;
    Event channelOpenEvent;
    bool channelOpenOk;
    bool channelOpenFinished;
    std::shared_ptr<SshChannelPrivate> pendingChannel;
    Event requestReplyEvent;
    bool requestReplyOk;
    bool requestReplyFinished;
    Event serviceAcceptEvent;
    bool serviceAcceptOk;
    Event kexDoneEvent;
    std::shared_ptr<SshHostKeyVerifier> hostKeyVerifier;
    float loginTimeout;
    std::string clientUser;
    std::string clientKeyAlgo;
    std::string clientKeyBlob;

    // Background read-loop coroutine (client only).
    std::shared_ptr<Coroutine> readLoopCoroutine;
    Event connectionClosed;
};

class SshServerPrivate
{
public:
    PrivateKey hostKey;
    std::shared_ptr<SshAuthenticator> auth;
    std::shared_ptr<SshApplication> app;
    std::string banner;
    int maxAuthTries = 6;
    float loginTimeout = 30.0f;
};

class SshClientPrivate
{
public:
    std::shared_ptr<SshConnectionPrivate> conn;
    std::shared_ptr<SshHostKeyVerifier> hostKeyVerifier;
    float loginTimeout = 30.0f;
};

}  // namespace qtng

#endif  // QTNG_SSH_P_H
