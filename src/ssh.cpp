#include "qtng/private/ssh_p.h"
#include "qtng/private/crypto_p.h"

#include <algorithm>
#include <cstring>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/poly1305.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <zlib.h>

#include "qtng/utils/datetime.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

namespace {

const uint32_t kLocalWindowSize = 2 * 1024 * 1024;
const uint32_t kMaxPacketSize = 32 * 1024;
const uint32_t kMaxPacketLength = 1024 * 1024;

const vector<string> kKexAlgorithmsBase = {"curve25519-sha256", "curve25519-sha256@libssh.org"};
const vector<string> kHostKeyAlgorithms = {"ssh-ed25519", "rsa-sha2-512", "rsa-sha2-256", "ssh-rsa"};
const vector<string> kEncryptionAlgorithms = {"chacha20-poly1305@openssh.com", "aes128-gcm@openssh.com",
                                              "aes256-gcm@openssh.com", "aes128-ctr", "aes256-ctr"};
const vector<string> kMacAlgorithms = {"hmac-sha2-256-etm@openssh.com", "hmac-sha2-256", "hmac-sha1"};
const char kExtInfoC[] = "ext-info-c";
const char kStrictKexC[] = "kex-strict-c-v00@openssh.com";
const char kStrictKexS[] = "kex-strict-s-v00@openssh.com";

void poly1305Auth(unsigned char out[16], const unsigned char *m, size_t len, const unsigned char key[32])
{
    poly1305_state state;
    CRYPTO_poly1305_init(&state, key);
    CRYPTO_poly1305_update(&state, m, len);
    CRYPTO_poly1305_finish(&state, out);
}

bool isAeadCipherName(const string &name)
{
    return name == "chacha20-poly1305@openssh.com" || name == "aes128-gcm@openssh.com"
            || name == "aes256-gcm@openssh.com";
}

bool isChaChaCipherName(const string &name)
{
    return name == "chacha20-poly1305@openssh.com";
}

bool isEtmMacName(const string &name)
{
    return name.size() > 4 && name.compare(name.size() - 4, 4, "@openssh.com") == 0
            && name.find("-etm@") != string::npos;
}

MessageDigest::Algorithm macHashForAlgo(const string &algo)
{
    if (algo == "hmac-sha1") {
        return MessageDigest::Sha1;
    }
    if (algo.find("hmac-sha2-512") == 0) {
        return MessageDigest::Sha512;
    }
    return MessageDigest::Sha256;
}

int macLengthForAlgo(const string &algo)
{
    if (algo == "hmac-sha1") {
        return 20;
    }
    if (algo.find("hmac-sha2-512") == 0) {
        return 64;
    }
    if (algo.find("hmac-sha2-256") == 0) {
        return 32;
    }
    return 0;
}

bool chacha20Xor(const string &key32, const unsigned char iv[16], const unsigned char *in, unsigned char *out,
                 size_t len)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    bool ok = EVP_EncryptInit_ex(ctx, EVP_chacha20(), nullptr,
                                 reinterpret_cast<const unsigned char *>(key32.data()), iv)
            == 1;
    int outl = 0;
    if (ok) {
        ok = EVP_EncryptUpdate(ctx, out, &outl, in, static_cast<int>(len)) == 1
                && static_cast<size_t>(outl) == len;
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

string sshString(const string &s)
{
    string out;
    const size_t pos = out.size();
    out.append(4, '\0');
    ngToBigEndian(static_cast<uint32_t>(s.size()), &out[pos]);
    out.append(s);
    return out;
}

// curve25519-sha256 (RFC 8731): the X25519 output octets are used directly as
// the big-endian mpint bytes for the shared secret K (OpenSSH's
// sshbuf_put_bignum2_bytes keeps the byte order as-is).
string sshMpintString(const string &bytes)
{
    SshBuffer buf;
    buf.putMpint(bytes);
    return buf.raw();  // string-prefixed mpint
}

uint32_t readBe32(const string &s)
{
    return s.size() >= sizeof(uint32_t) ? ngFromBigEndian<uint32_t>(s.data()) : 0;
}

string pickAlgorithm(const vector<string> &mine, const vector<string> &peer)
{
    for (const string &a : mine) {
        if (find(peer.begin(), peer.end(), a) != peer.end()) {
            return a;
        }
    }
    return string();
}

MessageDigest::Algorithm rsaHashForAlgo(const string &algo)
{
    if (algo == "rsa-sha2-512") {
        return MessageDigest::Sha512;
    }
    if (algo == "rsa-sha2-256") {
        return MessageDigest::Sha256;
    }
    return MessageDigest::Sha1;  // ssh-rsa
}

}  // namespace

void SshBuffer::putString(const string &s)
{
    const size_t pos = data.size();
    data.append(4, '\0');
    ngToBigEndian(static_cast<uint32_t>(s.size()), &data[pos]);
    data.append(s);
}

void SshBuffer::putMpint(const string &bytes)
{
    if (bytes.empty()) {
        putString(string());  // RFC 4251: value zero is a zero-length string
        return;
    }
    size_t i = 0;
    while (i + 1 < bytes.size() && bytes[i] == '\0') {
        ++i;
    }
    string mp;
    if (static_cast<unsigned char>(bytes[i]) & 0x80) {
        mp.push_back('\0');
    }
    mp.append(bytes, i, string::npos);
    putString(mp);
}

void SshBuffer::putNameList(const vector<string> &names)
{
    string list;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            list.push_back(',');
        }
        list.append(names[i]);
    }
    putString(list);
}

bool SshBuffer::getByte(uint8_t *b)
{
    if (offset + 1 > data.size()) {
        return false;
    }
    *b = static_cast<uint8_t>(data[offset]);
    ++offset;
    return true;
}

bool SshBuffer::getUint32(uint32_t *v)
{
    return ngFromBigEndian(data.data(), data.size(), &offset, v);
}

bool SshBuffer::getString(string *s)
{
    uint32_t len;
    const size_t saved = offset;
    if (!ngFromBigEndian(data.data(), data.size(), &offset, &len)) {
        return false;
    }
    if (offset + len > data.size()) {
        offset = saved;
        return false;
    }
    s->assign(data.data() + offset, len);
    offset += len;
    return true;
}

bool SshBuffer::getMpint(string *bytes)
{
    string mp;
    if (!getString(&mp)) {
        return false;
    }
    if (mp.empty()) {
        *bytes = string();  // value zero
        return true;
    }
    size_t i = 0;
    while (i + 1 < mp.size() && mp[i] == '\0' && (static_cast<unsigned char>(mp[i + 1]) & 0x80) == 0) {
        ++i;
    }
    *bytes = mp.substr(i);
    return true;
}

bool SshBuffer::getBoolean(bool *b)
{
    uint8_t v;
    if (!getByte(&v)) {
        return false;
    }
    *b = v != 0;
    return true;
}

bool SshBuffer::getNameList(vector<string> *names)
{
    string list;
    if (!getString(&list)) {
        return false;
    }
    names->clear();
    if (list.empty()) {
        return true;
    }
    size_t start = 0;
    for (size_t i = 0; i <= list.size(); ++i) {
        if (i == list.size() || list[i] == ',') {
            names->push_back(list.substr(start, i - start));
            start = i + 1;
        }
    }
    return true;
}

bool SshBuffer::getBytes(size_t len, string *out)
{
    if (offset + len > data.size()) {
        return false;
    }
    out->assign(data.data() + offset, len);
    offset += len;
    return true;
}

string SshBuffer::takeRest()
{
    string rest = data.substr(offset);
    offset = data.size();
    return rest;
}

string sshRsaKeyBlob(const PublicKey &key)
{
    EVP_PKEY *pkey = reinterpret_cast<EVP_PKEY *>(key.handle());
    if (!pkey || EVP_PKEY_base_id(pkey) != EVP_PKEY_RSA) {
        return string();
    }
    RSA *rsa = EVP_PKEY_get1_RSA(pkey);
    if (!rsa) {
        return string();
    }
    const BIGNUM *n = nullptr;
    const BIGNUM *e = nullptr;
    RSA_get0_key(rsa, &n, &e, nullptr);
    SshBuffer blob;
    blob.putString("ssh-rsa");
    string eStr(static_cast<size_t>(BN_num_bytes(e)), '\0');
    string nStr(static_cast<size_t>(BN_num_bytes(n)), '\0');
    BN_bn2bin(e, reinterpret_cast<unsigned char *>(&eStr[0]));
    BN_bn2bin(n, reinterpret_cast<unsigned char *>(&nStr[0]));
    blob.putMpint(eStr);
    blob.putMpint(nStr);
    RSA_free(rsa);
    return blob.raw();
}

string sshKeyAlgorithmName(const string &blob)
{
    SshBuffer buf(blob);
    string algo;
    if (!buf.getString(&algo)) {
        return string();
    }
    return algo;
}

bool sshParseRsaKeyBlob(const string &blob, PublicKey *key)
{
    SshBuffer buf(blob);
    string algo, e, n;
    if (!buf.getString(&algo) || algo != "ssh-rsa") {
        return false;
    }
    if (!buf.getMpint(&e) || !buf.getMpint(&n) || e.empty() || n.empty()) {
        return false;
    }
    RSA *rsa = RSA_new();
    if (!rsa) {
        return false;
    }
    BIGNUM *bnE = BN_bin2bn(reinterpret_cast<const unsigned char *>(e.data()), static_cast<int>(e.size()), nullptr);
    BIGNUM *bnN = BN_bin2bn(reinterpret_cast<const unsigned char *>(n.data()), static_cast<int>(n.size()), nullptr);
    if (!bnE || !bnN) {
        BN_free(bnE);
        BN_free(bnN);
        RSA_free(rsa);
        return false;
    }
    if (RSA_set0_key(rsa, bnN, bnE, nullptr) != 1) {
        BN_free(bnE);
        BN_free(bnN);
        RSA_free(rsa);
        return false;
    }
    EVP_PKEY *pkey = EVP_PKEY_new();
    if (!pkey || EVP_PKEY_assign_RSA(pkey, rsa) != 1) {
        EVP_PKEY_free(pkey);
        RSA_free(rsa);
        return false;
    }
    int derLen = i2d_PUBKEY(pkey, nullptr);
    string der(static_cast<size_t>(derLen), '\0');
    unsigned char *p = reinterpret_cast<unsigned char *>(&der[0]);
    i2d_PUBKEY(pkey, &p);
    EVP_PKEY_free(pkey);
    *key = PublicKey::load(der, Ssl::Der);
    return !key->isNull();
}

string sshEd25519KeyBlob(const PublicKey &key)
{
    EVP_PKEY *pkey = reinterpret_cast<EVP_PKEY *>(key.handle());
    if (!pkey || EVP_PKEY_base_id(pkey) != EVP_PKEY_ED25519) {
        return string();
    }
    size_t len = 32;
    string raw(32, '\0');
    if (EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<unsigned char *>(&raw[0]), &len) != 1 || len != 32) {
        return string();
    }
    SshBuffer blob;
    blob.putString("ssh-ed25519");
    blob.putString(raw);
    return blob.raw();
}

string sshPublicKeyBlob(const PublicKey &key)
{
    if (key.algorithm() == PublicKey::Ed25519) {
        return sshEd25519KeyBlob(key);
    }
    return sshRsaKeyBlob(key);
}

bool sshParseEd25519KeyBlob(const string &blob, PublicKey *key)
{
    SshBuffer buf(blob);
    string algo, raw;
    if (!buf.getString(&algo) || algo != "ssh-ed25519" || !buf.getString(&raw) || raw.size() != 32) {
        return false;
    }
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                 reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
    if (!pkey) {
        return false;
    }
    if (!openssl_setPkey(key, pkey, false)) {
        EVP_PKEY_free(pkey);
        return false;
    }
    return key->isValid();
}

bool sshParsePublicKeyBlob(const string &blob, PublicKey *key)
{
    const string algo = sshKeyAlgorithmName(blob);
    if (algo == "ssh-ed25519") {
        return sshParseEd25519KeyBlob(blob, key);
    }
    if (algo == "ssh-rsa") {
        return sshParseRsaKeyBlob(blob, key);
    }
    return false;
}

SshChannelPrivate::SshChannelPrivate(SshConnectionPrivate *conn, uint32_t localId, uint32_t peerId,
                                     uint32_t remoteWindow, uint32_t localWindow, uint32_t maxPacketSize)
    : conn(conn)
    , localId(localId)
    , peerId(peerId)
    , remoteWindow(remoteWindow)
    , localWindow(localWindow)
    , initialWindow(localWindow)
    , maxPacketSize(maxPacketSize)
    , eofSent(false)
    , eofReceived(false)
    , closed(false)
    , remoteClosed(false)
    , exitStatusSent(false)
    , hasExitStatus(false)
    , exitStatus(0)
{
}

