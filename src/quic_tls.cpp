#include "qtng/private/quic_tls.h"

#include <cstring>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "qtng/aead.h"
#include "qtng/md.h"
#include "qtng/private/crypto_p.h"
#include "qtng/random.h"

using namespace std;

namespace qtng {

namespace {

const uint16_t kExtServerName = 0x0000;
const uint16_t kExtSupportedGroups = 0x000a;
const uint16_t kExtSignatureAlgorithms = 0x000d;
const uint16_t kExtAlpn = 0x0010;
const uint16_t kExtSupportedVersions = 0x002b;
const uint16_t kExtKeyShare = 0x0033;
const uint16_t kExtQuicTp = 0x0039;

const uint16_t kGroupX25519 = 0x001d;
const uint16_t kSigEcdsaSecp256r1Sha256 = 0x0403;
const uint16_t kSigRsaPssRsaeSha256 = 0x0804;

void writeUint16(string *o, uint16_t v)
{
    o->push_back(static_cast<char>((v >> 8) & 0xff));
    o->push_back(static_cast<char>(v & 0xff));
}

void writeUint24(string *o, uint32_t v)
{
    o->push_back(static_cast<char>((v >> 16) & 0xff));
    o->push_back(static_cast<char>((v >> 8) & 0xff));
    o->push_back(static_cast<char>(v & 0xff));
}

void writeUint32(string *o, uint32_t v)
{
    o->push_back(static_cast<char>((v >> 24) & 0xff));
    o->push_back(static_cast<char>((v >> 16) & 0xff));
    o->push_back(static_cast<char>((v >> 8) & 0xff));
    o->push_back(static_cast<char>(v & 0xff));
}

bool readUint16(const char *d, size_t n, size_t *off, uint16_t *v)
{
    if (*off + 2 > n) {
        return false;
    }
    *v = (static_cast<uint16_t>(static_cast<unsigned char>(d[*off])) << 8)
            | static_cast<uint16_t>(static_cast<unsigned char>(d[*off + 1]));
    *off += 2;
    return true;
}

bool readUint24(const char *d, size_t n, size_t *off, uint32_t *v)
{
    if (*off + 3 > n) {
        return false;
    }
    *v = (static_cast<uint32_t>(static_cast<unsigned char>(d[*off])) << 16)
            | (static_cast<uint32_t>(static_cast<unsigned char>(d[*off + 1])) << 8)
            | static_cast<uint32_t>(static_cast<unsigned char>(d[*off + 2]));
    *off += 3;
    return true;
}

string handshakeRecord(uint8_t type, const string &body)
{
    string m;
    m.push_back(static_cast<char>(type));
    writeUint24(&m, static_cast<uint32_t>(body.size()));
    m.append(body);
    return m;
}

bool generateX25519(string *priv, string *pub)
{
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!pctx) {
        return false;
    }
    EVP_PKEY *pkey = nullptr;
    if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    EVP_PKEY_CTX_free(pctx);
    priv->resize(32);
    pub->resize(32);
    size_t privLen = 32, pubLen = 32;
    bool ok = EVP_PKEY_get_raw_private_key(pkey, reinterpret_cast<unsigned char *>(&(*priv)[0]), &privLen) == 1
            && privLen == 32
            && EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<unsigned char *>(&(*pub)[0]), &pubLen) == 1
            && pubLen == 32;
    EVP_PKEY_free(pkey);
    return ok;
}

string x25519Shared(const string &priv, const string &peerPub)
{
    if (priv.size() != 32 || peerPub.size() != 32) {
        return string();
    }
    EVP_PKEY *ours = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                   reinterpret_cast<const unsigned char *>(priv.data()), 32);
    EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                    reinterpret_cast<const unsigned char *>(peerPub.data()), 32);
    if (!ours || !theirs) {
        EVP_PKEY_free(ours);
        EVP_PKEY_free(theirs);
        return string();
    }
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(ours, nullptr);
    string shared(32, '\0');
    size_t len = 32;
    bool ok = ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, theirs) > 0
            && EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(&shared[0]), &len) > 0 && len == 32;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(ours);
    EVP_PKEY_free(theirs);
    return ok ? shared : string();
}

