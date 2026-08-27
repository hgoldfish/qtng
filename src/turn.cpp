#include "qtng/private/turn_p.h"

#include <algorithm>
#include <cstring>

#include "qtng/md.h"
#include "qtng/random.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

namespace {

int64_t nowUnix()
{
    return utils::DateTime::currentDateTimeUtc().toSecsSinceEpoch();
}

}  // namespace

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

Allocation::Allocation(const HostAddress &ca, uint16_t cp, const string &u, const string &k)
    : clientAddr(ca)
    , clientPort(cp)
    , username(u)
    , key(k)
    , relayedPort(0)
    , expireUnix(0)
    , alive(false)
{
}

bool Allocation::hasPermission(const HostAddress &peer) const
{
    bool found = false;
    lock.tryAcquire();
    found = permissions.find(peer) != permissions.end();
    lock.release();
    return found;
}

void Allocation::addPermission(const HostAddress &peer)
{
    lock.tryAcquire();
    permissions.insert(peer);
    lock.release();
}

uint16_t Allocation::channelFor(const HostAddress &peer, uint16_t port) const
{
    uint16_t ch = 0;
    lock.tryAcquire();
    map<TurnEndpoint, uint16_t>::const_iterator it = channelPeers.find(TurnEndpoint(peer, port));
    if (it != channelPeers.end()) {
        ch = it->second;
    }
    lock.release();
    return ch;
}

void Allocation::bindChannel(uint16_t channel, const TurnEndpoint &peer)
{
    lock.tryAcquire();
    channels[channel] = peer;
    channelPeers[peer] = channel;
    lock.release();
}

// ---------------------------------------------------------------------------
// TurnClient
// ---------------------------------------------------------------------------

TurnClient::TurnClient(HostAddress::NetworkLayerProtocol proto)
    : d_ptr(new TurnClientPrivate(this, proto))
{
}

TurnClient::~TurnClient()
{
    close();
    delete d_ptr;
}

bool TurnClient::open(const HostAddress &server, uint16_t port, const string &username,
                      const string &password, float timeoutSecs)
{
    NG_D(TurnClient);
    return d->open(server, port, username, password, timeoutSecs);
}

bool TurnClient::open(const string &server, uint16_t port, const string &username,
                      const string &password, float timeoutSecs)
{
    NG_D(TurnClient);
    HostAddress addr;
    if (addr.setAddress(server)) {
        return d->open(addr, port, username, password, timeoutSecs);
    }
    vector<HostAddress> resolved = Socket::resolve(server);
    for (size_t i = 0; i < resolved.size(); ++i) {
        if (d->open(resolved[i], port, username, password, timeoutSecs)) {
            return true;
        }
    }
    d->error = "cannot resolve server: " + server;
    return false;
}

void TurnClient::close()
{
    NG_D(TurnClient);
    d->close();
}

bool TurnClient::isOpen() const
{
    NG_D(const TurnClient);
    return d->opened;
}

HostAddress TurnClient::localAddress() const
{
    NG_D(const TurnClient);
    return d->socket ? d->socket->localAddress() : HostAddress();
}

uint16_t TurnClient::localPort() const
{
    NG_D(const TurnClient);
    return d->socket ? d->socket->localPort() : 0;
}

HostAddress TurnClient::relayedAddress() const
{
    NG_D(const TurnClient);
    return d->relayedAddr;
}

uint16_t TurnClient::relayedPort() const
{
    NG_D(const TurnClient);
    return d->relayedPort;
}

uint32_t TurnClient::lifetime() const
{
    NG_D(const TurnClient);
    return d->lifetimeSecs;
}

bool TurnClient::refresh(float timeoutSecs)
{
    NG_D(TurnClient);
    return d->refresh(timeoutSecs);
}

bool TurnClient::sendTo(const HostAddress &peer, uint16_t port, const string &data)
{
    NG_D(TurnClient);
    return d->sendTo(peer, port, data);
}

bool TurnClient::sendIndication(const HostAddress &peer, uint16_t port, const string &data)
{
    NG_D(TurnClient);
    return d->sendIndication(peer, port, data);
}

