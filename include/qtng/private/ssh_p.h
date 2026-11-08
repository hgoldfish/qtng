#ifndef QTNG_SSH_P_H
#define QTNG_SSH_P_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/aead.h"
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

struct z_stream_s;
typedef struct z_stream_s z_stream;

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
    SSH_MSG_EXT_INFO = 7,  // RFC 8308
    SSH_MSG_KEXINIT = 20,
    SSH_MSG_NEWKEYS = 21,
    SSH_MSG_KEX_ECDH_INIT = 30,   // curve25519-sha256 uses ECDH message numbers
    SSH_MSG_KEX_ECDH_REPLY = 31,
    SSH_MSG_USERAUTH_REQUEST = 50,
    SSH_MSG_USERAUTH_FAILURE = 51,
    SSH_MSG_USERAUTH_SUCCESS = 52,
    SSH_MSG_USERAUTH_BANNER = 53,
    // Type 60 is method-specific (RFC 4252 / 4256): PK_OK, PASSWD_CHANGEREQ,
    // or INFO_REQUEST. Dispatch by clientAuthMethod on the client side.
    SSH_MSG_USERAUTH_PK_OK = 60,
    SSH_MSG_USERAUTH_INFO_RESPONSE = 61,
    SSH_MSG_GLOBAL_REQUEST = 80,
    SSH_MSG_REQUEST_SUCCESS = 81,
    SSH_MSG_REQUEST_FAILURE = 82,
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
std::string sshEd25519KeyBlob(const PublicKey &key);
std::string sshPublicKeyBlob(const PublicKey &key);
std::string sshKeyAlgorithmName(const std::string &blob);  // "ssh-rsa", "ssh-ed25519", ...
bool sshParseRsaKeyBlob(const std::string &blob, PublicKey *key);
bool sshParseEd25519KeyBlob(const std::string &blob, PublicKey *key);
bool sshParsePublicKeyBlob(const std::string &blob, PublicKey *key);
// RFC 4253 §5.1: "SSH-2.0-..." or compatibility "SSH-1.99-..." both mean protocol 2.
bool sshIsProtocol2Identification(const std::string &line);

enum class SshClientAuthMethod {
    None = 0,
    Password,
    PublicKey,
    KeyboardInteractive,
};

class SshConnectionPrivate;

class SshChannelPrivate
{
public:
    SshChannelPrivate(SshConnectionPrivate *conn, std::uint32_t localId, std::uint32_t peerId,
                      std::uint32_t remoteWindow, std::uint32_t localWindow, std::uint32_t maxPacketSize);

    std::int32_t recv(Queue<std::string> *queue, char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size, bool stderr);
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
    Queue<std::string> stderrIncoming;
    Event windowAdjust;
    Lock readLock;
    Lock writeLock;
    bool eofSent;
    bool eofReceived;
    bool closed;  // our side initiated close
    bool remoteClosed;
    bool exitStatusSent;
    bool hasExitStatus;
    std::uint32_t exitStatus;
    std::string exitSignal;
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
    bool setupTransportKeys(bool sendSide);
    bool sendPacket(const std::string &payload);
    bool recvPacket(std::string *payload);
    std::string readLine();
    void computeExchangeHash(const std::string &serverHostKeyBlob);
    bool verifyPublicKeySignature(const std::string &user, const std::string &algo, const std::string &blob,
                                  const std::string &signature);
    bool maybeRekey();
    bool beginRekey(bool sendInit);
    void handleGlobalRequest(const std::string &payload);
    void handleExtInfo(const std::string &payload);
    void handleExtendedData(const std::string &payload);
    void handleRequestReply(bool success);
    bool sendKeepalive();
    void keepaliveLoop();
    void activateDelayedCompression();
    bool compressPayload(const std::string &in, std::string *out);
    bool decompressPayload(const std::string &in, std::string *out);
    std::vector<std::string> hostKeyAlgorithmsForKey() const;
    std::vector<std::string> compressionAlgorithms() const;
    std::vector<std::string> kexAlgorithmsForRole() const;
    bool chachaCrypt(bool encrypt, std::uint32_t seq, const std::string &key, std::string *packetWithTag);
    bool chachaDecryptLength(std::uint32_t seq, const std::string &key, const std::string &encLen4,
                             std::uint32_t *packetLen);

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
    bool sendChannelExtendedData(SshChannelPrivate *ch, std::uint32_t dataType, const std::string &data);
    bool sendChannelEof(SshChannelPrivate *ch);
    bool sendChannelClose(SshChannelPrivate *ch);
    bool sendWindowAdjust(SshChannelPrivate *ch, std::uint32_t n);
    bool sendChannelRequest(SshChannelPrivate *ch, const std::string &type, const SshBuffer &extra, bool wantReply);
    bool sendOpenChannel();
    bool sendChannelSuccess(std::uint32_t peerChannelId);
    bool sendChannelFailure(std::uint32_t peerChannelId);
    bool sendConnectionService();