string deriveSecret(const string &secret, const string &label, const string &messages, size_t outLen)
{
    const string ctx = MessageDigest::digest(messages, MessageDigest::Sha256);
    return hkdfExpandLabel(MessageDigest::Sha256, secret, label, ctx, outLen);
}

string hmacSha256(const string &key, const string &data)
{
    unsigned int len = 0;
    unsigned char out[EVP_MAX_MD_SIZE];
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char *>(data.data()), data.size(), out, &len)) {
        return string();
    }
    return string(reinterpret_cast<char *>(out), len);
}

string rsaPssSign(EVP_PKEY *pkey, const string &data)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return string();
    }
    EVP_PKEY_CTX *pctx = nullptr;
    string sig;
    bool ok = false;
    do {
        if (EVP_DigestSignInit(ctx, &pctx, EVP_sha256(), nullptr, pkey) != 1) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) <= 0) {
            break;
        }
        size_t sigLen = 0;
        if (EVP_DigestSign(ctx, nullptr, &sigLen, reinterpret_cast<const unsigned char *>(data.data()), data.size())
            != 1) {
            break;
        }
        sig.resize(sigLen);
        if (EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(&sig[0]), &sigLen,
                           reinterpret_cast<const unsigned char *>(data.data()), data.size())
            != 1) {
            break;
        }
        sig.resize(sigLen);
        ok = true;
    } while (false);
    EVP_MD_CTX_free(ctx);
    return ok ? sig : string();
}

bool rsaPssVerify(EVP_PKEY *pkey, const string &data, const string &sig)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return false;
    }
    EVP_PKEY_CTX *pctx = nullptr;
    bool ok = false;
    do {
        if (EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), nullptr, pkey) != 1) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) <= 0) {
            break;
        }
        ok = EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(sig.data()), sig.size(),
                              reinterpret_cast<const unsigned char *>(data.data()), data.size())
                == 1;
    } while (false);
    EVP_MD_CTX_free(ctx);
    return ok;
}

string ecdsaSign(EVP_PKEY *pkey, const string &data)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return string();
    }
    size_t sigLen = 0;
    string sig;
    bool ok = EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1
            && EVP_DigestSign(ctx, nullptr, &sigLen, reinterpret_cast<const unsigned char *>(data.data()), data.size())
                    == 1;
    if (ok) {
        sig.resize(sigLen);
        ok = EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(&sig[0]), &sigLen,
                            reinterpret_cast<const unsigned char *>(data.data()), data.size())
                == 1;
        sig.resize(sigLen);
    }
    EVP_MD_CTX_free(ctx);
    return ok ? sig : string();
}

bool ecdsaVerify(EVP_PKEY *pkey, const string &data, const string &sig)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return false;
    }
    bool ok = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1
            && EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(sig.data()), sig.size(),
                                reinterpret_cast<const unsigned char *>(data.data()), data.size())
                    == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}

}  // namespace

QuicTlsHandshake::QuicTlsHandshake(Role role, const QuicTransportParams &params)
    : m_role(role)
    , m_localParams(params)
{
}

QuicTlsHandshake::~QuicTlsHandshake() {}

void QuicTlsHandshake::setServerName(const string &sni)
{
    m_sni = sni;
}

void QuicTlsHandshake::setAlpn(const vector<string> &alpn)
{
    m_alpn = alpn;
}

void QuicTlsHandshake::setCredentials(const PrivateKey &key, const Certificate &cert)
{
    m_key = key;
    m_cert = cert;
}

void QuicTlsHandshake::setVerifyPeer(bool verify)
{
    m_verifyPeer = verify;
}

void QuicTlsHandshake::appendTranscript(const string &handshakeMsg)
{
    m_transcript.append(handshakeMsg);
}

string QuicTlsHandshake::transcriptHash() const
{
    return MessageDigest::digest(m_transcript, MessageDigest::Sha256);
}