bool TurnClient::permit(const HostAddress &peer, uint16_t port, float timeoutSecs)
{
    NG_D(TurnClient);
    return d->ensurePermission(peer, port, timeoutSecs);
}

string TurnClient::recvFrom(HostAddress *peer, uint16_t *port, float timeoutSecs)
{
    NG_D(TurnClient);
    return d->recvFrom(peer, port, timeoutSecs);
}

string TurnClient::errorString() const
{
    NG_D(const TurnClient);
    return d->error;
}

int TurnClient::errorCode() const
{
    NG_D(const TurnClient);
    return d->errorCode;
}

TurnClientPrivate::TurnClientPrivate(TurnClient *q, HostAddress::NetworkLayerProtocol p)
    : q_ptr(q)
    , proto(p)
    , opened(false)
    , errorCode(0)
    , serverPort(0)
    , relayedPort(0)
    , lifetimeSecs(0)
    , channelCounter(0x3fff)
{
}

TurnClientPrivate::~TurnClientPrivate() { }

bool TurnClientPrivate::open(const HostAddress &server, uint16_t port, const string &user,
                             const string &pass, float timeoutSecs)
{
    if (opened) {
        return true;
    }
    serverAddr = server;
    serverPort = port;
    username = user;
    password = pass;

    socket = make_shared<Socket>(proto, Socket::UdpSocket);
    const HostAddress bindAddr = (proto == HostAddress::IPv6Protocol) ? HostAddress::AnyIPv6
                                                                      : HostAddress::AnyIPv4;
    if (!socket->bind(bindAddr, 0)) {
        error = "UDP bind failed: " + socket->errorString();
        socket.reset();
        return false;
    }
    workers = make_unique<CoroutineGroup>();
    workers->spawnWithName("turn-client-recv", [this] { recvLoop(); });
    opened = true;
    if (!allocate(timeoutSecs)) {
        close();
        return false;
    }
    return true;
}

void TurnClientPrivate::close()
{
    if (!opened && !socket) {
        return;
    }
    opened = false;
    if (socket) {
        socket->abort();
    }
    if (workers) {
        workers->killall(true);
        workers.reset();
    }
    {
        waitersLock.tryAcquire();
        waiters.clear();
        waitersLock.release();
    }
    socket.reset();
    inbox.clear();
    peerChannels.clear();
    channelPeers.clear();
    permissions.clear();
}

string TurnClientPrivate::currentKey() const
{
    if (realm.empty() || username.empty()) {
        return string();
    }
    return stunLongTermKey(username, realm, password);
}

void TurnClientPrivate::authenticate(StunMessage *msg) const
{
    if (realm.empty() || nonce.empty() || username.empty()) {
        return;
    }
    msg->addAttribute(StunAttrUsername, username);
    msg->addAttribute(StunAttrRealm, realm);
    msg->addAttribute(StunAttrNonce, nonce);
    msg->addAttribute(StunAttrMessageIntegrity, string(20, '\0'));
    msg->setIntegrityKey(stunLongTermKey(username, realm, password));
}

StunReply TurnClientPrivate::rpc(const StunMessage &msg, float timeoutSecs)
{
    StunReply reply;
    if (!opened || !socket) {
        reply.localError = "client is not open";
        return reply;
    }
    shared_ptr<ValueEvent<StunReply>> waiter = make_shared<ValueEvent<StunReply>>();
    {
        waitersLock.tryAcquire();
        waiters[msg.transactionId()] = waiter;
        waitersLock.release();
    }
    const string packet = msg.encode();
    if (socket->sendto(packet, serverAddr, serverPort) <= 0) {
        waitersLock.tryAcquire();
        waiters.erase(msg.transactionId());
        waitersLock.release();
        reply.localError = "send failed: " + socket->errorString();
        return reply;
    }
    try {
        Timeout timeout(timeoutSecs);
        (void) timeout;
        reply = waiter->tryWait(static_cast<uint32_t>(timeoutSecs * 1000));
    } catch (TimeoutException &) {
        reply.localError = "timeout";
    }
    waitersLock.tryAcquire();
    waiters.erase(msg.transactionId());
    waitersLock.release();
    return reply;
}