int32_t SshChannelPrivate::recv(Queue<string> *queue, char *data, int32_t size)
{
    if (size <= 0) {
        return 0;
    }
    while (queue->isEmpty()) {
        if (eofReceived || closed || remoteClosed) {
            return 0;
        }
        if (!queue->waitNotEmpty()) {
            return -1;
        }
    }
    int32_t got = 0;
    while (got < size && !queue->isEmpty()) {
        string chunk = queue->get();
        if (chunk.empty()) {
            continue;
        }
        const size_t need = static_cast<size_t>(size - got);
        if (chunk.size() <= need) {
            memcpy(data + got, chunk.data(), chunk.size());
            got += static_cast<int32_t>(chunk.size());
        } else {
            memcpy(data + got, chunk.data(), need);
            queue->returns(chunk.substr(need));
            got += static_cast<int32_t>(need);
            break;
        }
    }
    return got;
}

int32_t SshChannelPrivate::send(const char *data, int32_t size, bool stderr)
{
    if (closed || remoteClosed || eofSent) {
        return -1;
    }
    if (size <= 0) {
        return 0;
    }
    int32_t sent = 0;
    while (sent < size) {
        while (remoteWindow == 0) {
            windowAdjust.clear();
            if (remoteWindow > 0 || closed || remoteClosed) {
                break;
            }
            if (!windowAdjust.tryWait(30000)) {
                return sent > 0 ? sent : -1;
            }
        }
        if (closed || remoteClosed) {
            return sent > 0 ? sent : -1;
        }
        if (remoteWindow == 0) {
            continue;
        }
        const uint32_t packetCap = maxPacketSize == 0 ? remoteWindow : maxPacketSize;
        const uint32_t chunk = min({static_cast<uint32_t>(size - sent), remoteWindow, packetCap});
        const string piece(data + sent, chunk);
        const bool ok = stderr ? conn->sendChannelExtendedData(this, 1, piece) : conn->sendChannelData(this, piece);
        if (!ok) {
            return sent > 0 ? sent : -1;
        }
        remoteWindow -= chunk;
        sent += static_cast<int32_t>(chunk);
    }
    return sent;
}

void SshChannelPrivate::closeChannel()
{
    if (closed) {
        return;
    }
    if (!eofSent) {
        conn->sendChannelEof(this);
        eofSent = true;
    }
    conn->sendChannelClose(this);
    closed = true;
    incoming.notifyNotEmpty();
    stderrIncoming.notifyNotEmpty();
    windowAdjust.set();
}

void SshChannelPrivate::notifyEof()
{
    eofReceived = true;
    incoming.notifyNotEmpty();
    stderrIncoming.notifyNotEmpty();
}

void SshChannelPrivate::notifyRemoteClose()
{
    eofReceived = true;
    remoteClosed = true;
    incoming.notifyNotEmpty();
    stderrIncoming.notifyNotEmpty();
    windowAdjust.set();
    if (callback) {
        callback->onClose();
    }
}

SshConnectionPrivate::SshConnectionPrivate()
    : socket()
    , serverSide(false)
    , running(false)
    , error(Socket::NoError)
    , kexStarted(false)
    , kexDone(false)
    , initialKexDone(false)
    , rekeyInProgress(false)
    , peerExtInfoC(false)
    , peerExtInfoS(false)
    , peerStrictKex(false)
    , localStrictKex(false)
    , strictKexActive(false)
    , sendSeq(0)
    , recvSeq(0)
    , blockSize(8)
    , macLength(0)
    , sendEncrypted(false)
    , recvEncrypted(false)
    , macEtm(false)
    , isAeadCipher(false)
    , isChaChaCipher(false)
    , delayedCompressionActive(false)
    , sendZ(nullptr)
    , recvZ(nullptr)
    , bytesSent(0)
    , bytesReceived(0)
    , rekeyLimitBytes(0)
    , rekeyIntervalSeconds(0.0f)
    , lastRekeyTimeMs(0)
    , serverAliveInterval(0.0f)
    , serverAliveCountMax(3)
    , serverAliveOutstanding(0)
    , compressionEnabled(false)
    , connectionServiceAccepted(false)
    , maxAuthTries(6)
    , authTries(0)
    , authenticated(false)
    , bannerSent(false)
    , nextLocalChannelId(0)
    , authOk(false)
    , authFinished(false)
    , channelOpenOk(false)
    , channelOpenFinished(false)
    , requestReplyOk(false)
    , requestReplyFinished(false)
    , serviceAcceptOk(false)
    , loginTimeout(30.0f)
{
}

SshConnectionPrivate::~SshConnectionPrivate()
{
    if (sendZ) {
        deflateEnd(sendZ);
        delete sendZ;
        sendZ = nullptr;
    }
    if (recvZ) {
        inflateEnd(recvZ);
        delete recvZ;
        recvZ = nullptr;
    }
}

void SshConnectionPrivate::setError(const string &message, Socket::SocketError err)
{
    if (error != Socket::NoError) {
        return;
    }
    error = err;
    errorString = message;
}

void SshConnectionPrivate::runServer(const shared_ptr<SocketLike> &sock, const PrivateKey &hostKey_,
                                     const shared_ptr<SshAuthenticator> &auth_,
                                     const shared_ptr<SshApplication> &app_, const string &banner_,
                                     int maxAuthTries_, float loginTimeout_)
{
    socket = sock;
    serverSide = true;
    hostKey = hostKey_;
    auth = auth_;
    app = app_;
    banner = banner_;
    maxAuthTries = maxAuthTries_;
    loginTimeout = loginTimeout_;
    running = true;
    error = Socket::NoError;
    errorString.clear();

    try {
        Timeout timeout(loginTimeout);
        if (!readVersion()) {
            setError("version exchange failed");
            return;
        }
        if (!sendKexInit()) {
            return;
        }
        readLoop(true);  // version + kex + authentication phase
    } catch (TimeoutException &) {
        setError("login timeout");
    }
    if (error != Socket::NoError) {
        running = false;
        return;
    }
    if (authenticated) {
        readLoop(false);  // connection phase
    }
    running = false;
    notifyChannelsClosed();
    for (const shared_ptr<Coroutine> &c : appCoroutines) {
        if (c) {
            c->join();
        }
    }
    appCoroutines.clear();
    channels.clear();
}

bool SshConnectionPrivate::startClient(const shared_ptr<SocketLike> &sock, float loginTimeout_)
{
    socket = sock;
    serverSide = false;
    loginTimeout = loginTimeout_;
    running = true;
    error = Socket::NoError;
    errorString.clear();
    try {
        Timeout timeout(loginTimeout);
        if (!readVersion()) {
            setError("version exchange failed");
            return false;
        }
        if (!sendKexInit()) {
            return false;
        }
    } catch (TimeoutException &) {
        setError("login timeout");
        return false;
    }
    shared_ptr<SshConnectionPrivate> self = shared_from_this();
    readLoopCoroutine = shared_ptr<Coroutine>(Coroutine::spawn([self] { self->readLoop(false); }));
    if (!kexDoneEvent.tryWait(static_cast<uint32_t>(loginTimeout * 1000))) {
        setError("kex timeout");
        return false;
    }
    return true;
}

void SshConnectionPrivate::stopClient()
{
    running = false;
    connectionClosed.set();
    if (socket) {
        socket->abort();
    }
    if (keepaliveCoroutine) {
        keepaliveCoroutine->join();
        keepaliveCoroutine.reset();
    }
    if (readLoopCoroutine) {
        readLoopCoroutine->join();
        readLoopCoroutine.reset();
    }
    if (socket) {
        socket->close();
    }
}

string SshConnectionPrivate::readLine()
{
    string line;
    char c;
    while (socket && socket->isValid()) {
        int32_t n = socket->recv(&c, 1);
        if (n <= 0) {
            return string();
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            line.push_back(c);
        }
    }
    return line;
}

bool sshIsProtocol2Identification(const string &line)
{
    // RFC 4253 §5.1: servers that still advertise SSH-1 compatibility send
    // "SSH-1.99-..."; clients must treat that as protocol 2.
    if (line.size() >= 8 && line.compare(0, 8, "SSH-1.99") == 0) {
        return true;
    }
    if (line.size() >= 7 && line.compare(0, 7, "SSH-2.0") == 0) {
        return true;
    }
    return false;
}

bool SshConnectionPrivate::readVersion()
{
    if (serverSide) {
        string line;
        while (true) {
            line = readLine();
            if (line.empty()) {
                return false;
            }
            if (line.size() >= 4 && line.substr(0, 4) == "SSH-") {
                break;
            }
            // ignore preamble lines
        }
        peerVersion = line;
        localVersion = "SSH-2.0-qtng";
    } else {
        localVersion = "SSH-2.0-qtng";
        string out = localVersion + "\r\n";
        if (socket->sendall(out) != static_cast<int32_t>(out.size())) {
            return false;
        }
        peerVersion = readLine();
        if (peerVersion.empty()) {
            return false;
        }
    }
    if (!sshIsProtocol2Identification(peerVersion)) {
        setError("unsupported protocol version: " + peerVersion);
        return false;
    }
    if (serverSide) {
        string out = localVersion + "\r\n";
        if (socket->sendall(out) != static_cast<int32_t>(out.size())) {
            return false;
        }
    }
    return true;
}

bool SshConnectionPrivate::sendKexInit()
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_KEXINIT);
    buf.putBytes(randomBytes(16));
    buf.putNameList(kexAlgorithmsForRole());
    buf.putNameList(hostKeyAlgorithmsForKey());
    buf.putNameList(kEncryptionAlgorithms);
    buf.putNameList(kEncryptionAlgorithms);
    buf.putNameList(kMacAlgorithms);
    buf.putNameList(kMacAlgorithms);
    const vector<string> comps = compressionAlgorithms();
    buf.putNameList(comps);
    buf.putNameList(comps);
    buf.putNameList(vector<string>());
    buf.putNameList(vector<string>());
    buf.putBoolean(false);
    buf.putUint32(0);
    myKexInitPayload = buf.raw();
    return sendPacket(myKexInitPayload);
}

vector<string> SshConnectionPrivate::kexAlgorithmsForRole() const
{
    vector<string> names = kKexAlgorithmsBase;
    if (serverSide) {
        names.push_back(kStrictKexS);
    } else {
        names.push_back(kExtInfoC);
        names.push_back(kStrictKexC);
    }
    return names;
}

vector<string> SshConnectionPrivate::hostKeyAlgorithmsForKey() const
{
    if (!serverSide) {
        return kHostKeyAlgorithms;
    }
    if (hostKey.algorithm() == PublicKey::Ed25519) {
        return vector<string>({"ssh-ed25519"});
    }
    return vector<string>({"rsa-sha2-512", "rsa-sha2-256", "ssh-rsa"});
}

vector<string> SshConnectionPrivate::compressionAlgorithms() const
{
    if (compressionEnabled || serverSide) {
        return vector<string>({"zlib@openssh.com", "none"});
    }
    return vector<string>({"none"});
}

bool SshConnectionPrivate::handleKexInitMessage(const string &payload)
{
    if (kexDone && !rekeyInProgress) {
        if (!beginRekey(true)) {
            return false;
        }
    }
    peerKexInitPayload = payload;
    maybeStartKex();
    return true;
}

void SshConnectionPrivate::maybeStartKex()
{
    if (kexStarted || error != Socket::NoError) {
        return;
    }
    if (myKexInitPayload.empty() || peerKexInitPayload.empty()) {
        return;
    }
    kexStarted = true;
    if (!negotiateAlgorithms()) {
        return;
    }
    if (!serverSide) {
        NoiseKey k = NoiseKey::generate();
        clientEphPriv = k.privateKey();
        clientEphPub = k.publicKey();
        sendKexEcdhInit();
    }
}