string QuicTlsHandshake::encodeTransportParams(bool isClient) const
{
    string body;
    auto add = [&](uint64_t id, const string &val) {
        quicEncodeVarint(id, &body);
        quicEncodeVarint(val.size(), &body);
        body.append(val);
    };
    auto addVar = [&](uint64_t id, uint64_t v) {
        string enc;
        quicEncodeVarint(v, &enc);
        add(id, enc);
    };
    addVar(0x01, m_localParams.maxIdleTimeoutMs);
    addVar(0x03, m_localParams.maxUdpPayloadSize);
    addVar(0x04, m_localParams.initialMaxData);
    addVar(0x05, m_localParams.initialMaxStreamDataBidiLocal);
    addVar(0x06, m_localParams.initialMaxStreamDataBidiRemote);
    addVar(0x07, m_localParams.initialMaxStreamDataUni);
    addVar(0x08, m_localParams.initialMaxStreamsBidi);
    addVar(0x09, m_localParams.initialMaxStreamsUni);
    if (m_localParams.disableActiveMigration) {
        add(0x0c, string());
    }
    if (isClient) {
        // initial_source_connection_id
        add(0x0f, m_localParams.initialSourceConnectionId);
    } else {
        add(0x0f, m_localParams.initialSourceConnectionId);
        if (!m_localParams.originalDestinationConnectionId.empty()) {
            add(0x00, m_localParams.originalDestinationConnectionId);
        }
    }
    return body;
}

bool QuicTlsHandshake::decodeTransportParams(const string &data, QuicTransportParams *out) const
{
    size_t off = 0;
    while (off < data.size()) {
        size_t c = 0;
        uint64_t id = 0, len = 0;
        if (!quicDecodeVarint(data.data() + off, data.size() - off, &c, &id)) {
            return false;
        }
        off += c;
        if (!quicDecodeVarint(data.data() + off, data.size() - off, &c, &len)) {
            return false;
        }
        off += c;
        if (off + len > data.size()) {
            return false;
        }
        string val(data.data() + off, static_cast<size_t>(len));
        off += static_cast<size_t>(len);
        auto asVar = [&](uint64_t *v) {
            size_t cc = 0;
            return quicDecodeVarint(val.data(), val.size(), &cc, v) && cc == val.size();
        };
        switch (id) {
        case 0x00:
            out->originalDestinationConnectionId = val;
            break;
        case 0x01:
            asVar(&out->maxIdleTimeoutMs);
            break;
        case 0x03:
            asVar(&out->maxUdpPayloadSize);
            break;
        case 0x04:
            asVar(&out->initialMaxData);
            break;
        case 0x05:
            asVar(&out->initialMaxStreamDataBidiLocal);
            break;
        case 0x06:
            asVar(&out->initialMaxStreamDataBidiRemote);
            break;
        case 0x07:
            asVar(&out->initialMaxStreamDataUni);
            break;
        case 0x08:
            asVar(&out->initialMaxStreamsBidi);
            break;
        case 0x09:
            asVar(&out->initialMaxStreamsUni);
            break;
        case 0x0c:
            out->disableActiveMigration = true;
            break;
        case 0x0f:
            out->initialSourceConnectionId = val;
            break;
        default:
            break;
        }
    }
    return true;
}