bool TurnClientPrivate::allocate(float timeoutSecs)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        StunMessage msg;
        msg.setMethod(StunAllocateMethod);
        msg.setMessageClass(StunRequestClass);
        msg.setTransactionId(StunMessage::newTransactionId());
        msg.setRequestedTransport(17);  // UDP
        authenticate(&msg);

        const StunReply reply = rpc(msg, timeoutSecs);
        if (!reply.ok) {
            error = reply.localError.empty() ? "timeout" : reply.localError;
            errorCode = 0;
            return false;
        }
        if (reply.success) {
            HostAddress a;
            uint16_t p = 0;
            if (!reply.message.xorRelayedAddress(&a, &p)) {
                error = "Allocate response missing relayed address";
                errorCode = 0;
                return false;
            }
            relayedAddr = a;
            relayedPort = p;
            uint32_t lt = 0;
            lifetimeSecs = reply.message.lifetime(&lt) ? lt : 600u;
            error.clear();
            errorCode = 0;
            return true;
        }
        int code = 0;
        string reason;
        if (reply.message.errorCode(&code, &reason)) {
            if (code == 401) {
                const StunAttribute *r = reply.message.attribute(StunAttrRealm);
                const StunAttribute *n = reply.message.attribute(StunAttrNonce);
                if (r) {
                    realm = r->value;
                }
                if (n) {
                    nonce = n->value;
                }
                if (username.empty()) {
                    error = "authentication required";
                    errorCode = code;
                    return false;
                }
                continue;  // retry with the learned realm/nonce
            }
            error = reason.empty() ? "TURN error" : reason;
            errorCode = code;
            return false;
        }
        error = "protocol error";
        errorCode = 0;
        return false;
    }
    error = "authentication failed";
    errorCode = 401;
    return false;
}

bool TurnClientPrivate::refresh(float timeoutSecs)
{
    if (!opened) {
        return false;
    }
    StunMessage msg;
    msg.setMethod(StunRefreshMethod);
    msg.setMessageClass(StunRequestClass);
    msg.setTransactionId(StunMessage::newTransactionId());
    msg.setLifetime(600);
    authenticate(&msg);
    const StunReply reply = rpc(msg, timeoutSecs);
    if (reply.ok && reply.success) {
        uint32_t lt = 0;
        if (reply.message.lifetime(&lt)) {
            lifetimeSecs = lt;
        }
        return true;
    }
    return false;
}

bool TurnClientPrivate::ensurePermission(const HostAddress &peer, uint16_t port, float timeoutSecs)
{
    if (permissions.find(peer) != permissions.end()) {
        return true;
    }
    StunMessage msg;
    msg.setMethod(StunCreatePermissionMethod);
    msg.setMessageClass(StunRequestClass);
    msg.setTransactionId(StunMessage::newTransactionId());
    msg.setXorPeerAddress(peer, port);
    authenticate(&msg);
    const StunReply reply = rpc(msg, timeoutSecs);
    if (reply.ok && reply.success) {
        permissions.insert(peer);
        return true;
    }
    return false;
}

bool TurnClientPrivate::bindChannel(const HostAddress &peer, uint16_t port, float timeoutSecs)
{
    map<TurnEndpoint, uint16_t>::iterator existing = peerChannels.find(TurnEndpoint(peer, port));
    if (existing != peerChannels.end()) {
        return true;
    }
    ++channelCounter;
    if (channelCounter < 0x4000 || channelCounter > 0x7fff) {
        channelCounter = 0x4000;
    }
    const uint16_t ch = channelCounter;
    StunMessage msg;
    msg.setMethod(StunChannelBindMethod);
    msg.setMessageClass(StunRequestClass);
    msg.setTransactionId(StunMessage::newTransactionId());
    msg.setChannelNumber(ch);
    msg.setXorPeerAddress(peer, port);
    authenticate(&msg);
    const StunReply reply = rpc(msg, timeoutSecs);
    if (reply.ok && reply.success) {
        peerChannels[TurnEndpoint(peer, port)] = ch;
        channelPeers[ch] = TurnEndpoint(peer, port);
        return true;
    }
    return false;
}