bool SshConnectionPrivate::sendKexEcdhInit()
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_KEX_ECDH_INIT);
    buf.putString(clientEphPub);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::negotiateAlgorithms()
{
    vector<string> myKex, myHk, myEnc, myMac, myComp;
    vector<string> peerKex, peerHk, peerEnc, peerMac, peerComp;
    SshBuffer my(myKexInitPayload);
    SshBuffer peer(peerKexInitPayload);
    uint8_t type;
    string cookie;
    if (!my.getByte(&type) || type != SSH_MSG_KEXINIT || !my.getBytes(16, &cookie)) {
        setError("bad KEXINIT");
        return false;
    }
    if (!peer.getByte(&type) || type != SSH_MSG_KEXINIT || !peer.getBytes(16, &cookie)) {
        setError("bad KEXINIT");
        return false;
    }
    my.getNameList(&myKex);
    my.getNameList(&myHk);
    my.getNameList(&myEnc);
    my.getNameList(&myEnc);
    my.getNameList(&myMac);
    my.getNameList(&myMac);
    my.getNameList(&myComp);
    my.getNameList(&myComp);
    peer.getNameList(&peerKex);
    peer.getNameList(&peerHk);
    peer.getNameList(&peerEnc);
    peer.getNameList(&peerEnc);
    peer.getNameList(&peerMac);
    peer.getNameList(&peerMac);
    peer.getNameList(&peerComp);
    peer.getNameList(&peerComp);
    peerExtInfoC = find(peerKex.begin(), peerKex.end(), kExtInfoC) != peerKex.end();
    peerExtInfoS = find(peerKex.begin(), peerKex.end(), "ext-info-s") != peerKex.end();
    peerStrictKex = find(peerKex.begin(), peerKex.end(), serverSide ? kStrictKexC : kStrictKexS) != peerKex.end();
    localStrictKex = find(myKex.begin(), myKex.end(), serverSide ? kStrictKexS : kStrictKexC) != myKex.end();
    strictKexActive = peerStrictKex && localStrictKex;

    if (serverSide) {
        kexAlgo = pickAlgorithm(peerKex, myKex);
        hostKeyAlgo = pickAlgorithm(peerHk, myHk);
        cipherAlgo = pickAlgorithm(peerEnc, myEnc);
        macAlgo = pickAlgorithm(peerMac, myMac);
        compressionAlgo = pickAlgorithm(peerComp, myComp);
    } else {
        kexAlgo = pickAlgorithm(myKex, peerKex);
        hostKeyAlgo = pickAlgorithm(myHk, peerHk);
        cipherAlgo = pickAlgorithm(myEnc, peerEnc);
        macAlgo = pickAlgorithm(myMac, peerMac);
        compressionAlgo = pickAlgorithm(myComp, peerComp);
    }
    if (kexAlgo == kExtInfoC || kexAlgo == kStrictKexC || kexAlgo == kStrictKexS || kexAlgo == "ext-info-s") {
        setError("no common kex algorithms");
        return false;
    }
    if (kexAlgo.empty() || hostKeyAlgo.empty() || cipherAlgo.empty()) {
        setError("no common algorithms");
        return false;
    }
    if (compressionAlgo.empty()) {
        compressionAlgo = "none";
    }
    if (kexAlgo != "curve25519-sha256" && kexAlgo != "curve25519-sha256@libssh.org") {
        setError("unsupported kex algorithm: " + kexAlgo);
        return false;
    }
    if (hostKeyAlgo != "rsa-sha2-256" && hostKeyAlgo != "rsa-sha2-512" && hostKeyAlgo != "ssh-rsa"
        && hostKeyAlgo != "ssh-ed25519") {
        setError("unsupported host key algorithm: " + hostKeyAlgo);
        return false;
    }
    isChaChaCipher = isChaChaCipherName(cipherAlgo);
    isAeadCipher = isAeadCipherName(cipherAlgo);
    macEtm = false;
    if (isChaChaCipher) {
        blockSize = 8;
        macLength = 16;
        macAlgo = "none";
    } else if (cipherAlgo == "aes128-gcm@openssh.com" || cipherAlgo == "aes256-gcm@openssh.com") {
        blockSize = 16;
        macLength = 16;
        if (macAlgo.empty()) {
            macAlgo = "hmac-sha2-256";
        }
    } else if (cipherAlgo == "aes128-ctr" || cipherAlgo == "aes256-ctr") {
        blockSize = 16;
        if (macAlgo.empty()) {
            setError("no common MAC");
            return false;
        }
        macEtm = isEtmMacName(macAlgo);
        macLength = macLengthForAlgo(macAlgo);
        if (macLength <= 0) {
            setError("unsupported MAC: " + macAlgo);
            return false;
        }
    } else {
        setError("unsupported cipher: " + cipherAlgo);
        return false;
    }
    return true;
}

void SshConnectionPrivate::computeExchangeHash(const string &serverHostKeyBlob)
{
    const string peerVersionStr = sshString(peerVersion);
    const string localVersionStr = sshString(localVersion);
    const string peerKexStr = sshString(peerKexInitPayload);
    const string localKexStr = sshString(myKexInitPayload);
    const string ksStr = sshString(serverHostKeyBlob);
    const string qcStr = sshString(clientEphPub);
    const string qsStr = sshString(serverEphPub);
    const string kMpint = sshMpintString(sharedSecret);

    const string vc = serverSide ? peerVersionStr : localVersionStr;
    const string vs = serverSide ? localVersionStr : peerVersionStr;
    const string ic = serverSide ? peerKexStr : localKexStr;
    const string is = serverSide ? localKexStr : peerKexStr;

    MessageDigest md(MessageDigest::Sha256);
    md.addData(vc.data(), static_cast<int>(vc.size()));
    md.addData(vs.data(), static_cast<int>(vs.size()));
    md.addData(ic.data(), static_cast<int>(ic.size()));
    md.addData(is.data(), static_cast<int>(is.size()));
    md.addData(ksStr.data(), static_cast<int>(ksStr.size()));
    md.addData(qcStr.data(), static_cast<int>(qcStr.size()));
    md.addData(qsStr.data(), static_cast<int>(qsStr.size()));
    md.addData(kMpint.data(), static_cast<int>(kMpint.size()));
    exchangeHash = md.result();
    if (sessionId.empty()) {
        sessionId = exchangeHash;
    }
}

string SshConnectionPrivate::deriveKey(char label, size_t len)
{
    const string kMpint = sshMpintString(sharedSecret);
    string one(1, label);
    MessageDigest m(MessageDigest::Sha256);
    m.addData(kMpint.data(), static_cast<int>(kMpint.size()));
    m.addData(exchangeHash.data(), static_cast<int>(exchangeHash.size()));
    m.addData(one.data(), 1);
    m.addData(sessionId.data(), static_cast<int>(sessionId.size()));
    string result = m.result();
    while (result.size() < len) {
        MessageDigest m2(MessageDigest::Sha256);
        m2.addData(kMpint.data(), static_cast<int>(kMpint.size()));
        m2.addData(exchangeHash.data(), static_cast<int>(exchangeHash.size()));
        m2.addData(result.data(), static_cast<int>(result.size()));
        result += m2.result();
    }
    result.resize(len);
    return result;
}

bool SshConnectionPrivate::deriveKeys()
{
    size_t keyLen = 16;
    size_t ivLen = 16;
    size_t macLen = static_cast<size_t>(macLength);
    if (isChaChaCipher) {
        keyLen = 64;
        ivLen = 0;
        macLen = 0;
    } else if (cipherAlgo == "aes256-gcm@openssh.com") {
        keyLen = 32;
        ivLen = 12;
        macLen = 0;
    } else if (cipherAlgo == "aes128-gcm@openssh.com") {
        keyLen = 16;
        ivLen = 12;
        macLen = 0;
    } else if (cipherAlgo == "aes256-ctr") {
        keyLen = 32;
        ivLen = 16;
    } else {
        keyLen = 16;
        ivLen = 16;
    }
    keyC2S = deriveKey('C', keyLen);
    keyS2C = deriveKey('D', keyLen);
    ivC2S = ivLen ? deriveKey('A', ivLen) : string();
    ivS2C = ivLen ? deriveKey('B', ivLen) : string();
    macC2S = macLen ? deriveKey('E', macLen) : string();
    macS2C = macLen ? deriveKey('F', macLen) : string();
    return true;
}

bool SshConnectionPrivate::handleKexEcdhInit(const string &payload)
{
    if (!serverSide) {
        return true;
    }
    SshBuffer buf(payload);
    uint8_t type;
    string qc;
    if (!buf.getByte(&type) || !buf.getString(&qc) || qc.size() != 32) {
        setError("bad KEX_ECDH_INIT");
        return false;
    }
    clientEphPub = qc;
    NoiseKey sk = NoiseKey::generate();
    serverEphPriv = sk.privateKey();
    serverEphPub = sk.publicKey();
    sharedSecret = NoiseKey::dh(serverEphPriv, clientEphPub);
    if (sharedSecret.empty()) {
        setError("x25519 key agreement failed");
        return false;
    }
    const string ks = sshPublicKeyBlob(hostKey.publicKey());
    if (ks.empty()) {
        setError("unsupported host key type");
        return false;
    }
    computeExchangeHash(ks);
    string signature;
    if (hostKeyAlgo == "ssh-ed25519") {
        signature = hostKey.sign(exchangeHash, MessageDigest::Sha256);
    } else {
        signature = hostKey.sign(exchangeHash, rsaHashForAlgo(hostKeyAlgo));
    }
    if (signature.empty()) {
        setError("host key signing failed");
        return false;
    }
    SshBuffer sigMsg;
    sigMsg.putString(hostKeyAlgo);
    sigMsg.putString(signature);
    SshBuffer reply;
    reply.putByte(SSH_MSG_KEX_ECDH_REPLY);
    reply.putString(ks);
    reply.putString(serverEphPub);
    reply.putString(sigMsg.raw());
    if (!sendPacket(reply.raw())) {
        return false;
    }
    if (!deriveKeys()) {
        return false;
    }
    if (!sendNewKeys()) {
        return false;
    }
    kexDone = true;
    rekeyInProgress = false;
    kexDoneEvent.set();
    return true;
}

bool SshConnectionPrivate::handleKexEcdhReply(const string &payload)
{
    if (serverSide) {
        return true;
    }
    SshBuffer buf(payload);
    uint8_t type;
    string ks, qs, sigMsg;
    if (!buf.getByte(&type) || !buf.getString(&ks) || !buf.getString(&qs) || !buf.getString(&sigMsg)
        || qs.size() != 32) {
        setError("bad KEX_ECDH_REPLY");
        return false;
    }
    serverEphPub = qs;
    sharedSecret = NoiseKey::dh(clientEphPriv, serverEphPub);
    if (sharedSecret.empty()) {
        setError("x25519 key agreement failed");
        return false;
    }
    PublicKey serverKey;
    if (!sshParsePublicKeyBlob(ks, &serverKey)) {
        setError("bad host key");
        return false;
    }
    if (hostKeyVerifier) {
        string host = peerHostName;
        if (host.empty() && socket) {
            host = socket->peerName();
        }
        if (!hostKeyVerifier->verify(host, ks)) {
            setError("host key rejected by verifier", Socket::OperationError);
            return false;
        }
    }
    computeExchangeHash(ks);
    SshBuffer sigBuf(sigMsg);
    string sigAlgo, sig;
    if (!sigBuf.getString(&sigAlgo) || !sigBuf.getString(&sig)) {
        setError("bad signature message");
        return false;
    }
    bool ok = false;
    if (sigAlgo == "ssh-ed25519") {
        ok = serverKey.verify(exchangeHash, sig, MessageDigest::Sha256);
    } else {
        ok = serverKey.verify(exchangeHash, sig, rsaHashForAlgo(sigAlgo));
    }
    if (!ok) {
        setError("host key signature verification failed");
        return false;
    }
    if (!deriveKeys()) {
        return false;
    }
    if (!sendNewKeys()) {
        return false;
    }
    kexDone = true;
    rekeyInProgress = false;
    kexDoneEvent.set();
    return true;
}

bool SshConnectionPrivate::setupTransportKeys(bool sendSide)
{
    const string &k = sendSide ? (serverSide ? keyS2C : keyC2S) : (serverSide ? keyC2S : keyS2C);
    const string &iv = sendSide ? (serverSide ? ivS2C : ivC2S) : (serverSide ? ivC2S : ivS2C);
    const string &macKey = sendSide ? (serverSide ? macS2C : macC2S) : (serverSide ? macC2S : macS2C);
    if (isChaChaCipher) {
        if (sendSide) {
            sendChaChaKey = k;
            sendCipher.reset();
            sendAead.reset();
            sendMacKey.clear();
            sendEncrypted = true;
        } else {
            recvChaChaKey = k;
            recvCipher.reset();
            recvAead.reset();
            recvMacKey.clear();
            recvEncrypted = true;
        }
        return true;
    }
    if (cipherAlgo == "aes128-gcm@openssh.com" || cipherAlgo == "aes256-gcm@openssh.com") {
        Aead::Algorithm algo = (cipherAlgo == "aes256-gcm@openssh.com") ? Aead::Aes256Gcm : Aead::Aes128Gcm;
        unique_ptr<Aead> aead(new Aead(algo));
        if (!aead->setKey(k)) {
            setError("aead key init failed");
            return false;
        }
        string gcmIv = iv;
        if (gcmIv.size() < 12) {
            gcmIv.resize(12, '\0');
        } else {
            gcmIv.resize(12);
        }
        if (sendSide) {
            sendAead = std::move(aead);
            sendCipher.reset();
            sendMacKey.clear();
            sendGcmIv = gcmIv;
            sendEncrypted = true;
        } else {
            recvAead = std::move(aead);
            recvCipher.reset();
            recvMacKey.clear();
            recvGcmIv = gcmIv;
            recvEncrypted = true;
        }
        return true;
    }
    Cipher::Algorithm algo = (cipherAlgo == "aes256-ctr") ? Cipher::AES256 : Cipher::AES128;
    unique_ptr<Cipher> cipher(new Cipher(algo, Cipher::CTR, sendSide ? Cipher::Encrypt : Cipher::Decrypt));
    cipher->setKey(k);
    if (!cipher->setInitialVector(iv)) {
        setError(sendSide ? "send cipher init failed" : "receive cipher init failed");
        return false;
    }
    if (sendSide) {
        sendCipher = std::move(cipher);
        sendAead.reset();
        sendMacKey = macKey;
        sendEncrypted = true;
    } else {
        recvCipher = std::move(cipher);
        recvAead.reset();
        recvMacKey = macKey;
        recvEncrypted = true;
    }
    return true;
}