string QuicTlsHandshake::buildClientHello()
{
    m_clientRandom = randomBytes(32);
    generateX25519(&m_clientKeySharePriv, &m_clientKeySharePub);

    string legacySessionId;  // empty for QUIC
    string cipherSuites;
    writeUint16(&cipherSuites, 0x1301);  // TLS_AES_128_GCM_SHA256

    string extensions;
    // supported_versions
    {
        string body;
        body.push_back(2);
        writeUint16(&body, 0x0304);
        writeUint16(&extensions, kExtSupportedVersions);
        writeUint16(&extensions, static_cast<uint16_t>(body.size()));
        extensions.append(body);
    }
    // supported_groups
    {
        string body;
        writeUint16(&body, 2);
        writeUint16(&body, kGroupX25519);
        writeUint16(&extensions, kExtSupportedGroups);
        writeUint16(&extensions, static_cast<uint16_t>(body.size()));
        extensions.append(body);
    }
    // key_share
    {
        string body;
        writeUint16(&body, static_cast<uint16_t>(2 + 2 + m_clientKeySharePub.size()));
        writeUint16(&body, kGroupX25519);
        writeUint16(&body, static_cast<uint16_t>(m_clientKeySharePub.size()));
        body.append(m_clientKeySharePub);
        writeUint16(&extensions, kExtKeyShare);
        writeUint16(&extensions, static_cast<uint16_t>(body.size()));
        extensions.append(body);
    }
    // signature_algorithms
    {
        string body;
        writeUint16(&body, 4);
        writeUint16(&body, kSigRsaPssRsaeSha256);
        writeUint16(&body, kSigEcdsaSecp256r1Sha256);
        writeUint16(&extensions, kExtSignatureAlgorithms);
        writeUint16(&extensions, static_cast<uint16_t>(body.size()));
        extensions.append(body);
    }
    if (!m_sni.empty()) {
        string body;
        writeUint16(&body, static_cast<uint16_t>(m_sni.size() + 3));
        body.push_back(0);  // host_name
        writeUint16(&body, static_cast<uint16_t>(m_sni.size()));
        body.append(m_sni);
        writeUint16(&extensions, kExtServerName);
        writeUint16(&extensions, static_cast<uint16_t>(body.size()));
        extensions.append(body);
    }
    if (!m_alpn.empty()) {
        string list;
        for (const string &p : m_alpn) {
            list.push_back(static_cast<char>(p.size()));
            list.append(p);
        }
        string body;
        writeUint16(&body, static_cast<uint16_t>(list.size()));
        body.append(list);
        writeUint16(&extensions, kExtAlpn);
        writeUint16(&extensions, static_cast<uint16_t>(body.size()));
        extensions.append(body);
    }
    {
        string tp = encodeTransportParams(true);
        writeUint16(&extensions, kExtQuicTp);
        writeUint16(&extensions, static_cast<uint16_t>(tp.size()));
        extensions.append(tp);
    }

    string body;
    writeUint16(&body, 0x0303);  // legacy_version
    body.append(m_clientRandom);
    body.push_back(static_cast<char>(legacySessionId.size()));
    body.append(legacySessionId);
    writeUint16(&body, static_cast<uint16_t>(cipherSuites.size()));
    body.append(cipherSuites);
    body.push_back(1);
    body.push_back(0);  // null compression
    writeUint16(&body, static_cast<uint16_t>(extensions.size()));
    body.append(extensions);
    return handshakeRecord(1, body);  // client_hello
}

string QuicTlsHandshake::buildServerHello()
{
    m_serverRandom = randomBytes(32);
    generateX25519(&m_serverKeySharePriv, &m_serverKeySharePub);

    string extensions;
    {
        string body;
        writeUint16(&body, 0x0304);
        writeUint16(&extensions, kExtSupportedVersions);
        writeUint16(&extensions, static_cast<uint16_t>(body.size()));
        extensions.append(body);
    }
    {
        string body;
        writeUint16(&body, kGroupX25519);
        writeUint16(&body, static_cast<uint16_t>(m_serverKeySharePub.size()));
        body.append(m_serverKeySharePub);
        writeUint16(&extensions, kExtKeyShare);
        writeUint16(&extensions, static_cast<uint16_t>(body.size()));
        extensions.append(body);
    }

    string body;
    writeUint16(&body, 0x0303);
    body.append(m_serverRandom);
    body.push_back(0);  // legacy_session_id_echo empty
    writeUint16(&body, 0x1301);
    body.push_back(0);  // null compression
    writeUint16(&body, static_cast<uint16_t>(extensions.size()));
    body.append(extensions);
    return handshakeRecord(2, body);
}