bool TurnClientPrivate::sendTo(const HostAddress &peer, uint16_t port, const string &data)
{
    if (!opened || !socket) {
        return false;
    }
    if (!ensurePermission(peer, port, 5.0f)) {
        return false;
    }
    uint16_t channel = 0;
    map<TurnEndpoint, uint16_t>::iterator it = peerChannels.find(TurnEndpoint(peer, port));
    if (it != peerChannels.end()) {
        channel = it->second;
    }
    if (channel == 0) {
        if (bindChannel(peer, port, 5.0f)) {
            map<TurnEndpoint, uint16_t>::iterator it2 = peerChannels.find(TurnEndpoint(peer, port));
            if (it2 != peerChannels.end()) {
                channel = it2->second;
            }
        }
    }
    if (channel != 0) {
        return socket->sendto(StunMessage::encodeChannelData(channel, data), serverAddr, serverPort) > 0;
    }
    return sendIndication(peer, port, data);
}

bool TurnClientPrivate::sendIndication(const HostAddress &peer, uint16_t port, const string &data)
{
    if (!opened || !socket) {
        return false;
    }
    // Send indications require a permission for the peer (RFC 8656 §6.2).
    if (!ensurePermission(peer, port, 5.0f)) {
        return false;
    }
    StunMessage ind;
    ind.setMethod(StunSendMethod);
    ind.setMessageClass(StunIndicationClass);
    ind.setTransactionId(StunMessage::newTransactionId());
    ind.setXorPeerAddress(peer, port);
    ind.addAttribute(StunAttrData, data);
    return socket->sendto(ind.encode(), serverAddr, serverPort) > 0;
}

string TurnClientPrivate::recvFrom(HostAddress *peer, uint16_t *port, float timeoutSecs)
{
    if (!opened) {
        return string();
    }
    if (!inbox.waitNotEmpty(static_cast<uint32_t>(timeoutSecs * 1000))) {
        return string();
    }
    const IncomingTurnData incoming = inbox.get();
    if (incoming.data.empty()) {
        return string();
    }
    if (peer) {
        *peer = incoming.peer;
    }
    if (port) {
        *port = incoming.port;
    }
    return incoming.data;
}

void TurnClientPrivate::recvLoop()
{
    while (opened && socket) {
        HostAddress from;
        uint16_t fromPort = 0;
        string data = socket->recvfrom(65535, &from, &fromPort);
        if (data.empty()) {
            if (!opened) {
                break;
            }
            Coroutine::msleep(10);
            continue;
        }
        if (StunMessage::isChannelData(data)) {
            ChannelDataFrame frame;
            if (StunMessage::parseChannelData(data, &frame)) {
                map<uint16_t, TurnEndpoint>::iterator it = channelPeers.find(frame.channel);
                if (it != channelPeers.end()) {
                    IncomingTurnData incoming;
                    incoming.peer = it->second.address;
                    incoming.port = it->second.port;
                    incoming.data = frame.data;
                    inbox.put(incoming);
                }
            }
            continue;
        }
        StunMessage msg;
        if (!msg.parse(data)) {
            continue;
        }
        if (msg.messageClass() == StunIndicationClass && msg.method() == StunDataMethod) {
            HostAddress peer;
            uint16_t peerPort = 0;
            const StunAttribute *d = msg.attribute(StunAttrData);
            if (msg.xorPeerAddress(&peer, &peerPort) && d) {
                IncomingTurnData incoming;
                incoming.peer = peer;
                incoming.port = peerPort;
                incoming.data = d->value;
                inbox.put(incoming);
            }
            continue;
        }
        if (msg.messageClass() != StunSuccessClass && msg.messageClass() != StunErrorClass) {
            continue;
        }
        // Verify response integrity when we hold long-term credentials.
        const string key = currentKey();
        if (msg.hasMessageIntegrity() && !key.empty() && !msg.verifyIntegrity(key)) {
            continue;
        }
        StunReply reply;
        reply.ok = true;
        reply.success = (msg.messageClass() == StunSuccessClass);
        reply.message = msg;
        reply.fromAddress = from;
        reply.fromPort = fromPort;
        waitersLock.tryAcquire();
        map<string, shared_ptr<ValueEvent<StunReply>>>::iterator it = waiters.find(msg.transactionId());
        if (it != waiters.end()) {
            it->second->send(reply);
        }
        waitersLock.release();
    }
}

