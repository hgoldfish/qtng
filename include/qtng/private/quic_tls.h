#ifndef QTNG_QUIC_TLS_H
#define QTNG_QUIC_TLS_H

#include <cstdint>
#include <string>
#include <vector>

#include "qtng/certificate.h"
#include "qtng/pkey.h"
#include "qtng/private/quic_p.h"

namespace qtng {

struct QuicTransportParams {
    std::uint64_t maxIdleTimeoutMs = 30000;
    std::uint64_t initialMaxData = 1024 * 1024;
    std::uint64_t initialMaxStreamDataBidiLocal = 256 * 1024;
    std::uint64_t initialMaxStreamDataBidiRemote = 256 * 1024;
    std::uint64_t initialMaxStreamDataUni = 256 * 1024;
    std::uint64_t initialMaxStreamsBidi = 100;
    std::uint64_t initialMaxStreamsUni = 100;
    std::uint64_t maxUdpPayloadSize = 1452;
    bool disableActiveMigration = false;
    std::string initialSourceConnectionId;
    std::string originalDestinationConnectionId;
};

class QuicTlsHandshake
{
public:
    enum Role { Client, Server };

    QuicTlsHandshake(Role role, const QuicTransportParams &params);
    ~QuicTlsHandshake();

    void setServerName(const std::string &sni);
    void setAlpn(const std::vector<std::string> &alpn);
    void setCredentials(const PrivateKey &key, const Certificate &cert);
    void setVerifyPeer(bool verify);

    // Feed CRYPTO stream bytes; may produce output CRYPTO bytes and update secrets.
    bool feedCryptoData(const std::string &data, std::string *error);
    std::string takeCryptoToSend();

    bool startClientHello(std::string *error);
    bool isHandshakeComplete() const;
    bool isConnected() const;  // 1-RTT keys ready

    const QuicTlsSecrets &secrets() const { return m_secrets; }
    std::string negotiatedAlpn() const { return m_negotiatedAlpn; }
    QuicTransportParams peerParams() const { return m_peerParams; }

    // Client initial destination CID used for retry/odcid
    void setClientOdCid(const std::string &cid) { m_odcid = cid; }

    // --- Session resumption / 0-RTT (RFC 8446 PSK + RFC 9001 §8) ---
    void setSessionTicket(const std::string &ticket, const std::string &ticketNonce,
                          const std::string &resumptionSecret);
    bool hasSessionTicket() const { return !m_sessionTicket.empty(); }
    std::string sessionTicket() const { return m_sessionTicket; }
    std::string sessionTicketNonce() const { return m_ticketNonce; }
    std::string sessionResumptionSecret() const { return m_resumptionSecret; }
    // true when the server accepted early data in this handshake.
    bool earlyDataAccepted() const { return m_earlyDataAccepted; }
    // Server-generated NewSessionTicket message bytes (appears in the 1-RTT crypto
    // stream after the handshake flight).
    std::string buildNewSessionTicket();
    void feedSessionTicket(const std::string &ticket);  // client: store NST ticket
    std::string clientEarlyTrafficSecret() const { return m_clientEarlyTrafficSecret; }
    bool hasEarlyTrafficSecret() const { return !m_clientEarlyTrafficSecret.empty(); }
private:
    std::string buildBinderForClientHello(const std::string &clientHelloWithoutBinders);
    std::string recoverPskFromTicket(const std::string &ticket) const;
    bool processHandshakeMessage(const std::string &msg, std::string *error);
    bool handleClientHello(const std::string &msg, std::string *error);
    bool handleServerHello(const std::string &msg, std::string *error);
    bool handleEncryptedExtensions(const std::string &msg, std::string *error);
    bool handleCertificate(const std::string &msg, std::string *error);
    bool handleCertificateVerify(const std::string &msg, std::string *error);
    bool handleFinished(const std::string &msg, std::string *error);

    std::string buildClientHello();
    std::string buildServerHello();
    std::string buildEncryptedExtensions();
    std::string buildCertificate();
    std::string buildCertificateVerify();
    std::string buildFinished(bool isClient);

    void appendTranscript(const std::string &handshakeMsg);
    std::string transcriptHash() const;
    void deriveHandshakeSecrets(const std::string &sharedSecret);
    void deriveApplicationSecrets();

    std::string encodeTransportParams(bool isClient) const;
    bool decodeTransportParams(const std::string &data, QuicTransportParams *out) const;

    Role m_role;
    QuicTransportParams m_localParams;
    QuicTransportParams m_peerParams;
    std::string m_sni;
    std::vector<std::string> m_alpn;
    std::string m_negotiatedAlpn;
    PrivateKey m_key;
    Certificate m_cert;
    Certificate m_peerCert;
    bool m_verifyPeer = true;

    std::string m_transcript;
    std::string m_cryptoIn;
    std::string m_cryptoOut;
    std::string m_clientRandom;
    std::string m_serverRandom;
    std::string m_clientKeySharePub;
    std::string m_clientKeySharePriv;
    std::string m_serverKeySharePub;
    std::string m_serverKeySharePriv;
    std::string m_odcid;

    std::string m_handshakeSecret;
    std::string m_clientHandshakeTrafficSecret;
    std::string m_serverHandshakeTrafficSecret;
    std::string m_masterSecret;
    std::string m_resumptionSecret;
    std::string m_sessionTicket;
    std::string m_earlySecret;
    std::string m_clientEarlyTrafficSecret;
    std::string m_earlyDataOffered;
    std::string m_ticketNonce;
    bool m_earlyDataAccepted = false;
    bool m_gotNewSessionTicket = false;

    QuicTlsSecrets m_secrets;
    bool m_gotServerHello = false;
    bool m_gotEe = false;
    bool m_gotCert = false;
    bool m_gotCertVerify = false;
    bool m_gotFinished = false;
    bool m_sentFinished = false;
    bool m_complete = false;
};

}  // namespace qtng

#endif