string QuicTlsHandshake::buildEncryptedExtensions()
{
    string extensions;
    if (!m_alpn.empty()) {
        m_negotiatedAlpn = m_alpn.front();
        string list;
        list.push_back(static_cast<char>(m_negotiatedAlpn.size()));
        list.append(m_negotiatedAlpn);
        string body;
        writeUint16(&body, static_cast<uint16_t>(list.size()));
        body.append(list);
        writeUint16(&extensions, kExtAlpn);
        writeUint16(&extensions, static_cast<uint16_t>(body.size()));
        extensions.append(body);
    }
    {
        string tp = encodeTransportParams(false);
        writeUint16(&extensions, kExtQuicTp);
        writeUint16(&extensions, static_cast<uint16_t>(tp.size()));
        extensions.append(tp);
    }
    string body;
    writeUint16(&body, static_cast<uint16_t>(extensions.size()));
    body.append(extensions);
    return handshakeRecord(8, body);
}

string QuicTlsHandshake::buildCertificate()
{
    string certDer = m_cert.save(Ssl::Der);
    string certList;
    writeUint24(&certList, static_cast<uint32_t>(certDer.size()));
    certList.append(certDer);
    writeUint16(&certList, 0);  // extensions
    string body;
    body.push_back(0);  // certificate_request_context
    writeUint24(&body, static_cast<uint32_t>(certList.size()));
    body.append(certList);
    return handshakeRecord(11, body);
}

string QuicTlsHandshake::buildCertificateVerify()
{
    // RFC 8446: 64 spaces + context string + 0x00 + transcript hash
    string toSign(64, ' ');
    toSign += "TLS 1.3, server CertificateVerify";
    toSign.push_back('\0');
    toSign += transcriptHash();

    EVP_PKEY *pkey = static_cast<EVP_PKEY *>(m_key.handle());
    string sig;
    uint16_t scheme = kSigRsaPssRsaeSha256;
    if (m_key.algorithm() == PublicKey::Ec) {
        scheme = kSigEcdsaSecp256r1Sha256;
        sig = ecdsaSign(pkey, toSign);
    } else {
        sig = rsaPssSign(pkey, toSign);
    }
    string body;
    writeUint16(&body, scheme);
    writeUint16(&body, static_cast<uint16_t>(sig.size()));
    body.append(sig);
    return handshakeRecord(15, body);
}

string QuicTlsHandshake::buildFinished(bool isClient)
{
    const string &base = isClient ? m_clientHandshakeTrafficSecret : m_serverHandshakeTrafficSecret;
    const string finishedKey = hkdfExpandLabel(MessageDigest::Sha256, base, "finished", string(), 32);
    const string verifyData = hmacSha256(finishedKey, transcriptHash());
    return handshakeRecord(20, verifyData);
}

void QuicTlsHandshake::deriveHandshakeSecrets(const string &sharedSecret)
{
    const string zeros(32, '\0');
    const string earlySecret = hkdfExtract(MessageDigest::Sha256, string(), zeros);
    const string derived = deriveSecret(earlySecret, "derived", string(), 32);
    m_handshakeSecret = hkdfExtract(MessageDigest::Sha256, derived, sharedSecret);
    m_clientHandshakeTrafficSecret = deriveSecret(m_handshakeSecret, "c hs traffic", m_transcript, 32);
    m_serverHandshakeTrafficSecret = deriveSecret(m_handshakeSecret, "s hs traffic", m_transcript, 32);
    m_secrets.clientHandshakeSecret = m_clientHandshakeTrafficSecret;
    m_secrets.serverHandshakeSecret = m_serverHandshakeTrafficSecret;
    m_secrets.handshakeReady = true;
}

void QuicTlsHandshake::deriveApplicationSecrets()
{
    const string derived = deriveSecret(m_handshakeSecret, "derived", string(), 32);
    const string zeros(32, '\0');
    m_masterSecret = hkdfExtract(MessageDigest::Sha256, derived, zeros);
    m_secrets.clientAppSecret = deriveSecret(m_masterSecret, "c ap traffic", m_transcript, 32);
    m_secrets.serverAppSecret = deriveSecret(m_masterSecret, "s ap traffic", m_transcript, 32);
    m_secrets.appReady = true;
}