// ---------------------------------------------------------------------------
// TurnServer
// ---------------------------------------------------------------------------

TurnServer::TurnServer(HostAddress::NetworkLayerProtocol proto)
    : d_ptr(new TurnServerPrivate(this, proto))
{
}

TurnServer::~TurnServer()
{
    close();
    delete d_ptr;
}

bool TurnServer::open(const HostAddress &addr, uint16_t port, const string &realm, AuthCallback auth)
{
    NG_D(TurnServer);
    return d->open(addr, port, realm, auth);
}

bool TurnServer::open(uint16_t port, const string &realm, AuthCallback auth)
{
    NG_D(TurnServer);
    const HostAddress addr =
            (d->proto == HostAddress::IPv6Protocol) ? HostAddress::AnyIPv6 : HostAddress::AnyIPv4;
    return d->open(addr, port, realm, auth);
}

void TurnServer::close()
{
    NG_D(TurnServer);
    d->close();
}

bool TurnServer::isOpen() const
{
    NG_D(const TurnServer);
    return d->opened;
}

HostAddress TurnServer::localAddress() const
{
    NG_D(const TurnServer);
    return d->listener ? d->listener->localAddress() : HostAddress();
}

uint16_t TurnServer::localPort() const
{
    NG_D(const TurnServer);
    return d->listener ? d->listener->localPort() : 0;
}

string TurnServer::errorString() const
{
    NG_D(const TurnServer);
    return d->error;
}

void TurnServer::setDefaultLifetime(uint32_t secs)
{
    NG_D(TurnServer);
    d->defaultLifetime = secs;
}

TurnServerPrivate::TurnServerPrivate(TurnServer *q, HostAddress::NetworkLayerProtocol p)
    : q_ptr(q)
    , proto(p)
    , opened(false)
    , defaultLifetime(600)
{
}

TurnServerPrivate::~TurnServerPrivate() { }

bool TurnServerPrivate::open(const HostAddress &addr, uint16_t port, const string &r,
                             TurnServer::AuthCallback a)
{
    if (opened) {
        return true;
    }
    realm = r;
    auth = a;
    nonce = utils::bytesToHex(randomBytes(16));
    prevNonce.clear();
    defaultLifetime = 600;

    listener = make_shared<Socket>(proto, Socket::UdpSocket);
    listener->setOption(Socket::AddressReusable, true);
    if (!listener->bind(addr, port)) {
        error = "UDP bind failed: " + listener->errorString();
        listener.reset();
        return false;
    }
    workers = make_unique<CoroutineGroup>();
    workers->spawnWithName("turn-server-recv", [this] { recvLoop(); });
    workers->spawnWithName("turn-server-maint", [this] { maintenanceLoop(); });
    opened = true;
    return true;
}

void TurnServerPrivate::close()
{
    if (!opened && !listener) {
        return;
    }
    opened = false;
    if (listener) {
        listener->abort();
    }
    {
        allocLock.tryAcquire();
        for (map<AllocationKey, shared_ptr<Allocation>>::iterator it = allocations.begin();
             it != allocations.end(); ++it) {
            it->second->alive = false;
            if (it->second->relaySocket) {
                it->second->relaySocket->abort();
            }
        }
        allocations.clear();
        allocLock.release();
    }
    if (workers) {
        workers->killall(true);
        workers.reset();
    }
    listener.reset();
}

void TurnServerPrivate::recvLoop()
{
    while (opened && listener) {
        HostAddress addr;
        uint16_t port = 0;
        string data = listener->recvfrom(65535, &addr, &port);
        if (data.empty()) {
            if (!opened) {
                break;
            }
            Coroutine::msleep(10);
            continue;
        }
        handleMessage(data, addr, port);
    }
}