    // Auth (server).
    void handleAuthRequest(const std::string &payload);
    void handleServiceRequest(const std::string &payload);
    bool sendAuthFailure();
    bool sendAuthSuccess();
    bool sendPkOk(const std::string &algo, const std::string &blob);

    // Client helpers.
    bool startClientAuth(const std::string &user, const std::string &password);
    bool startClientPublicKeyAuth(const std::string &user, const PrivateKey &key);
    bool startClientKeyboardInteractiveAuth();
    bool handleUserauthInfoRequest(const std::string &payload);
    bool sendUserauthService();
    bool waitAuthReply();
    bool waitChannelOpen();
    bool waitRequestReply();
    bool authMethodAvailable(const std::string &method) const;
    void finishClientAuthReply(bool ok);
    void setAuthErrorIfEmpty(const std::string &fallback);
    std::string pickPublicKeyAlgo(const PrivateKey &key) const;

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
    bool initialKexDone;
    bool rekeyInProgress;
    std::string myKexInitPayload;
    std::string peerKexInitPayload;
    bool peerExtInfoC;
    bool peerExtInfoS;
    bool peerStrictKex;
    bool localStrictKex;
    bool strictKexActive;
    std::string kexAlgo;
    std::string hostKeyAlgo;
    std::string cipherAlgo;
    std::string macAlgo;
    std::string compressionAlgo;
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
    std::unique_ptr<Aead> sendAead;
    std::unique_ptr<Aead> recvAead;
    std::string sendMacKey;
    std::string recvMacKey;
    std::string sendChaChaKey;
    std::string recvChaChaKey;
    std::string sendGcmIv;
    std::string recvGcmIv;
    std::uint32_t sendSeq;
    std::uint32_t recvSeq;
    int blockSize;
    int macLength;
    bool sendEncrypted;
    bool recvEncrypted;
    bool macEtm;
    bool isAeadCipher;
    bool isChaChaCipher;
    bool delayedCompressionActive;
    z_stream *sendZ;
    z_stream *recvZ;

    // Rekey / keepalive counters.
    std::uint64_t bytesSent;
    std::uint64_t bytesReceived;
    std::uint64_t rekeyLimitBytes;
    float rekeyIntervalSeconds;
    std::int64_t lastRekeyTimeMs;
    float serverAliveInterval;
    int serverAliveCountMax;
    int serverAliveOutstanding;
    bool compressionEnabled;
    bool connectionServiceAccepted;
    std::vector<std::string> serverSigAlgs;

    // Serialization.
    Lock sendLock;

    // Server auth state.
    std::shared_ptr<SshAuthenticator> auth;
    int maxAuthTries;
    int authTries;
    bool authenticated;
    std::string authUser;
    std::string banner;
    bool bannerSent;

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
    std::string peerHostName;
    std::string clientUser;
    std::string clientPassword;
    SshClientAuthMethod clientAuthMethod = SshClientAuthMethod::None;
    std::vector<std::string> peerAuthMethods;
    std::string clientAuthError;
    std::string clientKeyAlgo;
    std::string clientKeyBlob;

    // Background coroutines (client).
    std::shared_ptr<Coroutine> readLoopCoroutine;
    std::shared_ptr<Coroutine> keepaliveCoroutine;
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
    float serverAliveInterval = 0.0f;
    int serverAliveCountMax = 3;
    std::uint64_t rekeyLimitBytes = 0;
    float rekeyIntervalSeconds = 0.0f;
    bool compressionEnabled = false;
};

}  // namespace qtng

#endif  // QTNG_SSH_P_H