bool QuicTlsHandshake::startClientHello(string *error)
{
    if (m_role != Client) {
        if (error) {
            *error = "not a client";
        }
        return false;
    }
    const string ch = buildClientHello();
    appendTranscript(ch);
    m_cryptoOut.append(ch);
    return true;
}

string QuicTlsHandshake::takeCryptoToSend()
{
    string out = m_cryptoOut;
    m_cryptoOut.clear();
    return out;
}

bool QuicTlsHandshake::isHandshakeComplete() const
{
    return m_complete;
}

bool QuicTlsHandshake::isConnected() const
{
    return m_secrets.appReady;
}

bool QuicTlsHandshake::feedCryptoData(const string &data, string *error)
{
    m_cryptoIn.append(data);
    while (m_cryptoIn.size() >= 4) {
        const uint8_t type = static_cast<uint8_t>(m_cryptoIn[0]);
        const uint32_t len = (static_cast<uint32_t>(static_cast<unsigned char>(m_cryptoIn[1])) << 16)
                | (static_cast<uint32_t>(static_cast<unsigned char>(m_cryptoIn[2])) << 8)
                | static_cast<uint32_t>(static_cast<unsigned char>(m_cryptoIn[3]));
        if (m_cryptoIn.size() < 4 + len) {
            break;
        }
        string msg = m_cryptoIn.substr(0, 4 + len);
        m_cryptoIn.erase(0, 4 + len);
        if (!processHandshakeMessage(msg, error)) {
            return false;
        }
        (void) type;
    }
    return true;
}

bool QuicTlsHandshake::processHandshakeMessage(const string &msg, string *error)
{
    if (msg.size() < 4) {
        return false;
    }
    const uint8_t type = static_cast<uint8_t>(msg[0]);
    if (m_role == Server && type == 1) {
        return handleClientHello(msg, error);
    }
    if (m_role == Client && type == 2) {
        return handleServerHello(msg, error);
    }
    if (type == 8) {
        return handleEncryptedExtensions(msg, error);
    }
    if (type == 11) {
        return handleCertificate(msg, error);
    }
    if (type == 15) {
        return handleCertificateVerify(msg, error);
    }
    if (type == 20) {
        return handleFinished(msg, error);
    }
    if (error) {
        *error = "unexpected handshake message";
    }
    return false;
}