void TurnServerPrivate::maintenanceLoop()
{
    while (opened) {
        Coroutine::sleep(2.0f);
        if (!opened) {
            break;
        }
        const int64_t now = nowUnix();
        vector<shared_ptr<Allocation>> expired;
        {
            allocLock.tryAcquire();
            for (map<AllocationKey, shared_ptr<Allocation>>::iterator it = allocations.begin();
                 it != allocations.end();) {
                if (it->second->expireUnix <= now) {
                    expired.push_back(it->second);
                    it = allocations.erase(it);
                } else {
                    ++it;
                }
            }
            allocLock.release();
        }
        for (size_t i = 0; i < expired.size(); ++i) {
            expired[i]->alive = false;
            if (expired[i]->relaySocket) {
                expired[i]->relaySocket->abort();
            }
            if (expired[i]->relayCoroutine) {
                expired[i]->relayCoroutine->kill();
                expired[i]->relayCoroutine->join();
                expired[i]->relayCoroutine.reset();
            }
            expired[i]->relaySocket.reset();
        }
    }
}

shared_ptr<Allocation> TurnServerPrivate::findAllocation(const HostAddress &addr, uint16_t port)
{
    shared_ptr<Allocation> result;
    allocLock.tryAcquire();
    map<AllocationKey, shared_ptr<Allocation>>::iterator it =
            allocations.find(AllocationKey(addr, port));
    if (it != allocations.end()) {
        result = it->second;
    }
    allocLock.release();
    return result;
}

void TurnServerPrivate::handleMessage(const string &data, const HostAddress &addr, uint16_t port)
{
    if (StunMessage::isChannelData(data)) {
        ChannelDataFrame frame;
        if (!StunMessage::parseChannelData(data, &frame)) {
            return;
        }
        shared_ptr<Allocation> alloc = findAllocation(addr, port);
        if (alloc) {
            handleChannelData(frame, alloc);
        }
        return;
    }
    StunMessage msg;
    if (!msg.parse(data)) {
        return;
    }
    shared_ptr<Allocation> alloc = findAllocation(addr, port);
    switch (msg.method()) {
    case StunAllocateMethod:
        if (msg.messageClass() == StunRequestClass) {
            handleAllocate(msg, addr, port);
        }
        break;
    case StunRefreshMethod:
        if (msg.messageClass() == StunRequestClass && alloc) {
            handleRefresh(msg, alloc, addr, port);
        }
        break;
    case StunCreatePermissionMethod:
        if (msg.messageClass() == StunRequestClass && alloc) {
            handleCreatePermission(msg, alloc, addr, port);
        }
        break;
    case StunChannelBindMethod:
        if (msg.messageClass() == StunRequestClass && alloc) {
            handleChannelBind(msg, alloc, addr, port);
        }
        break;
    case StunSendMethod:
        if (msg.messageClass() == StunIndicationClass && alloc) {
            handleSendIndication(msg, alloc);
        }
        break;
    case StunBindingMethod:
        if (msg.messageClass() == StunRequestClass) {
            sendError(msg, 400, "Bad Request", addr, port);
        }
        break;
    default:
        break;
    }
}

TurnAuthResult TurnServerPrivate::authenticate(const StunMessage &msg, string *key) const
{
    if (!auth) {
        if (key) {
            key->clear();
        }
        return TurnAuthOk;
    }
    const StunAttribute *user = msg.attribute(StunAttrUsername);
    const StunAttribute *realmAttr = msg.attribute(StunAttrRealm);
    const StunAttribute *nonceAttr = msg.attribute(StunAttrNonce);
    if (!user || !realmAttr || !nonceAttr || !msg.hasMessageIntegrity()) {
        return TurnAuthNoCredentials;
    }
    if (realmAttr->value != realm) {
        return TurnAuthNoCredentials;
    }
    if (nonceAttr->value != nonce && nonceAttr->value != prevNonce) {
        return TurnAuthStaleNonce;
    }
    const string pw = auth(user->value, realm);
    if (pw.empty()) {
        return TurnAuthFailed;
    }
    const string k = stunLongTermKey(user->value, realm, pw);
    if (!msg.verifyIntegrity(k)) {
        return TurnAuthFailed;
    }
    if (key) {
        *key = k;
    }
    return TurnAuthOk;
}

void TurnServerPrivate::sendError(const StunMessage &req, int code, const string &reason,
                                  const HostAddress &addr, uint16_t port)
{
    StunMessage resp;
    resp.setMethod(req.method());
    resp.setMessageClass(StunErrorClass);
    resp.setTransactionId(req.transactionId());
    resp.setErrorCode(code, reason);
    listener->sendto(resp.encode(), addr, port);
}