bool SshConnectionPrivate::handleNewKeysMessage()
{
    if (!setupTransportKeys(false)) {
        return false;
    }
    if (strictKexActive && !initialKexDone) {
        recvSeq = 0;
    }
    if (!initialKexDone) {
        initialKexDone = true;
    }
    lastRekeyTimeMs = utils::DateTime::currentMSecsSinceEpoch();
    bytesSent = 0;
    bytesReceived = 0;
    rekeyInProgress = false;
    return true;
}

bool SshConnectionPrivate::sendNewKeys()
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_NEWKEYS);
    if (!sendPacket(buf.raw())) {
        return false;
    }
    if (!setupTransportKeys(true)) {
        return false;
    }
    if (strictKexActive && !initialKexDone) {
        sendSeq = 0;
    }
    if (serverSide && peerExtInfoC && !initialKexDone) {
        SshBuffer ext;
        ext.putByte(SSH_MSG_EXT_INFO);
        ext.putUint32(1);
        ext.putString("server-sig-algs");
        ext.putString("ssh-ed25519,rsa-sha2-512,rsa-sha2-256");
        if (!sendPacket(ext.raw())) {
            return false;
        }
    }
    return true;
}

bool SshConnectionPrivate::chachaDecryptLength(uint32_t seq, const string &key, const string &encLen4,
                                               uint32_t *packetLen)
{
    if (key.size() < 64 || encLen4.size() != 4) {
        return false;
    }
    unsigned char seqbuf[16];
    memset(seqbuf, 0, sizeof(seqbuf));
    ngToBigEndian(static_cast<uint64_t>(seq), seqbuf + 8);
    string plain(4, '\0');
    if (!chacha20Xor(key.substr(32, 32), seqbuf, reinterpret_cast<const unsigned char *>(encLen4.data()),
                     reinterpret_cast<unsigned char *>(&plain[0]), 4)) {
        return false;
    }
    *packetLen = readBe32(plain);
    return true;
}

bool SshConnectionPrivate::chachaCrypt(bool encrypt, uint32_t seq, const string &key, string *packetWithTag)
{
    if (key.size() < 64 || !packetWithTag || packetWithTag->size() < 4) {
        return false;
    }
    const size_t aadlen = 4;
    const size_t taglen = 16;
    unsigned char seqbuf[16];
    memset(seqbuf, 0, sizeof(seqbuf));
    ngToBigEndian(static_cast<uint64_t>(seq), seqbuf + 8);

    unsigned char polyKey[32];
    memset(polyKey, 0, sizeof(polyKey));
    if (!chacha20Xor(key.substr(0, 32), seqbuf, polyKey, polyKey, sizeof(polyKey))) {
        return false;
    }

    if (!encrypt) {
        if (packetWithTag->size() < aadlen + taglen) {
            return false;
        }
        const size_t ctLen = packetWithTag->size() - taglen;
        unsigned char expected[16];
        poly1305Auth(expected, reinterpret_cast<const unsigned char *>(packetWithTag->data()), ctLen, polyKey);
        if (memcmp(expected, packetWithTag->data() + ctLen, 16) != 0) {
            return false;
        }
    }

    string out = *packetWithTag;
    if (!encrypt) {
        out.resize(packetWithTag->size() - taglen);
    }
    if (!chacha20Xor(key.substr(32, 32), seqbuf, reinterpret_cast<const unsigned char *>(out.data()),
                     reinterpret_cast<unsigned char *>(&out[0]), aadlen)) {
        return false;
    }
    seqbuf[0] = 1;
    const size_t payloadLen = out.size() - aadlen;
    if (payloadLen > 0) {
        if (!chacha20Xor(key.substr(0, 32), seqbuf,
                         reinterpret_cast<const unsigned char *>(out.data() + aadlen),
                         reinterpret_cast<unsigned char *>(&out[0] + aadlen), payloadLen)) {
            return false;
        }
    }
    if (encrypt) {
        unsigned char tag[16];
        poly1305Auth(tag, reinterpret_cast<const unsigned char *>(out.data()), out.size(), polyKey);
        out.append(reinterpret_cast<const char *>(tag), 16);
    }
    *packetWithTag = out;
    return true;
}

bool SshConnectionPrivate::sendPacket(const string &payloadIn)
{
    ScopedLock<Lock> lock(sendLock);
    if (!lock.isSuccess()) {
        return false;
    }
    if (!socket || !socket->isValid()) {
        return false;
    }
    string payload = payloadIn;
    if (delayedCompressionActive) {
        string compressed;
        if (!compressPayload(payload, &compressed)) {
            setError("compression failed");
            return false;
        }
        payload = compressed;
    }
    const uint32_t align = static_cast<uint32_t>(blockSize);
    uint32_t pad = 4;
    if (sendEncrypted && (isAeadCipher || isChaChaCipher)) {
        while ((1 + static_cast<uint32_t>(payload.size()) + pad) % align != 0) {
            ++pad;
        }
    } else {
        while ((4 + 1 + static_cast<uint32_t>(payload.size()) + pad) % align != 0) {
            ++pad;
        }
    }
    string plaintext;
    const size_t plainLenPos = plaintext.size();
    plaintext.append(4, '\0');
    ngToBigEndian(1 + static_cast<uint32_t>(payload.size()) + pad, &plaintext[plainLenPos]);
    plaintext.push_back(static_cast<char>(pad));
    plaintext.append(payload);
    plaintext += randomBytes(pad);

    string out;
    if (!sendEncrypted) {
        out = plaintext;
        ++sendSeq;
    } else if (isChaChaCipher) {
        out = plaintext;
        if (!chachaCrypt(true, sendSeq, sendChaChaKey, &out)) {
            setError("chacha encrypt failed");
            return false;
        }
        ++sendSeq;
    } else if (sendAead) {
        string lenField = plaintext.substr(0, 4);
        string body = plaintext.substr(4);
        string nonce = sendGcmIv;
        if (nonce.size() != 12) {
            setError("gcm iv missing");
            return false;
        }
        string ctAndTag;
        if (!sendAead->seal(nonce, lenField, body, &ctAndTag)) {
            setError("gcm encrypt failed");
            return false;
        }
        uint64_t ctr = ngFromBigEndian<uint64_t>(sendGcmIv.data() + 4);
        ++ctr;
        ngToBigEndian(ctr, &sendGcmIv[4]);
        out = lenField + ctAndTag;
        ++sendSeq;
    } else if (macEtm) {
        string encrypted = sendCipher->addData(plaintext);
        if (encrypted.size() != plaintext.size()) {
            return false;
        }
        string seqStr;
        seqStr.append(4, '\0');
        ngToBigEndian(sendSeq, &seqStr[0]);
        string mac = qtng::hmac(macHashForAlgo(macAlgo), sendMacKey, seqStr + encrypted);
        out = encrypted + mac;
        ++sendSeq;
    } else {
        string seqStr;
        seqStr.append(4, '\0');
        ngToBigEndian(sendSeq, &seqStr[0]);
        string mac = qtng::hmac(macHashForAlgo(macAlgo), sendMacKey, seqStr + plaintext);
        string encrypted = sendCipher->addData(plaintext);
        if (encrypted.size() != plaintext.size()) {
            return false;
        }
        out = encrypted + mac;
        ++sendSeq;
    }
    bytesSent += out.size();
    return socket->sendall(out) == static_cast<int32_t>(out.size());
}

bool SshConnectionPrivate::recvPacket(string *payload)
{
    if (!socket || !socket->isValid()) {
        return false;
    }
    string plainPayload;
    if (!recvEncrypted) {
        const string header = socket->recvall(4);
        if (header.size() < 4) {
            return false;
        }
        const uint32_t packetLen = readBe32(header);
        if (packetLen < 1 || packetLen > kMaxPacketLength) {
            setError("invalid packet length");
            return false;
        }
        const string body = socket->recvall(packetLen);
        if (body.size() < packetLen) {
            return false;
        }
        const uint8_t padLen = static_cast<uint8_t>(body[0]);
        if (padLen + 1 > packetLen) {
            setError("invalid padding length");
            return false;
        }
        plainPayload.assign(body.data() + 1, packetLen - padLen - 1);
        ++recvSeq;
        bytesReceived += 4 + packetLen;
    } else if (isChaChaCipher) {
        const string encLen = socket->recvall(4);
        if (encLen.size() < 4) {
            return false;
        }
        uint32_t packetLen = 0;
        if (!chachaDecryptLength(recvSeq, recvChaChaKey, encLen, &packetLen) || packetLen < 1
            || packetLen > kMaxPacketLength) {
            setError("invalid packet length");
            return false;
        }
        const string rest = socket->recvall(static_cast<int32_t>(packetLen + 16));
        if (rest.size() < packetLen + 16) {
            return false;
        }
        string packet = encLen + rest;
        if (!chachaCrypt(false, recvSeq, recvChaChaKey, &packet)) {
            setError("chacha decrypt/MAC failed");
            return false;
        }
        const uint8_t padLen = static_cast<uint8_t>(packet[4]);
        if (padLen + 1 > packetLen) {
            setError("invalid padding length");
            return false;
        }
        plainPayload.assign(packet.data() + 5, packetLen - padLen - 1);
        ++recvSeq;
        bytesReceived += 4 + packetLen + 16;
    } else if (recvAead) {
        const string lenField = socket->recvall(4);
        if (lenField.size() < 4) {
            return false;
        }
        const uint32_t packetLen = readBe32(lenField);
        if (packetLen < 1 || packetLen > kMaxPacketLength) {
            setError("invalid packet length");
            return false;
        }
        const string ctAndTag = socket->recvall(static_cast<int32_t>(packetLen + 16));
        if (ctAndTag.size() < packetLen + 16) {
            return false;
        }
        if (recvGcmIv.size() != 12) {
            setError("gcm iv missing");
            return false;
        }
        string nonce = recvGcmIv;
        string body;
        if (!recvAead->open(nonce, lenField, ctAndTag, &body)) {
            setError("gcm decrypt failed");
            return false;
        }
        uint64_t ctr = ngFromBigEndian<uint64_t>(recvGcmIv.data() + 4);
        ++ctr;
        ngToBigEndian(ctr, &recvGcmIv[4]);
        if (body.size() != packetLen) {
            setError("gcm length mismatch");
            return false;
        }
        const uint8_t padLen = static_cast<uint8_t>(body[0]);
        if (padLen + 1 > packetLen) {
            setError("invalid padding length");
            return false;
        }
        plainPayload.assign(body.data() + 1, packetLen - padLen - 1);
        ++recvSeq;
        bytesReceived += 4 + packetLen + 16;
    } else if (macEtm) {
        const string encHeader = socket->recvall(4);
        if (encHeader.size() < 4) {
            return false;
        }
        const string encLenPlain = recvCipher->addData(encHeader);
        const uint32_t packetLen = readBe32(encLenPlain);
        if (packetLen < 1 || packetLen > kMaxPacketLength) {
            setError("invalid packet length");
            return false;
        }
        const string encRest = socket->recvall(static_cast<int32_t>(packetLen));
        if (encRest.size() < packetLen) {
            return false;
        }
        string ciphertext = encHeader + encRest;
        const string mac = socket->recvall(static_cast<int32_t>(macLength));
        if (mac.size() < static_cast<size_t>(macLength)) {
            return false;
        }
        string seqStr;
        seqStr.append(4, '\0');
        ngToBigEndian(recvSeq, &seqStr[0]);
        string expected = qtng::hmac(macHashForAlgo(macAlgo), recvMacKey, seqStr + ciphertext);
        if (expected != mac) {
            setError("MAC mismatch");
            return false;
        }
        const string decRest = recvCipher->addData(encRest);
        const string plaintext = encLenPlain + decRest;
        const uint8_t padLen = static_cast<uint8_t>(plaintext[4]);
        if (padLen + 1 > packetLen) {
            setError("invalid padding length");
            return false;
        }
        plainPayload.assign(plaintext.data() + 5, packetLen - padLen - 1);
        ++recvSeq;
        bytesReceived += 4 + packetLen + static_cast<uint64_t>(macLength);
    } else {
        const uint32_t block = static_cast<uint32_t>(blockSize);
        const string encBlock = socket->recvall(static_cast<int32_t>(block));
        if (encBlock.size() < block) {
            return false;
        }
        const string decBlock = recvCipher->addData(encBlock);
        const uint32_t packetLen = readBe32(decBlock);
        if (packetLen < 1 || packetLen > kMaxPacketLength) {
            setError("invalid packet length");
            return false;
        }
        const uint32_t total = 4 + packetLen;
        if (total < block) {
            setError("packet too small");
            return false;
        }
        const uint32_t remaining = total - block;
        const string encRest = socket->recvall(static_cast<int32_t>(remaining));
        if (encRest.size() < remaining) {
            return false;
        }
        const string decRest = recvCipher->addData(encRest);
        const string plaintext = decBlock + decRest;
        if (macLength > 0) {
            string seqStr;
            seqStr.append(4, '\0');
            ngToBigEndian(recvSeq, &seqStr[0]);
            string expected = qtng::hmac(macHashForAlgo(macAlgo), recvMacKey, seqStr + plaintext);
            const string actual = socket->recvall(static_cast<int32_t>(macLength));
            if (actual.size() < static_cast<size_t>(macLength) || expected != actual) {
                setError("MAC mismatch");
                return false;
            }
        }
        ++recvSeq;
        const uint8_t padLen = static_cast<uint8_t>(plaintext[4]);
        if (padLen + 1 > packetLen) {
            setError("invalid padding length");
            return false;
        }
        plainPayload.assign(plaintext.data() + 5, packetLen - padLen - 1);
        bytesReceived += total + static_cast<uint64_t>(macLength);
    }

    if (delayedCompressionActive) {
        string decompressed;
        if (!decompressPayload(plainPayload, &decompressed)) {
            setError("decompression failed");
            return false;
        }
        *payload = decompressed;
    } else {
        *payload = plainPayload;
    }
    maybeRekey();
    return true;
}

