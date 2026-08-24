#include "qtng/private/ssh_p.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "qtng/utils/logging.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

namespace {

const uint32_t kLocalWindowSize = 2 * 1024 * 1024;
const uint32_t kMaxPacketSize = 32 * 1024;
const uint32_t kMaxPacketLength = 1024 * 1024;

const vector<string> kKexAlgorithms = {"curve25519-sha256", "curve25519-sha256@libssh.org"};
const vector<string> kHostKeyAlgorithms = {"rsa-sha2-256", "rsa-sha2-512", "ssh-rsa"};
const vector<string> kEncryptionAlgorithms = {"aes128-ctr", "aes256-ctr"};
const vector<string> kMacAlgorithms = {"hmac-sha2-256", "hmac-sha1"};
const vector<string> kCompressionAlgorithms = {"none"};

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
{
}

string SshChannelPrivate::recv(int32_t maxSize)
{
    string result;
    while (static_cast<int32_t>(result.size()) < maxSize) {
        if (incoming.isEmpty()) {
            // Return as soon as some data is available; only block on an
            // empty queue, and only while the channel is still open.
            if (!result.empty() || eofReceived || closed || remoteClosed) {
                break;
            }
            if (!incoming.notEmpty.tryWait()) {
                break;
            }
            continue;
        }
        string chunk = incoming.get();
        if (chunk.empty()) {
            continue;
        }
        result.append(chunk);
    }
    return result;
}

bool SshChannelPrivate::send(const string &data)
{
    if (closed || remoteClosed || eofSent) {
        return false;
    }
    if (data.empty()) {
        return true;
    }
    while (remoteWindow < data.size()) {
        windowAdjust.clear();
        if (!windowAdjust.tryWait(30000)) {
            return false;
        }
        if (closed || remoteClosed) {
            return false;
        }
    }
    remoteWindow -= static_cast<uint32_t>(data.size());
    return conn->sendChannelData(this, data);
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
    incoming.notEmpty.set();
}

void SshChannelPrivate::notifyEof()
{
    eofReceived = true;
    incoming.notEmpty.set();
}

void SshChannelPrivate::notifyRemoteClose()
{
    eofReceived = true;
    remoteClosed = true;
    incoming.notEmpty.set();
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
    , sendSeq(0)
    , recvSeq(0)
    , blockSize(8)
    , macLength(0)
    , sendEncrypted(false)
    , recvEncrypted(false)
    , maxAuthTries(6)
    , authTries(0)
    , authenticated(false)
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
    if (socket) {
        socket->abort();
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
    if (peerVersion.size() < 7 || peerVersion.substr(0, 7) != "SSH-2.0") {
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
    buf.putBytes(randomBytes(16));  // cookie
    buf.putNameList(kKexAlgorithms);
    buf.putNameList(kHostKeyAlgorithms);
    buf.putNameList(kEncryptionAlgorithms);
    buf.putNameList(kEncryptionAlgorithms);
    buf.putNameList(kMacAlgorithms);
    buf.putNameList(kMacAlgorithms);
    buf.putNameList(kCompressionAlgorithms);
    buf.putNameList(kCompressionAlgorithms);
    buf.putNameList(vector<string>());
    buf.putNameList(vector<string>());
    buf.putBoolean(false);
    buf.putUint32(0);
    myKexInitPayload = buf.raw();
    return sendPacket(myKexInitPayload);
}

bool SshConnectionPrivate::handleKexInitMessage(const string &payload)
{
    peerKexInitPayload = payload;
    maybeStartKex();
    return true;
}

void SshConnectionPrivate::maybeStartKex()
{
    if (kexStarted || kexDone || error != Socket::NoError) {
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
        clientEphPriv = k.privateKey;
        clientEphPub = k.publicKey;
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
    vector<string> myKex, myHk, myEnc, myMac;
    vector<string> peerKex, peerHk, peerEnc, peerMac;
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
    peer.getNameList(&peerKex);
    peer.getNameList(&peerHk);
    peer.getNameList(&peerEnc);
    peer.getNameList(&peerEnc);
    peer.getNameList(&peerMac);
    peer.getNameList(&peerMac);

    // RFC 4253 §7.1: the first algorithm on the *client's* list that also
    // appears on the server's list is chosen, so each side must search the
    // peer's (client's) proposal when it is the server.
    if (serverSide) {
        kexAlgo = pickAlgorithm(peerKex, myKex);
        hostKeyAlgo = pickAlgorithm(peerHk, myHk);
        cipherAlgo = pickAlgorithm(peerEnc, myEnc);
        macAlgo = pickAlgorithm(peerMac, myMac);
    } else {
        kexAlgo = pickAlgorithm(myKex, peerKex);
        hostKeyAlgo = pickAlgorithm(myHk, peerHk);
        cipherAlgo = pickAlgorithm(myEnc, peerEnc);
        macAlgo = pickAlgorithm(myMac, peerMac);
    }
    if (kexAlgo.empty() || hostKeyAlgo.empty() || cipherAlgo.empty() || macAlgo.empty()) {
        setError("no common algorithms");
        return false;
    }
    if (kexAlgo != "curve25519-sha256" && kexAlgo != "curve25519-sha256@libssh.org") {
        setError("unsupported kex algorithm: " + kexAlgo);
        return false;
    }
    if (hostKeyAlgo != "rsa-sha2-256" && hostKeyAlgo != "rsa-sha2-512" && hostKeyAlgo != "ssh-rsa") {
        setError("unsupported host key algorithm: " + hostKeyAlgo);
        return false;
    }
    if (cipherAlgo != "aes128-ctr" && cipherAlgo != "aes256-ctr") {
        setError("unsupported cipher: " + cipherAlgo);
        return false;
    }
    blockSize = 16;
    if (macAlgo == "hmac-sha2-256") {
        macLength = 32;
    } else if (macAlgo == "hmac-sha1") {
        macLength = 20;
    } else if (macAlgo == "hmac-sha2-512") {
        macLength = 64;
    } else {
        setError("unsupported MAC: " + macAlgo);
        return false;
    }
    return true;
}

void SshConnectionPrivate::computeExchangeHash(const string &serverHostKeyBlob)
{
    // RFC 4253 §8: V_C, V_S, I_C, I_S, K_S, Q_C, Q_S are all SSH "string"
    // values (4-byte length prefix) and K is an mpint.
    // OpenSSH stores the identification strings WITHOUT the trailing CRLF
    // (it trims it after sending, and strips it while reading), so V_C/V_S
    // must not include the CRLF to interoperate.
    const string vsStr = sshString(peerVersion);   // V_C
    const string vcStr = sshString(localVersion);  // V_S
    const string icStr = sshString(peerKexInitPayload);     // I_C
    const string isStr = sshString(myKexInitPayload);       // I_S
    const string ksStr = sshString(serverHostKeyBlob);      // K_S
    const string qcStr = sshString(clientEphPub);           // Q_C
    const string qsStr = sshString(serverEphPub);           // Q_S
    const string kMpint = sshMpintString(sharedSecret);  // K
    MessageDigest md(MessageDigest::Sha256);
    if (serverSide) {
        md.addData(vsStr.data(), static_cast<int>(vsStr.size()));  // V_C
        md.addData(vcStr.data(), static_cast<int>(vcStr.size()));  // V_S
        md.addData(icStr.data(), static_cast<int>(icStr.size()));  // I_C
        md.addData(isStr.data(), static_cast<int>(isStr.size()));  // I_S
    } else {
        md.addData(vcStr.data(), static_cast<int>(vcStr.size()));  // V_C
        md.addData(vsStr.data(), static_cast<int>(vsStr.size()));  // V_S
        md.addData(isStr.data(), static_cast<int>(isStr.size()));  // I_C
        md.addData(icStr.data(), static_cast<int>(icStr.size()));  // I_S
    }
    md.addData(ksStr.data(), static_cast<int>(ksStr.size()));  // K_S
    md.addData(qcStr.data(), static_cast<int>(qcStr.size()));  // Q_C
    md.addData(qsStr.data(), static_cast<int>(qsStr.size()));  // Q_S
    md.addData(kMpint.data(), static_cast<int>(kMpint.size()));  // K
    exchangeHash = md.result();
    if (sessionId.empty()) {
        sessionId = exchangeHash;
    }
    auto hx = [](const string &s) {
        string out;
        static const char *digits = "0123456789abcdef";
        for (char c : s) {
            out.push_back(digits[(static_cast<uint8_t>(c) >> 4) & 0xf]);
            out.push_back(digits[static_cast<uint8_t>(c) & 0xf]);
        }
        return out;
    };
    FILE *df = fopen("/tmp/hseg_dump.txt", "a");
    if (df) {
        fprintf(df, "VC %zu %s\n", vsStr.size(), hx(vsStr).c_str());
        fprintf(df, "VS %zu %s\n", vcStr.size(), hx(vcStr).c_str());
        fprintf(df, "IC %zu %s\n", icStr.size(), hx(icStr).c_str());
        fprintf(df, "IS %zu %s\n", isStr.size(), hx(isStr).c_str());
        fprintf(df, "KS %zu %s\n", ksStr.size(), hx(ksStr).c_str());
        fprintf(df, "QC %zu %s\n", qcStr.size(), hx(qcStr).c_str());
        fprintf(df, "QS %zu %s\n", qsStr.size(), hx(qsStr).c_str());
        fprintf(df, "KM %zu %s\n", kMpint.size(), hx(kMpint).c_str());
        fprintf(df, "H  %zu %s\n", exchangeHash.size(), hx(exchangeHash).c_str());
        MessageDigest md2(MessageDigest::Sha256);
        const string all = vsStr + vcStr + icStr + isStr + ksStr + qcStr + qsStr + kMpint;
        md2.addData(all.data(), static_cast<int>(all.size()));
        fprintf(df, "H2 %zu %s\n", md2.result().size(), hx(md2.result()).c_str());
        fprintf(df, "---\n");
        fclose(df);
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
    const size_t keyLen = (cipherAlgo == "aes256-ctr") ? 32 : 16;
    const size_t ivLen = 16;
    keyC2S = deriveKey('C', keyLen);
    keyS2C = deriveKey('D', keyLen);
    ivC2S = deriveKey('A', ivLen);
    ivS2C = deriveKey('B', ivLen);
    macC2S = deriveKey('E', static_cast<size_t>(macLength));
    macS2C = deriveKey('F', static_cast<size_t>(macLength));
    return true;
}

bool SshConnectionPrivate::handleKexEcdhInit(const string &payload)
{
    if (!serverSide || kexDone) {
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
    serverEphPriv = sk.privateKey;
    serverEphPub = sk.publicKey;
    sharedSecret = NoiseKey::dh(serverEphPriv, clientEphPub);
    if (sharedSecret.empty()) {
        setError("x25519 key agreement failed");
        return false;
    }
    const string ks = sshRsaKeyBlob(hostKey.publicKey());
    if (ks.empty()) {
        setError("host key is not an RSA key");
        return false;
    }
    computeExchangeHash(ks);
    const string signature = hostKey.sign(exchangeHash, rsaHashForAlgo(hostKeyAlgo));
    if (signature.empty()) {
        setError("host key signing failed");
        return false;
    }
    {
        auto hx = [](const string &s) {
            string out;
            static const char *digits = "0123456789abcdef";
            for (char c : s) {
                out.push_back(digits[(static_cast<uint8_t>(c) >> 4) & 0xf]);
                out.push_back(digits[static_cast<uint8_t>(c) & 0xf]);
            }
            return out;
        };
        FILE *df = fopen("/tmp/hseg_dump.txt", "a");
        if (df) {
            fprintf(df, "SP %zu %s\n", serverEphPriv.size(), hx(serverEphPriv).c_str());
            fprintf(df, "SIG %zu %s\n", signature.size(), hx(signature).c_str());
            fprintf(df, "ALG %s\n", hostKeyAlgo.c_str());
            fclose(df);
        }
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
    return true;
}

bool SshConnectionPrivate::handleKexEcdhReply(const string &payload)
{
    if (serverSide || kexDone) {
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
    if (!sshParseRsaKeyBlob(ks, &serverKey)) {
        setError("bad host key");
        return false;
    }
    if (hostKeyVerifier && !hostKeyVerifier->verify(socket ? socket->peerName() : string(), ks)) {
        setError("host key rejected by verifier", Socket::OperationError);
        return false;
    }
    computeExchangeHash(ks);
    SshBuffer sigBuf(sigMsg);
    string sigAlgo, sig;
    if (!sigBuf.getString(&sigAlgo) || !sigBuf.getString(&sig)) {
        setError("bad signature message");
        return false;
    }
    if (!serverKey.verify(exchangeHash, sig, rsaHashForAlgo(sigAlgo))) {
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
    kexDoneEvent.set();
    return true;
}

bool SshConnectionPrivate::handleNewKeysMessage()
{
    if (recvEncrypted) {
        return true;
    }
    Cipher::Algorithm algo = (cipherAlgo == "aes256-ctr") ? Cipher::AES256 : Cipher::AES128;
    recvCipher.reset(new Cipher(algo, Cipher::CTR, Cipher::Decrypt));
    const string &k = serverSide ? keyC2S : keyS2C;
    const string &iv = serverSide ? ivC2S : ivS2C;
    // setKey() cannot succeed until the IV is set; check the final call only.
    recvCipher->setKey(k);
    if (!recvCipher->setInitialVector(iv)) {
        setError("receive cipher init failed");
        return false;
    }
    recvMacKey = serverSide ? macC2S : macS2C;
    recvEncrypted = true;
    return true;
}

bool SshConnectionPrivate::sendNewKeys()
{
    SshBuffer buf;
    buf.putByte(SSH_MSG_NEWKEYS);
    if (!sendPacket(buf.raw())) {
        return false;
    }
    if (!sendEncrypted) {
        Cipher::Algorithm algo = (cipherAlgo == "aes256-ctr") ? Cipher::AES256 : Cipher::AES128;
        sendCipher.reset(new Cipher(algo, Cipher::CTR, Cipher::Encrypt));
        const string &k = serverSide ? keyS2C : keyC2S;
        const string &iv = serverSide ? ivS2C : ivC2S;
        sendCipher->setKey(k);
        if (!sendCipher->setInitialVector(iv)) {
            setError("send cipher init failed");
            return false;
        }
        sendMacKey = serverSide ? macS2C : macC2S;
        sendEncrypted = true;
    }
    return true;
}

bool SshConnectionPrivate::sendPacket(const string &payload)
{
    ScopedLock<Lock> lock(sendLock);
    if (!lock.isSuccess()) {
        return false;
    }
    if (!socket || !socket->isValid()) {
        return false;
    }
    // RFC 4253 §6: packet_length must be a multiple of the cipher block size
    // or 8, whichever is larger. blockSize is 8 before key exchange and 16
    // for aes128-ctr/aes256-ctr after negotiation.
    const uint32_t align = blockSize;
    uint32_t pad = 4;
    while ((4 + 1 + static_cast<uint32_t>(payload.size()) + pad) % align != 0) {
        ++pad;
    }
    string plaintext;
    const size_t plainLenPos = plaintext.size();
    plaintext.append(4, '\0');
    ngToBigEndian(1 + static_cast<uint32_t>(payload.size()) + pad, &plaintext[plainLenPos]);
    plaintext.push_back(static_cast<char>(pad));
    plaintext.append(payload);
    plaintext += randomBytes(pad);
    string mac;
    if (sendEncrypted) {
        string seqStr;
        const size_t seqPos = seqStr.size();
        seqStr.append(4, '\0');
        ngToBigEndian(sendSeq, &seqStr[seqPos]);
        const string data = seqStr + plaintext;
        if (macAlgo == "hmac-sha2-256") {
            mac = qtng::hmac(MessageDigest::Sha256, sendMacKey, data);
        } else if (macAlgo == "hmac-sha1") {
            mac = qtng::hmac(MessageDigest::Sha1, sendMacKey, data);
        } else {
            mac = qtng::hmac(MessageDigest::Sha512, sendMacKey, data);
        }
    }
    string encrypted = sendEncrypted ? sendCipher->addData(plaintext) : plaintext;
    if (encrypted.size() != plaintext.size()) {
        return false;
    }
    ++sendSeq;
    const string out = encrypted + mac;
    return socket->sendall(out) == static_cast<int32_t>(out.size());
}

bool SshConnectionPrivate::recvPacket(string *payload)
{
    if (!socket || !socket->isValid()) {
        return false;
    }
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
        payload->assign(body.data() + 1, packetLen - padLen - 1);
        ++recvSeq;
        return true;
    }
    const uint32_t block = blockSize;  // RFC 4253 §6: packet_length multiple of max(block size, 8)
    const string encBlock = socket->recvall(static_cast<int32_t>(block));
    if (encBlock.size() < static_cast<size_t>(block)) {
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
        const size_t seqPos = seqStr.size();
        seqStr.append(4, '\0');
        ngToBigEndian(recvSeq, &seqStr[seqPos]);
        const string data = seqStr + plaintext;
        string expected;
        if (macAlgo == "hmac-sha2-256") {
            expected = qtng::hmac(MessageDigest::Sha256, recvMacKey, data);
        } else if (macAlgo == "hmac-sha1") {
            expected = qtng::hmac(MessageDigest::Sha1, recvMacKey, data);
        } else {
            expected = qtng::hmac(MessageDigest::Sha512, recvMacKey, data);
        }
        // The MAC is appended to the ciphertext unencrypted; read it as-is.
        const string actual = socket->recvall(static_cast<int32_t>(macLength));
        if (actual.size() < static_cast<size_t>(macLength)) {
            return false;
        }
        if (expected != actual) {
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
    payload->assign(plaintext.data() + 5, packetLen - padLen - 1);
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
    for (const auto &pair : channels) {
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
                authOk = true;
                authFinished = true;
                authEvent.set();
            }
            break;
        case SSH_MSG_USERAUTH_FAILURE:
            if (!serverSide) {
                authOk = false;
                authFinished = true;
                authEvent.set();
            }
            break;
        case SSH_MSG_USERAUTH_BANNER:
            break;
        case SSH_MSG_USERAUTH_PK_OK:
            if (!serverSide) {
                authOk = true;
                authFinished = true;
                authEvent.set();
            }
            break;
        case SSH_MSG_GLOBAL_REQUEST:
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
    auto it = channels.find(recipient);
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
    auto it = channels.find(id);
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
    auto it = channels.find(id);
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
    auto it = channels.find(id);
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
    auto it = channels.find(id);
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
    auto it = channels.find(id);
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
            ch->termSize.columns = cols;
            ch->termSize.rows = rows;
            if (ch->callback) {
                ch->callback->onResize(ch->termSize);
            }
        }
    } else if (requestType == "window-change") {
        uint32_t cols, rows, width, height;
        if (!buf.getUint32(&cols) || !buf.getUint32(&rows) || !buf.getUint32(&width) || !buf.getUint32(&height)) {
            success = false;
        } else {
            ch->termSize.columns = cols;
            ch->termSize.rows = rows;
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
    } else if (requestType == "shell" || requestType == "env" || requestType == "exec") {
        // accepted; no extra action
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
    if (!banner.empty()) {
        SshBuffer b;
        b.putByte(SSH_MSG_USERAUTH_BANNER);
        b.putString(banner);
        b.putString("en");
        sendPacket(b.raw());
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
    return sendPacket(buf.raw());
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
    if (!sshParseRsaKeyBlob(blob, &key)) {
        return false;
    }
    SshBuffer sigBuf(signature);
    string sigAlgo, sig;
    if (!sigBuf.getString(&sigAlgo) || !sigBuf.getString(&sig)) {
        return false;
    }
    SshBuffer dataBuf;
    dataBuf.putBytes(sessionId);
    dataBuf.putByte(SSH_MSG_USERAUTH_REQUEST);
    dataBuf.putString(user);
    dataBuf.putString("ssh-connection");
    dataBuf.putString("publickey");
    dataBuf.putBoolean(true);
    dataBuf.putString(algo);
    dataBuf.putString(blob);
    return key.verify(dataBuf.raw(), sig, rsaHashForAlgo(algo));
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

bool SshConnectionPrivate::startClientAuth(const string &user, const string &password)
{
    clientUser = user;
    if (!sendUserauthService()) {
        return false;
    }
    authFinished = false;
    authEvent.clear();
    SshBuffer buf;
    buf.putByte(SSH_MSG_USERAUTH_REQUEST);
    buf.putString(user);
    buf.putString("ssh-connection");
    buf.putString("password");
    buf.putBoolean(false);
    buf.putString(password);
    if (!sendPacket(buf.raw())) {
        return false;
    }
    return waitAuthReply();
}

bool SshConnectionPrivate::startClientPublicKeyAuth(const string &user, const PrivateKey &key)
{
    clientUser = user;
    if (!sendUserauthService()) {
        return false;
    }
    clientKeyBlob = sshRsaKeyBlob(key.publicKey());
    clientKeyAlgo = "rsa-sha2-256";
    if (clientKeyBlob.empty()) {
        return false;
    }
    // probe
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
    if (!waitAuthReply()) {
        return false;
    }
    if (!authOk) {
        return false;  // server did not send PK_OK
    }
    // signed request
    authFinished = false;
    authEvent.clear();
    SshBuffer dataBuf;
    dataBuf.putBytes(sessionId);
    dataBuf.putByte(SSH_MSG_USERAUTH_REQUEST);
    dataBuf.putString(user);
    dataBuf.putString("ssh-connection");
    dataBuf.putString("publickey");
    dataBuf.putBoolean(true);
    dataBuf.putString(clientKeyAlgo);
    dataBuf.putString(clientKeyBlob);
    PrivateKey signingKey = key;
    const string signature = signingKey.sign(dataBuf.raw(), rsaHashForAlgo(clientKeyAlgo));
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
    return waitAuthReply();
}

bool SshConnectionPrivate::sendServiceAcceptRequest()
{
    serviceAcceptOk = false;
    serviceAcceptEvent.clear();
    SshBuffer buf;
    buf.putByte(SSH_MSG_SERVICE_REQUEST);
    buf.putString("ssh-connection");
    if (!sendPacket(buf.raw())) {
        return false;
    }
    return serviceAcceptEvent.tryWait() && serviceAcceptOk;
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
    while (!authFinished) {
        if (!authEvent.tryWait()) {
            return false;
        }
    }
    return authOk;
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

// ---- SshChannel ----

SshChannel::SshChannel(const shared_ptr<SshChannelPrivate> &d)
    : d(d)
{
}

SshChannel::~SshChannel()
{
}

int32_t SshChannel::send(const string &data)
{
    if (!d->send(data)) {
        return -1;
    }
    return static_cast<int32_t>(data.size());
}

string SshChannel::recv(int32_t maxSize)
{
    return d->recv(maxSize);
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

bool SshChannel::requestWindowChange(uint32_t cols, uint32_t rows)
{
    SshBuffer extra;
    extra.putUint32(cols);
    extra.putUint32(rows);
    extra.putUint32(0);
    extra.putUint32(0);
    return d->conn->sendChannelRequest(d.get(), "window-change", extra, true) && d->conn->waitRequestReply();
}

bool SshChannel::sendSignal(const string &signalName)
{
    SshBuffer extra;
    extra.putString(signalName);
    return d->conn->sendChannelRequest(d.get(), "signal", extra, true) && d->conn->waitRequestReply();
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

SshClient::SshClient()
    : d_ptr(new SshClientPrivate)
{
}

SshClient::~SshClient()
{
    close();
    delete d_ptr;
}

bool SshClient::connect(const string &host, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    NG_D(SshClient);
    if (d->conn) {
        d->conn->stopClient();
        d->conn.reset();
    }
    Socket::SocketError err = Socket::NoError;
    Socket *s = Socket::createConnection(host, port, &err, dnsCache);
    if (!s) {
        shared_ptr<SshConnectionPrivate> tmp(new SshConnectionPrivate);
        tmp->setError("connect failed", err);
        d->conn = tmp;
        return false;
    }
    shared_ptr<SshConnectionPrivate> conn(new SshConnectionPrivate);
    if (d->hostKeyVerifier) {
        conn->hostKeyVerifier = d->hostKeyVerifier;
    }
    if (!conn->startClient(asSocketLike(shared_ptr<Socket>(s)), d->loginTimeout)) {
        d->conn = conn;
        return false;
    }
    d->conn = conn;
    return true;
}

bool SshClient::connect(const HostAddress &addr, uint16_t port)
{
    NG_D(SshClient);
    if (d->conn) {
        d->conn->stopClient();
        d->conn.reset();
    }
    Socket::SocketError err = Socket::NoError;
    Socket *s = Socket::createConnection(addr, port, &err);
    if (!s) {
        shared_ptr<SshConnectionPrivate> tmp(new SshConnectionPrivate);
        tmp->setError("connect failed", err);
        d->conn = tmp;
        return false;
    }
    shared_ptr<SshConnectionPrivate> conn(new SshConnectionPrivate);
    if (d->hostKeyVerifier) {
        conn->hostKeyVerifier = d->hostKeyVerifier;
    }
    if (!conn->startClient(asSocketLike(shared_ptr<Socket>(s)), d->loginTimeout)) {
        d->conn = conn;
        return false;
    }
    d->conn = conn;
    return true;
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
    if (!d->conn->sendServiceAcceptRequest()) {
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