void TurnServerPrivate::sendChallenge(const StunMessage &req, const HostAddress &addr, uint16_t port)
{
    StunMessage resp;
    resp.setMethod(req.method());
    resp.setMessageClass(StunErrorClass);
    resp.setTransactionId(req.transactionId());
    resp.setErrorCode(401, "Unauthorized");
    resp.addAttribute(StunAttrRealm, realm);
    resp.addAttribute(StunAttrNonce, nonce);
    listener->sendto(resp.encode(), addr, port);
}

void TurnServerPrivate::sendSuccess(const StunMessage &req, const string &key, const HostAddress &addr,
                                    uint16_t port)
{
    StunMessage resp;
    resp.setMethod(req.method());
    resp.setMessageClass(StunSuccessClass);
    resp.setTransactionId(req.transactionId());
    if (!key.empty()) {
        resp.addAttribute(StunAttrMessageIntegrity, string(20, '\0'));
        resp.setIntegrityKey(key);
    }
    listener->sendto(resp.encode(), addr, port);
}

void TurnServerPrivate::handleAllocate(const StunMessage &msg, const HostAddress &addr, uint16_t port)
{
    if (findAllocation(addr, port)) {
        sendError(msg, 437, "Allocation Mismatch", addr, port);
        return;
    }
    uint8_t transportProto = 0;
    if (!msg.requestedTransport(&transportProto) || transportProto != 17) {
        sendError(msg, 442, "Unsupported Transport Protocol", addr, port);
        return;
    }
    string key;
    const TurnAuthResult authResult = authenticate(msg, &key);
    if (authResult != TurnAuthOk) {
        if (authResult == TurnAuthStaleNonce) {
            sendError(msg, 438, "Stale Nonce", addr, port);
        } else {
            sendChallenge(msg, addr, port);
        }
        return;
    }

    const StunAttribute *userAttr = msg.attribute(StunAttrUsername);
    const string allocUsername = userAttr ? userAttr->value : string();
    shared_ptr<Allocation> alloc = make_shared<Allocation>(addr, port, allocUsername, key);
    alloc->relaySocket = make_shared<Socket>(proto, Socket::UdpSocket);
    const HostAddress relayBind = (proto == HostAddress::IPv6Protocol) ? HostAddress::AnyIPv6
                                                                       : HostAddress::AnyIPv4;
    if (!alloc->relaySocket->bind(relayBind, 0)) {
        sendError(msg, 500, "Server Error", addr, port);
        return;
    }
    alloc->relayedAddr = listener->localAddress();
    alloc->relayedPort = alloc->relaySocket->localPort();
    alloc->expireUnix = nowUnix() + defaultLifetime;
    alloc->alive = true;
    alloc->relayCoroutine = workers->spawn([this, alloc] { relayLoop(alloc); });

    {
        allocLock.tryAcquire();
        allocations[AllocationKey(addr, port)] = alloc;
        allocLock.release();
    }

    StunMessage resp;
    resp.setMethod(StunAllocateMethod);
    resp.setMessageClass(StunSuccessClass);
    resp.setTransactionId(msg.transactionId());
    resp.setXorRelayedAddress(alloc->relayedAddr, alloc->relayedPort);
    resp.setLifetime(defaultLifetime);
    if (!key.empty()) {
        resp.addAttribute(StunAttrMessageIntegrity, string(20, '\0'));
        resp.setIntegrityKey(key);
    }
    listener->sendto(resp.encode(), addr, port);
}

void TurnServerPrivate::handleRefresh(const StunMessage &msg, const shared_ptr<Allocation> &alloc,
                                      const HostAddress &addr, uint16_t port)
{
    string key;
    const TurnAuthResult authResult = authenticate(msg, &key);
    if (authResult != TurnAuthOk) {
        sendChallenge(msg, addr, port);
        return;
    }
    uint32_t lt = defaultLifetime;
    uint32_t requested = 0;
    if (msg.lifetime(&requested)) {
        lt = requested;
    }
    alloc->expireUnix = nowUnix() + lt;

    StunMessage resp;
    resp.setMethod(StunRefreshMethod);
    resp.setMessageClass(StunSuccessClass);
    resp.setTransactionId(msg.transactionId());
    resp.setLifetime(lt);
    if (!key.empty()) {
        resp.addAttribute(StunAttrMessageIntegrity, string(20, '\0'));
        resp.setIntegrityKey(key);
    }
    listener->sendto(resp.encode(), addr, port);
}