bool SshConnectionPrivate::sendUnimplemented(uint32_t seq)
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_UNIMPLEMENTED);
    buf.putUint32(seq);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendDisconnect(uint32_t reason, const string &description)
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_DISCONNECT);
    buf.putUint32(reason);
    buf.putString(description);
    buf.putString("en");
    return sendPacket(buf.raw());
}

void SshConnectionPrivate::notifyChannelsClosed()
{
    for (const pair<const uint32_t, shared_ptr<SshChannelPrivate>> &pair : channels) {
        const shared_ptr<SshChannelPrivate> &ch = pair.second;
        ch->notifyRemoteClose();
    }
}

void SshConnectionPrivate::readLoop(bool untilAuthenticated)
{
    while (running) {
        string payload;
        if (!recvPacket(&payload)) {
            if (error == Socket::NoError) {
                setError("connection closed");
            }
            break;
        }
        if (payload.empty()) {
            continue;
        }
        const uint8_t type = static_cast<uint8_t>(payload[0]);
        switch (type) {
        case SSH_MSG_DISCONNECT:
            setError("peer disconnected");
            return;
        case SSH_MSG_IGNORE:
        case SSH_MSG_DEBUG:
            if (strictKexActive && !initialKexDone) {
                setError("unexpected message during strict kex");
                return;
            }
            break;
        case SSH_MSG_UNIMPLEMENTED:
            break;
        case SSH_MSG_KEXINIT:
            handleKexInitMessage(payload);
            break;
        case SSH_MSG_NEWKEYS:
            handleNewKeysMessage();
            break;
        case SSH_MSG_KEX_ECDH_INIT:
            if (!serverSide) {
                sendUnimplemented(recvSeq - 1);
                break;
            }
            handleKexEcdhInit(payload);
            break;
        case SSH_MSG_KEX_ECDH_REPLY:
            if (serverSide) {
                sendUnimplemented(recvSeq - 1);
                break;
            }
            handleKexEcdhReply(payload);
            break;
        case SSH_MSG_SERVICE_REQUEST:
            if (serverSide) {
                handleServiceRequest(payload);
            }
            break;
        case SSH_MSG_SERVICE_ACCEPT:
            if (!serverSide) {
                serviceAcceptOk = true;
                serviceAcceptEvent.set();
            }
            break;
        case SSH_MSG_USERAUTH_REQUEST:
            if (serverSide) {
                handleAuthRequest(payload);
            }
            break;
        case SSH_MSG_USERAUTH_SUCCESS:
            if (!serverSide) {
                authenticated = true;
                connectionServiceAccepted = true;
                activateDelayedCompression();
                finishClientAuthReply(true);
                if (serverAliveInterval > 0.0f && !keepaliveCoroutine) {
                    shared_ptr<SshConnectionPrivate> self = shared_from_this();
                    keepaliveCoroutine = shared_ptr<Coroutine>(Coroutine::spawn([self] { self->keepaliveLoop(); }));
                }
            }
            break;
        case SSH_MSG_USERAUTH_FAILURE:
            if (!serverSide) {
                SshBuffer failBuf(payload);
                uint8_t failType = 0;
                bool partial = false;
                peerAuthMethods.clear();
                if (failBuf.getByte(&failType) && failBuf.getNameList(&peerAuthMethods)
                    && failBuf.getBoolean(&partial)) {
                    string methods;
                    for (size_t i = 0; i < peerAuthMethods.size(); ++i) {
                        if (i) {
                            methods.push_back(',');
                        }
                        methods += peerAuthMethods[i];
                    }
                    if (partial && !methods.empty()) {
                        clientAuthError = "authentication failed (partial; available: " + methods + ")";
                    } else if (!methods.empty()) {
                        clientAuthError = "authentication failed (available: " + methods + ")";
                    } else {
                        clientAuthError = "authentication failed";
                    }
                } else {
                    clientAuthError = "authentication failed";
                }
                finishClientAuthReply(false);
            }
            break;
        case SSH_MSG_USERAUTH_BANNER:
            break;
        case SSH_MSG_EXT_INFO:
            handleExtInfo(payload);
            break;
        case SSH_MSG_USERAUTH_PK_OK:
            // Type 60: PK_OK / PASSWD_CHANGEREQ / INFO_REQUEST depending on method.
            if (!serverSide) {
                switch (clientAuthMethod) {
                case SshClientAuthMethod::PublicKey:
                    finishClientAuthReply(true);
                    break;
                case SshClientAuthMethod::KeyboardInteractive:
                    if (!handleUserauthInfoRequest(payload)) {
                        finishClientAuthReply(false);
                    }
                    break;
                case SshClientAuthMethod::Password:
                    // PASSWD_CHANGEREQ — not a transport failure; keep SocketError clean.
                    clientAuthError = "password change required by server";
                    finishClientAuthReply(false);
                    break;
                default:
                    sendUnimplemented(recvSeq - 1);
                    break;
                }
            }
            break;
        case SSH_MSG_GLOBAL_REQUEST:
            handleGlobalRequest(payload);
            break;
        case SSH_MSG_REQUEST_SUCCESS:
            handleRequestReply(true);
            break;
        case SSH_MSG_REQUEST_FAILURE:
            handleRequestReply(false);
            break;
        case SSH_MSG_CHANNEL_OPEN:
            if (serverSide) {
                handleChannelOpen(payload);
            }
            break;
        case SSH_MSG_CHANNEL_OPEN_CONFIRMATION:
            if (!serverSide) {
                handleChannelOpenConfirmation(payload);
            }
            break;
        case SSH_MSG_CHANNEL_OPEN_FAILURE:
            if (!serverSide) {
                channelOpenOk = false;
                channelOpenFinished = true;
                channelOpenEvent.set();
            }
            break;
        case SSH_MSG_CHANNEL_WINDOW_ADJUST:
            handleChannelWindowAdjust(payload);
            break;
        case SSH_MSG_CHANNEL_DATA:
            handleChannelData(payload);
            break;
        case SSH_MSG_CHANNEL_EXTENDED_DATA:
            handleExtendedData(payload);
            break;
        case SSH_MSG_CHANNEL_EOF:
            handleChannelEof(payload);
            break;
        case SSH_MSG_CHANNEL_CLOSE:
            handleChannelClose(payload);
            break;
        case SSH_MSG_CHANNEL_REQUEST:
            handleChannelRequest(payload);
            break;
        case SSH_MSG_CHANNEL_SUCCESS:
            if (!serverSide) {
                requestReplyOk = true;
                requestReplyFinished = true;
                requestReplyEvent.set();
            }
            break;
        case SSH_MSG_CHANNEL_FAILURE:
            if (!serverSide) {
                requestReplyOk = false;
                requestReplyFinished = true;
                requestReplyEvent.set();
            }
            break;
        default:
            sendUnimplemented(recvSeq - 1);
            break;
        }
        if (untilAuthenticated && serverSide && authenticated) {
            break;
        }
    }
}

// ---- Connection layer ----

shared_ptr<SshChannelPrivate> SshConnectionPrivate::createChannel(uint32_t peerId, uint32_t remoteWindow,
                                                                   uint32_t maxPacketSize, bool *ok)
{
    const uint32_t localId = nextLocalChannelId++;
    shared_ptr<SshChannelPrivate> ch(
            new SshChannelPrivate(this, localId, peerId, remoteWindow, kLocalWindowSize, maxPacketSize));
    channels[localId] = ch;
    *ok = true;
    return ch;
}