bool QuicTlsHandshake::handleClientHello(const string &msg, string *error)
{
    appendTranscript(msg);
    // Parse key_share and transport params / alpn (minimal)
    size_t off = 4;
    if (off + 2 + 32 + 1 > msg.size()) {
        if (error) {
            *error = "short client hello";
        }
        return false;
    }
    off += 2;  // legacy version
    m_clientRandom = msg.substr(off, 32);
    off += 32;
    const size_t sidLen = static_cast<unsigned char>(msg[off++]);
    off += sidLen;
    if (off + 2 > msg.size()) {
        return false;
    }
    uint16_t csLen = 0;
    readUint16(msg.data(), msg.size(), &off, &csLen);
    off += csLen;
    if (off >= msg.size()) {
        return false;
    }
    const size_t compLen = static_cast<unsigned char>(msg[off++]);
    off += compLen;
    uint16_t extLen = 0;
    if (!readUint16(msg.data(), msg.size(), &off, &extLen) || off + extLen > msg.size()) {
        return false;
    }
    const size_t extEnd = off + extLen;
    while (off + 4 <= extEnd) {
        uint16_t et = 0, el = 0;
        readUint16(msg.data(), msg.size(), &off, &et);
        readUint16(msg.data(), msg.size(), &off, &el);
        if (off + el > extEnd) {
            return false;
        }
        string ed = msg.substr(off, el);
        off += el;
        if (et == kExtKeyShare && ed.size() >= 6) {
            size_t i = 0;
            uint16_t listLen = 0;
            readUint16(ed.data(), ed.size(), &i, &listLen);
            while (i + 4 <= ed.size()) {
                uint16_t group = 0, klen = 0;
                readUint16(ed.data(), ed.size(), &i, &group);
                readUint16(ed.data(), ed.size(), &i, &klen);
                if (i + klen > ed.size()) {
                    break;
                }
                if (group == kGroupX25519 && klen == 32) {
                    m_clientKeySharePub = ed.substr(i, 32);
                }
                i += klen;
            }
        } else if (et == kExtAlpn) {
            size_t i = 0;
            uint16_t listLen = 0;
            if (readUint16(ed.data(), ed.size(), &i, &listLen)) {
                while (i < ed.size()) {
                    size_t l = static_cast<unsigned char>(ed[i++]);
                    if (i + l > ed.size()) {
                        break;
                    }
                    m_alpn.push_back(ed.substr(i, l));
                    i += l;
                }
            }
        } else if (et == kExtQuicTp) {
            decodeTransportParams(ed, &m_peerParams);
        }
    }
    if (m_clientKeySharePub.size() != 32) {
        if (error) {
            *error = "missing x25519 key share";
        }
        return false;
    }

    const string sh = buildServerHello();
    appendTranscript(sh);
    const string shared = x25519Shared(m_serverKeySharePriv, m_clientKeySharePub);
    if (shared.empty()) {
        if (error) {
            *error = "x25519 failed";
        }
        return false;
    }
    deriveHandshakeSecrets(shared);

    // CRYPTO flight: ServerHello in Initial; EE/Cert/CV/Finished in Handshake
    // We output all CRYPTO bytes; connection layer splits by encryption level.
    m_cryptoOut.append(sh);
    const string ee = buildEncryptedExtensions();
    appendTranscript(ee);
    m_cryptoOut.append(ee);
    const string cert = buildCertificate();
    appendTranscript(cert);
    m_cryptoOut.append(cert);
    const string cv = buildCertificateVerify();
    appendTranscript(cv);
    m_cryptoOut.append(cv);
    const string fin = buildFinished(false);
    appendTranscript(fin);
    m_cryptoOut.append(fin);
    m_sentFinished = true;
    deriveApplicationSecrets();
    return true;
}

bool QuicTlsHandshake::handleServerHello(const string &msg, string *error)
{
    appendTranscript(msg);
    size_t off = 4 + 2;
    if (off + 32 > msg.size()) {
        return false;
    }
    m_serverRandom = msg.substr(off, 32);
    off += 32;
    const size_t sidLen = static_cast<unsigned char>(msg[off++]);
    off += sidLen + 2 + 1;  // cipher + compression
    uint16_t extLen = 0;
    if (!readUint16(msg.data(), msg.size(), &off, &extLen)) {
        return false;
    }
    const size_t extEnd = off + extLen;
    while (off + 4 <= extEnd) {
        uint16_t et = 0, el = 0;
        readUint16(msg.data(), msg.size(), &off, &et);
        readUint16(msg.data(), msg.size(), &off, &el);
        string ed = msg.substr(off, el);
        off += el;
        if (et == kExtKeyShare && ed.size() >= 4) {
            size_t i = 0;
            uint16_t group = 0, klen = 0;
            readUint16(ed.data(), ed.size(), &i, &group);
            readUint16(ed.data(), ed.size(), &i, &klen);
            if (group == kGroupX25519 && klen == 32 && i + 32 <= ed.size()) {
                m_serverKeySharePub = ed.substr(i, 32);
            }
        }
    }
    if (m_serverKeySharePub.size() != 32) {
        if (error) {
            *error = "missing server key share";
        }
        return false;
    }
    const string shared = x25519Shared(m_clientKeySharePriv, m_serverKeySharePub);
    if (shared.empty()) {
        if (error) {
            *error = "x25519 failed";
        }
        return false;
    }
    deriveHandshakeSecrets(shared);
    m_gotServerHello = true;
    return true;
}