void TurnServerPrivate::handleCreatePermission(const StunMessage &msg,
                                               const shared_ptr<Allocation> &alloc,
                                               const HostAddress &addr, uint16_t port)
{
    string key;
    const TurnAuthResult authResult = authenticate(msg, &key);
    if (authResult != TurnAuthOk) {
        sendChallenge(msg, addr, port);
        return;
    }
    HostAddress peer;
    uint16_t peerPort = 0;
    if (!msg.xorPeerAddress(&peer, &peerPort)) {
        sendError(msg, 400, "Bad Request", addr, port);
        return;
    }
    alloc->addPermission(peer);
    sendSuccess(msg, key, addr, port);
}

void TurnServerPrivate::handleChannelBind(const StunMessage &msg, const shared_ptr<Allocation> &alloc,
                                          const HostAddress &addr, uint16_t port)
{
    string key;
    const TurnAuthResult authResult = authenticate(msg, &key);
    if (authResult != TurnAuthOk) {
        sendChallenge(msg, addr, port);
        return;
    }
    uint16_t channel = 0;
    HostAddress peer;
    uint16_t peerPort = 0;
    if (!msg.channelNumber(&channel) || !msg.xorPeerAddress(&peer, &peerPort)) {
        sendError(msg, 400, "Bad Request", addr, port);
        return;
    }
    if (channel < 0x4000 || channel > 0x7fff) {
        sendError(msg, 400, "Bad Request", addr, port);
        return;
    }
    alloc->bindChannel(channel, TurnEndpoint(peer, peerPort));
    sendSuccess(msg, key, addr, port);
}

void TurnServerPrivate::handleSendIndication(const StunMessage &msg,
                                             const shared_ptr<Allocation> &alloc)
{
    HostAddress peer;
    uint16_t peerPort = 0;
    const StunAttribute *d = msg.attribute(StunAttrData);
    if (!msg.xorPeerAddress(&peer, &peerPort) || !d) {
        return;
    }
    if (!alloc->hasPermission(peer)) {
        return;
    }
    alloc->relaySocket->sendto(d->value, peer, peerPort);
}

void TurnServerPrivate::handleChannelData(const ChannelDataFrame &frame,
                                          const shared_ptr<Allocation> &alloc)
{
    TurnEndpoint peer;
    {
        alloc->lock.tryAcquire();
        map<uint16_t, TurnEndpoint>::const_iterator it = alloc->channels.find(frame.channel);
        if (it != alloc->channels.end()) {
            peer = it->second;
        }
        alloc->lock.release();
    }
    if (peer.port == 0) {
        return;
    }
    if (!alloc->hasPermission(peer.address)) {
        return;
    }
    alloc->relaySocket->sendto(frame.data, peer.address, peer.port);
}

void TurnServerPrivate::relayLoop(const shared_ptr<Allocation> &alloc)
{
    while (opened && alloc->alive && alloc->relaySocket) {
        HostAddress peer;
        uint16_t peerPort = 0;
        string data = alloc->relaySocket->recvfrom(65535, &peer, &peerPort);
        if (data.empty()) {
            if (!opened || !alloc->alive) {
                break;
            }
            Coroutine::msleep(10);
            continue;
        }
        if (!alloc->hasPermission(peer)) {
            continue;
        }
        const uint16_t channel = alloc->channelFor(peer, peerPort);
        if (channel != 0) {
            listener->sendto(StunMessage::encodeChannelData(channel, data), alloc->clientAddr,
                             alloc->clientPort);
        } else {
            StunMessage ind;
            ind.setMethod(StunDataMethod);
            ind.setMessageClass(StunIndicationClass);
            ind.setTransactionId(StunMessage::newTransactionId());
            ind.setXorPeerAddress(peer, peerPort);
            ind.addAttribute(StunAttrData, data);
            listener->sendto(ind.encode(), alloc->clientAddr, alloc->clientPort);
        }
    }
}

}  // namespace qtng