void SshConnectionPrivate::handleChannelOpen(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type;
    string channelType;
    uint32_t senderId, window, maxPacket;
    if (!buf.getByte(&type) || !buf.getString(&channelType) || !buf.getUint32(&senderId) || !buf.getUint32(&window)
        || !buf.getUint32(&maxPacket)) {
        return;
    }
    if (channelType != "session") {
        SshBuffer fail;
        fail.putByte(SSH_MSG_CHANNEL_OPEN_FAILURE);
        fail.putUint32(senderId);
        fail.putUint32(3);  // SSH_OPEN_UNKNOWN_CHANNEL_TYPE
        fail.putString("unsupported channel type");
        fail.putString("en");
        sendPacket(fail.raw());
        return;
    }
    bool ok = false;
    shared_ptr<SshChannelPrivate> ch = createChannel(senderId, window, maxPacket, &ok);
    if (!ok) {
        return;
    }
    SshBuffer conf;
    conf.putByte(SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    conf.putUint32(senderId);
    conf.putUint32(ch->localId);
    conf.putUint32(ch->localWindow);
    conf.putUint32(ch->maxPacketSize);
    if (!sendPacket(conf.raw())) {
        return;
    }
    shared_ptr<SshChannel> channel(new SshChannel(ch));
    shared_ptr<SshConnectionPrivate> self = shared_from_this();
    if (app) {
        shared_ptr<Coroutine> c = shared_ptr<Coroutine>(Coroutine::spawn([this, self, channel] {
            app->run(channel.get());
            channel->sendExitStatus(0);
            channel->close();
        }));
        appCoroutines.push_back(c);
    }
}

void SshConnectionPrivate::handleChannelOpenConfirmation(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type;
    uint32_t recipient, sender, window, maxPacket;
    if (!buf.getByte(&type) || !buf.getUint32(&recipient) || !buf.getUint32(&sender) || !buf.getUint32(&window)
        || !buf.getUint32(&maxPacket)) {
        channelOpenOk = false;
        channelOpenFinished = true;
        channelOpenEvent.set();
        return;
    }
    map<uint32_t, shared_ptr<SshChannelPrivate>>::iterator it = channels.find(recipient);
    if (it == channels.end()) {
        channelOpenOk = false;
        channelOpenFinished = true;
        channelOpenEvent.set();
        return;
    }
    shared_ptr<SshChannelPrivate> &ch = it->second;
    ch->peerId = sender;
    ch->remoteWindow = window;
    ch->maxPacketSize = maxPacket;
    pendingChannel = ch;
    channelOpenOk = true;
    channelOpenFinished = true;
    channelOpenEvent.set();
}

void SshConnectionPrivate::handleChannelOpenFailure(const string &payload)
{
    channelOpenOk = false;
    channelOpenFinished = true;
    channelOpenEvent.set();
}

void SshConnectionPrivate::handleChannelData(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type;
    uint32_t id;
    string data;
    if (!buf.getByte(&type) || !buf.getUint32(&id) || !buf.getString(&data)) {
        return;
    }
    map<uint32_t, shared_ptr<SshChannelPrivate>>::iterator it = channels.find(id);
    if (it == channels.end()) {
        return;
    }
    shared_ptr<SshChannelPrivate> &ch = it->second;
    if (ch->localWindow >= data.size()) {
        ch->localWindow -= static_cast<uint32_t>(data.size());
    } else {
        ch->localWindow = 0;
    }
    if (ch->localWindow < ch->initialWindow / 2) {
        const uint32_t adjust = ch->initialWindow - ch->localWindow;
        ch->localWindow = ch->initialWindow;
        sendWindowAdjust(ch.get(), adjust);
    }
    ch->incoming.putForcedly(data);
}

void SshConnectionPrivate::handleChannelWindowAdjust(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type;
    uint32_t id, n;
    if (!buf.getByte(&type) || !buf.getUint32(&id) || !buf.getUint32(&n)) {
        return;
    }
    map<uint32_t, shared_ptr<SshChannelPrivate>>::iterator it = channels.find(id);
    if (it == channels.end()) {
        return;
    }
    shared_ptr<SshChannelPrivate> &ch = it->second;
    ch->remoteWindow += n;
    ch->windowAdjust.set();
}

void SshConnectionPrivate::handleChannelEof(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type;
    uint32_t id;
    if (!buf.getByte(&type) || !buf.getUint32(&id)) {
        return;
    }
    map<uint32_t, shared_ptr<SshChannelPrivate>>::iterator it = channels.find(id);
    if (it == channels.end()) {
        return;
    }
    it->second->notifyEof();
}

void SshConnectionPrivate::handleChannelClose(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type;
    uint32_t id;
    if (!buf.getByte(&type) || !buf.getUint32(&id)) {
        return;
    }
    map<uint32_t, shared_ptr<SshChannelPrivate>>::iterator it = channels.find(id);
    if (it == channels.end()) {
        return;
    }
    shared_ptr<SshChannelPrivate> &ch = it->second;
    ch->notifyRemoteClose();
    if (ch->closed) {
        channels.erase(it);
    }
}

void SshConnectionPrivate::handleChannelRequest(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type;
    uint32_t id;
    string requestType;
    bool wantReply;
    if (!buf.getByte(&type) || !buf.getUint32(&id) || !buf.getString(&requestType) || !buf.getBoolean(&wantReply)) {
        return;
    }
    map<uint32_t, shared_ptr<SshChannelPrivate>>::iterator it = channels.find(id);
    if (it == channels.end()) {
        if (wantReply) {
            sendChannelFailure(id);
        }
        return;
    }
    shared_ptr<SshChannelPrivate> &ch = it->second;
    bool success = true;
    if (requestType == "pty-req") {
        string term, modes;
        uint32_t cols, rows, width, height;
        if (!buf.getString(&term) || !buf.getUint32(&cols) || !buf.getUint32(&rows) || !buf.getUint32(&width)
            || !buf.getUint32(&height) || !buf.getString(&modes)) {
            success = false;
        } else {
            ch->termSize.setColumns(cols);
            ch->termSize.setRows(rows);
            if (ch->callback) {
                ch->callback->onResize(ch->termSize);
            }
        }
    } else if (requestType == "window-change") {
        uint32_t cols, rows, width, height;
        if (!buf.getUint32(&cols) || !buf.getUint32(&rows) || !buf.getUint32(&width) || !buf.getUint32(&height)) {
            success = false;
        } else {
            ch->termSize.setColumns(cols);
            ch->termSize.setRows(rows);
            if (ch->callback) {
                ch->callback->onResize(ch->termSize);
            }
        }
    } else if (requestType == "signal") {
        string name;
        if (!buf.getString(&name)) {
            success = false;
        } else if (ch->callback) {
            ch->callback->onSignal(name);
        }
    } else if (requestType == "shell" || requestType == "env" || requestType == "exec"
               || requestType == "subsystem") {
        if (requestType == "env") {
            string name, value;
            if (!buf.getString(&name) || !buf.getString(&value)) {
                success = false;
            }
        } else if (requestType == "exec" || requestType == "subsystem") {
            string cmd;
            if (!buf.getString(&cmd)) {
                success = false;
            }
        }
    } else if (requestType == "exit-status") {
        uint32_t status = 0;
        if (!buf.getUint32(&status)) {
            success = false;
        } else {
            ch->hasExitStatus = true;
            ch->exitStatus = status;
            if (ch->callback) {
                ch->callback->onExitStatus(status);
            }
        }
    } else if (requestType == "exit-signal") {
        string sigName, errMsg, lang;
        bool coreDumped = false;
        if (!buf.getString(&sigName) || !buf.getBoolean(&coreDumped) || !buf.getString(&errMsg)
            || !buf.getString(&lang)) {
            success = false;
        } else {
            ch->exitSignal = sigName;
            if (ch->callback) {
                ch->callback->onExitSignal(sigName);
            }
        }
    } else {
        success = false;
    }
    if (wantReply) {
        if (success) {
            sendChannelSuccess(id);
        } else {
            sendChannelFailure(id);
        }
    }
}

void SshConnectionPrivate::handleChannelRequestReply(const string &payload, bool success)
{
    requestReplyOk = success;
    requestReplyFinished = true;
    requestReplyEvent.set();
}

bool SshConnectionPrivate::sendChannelData(SshChannelPrivate *ch, const string &data)
{
    if (ch->eofSent) {
        return false;
    }
    SshBuffer buf;
    buf.putByte(SSH_MSG_CHANNEL_DATA);
    buf.putUint32(ch->peerId);
    buf.putString(data);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendChannelExtendedData(SshChannelPrivate *ch, uint32_t dataType, const string &data)
{
    if (ch->eofSent) {
        return false;
    }
    SshBuffer buf;
    buf.putByte(SSH_MSG_CHANNEL_EXTENDED_DATA);
    buf.putUint32(ch->peerId);
    buf.putUint32(dataType);
    buf.putString(data);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendChannelEof(SshChannelPrivate *ch)
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_CHANNEL_EOF);
    buf.putUint32(ch->peerId);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendChannelClose(SshChannelPrivate *ch)
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_CHANNEL_CLOSE);
    buf.putUint32(ch->peerId);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendWindowAdjust(SshChannelPrivate *ch, uint32_t n)
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_CHANNEL_WINDOW_ADJUST);
    buf.putUint32(ch->peerId);
    buf.putUint32(n);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendChannelRequest(SshChannelPrivate *ch, const string &type, const SshBuffer &extra,
                                              bool wantReply)
{
    if (wantReply) {
        requestReplyFinished = false;
        requestReplyEvent.clear();
    }
    SshBuffer buf;
    buf.putByte(SSH_MSG_CHANNEL_REQUEST);
    buf.putUint32(ch->peerId);
    buf.putString(type);
    buf.putBoolean(wantReply);
    buf.putBytes(extra.raw());
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendOpenChannel()
{
    channelOpenFinished = false;
    channelOpenEvent.clear();
    const uint32_t localId = nextLocalChannelId++;
    shared_ptr<SshChannelPrivate> ch(
            new SshChannelPrivate(this, localId, 0, 0, kLocalWindowSize, kMaxPacketSize));
    channels[localId] = ch;
    pendingChannel = ch;
    SshBuffer buf;
    buf.putByte(SSH_MSG_CHANNEL_OPEN);
    buf.putString("session");
    buf.putUint32(localId);
    buf.putUint32(kLocalWindowSize);
    buf.putUint32(kMaxPacketSize);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendChannelSuccess(uint32_t peerChannelId)
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_CHANNEL_SUCCESS);
    buf.putUint32(peerChannelId);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendChannelFailure(uint32_t peerChannelId)
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_CHANNEL_FAILURE);
    buf.putUint32(peerChannelId);
    return sendPacket(buf.raw());
}

// ---- Auth (server) ----

void SshConnectionPrivate::handleAuthRequest(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type;
    string user, service, method;
    if (!buf.getByte(&type) || !buf.getString(&user) || !buf.getString(&service) || !buf.getString(&method)) {
        return;
    }
    if (service != "ssh-connection") {
        sendAuthFailure();
        return;
    }
    authUser = user;
    if (method == "none") {
        sendAuthFailure();
        return;
    }
    if (method == "password") {
        bool change;
        string password;
        if (!buf.getBoolean(&change) || !buf.getString(&password)) {
            return;
        }
        if (auth && auth->checkPassword(user, password)) {
            authenticated = true;
            sendAuthSuccess();
        } else {
            sendAuthFailure();
        }
        return;
    }
    if (method == "publickey") {
        bool hasSig;
        string algo, blob;
        if (!buf.getBoolean(&hasSig) || !buf.getString(&algo) || !buf.getString(&blob)) {
            return;
        }
        if (hasSig) {
            string signature;
            if (!buf.getString(&signature)) {
                return;
            }
            if (auth && auth->checkPublicKey(user, blob)
                && verifyPublicKeySignature(user, algo, blob, signature)) {
                authenticated = true;
                sendAuthSuccess();
            } else {
                sendAuthFailure();
            }
        } else {
            if (auth && auth->checkPublicKey(user, blob)) {
                sendPkOk(algo, blob);
            } else {
                sendAuthFailure();
            }
        }
        return;
    }
    sendAuthFailure();
}

bool SshConnectionPrivate::sendAuthFailure()
{
    ++authTries;
    if (!banner.empty() && !bannerSent) {
        SshBuffer b;
        b.putByte(SSH_MSG_USERAUTH_BANNER);
        b.putString(banner);
        b.putString("en");
        sendPacket(b.raw());
        bannerSent = true;
    }
    if (authTries >= maxAuthTries) {
        sendDisconnect(14, "too many authentication failures");
        running = false;
        return false;
    }
    SshBuffer buf;
    buf.putByte(SSH_MSG_USERAUTH_FAILURE);
    vector<string> methods;
    if (auth) {
        methods.push_back("password");
        methods.push_back("publickey");
    }
    buf.putNameList(methods);
    buf.putBoolean(false);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::sendAuthSuccess()
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_USERAUTH_SUCCESS);
    if (!sendPacket(buf.raw())) {
        return false;
    }
    connectionServiceAccepted = true;
    activateDelayedCompression();
    return true;
}

bool SshConnectionPrivate::sendPkOk(const string &algo, const string &blob)
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_USERAUTH_PK_OK);
    buf.putString(algo);
    buf.putString(blob);
    return sendPacket(buf.raw());
}

bool SshConnectionPrivate::verifyPublicKeySignature(const string &user, const string &algo, const string &blob,
                                                    const string &signature)
{
    PublicKey key;
    if (!sshParsePublicKeyBlob(blob, &key)) {
        return false;
    }
    SshBuffer sigBuf(signature);
    string sigAlgo, sig;
    if (!sigBuf.getString(&sigAlgo) || !sigBuf.getString(&sig)) {
        return false;
    }
    SshBuffer dataBuf;
    dataBuf.putString(sessionId);
    dataBuf.putByte(SSH_MSG_USERAUTH_REQUEST);
    dataBuf.putString(user);
    dataBuf.putString("ssh-connection");
    dataBuf.putString("publickey");
    dataBuf.putBoolean(true);
    dataBuf.putString(algo);
    dataBuf.putString(blob);
    if (sigAlgo == "ssh-ed25519") {
        return key.verify(dataBuf.raw(), sig, MessageDigest::Sha256);
    }
    return key.verify(dataBuf.raw(), sig, rsaHashForAlgo(sigAlgo));
}

void SshConnectionPrivate::handleServiceRequest(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type;
    string service;
    if (!buf.getByte(&type) || !buf.getString(&service)) {
        return;
    }
    if (service == "ssh-userauth" || service == "ssh-connection") {
        SshBuffer accept;
        accept.putByte(SSH_MSG_SERVICE_ACCEPT);
        accept.putString(service);
        sendPacket(accept.raw());
    } else {
        sendUnimplemented(recvSeq - 1);
    }
}

// ---- Client helpers ----

bool SshConnectionPrivate::authMethodAvailable(const string &method) const
{
    return find(peerAuthMethods.begin(), peerAuthMethods.end(), method) != peerAuthMethods.end();
}

void SshConnectionPrivate::finishClientAuthReply(bool ok)
{
    authOk = ok;
    authFinished = true;
    authEvent.set();
}

void SshConnectionPrivate::setAuthErrorIfEmpty(const string &fallback)
{
    if (!errorString.empty()) {
        return;
    }
    errorString = clientAuthError.empty() ? fallback : clientAuthError;
}

bool SshConnectionPrivate::handleUserauthInfoRequest(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type = 0;
    string name, instruction, language;
    uint32_t numPrompts = 0;
    if (!buf.getByte(&type) || !buf.getString(&name) || !buf.getString(&instruction)
        || !buf.getString(&language) || !buf.getUint32(&numPrompts)) {
        clientAuthError = "bad USERAUTH_INFO_REQUEST";
        return false;
    }
    if (numPrompts > 64) {
        clientAuthError = "USERAUTH_INFO_REQUEST has too many prompts";
        return false;
    }
    vector<string> responses;
    responses.reserve(numPrompts);
    for (uint32_t i = 0; i < numPrompts; ++i) {
        string prompt;
        bool echo = true;
        if (!buf.getString(&prompt) || !buf.getBoolean(&echo)) {
            clientAuthError = "bad USERAUTH_INFO_REQUEST prompt";
            return false;
        }
        // Secret prompts get the password; visible ones stay empty.
        responses.push_back(echo ? string() : clientPassword);
    }
    SshBuffer resp;
    resp.putByte(SSH_MSG_USERAUTH_INFO_RESPONSE);
    resp.putUint32(static_cast<uint32_t>(responses.size()));
    for (const string &r : responses) {
        resp.putString(r);
    }
    return sendPacket(resp.raw());
}

bool SshConnectionPrivate::startClientKeyboardInteractiveAuth()
{
    clientAuthMethod = SshClientAuthMethod::KeyboardInteractive;
    clientAuthError.clear();
    authFinished = false;
    authEvent.clear();
    SshBuffer buf;
    buf.putByte(SSH_MSG_USERAUTH_REQUEST);
    buf.putString(clientUser);
    buf.putString("ssh-connection");
    buf.putString("keyboard-interactive");
    buf.putString("");  // language tag
    buf.putString("");  // submethods
    if (!sendPacket(buf.raw())) {
        setAuthErrorIfEmpty("failed to send keyboard-interactive request");
        return false;
    }
    if (!waitAuthReply()) {
        setAuthErrorIfEmpty("keyboard-interactive authentication failed");
        return false;
    }
    if (!authOk) {
        setAuthErrorIfEmpty("keyboard-interactive authentication failed");
    }
    return authOk;
}