bool QuicTlsHandshake::handleEncryptedExtensions(const string &msg, string *error)
{
    (void) error;
    appendTranscript(msg);
    size_t off = 4;
    uint16_t extLen = 0;
    if (!readUint16(msg.data(), msg.size(), &off, &extLen)) {
        return false;
    }
    const size_t extEnd = off + extLen;
    while (off + 4 <= extEnd) {
        uint16_t et = 0, el = 0;
        readUint16(msg.data(), msg.size(), &off, &et);
        readUint16(msg.data(), msg.size(), &off, &el);
        string ed = msg.substr(off, el);
        off += el;
        if (et == kExtAlpn && ed.size() >= 3) {
            size_t i = 0;
            uint16_t listLen = 0;
            readUint16(ed.data(), ed.size(), &i, &listLen);
            if (i < ed.size()) {
                size_t l = static_cast<unsigned char>(ed[i++]);
                if (i + l <= ed.size()) {
                    m_negotiatedAlpn = ed.substr(i, l);
                }
            }
        } else if (et == kExtQuicTp) {
            decodeTransportParams(ed, &m_peerParams);
        }
    }
    m_gotEe = true;
    return true;
}

bool QuicTlsHandshake::handleCertificate(const string &msg, string *error)
{
    appendTranscript(msg);
    size_t off = 4;
    if (off >= msg.size()) {
        return false;
    }
    const size_t ctxLen = static_cast<unsigned char>(msg[off++]);
    off += ctxLen;
    uint32_t certsLen = 0;
    if (!readUint24(msg.data(), msg.size(), &off, &certsLen)) {
        return false;
    }
    if (certsLen == 0) {
        if (error) {
            *error = "empty certificate";
        }
        return false;
    }
    uint32_t certLen = 0;
    if (!readUint24(msg.data(), msg.size(), &off, &certLen) || off + certLen > msg.size()) {
        return false;
    }
    string der = msg.substr(off, certLen);
    m_peerCert = Certificate::load(der, Ssl::Der);
    if (m_peerCert.isNull()) {
        if (error) {
            *error = "bad certificate";
        }
        return false;
    }
    m_gotCert = true;
    return true;
}

bool QuicTlsHandshake::handleCertificateVerify(const string &msg, string *error)
{
    // Transcript for CV does NOT include the CV message itself yet when verifying
    string toSign(64, ' ');
    toSign += "TLS 1.3, server CertificateVerify";
    toSign.push_back('\0');
    toSign += transcriptHash();

    size_t off = 4;
    uint16_t scheme = 0, sigLen = 0;
    if (!readUint16(msg.data(), msg.size(), &off, &scheme) || !readUint16(msg.data(), msg.size(), &off, &sigLen)
        || off + sigLen > msg.size()) {
        return false;
    }
    string sig = msg.substr(off, sigLen);
    EVP_PKEY *pkey = static_cast<EVP_PKEY *>(m_peerCert.publicKey().handle());
    bool ok = false;
    if (scheme == kSigRsaPssRsaeSha256) {
        ok = rsaPssVerify(pkey, toSign, sig);
    } else if (scheme == kSigEcdsaSecp256r1Sha256) {
        ok = ecdsaVerify(pkey, toSign, sig);
    }
    if (!ok && m_verifyPeer) {
        if (error) {
            *error = "certificate verify failed";
        }
        return false;
    }
    appendTranscript(msg);
    m_gotCertVerify = true;
    return true;
}

bool QuicTlsHandshake::handleFinished(const string &msg, string *error)
{
    const bool peerIsClient = (m_role == Server);
    const string &base = peerIsClient ? m_clientHandshakeTrafficSecret : m_serverHandshakeTrafficSecret;
    const string finishedKey = hkdfExpandLabel(MessageDigest::Sha256, base, "finished", string(), 32);
    const string expected = hmacSha256(finishedKey, transcriptHash());
    const string verifyData = msg.substr(4);
    if (verifyData != expected) {
        if (error) {
            *error = "finished verify failed";
        }
        return false;
    }
    appendTranscript(msg);
    m_gotFinished = true;

    if (m_role == Client) {
        // App secrets use transcript through server Finished (exclude client Finished).
        deriveApplicationSecrets();
        const string fin = buildFinished(true);
        appendTranscript(fin);
        m_cryptoOut.append(fin);
        m_sentFinished = true;
        m_complete = true;
    } else {
        // Server already derived app secrets after sending its Finished.
        m_complete = true;
    }
    return true;
}

}  // namespace qtng