bool SshConnectionPrivate::startClientAuth(const string &user, const string &password)
{
    clientUser = user;
    clientPassword = password;
    clientAuthMethod = SshClientAuthMethod::Password;
    clientAuthError.clear();
    peerAuthMethods.clear();
    if (!sendUserauthService()) {
        setAuthErrorIfEmpty("ssh-userauth service not accepted");
        return false;
    }

    // Try "password" first. Even when the server rejects the method, the
    // USERAUTH_FAILURE namelist tells us whether keyboard-interactive is next.
    authFinished = false;
    authEvent.clear();
    SshBuffer buf;
    buf.putByte(SSH_MSG_USERAUTH_REQUEST);
    buf.putString(user);
    buf.putString("ssh-connection");
    buf.putString("password");
    buf.putBoolean(false);
    buf.putString(password);
    if (!sendPacket(buf.raw()) || !waitAuthReply()) {
        setAuthErrorIfEmpty("password authentication failed");
        return false;
    }
    if (authOk) {
        return true;
    }

    // Many OpenSSH BBS deployments disable "password" and only offer
    // keyboard-interactive (often backed by the same account password).
    if (authMethodAvailable("keyboard-interactive") && startClientKeyboardInteractiveAuth()) {
        return true;
    }
    setAuthErrorIfEmpty("authentication failed");
    return false;
}

string SshConnectionPrivate::pickPublicKeyAlgo(const PrivateKey &key) const
{
    if (key.algorithm() == PublicKey::Ed25519) {
        return "ssh-ed25519";
    }
    const vector<string> preferred = {"rsa-sha2-512", "rsa-sha2-256", "ssh-rsa"};
    if (!serverSigAlgs.empty()) {
        for (const string &a : preferred) {
            if (find(serverSigAlgs.begin(), serverSigAlgs.end(), a) != serverSigAlgs.end()) {
                return a;
            }
        }
    }
    return "rsa-sha2-256";
}

bool SshConnectionPrivate::startClientPublicKeyAuth(const string &user, const PrivateKey &key)
{
    clientUser = user;
    clientAuthMethod = SshClientAuthMethod::PublicKey;
    clientAuthError.clear();
    if (!sendUserauthService()) {
        setAuthErrorIfEmpty("ssh-userauth service not accepted");
        return false;
    }
    clientKeyBlob = sshPublicKeyBlob(key.publicKey());
    clientKeyAlgo = pickPublicKeyAlgo(key);
    if (clientKeyBlob.empty() || clientKeyAlgo.empty()) {
        return false;
    }
    authFinished = false;
    authEvent.clear();
    SshBuffer probe;
    probe.putByte(SSH_MSG_USERAUTH_REQUEST);
    probe.putString(user);
    probe.putString("ssh-connection");
    probe.putString("publickey");
    probe.putBoolean(false);
    probe.putString(clientKeyAlgo);
    probe.putString(clientKeyBlob);
    if (!sendPacket(probe.raw())) {
        return false;
    }
    if (!waitAuthReply() || !authOk) {
        setAuthErrorIfEmpty("publickey rejected");
        return false;
    }
    authFinished = false;
    authEvent.clear();
    SshBuffer dataBuf;
    dataBuf.putString(sessionId);
    dataBuf.putByte(SSH_MSG_USERAUTH_REQUEST);
    dataBuf.putString(user);
    dataBuf.putString("ssh-connection");
    dataBuf.putString("publickey");
    dataBuf.putBoolean(true);
    dataBuf.putString(clientKeyAlgo);
    dataBuf.putString(clientKeyBlob);
    PrivateKey signingKey = key;
    MessageDigest::Algorithm hashAlgo =
            (clientKeyAlgo == "ssh-ed25519") ? MessageDigest::Sha256 : rsaHashForAlgo(clientKeyAlgo);
    const string signature = signingKey.sign(dataBuf.raw(), hashAlgo);
    if (signature.empty()) {
        return false;
    }
    SshBuffer sigMsg;
    sigMsg.putString(clientKeyAlgo);
    sigMsg.putString(signature);
    SshBuffer req;
    req.putByte(SSH_MSG_USERAUTH_REQUEST);
    req.putString(user);
    req.putString("ssh-connection");
    req.putString("publickey");
    req.putBoolean(true);
    req.putString(clientKeyAlgo);
    req.putString(clientKeyBlob);
    req.putString(sigMsg.raw());
    if (!sendPacket(req.raw())) {
        return false;
    }
    if (!waitAuthReply()) {
        setAuthErrorIfEmpty("publickey authentication failed");
        return false;
    }
    if (!authOk) {
        setAuthErrorIfEmpty("publickey authentication failed");
    }
    return authOk;
}

bool SshConnectionPrivate::sendUserauthService()
{
    serviceAcceptOk = false;
    serviceAcceptEvent.clear();
    SshBuffer buf;
    buf.putByte(SSH_MSG_SERVICE_REQUEST);
    buf.putString("ssh-userauth");
    if (!sendPacket(buf.raw())) {
        return false;
    }
    return serviceAcceptEvent.tryWait() && serviceAcceptOk;
}

bool SshConnectionPrivate::waitAuthReply()
{
    // Returns whether a USERAUTH reply arrived. Callers must check authOk for
    // success vs rejection; a transport failure leaves authFinished false.
    const uint32_t msecs = loginTimeout > 0.0f ? static_cast<uint32_t>(loginTimeout * 1000.0f) : UINT_MAX;
    while (!authFinished) {
        if (!authEvent.tryWait(msecs)) {
            if (errorString.empty()) {
                errorString = "authentication timeout";
            }
            return false;
        }
    }
    return true;
}

bool SshConnectionPrivate::waitChannelOpen()
{
    while (!channelOpenFinished) {
        if (!channelOpenEvent.tryWait()) {
            return false;
        }
    }
    return channelOpenOk;
}

bool SshConnectionPrivate::waitRequestReply()
{
    while (!requestReplyFinished) {
        if (!requestReplyEvent.tryWait()) {
            return false;
        }
    }
    return requestReplyOk;
}


bool SshConnectionPrivate::sendConnectionService()
{
    if (connectionServiceAccepted) {
        return true;
    }
    serviceAcceptOk = false;
    serviceAcceptEvent.clear();
    SshBuffer buf;
    buf.putByte(SSH_MSG_SERVICE_REQUEST);
    buf.putString("ssh-connection");
    if (!sendPacket(buf.raw())) {
        return false;
    }
    if (!(serviceAcceptEvent.tryWait() && serviceAcceptOk)) {
        return false;
    }
    connectionServiceAccepted = true;
    return true;
}

bool SshConnectionPrivate::beginRekey(bool sendInit)
{
    if (rekeyInProgress) {
        return true;
    }
    rekeyInProgress = true;
    kexDone = false;
    kexStarted = false;
    myKexInitPayload.clear();
    peerKexInitPayload.clear();
    clientEphPriv.clear();
    clientEphPub.clear();
    serverEphPriv.clear();
    serverEphPub.clear();
    sharedSecret.clear();
    exchangeHash.clear();
    if (sendInit) {
        return sendKexInit();
    }
    return true;
}

bool SshConnectionPrivate::maybeRekey()
{
    if (!initialKexDone || rekeyInProgress || !kexDone || error != Socket::NoError) {
        return true;
    }
    bool need = false;
    if (rekeyLimitBytes > 0 && (bytesSent >= rekeyLimitBytes || bytesReceived >= rekeyLimitBytes)) {
        need = true;
    }
    if (rekeyIntervalSeconds > 0.0f) {
        const int64_t now = utils::DateTime::currentMSecsSinceEpoch();
        if (lastRekeyTimeMs > 0
            && (now - lastRekeyTimeMs) >= static_cast<int64_t>(rekeyIntervalSeconds * 1000.0f)) {
            need = true;
        }
    }
    if (!need) {
        return true;
    }
    return beginRekey(true);
}

void SshConnectionPrivate::handleGlobalRequest(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type = 0;
    string name;
    bool wantReply = false;
    if (!buf.getByte(&type) || !buf.getString(&name) || !buf.getBoolean(&wantReply)) {
        return;
    }
    if (!wantReply) {
        return;
    }
    SshBuffer reply;
    if (name == "keepalive@openssh.com") {
        reply.putByte(SSH_MSG_REQUEST_SUCCESS);
    } else {
        reply.putByte(SSH_MSG_REQUEST_FAILURE);
    }
    sendPacket(reply.raw());
}

void SshConnectionPrivate::handleRequestReply(bool success)
{
    (void) success;
    if (serverAliveOutstanding > 0) {
        --serverAliveOutstanding;
    }
}

void SshConnectionPrivate::handleExtInfo(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type = 0;
    uint32_t n = 0;
    if (!buf.getByte(&type) || !buf.getUint32(&n)) {
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        string name, value;
        if (!buf.getString(&name) || !buf.getString(&value)) {
            return;
        }
        if (name == "server-sig-algs") {
            serverSigAlgs.clear();
            size_t start = 0;
            for (size_t j = 0; j <= value.size(); ++j) {
                if (j == value.size() || value[j] == ',') {
                    if (j > start) {
                        serverSigAlgs.push_back(value.substr(start, j - start));
                    }
                    start = j + 1;
                }
            }
        }
    }
}

void SshConnectionPrivate::handleExtendedData(const string &payload)
{
    SshBuffer buf(payload);
    uint8_t type = 0;
    uint32_t id = 0;
    uint32_t dataType = 0;
    string data;
    if (!buf.getByte(&type) || !buf.getUint32(&id) || !buf.getUint32(&dataType) || !buf.getString(&data)) {
        return;
    }
    if (dataType != 1) {
        return;
    }
    map<uint32_t, shared_ptr<SshChannelPrivate>>::iterator it = channels.find(id);
    if (it == channels.end()) {
        return;
    }
    shared_ptr<SshChannelPrivate> &ch = it->second;
    if (ch->localWindow >= data.size()) {
        ch->localWindow -= static_cast<uint32_t>(data.size());
    } else {
        ch->localWindow = 0;
    }
    if (ch->localWindow < ch->initialWindow / 2) {
        const uint32_t adjust = ch->initialWindow - ch->localWindow;
        ch->localWindow = ch->initialWindow;
        sendWindowAdjust(ch.get(), adjust);
    }
    ch->stderrIncoming.putForcedly(data);
    if (ch->callback) {
        ch->callback->onStderr(data);
    }
}

bool SshConnectionPrivate::sendKeepalive()
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_GLOBAL_REQUEST);
    buf.putString("keepalive@openssh.com");
    buf.putBoolean(true);
    if (!sendPacket(buf.raw())) {
        return false;
    }
    ++serverAliveOutstanding;
    if (serverAliveCountMax > 0 && serverAliveOutstanding >= serverAliveCountMax) {
        setError("server alive count exceeded");
        running = false;
        if (socket) {
            socket->abort();
        }
        return false;
    }
    return true;
}

void SshConnectionPrivate::keepaliveLoop()
{
    while (running && serverAliveInterval > 0.0f) {
        const uint32_t msecs = static_cast<uint32_t>(serverAliveInterval * 1000.0f);
        if (connectionClosed.tryWait(msecs)) {
            break;
        }
        if (!running || !authenticated) {
            continue;
        }
        sendKeepalive();
    }
}

void SshConnectionPrivate::activateDelayedCompression()
{
    if (delayedCompressionActive || compressionAlgo != "zlib@openssh.com") {
        return;
    }
    sendZ = new z_stream();
    memset(sendZ, 0, sizeof(z_stream));
    recvZ = new z_stream();
    memset(recvZ, 0, sizeof(z_stream));
    if (deflateInit(sendZ, Z_DEFAULT_COMPRESSION) != Z_OK || inflateInit(recvZ) != Z_OK) {
        if (sendZ) {
            deflateEnd(sendZ);
            delete sendZ;
            sendZ = nullptr;
        }
        if (recvZ) {
            inflateEnd(recvZ);
            delete recvZ;
            recvZ = nullptr;
        }
        setError("zlib init failed");
        return;
    }
    delayedCompressionActive = true;
}

bool SshConnectionPrivate::compressPayload(const string &in, string *out)
{
    if (!sendZ) {
        *out = in;
        return true;
    }
    out->clear();
    sendZ->next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    sendZ->avail_in = static_cast<uInt>(in.size());
    unsigned char buf[4096];
    int ret = Z_OK;
    do {
        sendZ->next_out = buf;
        sendZ->avail_out = sizeof(buf);
        ret = deflate(sendZ, Z_SYNC_FLUSH);
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            return false;
        }
        out->append(reinterpret_cast<char *>(buf), sizeof(buf) - sendZ->avail_out);
    } while (sendZ->avail_out == 0);
    return true;
}

bool SshConnectionPrivate::decompressPayload(const string &in, string *out)
{
    if (!recvZ) {
        *out = in;
        return true;
    }
    out->clear();
    recvZ->next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    recvZ->avail_in = static_cast<uInt>(in.size());
    unsigned char buf[4096];
    int ret = Z_OK;
    do {
        recvZ->next_out = buf;
        recvZ->avail_out = sizeof(buf);
        ret = inflate(recvZ, Z_SYNC_FLUSH);
        if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END) {
            return false;
        }
        out->append(reinterpret_cast<char *>(buf), sizeof(buf) - recvZ->avail_out);
    } while (recvZ->avail_out == 0);
    return true;
}

// ---- SshChannel ----

SshChannel::SshChannel(const shared_ptr<SshChannelPrivate> &d)
    : d(d)
{
}

SshChannel::~SshChannel()
{
}

int32_t SshChannel::recv(char *data, int32_t size)
{
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recv(&d->incoming, data, size);
}

int32_t SshChannel::recvall(char *data, int32_t size)
{
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    int32_t got = 0;
    while (got < size) {
        int32_t n = d->recv(&d->incoming, data + got, size - got);
        if (n <= 0) {
            return got > 0 ? got : n;
        }
        got += n;
    }
    return got;
}

int32_t SshChannel::send(const char *data, int32_t size)
{
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->send(data, size, false);
}

int32_t SshChannel::sendall(const char *data, int32_t size)
{
    return send(data, size);
}

string SshChannel::recv(int32_t size)
{
    string buf(static_cast<size_t>(size), '\0');
    int32_t n = recv(&buf[0], size);
    if (n < 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

string SshChannel::recvall(int32_t size)
{
    string buf(static_cast<size_t>(size), '\0');
    int32_t n = recvall(&buf[0], size);
    if (n < 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

int32_t SshChannel::send(const string &data)
{
    return send(data.data(), static_cast<int32_t>(data.size()));
}

int32_t SshChannel::sendall(const string &data)
{
    return sendall(data.data(), static_cast<int32_t>(data.size()));
}

int32_t SshChannel::recvStderr(char *data, int32_t size)
{
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recv(&d->stderrIncoming, data, size);
}

int32_t SshChannel::recvallStderr(char *data, int32_t size)
{
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    int32_t got = 0;
    while (got < size) {
        int32_t n = d->recv(&d->stderrIncoming, data + got, size - got);
        if (n <= 0) {
            return got > 0 ? got : n;
        }
        got += n;
    }
    return got;
}

int32_t SshChannel::sendStderr(const char *data, int32_t size)
{
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->send(data, size, true);
}

int32_t SshChannel::sendallStderr(const char *data, int32_t size)
{
    return sendStderr(data, size);
}

string SshChannel::recvStderr(int32_t size)
{
    string buf(static_cast<size_t>(size), '\0');
    int32_t n = recvStderr(&buf[0], size);
    if (n < 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

string SshChannel::recvallStderr(int32_t size)
{
    string buf(static_cast<size_t>(size), '\0');
    int32_t n = recvallStderr(&buf[0], size);
    if (n < 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

int32_t SshChannel::sendStderr(const string &data)
{
    return sendStderr(data.data(), static_cast<int32_t>(data.size()));
}

int32_t SshChannel::sendallStderr(const string &data)
{
    return sendallStderr(data.data(), static_cast<int32_t>(data.size()));
}

void SshChannel::close()
{
    d->closeChannel();
}

bool SshChannel::isClosed() const
{
    return d->closed || d->remoteClosed;
}

SshTerminalSize SshChannel::terminalSize() const
{
    return d->termSize;
}

void SshChannel::setCallback(const shared_ptr<SshChannelCallback> &callback)
{
    d->callback = callback;
}

bool SshChannel::requestPty(const string &term, uint32_t cols, uint32_t rows)
{
    SshBuffer extra;
    extra.putString(term);
    extra.putUint32(cols);
    extra.putUint32(rows);
    extra.putUint32(0);
    extra.putUint32(0);
    extra.putString("");
    return d->conn->sendChannelRequest(d.get(), "pty-req", extra, true) && d->conn->waitRequestReply();
}

bool SshChannel::requestShell()
{
    SshBuffer extra;
    return d->conn->sendChannelRequest(d.get(), "shell", extra, true) && d->conn->waitRequestReply();
}

bool SshChannel::requestExec(const string &command)
{
    SshBuffer extra;
    extra.putString(command);
    return d->conn->sendChannelRequest(d.get(), "exec", extra, true) && d->conn->waitRequestReply();
}

bool SshChannel::requestSubsystem(const string &name)
{
    SshBuffer extra;
    extra.putString(name);
    return d->conn->sendChannelRequest(d.get(), "subsystem", extra, true) && d->conn->waitRequestReply();
}

bool SshChannel::setEnv(const string &name, const string &value)
{
    SshBuffer extra;
    extra.putString(name);
    extra.putString(value);
    return d->conn->sendChannelRequest(d.get(), "env", extra, true) && d->conn->waitRequestReply();
}

bool SshChannel::requestWindowChange(uint32_t cols, uint32_t rows)
{
    SshBuffer extra;
    extra.putUint32(cols);
    extra.putUint32(rows);
    extra.putUint32(0);
    extra.putUint32(0);
    return d->conn->sendChannelRequest(d.get(), "window-change", extra, false);
}

bool SshChannel::sendSignal(const string &signalName)
{
    SshBuffer extra;
    extra.putString(signalName);
    return d->conn->sendChannelRequest(d.get(), "signal", extra, false);
}

bool SshChannel::sendExitStatus(uint32_t status)
{
    if (!d || d->exitStatusSent) {
        return false;
    }
    SshBuffer extra;
    extra.putUint32(status);
    const bool ok = d->conn->sendChannelRequest(d.get(), "exit-status", extra, false);
    if (ok) {
        d->exitStatusSent = true;
    }
    return ok;
}

bool SshChannel::hasExitStatus() const
{
    return d && d->hasExitStatus;
}

uint32_t SshChannel::exitStatus() const
{
    return d ? d->exitStatus : 0;
}

// ---- SshServer ----

SshServer::SshServer(const HostAddress &serverAddress, uint16_t serverPort)
    : BaseStreamServer(serverAddress, serverPort)
    , d_ptr(new SshServerPrivate)
{
}

SshServer::SshServer(uint16_t serverPort)
    : BaseStreamServer(serverPort)
    , d_ptr(new SshServerPrivate)
{
}

SshServer::~SshServer()
{
    delete d_ptr;
}

void SshServer::setHostKey(const PrivateKey &key)
{
    NG_D(SshServer);
    d->hostKey = key;
}

void SshServer::setAuthenticator(const shared_ptr<SshAuthenticator> &authenticator)
{
    NG_D(SshServer);
    d->auth = authenticator;
}

void SshServer::setApplication(const shared_ptr<SshApplication> &application)
{
    NG_D(SshServer);
    d->app = application;
}

void SshServer::setBanner(const string &banner)
{
    NG_D(SshServer);
    d->banner = banner;
}

void SshServer::setMaxAuthTries(int tries)
{
    NG_D(SshServer);
    d->maxAuthTries = tries;
}

void SshServer::setLoginTimeout(float seconds)
{
    NG_D(SshServer);
    d->loginTimeout = seconds;
}

shared_ptr<SocketLike> SshServer::serverCreate()
{
    return asSocketLike(Socket::createServer(serverAddress(), serverPort(), 0));
}

void SshServer::processRequest(shared_ptr<SocketLike> request)
{
    NG_D(SshServer);
    if (!d->hostKey.isValid() || !d->auth || !d->app) {
        request->close();
        return;
    }
    shared_ptr<SshConnectionPrivate> conn(new SshConnectionPrivate);
    conn->runServer(request, d->hostKey, d->auth, d->app, d->banner, d->maxAuthTries, d->loginTimeout);
}

// ---- SshClient ----

namespace {

bool failSshClientConnect(SshClientPrivate *d, const string &message, Socket::SocketError err)
{
    if (d->conn) {
        d->conn->stopClient();
        d->conn.reset();
    }
    shared_ptr<SshConnectionPrivate> tmp(new SshConnectionPrivate);
    tmp->setError(message, err);
    d->conn = tmp;
    return false;
}

bool startSshClientOnSocket(SshClientPrivate *d, shared_ptr<SocketLike> socket, const string &peerHostName)
{
    if (d->conn) {
        d->conn->stopClient();
        d->conn.reset();
    }
    shared_ptr<SshConnectionPrivate> conn(new SshConnectionPrivate);
    conn->peerHostName = peerHostName;
    conn->serverAliveInterval = d->serverAliveInterval;
    conn->serverAliveCountMax = d->serverAliveCountMax;
    conn->rekeyLimitBytes = d->rekeyLimitBytes;
    conn->rekeyIntervalSeconds = d->rekeyIntervalSeconds;
    conn->compressionEnabled = d->compressionEnabled;
    if (d->hostKeyVerifier) {
        conn->hostKeyVerifier = d->hostKeyVerifier;
    }
    if (!conn->startClient(socket, d->loginTimeout)) {
        d->conn = conn;
        return false;
    }
    d->conn = conn;
    return true;
}

}  // namespace

SshClient::SshClient()
    : d_ptr(new SshClientPrivate)
{
}

SshClient::~SshClient()
{
    close();
    delete d_ptr;
}

bool SshClient::connect(shared_ptr<SocketLike> socket)
{
    NG_D(SshClient);
    if (!socket) {
        return failSshClientConnect(d, "invalid socket", Socket::OperationError);
    }
    return startSshClientOnSocket(d, socket, string());
}

bool SshClient::connect(const string &host, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    NG_D(SshClient);
    Socket::SocketError err = Socket::NoError;
    Socket *s = Socket::createConnection(host, port, &err, dnsCache);
    if (!s) {
        return failSshClientConnect(d, "connect failed", err);
    }
    return startSshClientOnSocket(d, asSocketLike(shared_ptr<Socket>(s)), host);
}

bool SshClient::connect(const HostAddress &addr, uint16_t port)
{
    NG_D(SshClient);
    Socket::SocketError err = Socket::NoError;
    Socket *s = Socket::createConnection(addr, port, &err);
    if (!s) {
        return failSshClientConnect(d, "connect failed", err);
    }
    return startSshClientOnSocket(d, asSocketLike(shared_ptr<Socket>(s)), addr.toString());
}

void SshClient::setHostKeyVerifier(const shared_ptr<SshHostKeyVerifier> &verifier)
{
    NG_D(SshClient);
    d->hostKeyVerifier = verifier;
}

void SshClient::setLoginTimeout(float seconds)
{
    NG_D(SshClient);
    d->loginTimeout = seconds;
}

void SshClient::setServerAliveInterval(float seconds)
{
    NG_D(SshClient);
    d->serverAliveInterval = seconds;
    if (d->conn) {
        d->conn->serverAliveInterval = seconds;
    }
}

void SshClient::setServerAliveCountMax(int count)
{
    NG_D(SshClient);
    d->serverAliveCountMax = count;
    if (d->conn) {
        d->conn->serverAliveCountMax = count;
    }
}

void SshClient::setRekeyLimit(uint64_t bytes, float intervalSeconds)
{
    NG_D(SshClient);
    d->rekeyLimitBytes = bytes;
    d->rekeyIntervalSeconds = intervalSeconds;
    if (d->conn) {
        d->conn->rekeyLimitBytes = bytes;
        d->conn->rekeyIntervalSeconds = intervalSeconds;
    }
}

void SshClient::setCompression(bool enable)
{
    NG_D(SshClient);
    d->compressionEnabled = enable;
    if (d->conn) {
        d->conn->compressionEnabled = enable;
    }
}

bool SshClient::authenticate(const string &user, const string &password)
{
    NG_D(SshClient);
    if (!d->conn) {
        return false;
    }
    return d->conn->startClientAuth(user, password);
}

bool SshClient::authenticateWithPublicKey(const string &user, const PrivateKey &key)
{
    NG_D(SshClient);
    if (!d->conn) {
        return false;
    }
    return d->conn->startClientPublicKeyAuth(user, key);
}

shared_ptr<SshChannel> SshClient::openSessionChannel()
{
    NG_D(SshClient);
    if (!d->conn) {
        return nullptr;
    }
    if (!d->conn->sendConnectionService()) {
        return nullptr;
    }
    if (!d->conn->sendOpenChannel()) {
        return nullptr;
    }
    if (!d->conn->waitChannelOpen()) {
        return nullptr;
    }
    return shared_ptr<SshChannel>(new SshChannel(d->conn->pendingChannel));
}

void SshClient::close()
{
    NG_D(SshClient);
    if (d->conn) {
        d->conn->stopClient();
        d->conn.reset();
    }
}

bool SshClient::isConnected() const
{
    NG_D(const SshClient);
    return d->conn && d->conn->running;
}

Socket::SocketError SshClient::error() const
{
    NG_D(const SshClient);
    if (!d->conn) {
        return Socket::UnknownSocketError;
    }
    return d->conn->error;
}

string SshClient::errorString() const
{
    NG_D(const SshClient);
    if (!d->conn) {
        return string();
    }
    return d->conn->errorString;
}

}  // namespace qtng
