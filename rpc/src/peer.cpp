#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qtng/coroutine_utils.h"
#include "qtng/data_channel.h"
#include "qtng/random.h"

#include "qtng/rpc/peer.h"
#include "qtng/rpc/registration.h"
#include "rpc_p.h"

using namespace std;

namespace qtng {
namespace rpc {

namespace {

enum { GOT_REQUEST = 1, GOT_RESPONSE = 2, GOT_NOTHING = 3 };

string packRequest(const Request &request)
{
    ValueList l;
    l.emplace_back(Value(static_cast<std::int64_t>(1)));
    l.emplace_back(Value::bin(request.id));
    l.emplace_back(Value::str(request.methodName));
    l.emplace_back(Value(request.args));
    l.emplace_back(Value(request.kwargs));
    l.emplace_back(Value(request.header));
    l.emplace_back(Value(static_cast<std::uint64_t>(request.channel)));
    l.emplace_back(Value::bin(request.rawSocket));
    return Value::pack(Value(std::move(l)));
}

string packResponse(const Response &response)
{
    ValueList l;
    l.emplace_back(Value(static_cast<std::int64_t>(2)));
    l.emplace_back(Value::bin(response.id));
    l.emplace_back(response.result);
    l.emplace_back(response.exception);
    l.emplace_back(Value(static_cast<std::uint64_t>(response.channel)));
    l.emplace_back(Value::bin(response.rawSocket));
    return Value::pack(Value(std::move(l)));
}

int unpackRequestOrResponse(const string &data, Request *request, Response *response)
{
    Value v;
    try {
        v = Value::unpack(data);
    } catch (...) {
        return GOT_NOTHING;
    }
    if (v.type() != Value::Type::Array) {
        return GOT_NOTHING;
    }
    const vector<Value> &l = v.asArray();
    try {
        if (l.size() == 8) {
            if (l[0].asInt() != 1) {
                return GOT_NOTHING;
            }
            request->id = l[1].asBin();
            request->methodName = l[2].asStr();
            request->args = l[3].getAs<ValueList>();
            request->kwargs = l[4].getAs<ValueMap>();
            request->header = l[5].getAs<ValueMap>();
            request->channel = l[6].isNull() ? 0 : static_cast<std::uint32_t>(l[6].asUint());
            request->rawSocket = l[7].isNull() ? string() : l[7].asBin();
            return GOT_REQUEST;
        }
        if (l.size() == 6) {
            if (l[0].asInt() != 2) {
                return GOT_NOTHING;
            }
            response->id = l[1].asBin();
            response->result = l[2];
            response->exception = l[3];
            response->channel = l[4].isNull() ? 0 : static_cast<std::uint32_t>(l[4].asUint());
            response->rawSocket = l[5].isNull() ? string() : l[5].asBin();
            return GOT_RESPONSE;
        }
    } catch (...) {
        return GOT_NOTHING;
    }
    return GOT_NOTHING;
}

shared_ptr<UseStream> convertUseStream(const Value &v)
{
    if (v.type() == Value::Type::Object) {
        shared_ptr<Serializable> s = v.asObject();
        for (auto &f : detail::useStreamConvertors()) {
            shared_ptr<UseStream> p = f(s);
            if (p) {
                return p;
            }
        }
    }
    return shared_ptr<UseStream>();
}

void raiseRpcRemoteException(const shared_ptr<RpcRemoteException> &e)
{
    if (!e) {
        return;
    }
    for (auto &f : detail::exceptionRaisers()) {
        f(e);
    }
    e->raise();
}

}  // namespace

class PeerPrivate
{
public:
    typedef qtng::ValueEvent<shared_ptr<Response>> Waiter;

    PeerPrivate(const string &name, const shared_ptr<qtng::DataChannel> &channel, const shared_ptr<Rpc> &rpc,
                Peer *parent);
    ~PeerPrivate();
    void shutdown();
    Value call(const string &methodName, const ValueList &args, const ValueMap &kwargs);
    void handlePacket();
    void handleRequest(shared_ptr<Request> request);
    Value lookupAndCall(const string &methodName, const ValueList &args, const ValueMap &kwargs,
                        const ValueMap &header);

    map<string, shared_ptr<Waiter>> waiters;
    string name;
    string address;
    shared_ptr<qtng::DataChannel> channel;
    shared_ptr<Rpc> rpc;
    qtng::CoroutineGroup *operations;
    std::uint64_t nextRequestId;
    bool broken;
    map<string, Value> properties;
    Peer * const q_ptr;
};

PeerPrivate::PeerPrivate(const string &name, const shared_ptr<qtng::DataChannel> &channel,
                         const shared_ptr<Rpc> &rpc, Peer *parent)
    : name(name)
    , channel(channel)
    , rpc(rpc)
    , operations(new qtng::CoroutineGroup())
    , nextRequestId(1)
    , broken(false)
    , q_ptr(parent)
{
    operations->spawn([this] { handlePacket(); });
}

PeerPrivate::~PeerPrivate()
{
    // lightweight teardown: q is being destroyed, no event emission.
    broken = true;
    shared_ptr<Response> emptyResponse(new Response());
    for (auto &w : waiters) {
        w.second->send(emptyResponse);
    }
    waiters.clear();
    if (operations) {
        operations->killall();
        delete operations;
        operations = nullptr;
    }
    if (channel) {
        channel->abort();
    }
}

void PeerPrivate::shutdown()
{
    if (broken) {
        return;
    }
    broken = true;
    shared_ptr<Response> emptyResponse(new Response());
    for (auto &w : waiters) {
        w.second->send(emptyResponse);
    }
    waiters.clear();
    operations->killall();
    channel->abort();

    weak_ptr<Peer> self = q_ptr->weak_from_this();
    shared_ptr<Rpc> rpcSelf = rpc;
    qtng::callInEventLoopAsync([self, rpcSelf] {
        shared_ptr<Peer> peer = self.lock();
        if (!peer) {
            return;
        }
        peer->disconnected.emit(peer.get());
        if (!rpcSelf) {
            return;
        }
        // rpcSelf keeps the Rpc alive here, so the private state is valid even
        // if the Rpc was destroyed before this callback ran.
        rpcSelf->d_func()->removePeer(peer->name(), peer.get());
    });
}

Value PeerPrivate::call(const string &methodName, const ValueList &args, const ValueMap &kwargs)
{
    if (broken || !rpc) {
        throw RpcDisconnectedException("rpc is gone.");
    }
    RpcPrivate *rpcPrivate = rpc->d_func();

    shared_ptr<UseStream> streamFromClient;
    for (const Value &v : args) {
        shared_ptr<UseStream> p = convertUseStream(v);
        if (p) {
            if (streamFromClient) {
                throw RpcInternalException("there are two use stream arguments in " + methodName);
            }
            streamFromClient = p;
        }
    }
    for (const std::pair<const string, Value> &kv : kwargs) {
        shared_ptr<UseStream> p = convertUseStream(kv.second);
        if (p) {
            if (streamFromClient) {
                throw RpcInternalException("there are two use stream arguments in " + methodName);
            }
            streamFromClient = p;
        }
    }

    Request request;
    request.id = qtng::randomBytes(16);
    request.methodName = methodName;
    request.args = args;
    request.kwargs = kwargs;
    if (rpcPrivate->headerCallback) {
        request.header = rpcPrivate->headerCallback->make(q_ptr, methodName);
        if (broken || !rpc) {
            throw RpcDisconnectedException("rpc is gone.");
        }
    }

    if (streamFromClient) {
        shared_ptr<qtng::VirtualChannel> subChannelFromClient = channel->makeChannel();
        if (!subChannelFromClient) {
            throw RpcDisconnectedException("can not make sub channel.");
        }
        shared_ptr<qtng::SocketLike> rawSocket;
        string connectionId;
        if (streamFromClient->preferRawSocket) {
            rawSocket = rpcPrivate->makeRawSocket(name, connectionId);
        }
        streamFromClient->place = UseStream::ClientSide | UseStream::ParamInRequest;
        streamFromClient->channel = subChannelFromClient;
        streamFromClient->rawSocket = rawSocket;
        request.channel = subChannelFromClient->channelNumber();
        request.rawSocket = connectionId;
    }

    const string requestBytes = packRequest(request);
    if (broken || !rpc) {
        throw RpcDisconnectedException("rpc is gone.");
    }

    shared_ptr<Waiter> waiter(new Waiter());
    waiters.insert(make_pair(request.id, waiter));

    bool success = channel->sendPacket(requestBytes);
    if (!success) {
        shutdown();
        throw RpcDisconnectedException("can not send packet.");
    }
    if (broken || !rpc) {
        throw RpcDisconnectedException("rpc is gone.");
    }

    if (streamFromClient) {
        streamFromClient->ready.set();
    }

    shared_ptr<Response> response;
    try {
        response = waiter->tryWait();
        waiters.erase(request.id);
    } catch (...) {
        waiters.erase(request.id);
        throw;
    }

    if (!response || !response->isOk()) {
        throw RpcDisconnectedException("got empty response while waiting response of remote method: " + methodName);
    }
    if (broken || !rpc) {
        throw RpcDisconnectedException("rpc is gone.");
    }

    if (!response->exception.isNull()) {
        shared_ptr<Serializable> e = response->exception.asObject();
        shared_ptr<RpcRemoteException> rre = dynamic_pointer_cast<RpcRemoteException>(e);
        if (rre) {
            raiseRpcRemoteException(rre);
        }
        throw RpcInternalException("unknown exception.");
    }

    shared_ptr<UseStream> streamFromServer = convertUseStream(response->result);
    if (streamFromServer) {
        if (response->channel == 0) {
            throw RpcInternalException("the response of " + methodName + " is a use-stream, but has no channel number.");
        }
        shared_ptr<qtng::VirtualChannel> subChannelFromServer = channel->takeChannel(response->channel);
        if (!subChannelFromServer) {
            throw RpcRemoteException();
        }
        shared_ptr<qtng::SocketLike> rawSocket;
        if (!response->rawSocket.empty()) {
            rawSocket = rpcPrivate->takeRawSocket(response->rawSocket);
        }
        streamFromServer->place = UseStream::ClientSide | UseStream::ValueOfResponse;
        streamFromServer->channel = subChannelFromServer;
        streamFromServer->rawSocket = rawSocket;
        streamFromServer->ready.set();
    } else if (response->channel != 0) {
        // claim and drop to avoid pending channel leaks.
        channel->takeChannel(response->channel);
    }
    return response->result;
}

void PeerPrivate::handlePacket()
{
    while (true) {
        string packet;
        try {
            packet = channel->recvPacket();
        } catch (...) {
            return shutdown();
        }
        if (packet.empty()) {
            return shutdown();
        }
        if (broken || !rpc) {
            return shutdown();
        }

        shared_ptr<Request> request(new Request());
        shared_ptr<Response> response(new Response());
        const int what = unpackRequestOrResponse(packet, request.get(), response.get());
        if (what == GOT_REQUEST && request->isOk()) {
            operations->spawn([this, request] { handleRequest(request); });
        } else if (what == GOT_RESPONSE && response->isOk()) {
            map<string, shared_ptr<Waiter>>::iterator waiter = waiters.find(response->id);
            if (waiter == waiters.end()) {
                if (response->channel != 0) {
                    channel->takeChannel(response->channel);
                }
            } else {
                waiter->second->send(response);
            }
        } else {
            // unrecognized packet: ignore and keep the connection alive.
        }
    }
}

void PeerPrivate::handleRequest(shared_ptr<Request> request)
{
    if (broken || !rpc) {
        return;
    }
    RpcPrivate *rpcPrivate = rpc->d_func();
    if (rpcPrivate->headerCallback) {
        bool success = rpcPrivate->headerCallback->auth(q_ptr, request->methodName, request->header);
        if (!success) {
            return;
        }
    }
    if (broken || !rpc) {
        return;
    }

    shared_ptr<UseStream> streamFromClient;
    for (const Value &v : request->args) {
        streamFromClient = convertUseStream(v);
        if (streamFromClient) {
            break;
        }
    }
    if (!streamFromClient) {
        for (const std::pair<const string, Value> &kv : request->kwargs) {
            streamFromClient = convertUseStream(kv.second);
            if (streamFromClient) {
                break;
            }
        }
    }

    Response response;
    response.id = request->id;

    if (streamFromClient) {
        if (request->channel == 0) {
            response.exception = Value(make_shared<RpcRemoteException>("bad channel"));
        } else {
            shared_ptr<qtng::VirtualChannel> subChannelFromClient = channel->takeChannel(request->channel);
            if (!subChannelFromClient) {
                response.exception = Value(make_shared<RpcRemoteException>("bad channel"));
            } else {
                shared_ptr<qtng::SocketLike> rawSocket;
                if (!request->rawSocket.empty()) {
                    rawSocket = rpcPrivate->takeRawSocket(request->rawSocket);
                }
                streamFromClient->place = UseStream::ServerSide | UseStream::ParamInRequest;
                streamFromClient->channel = subChannelFromClient;
                streamFromClient->rawSocket = rawSocket;
            }
        }
    } else if (request->channel != 0) {
        channel->takeChannel(request->channel);
    }

    if (streamFromClient) {
        streamFromClient->ready.set();
    }

    if (response.exception.isNull()) {
        try {
            response.result = lookupAndCall(request->methodName, request->args, request->kwargs, request->header);
        } catch (qtng::CoroutineException &) {
            throw;
        } catch (RpcRemoteException &e) {
            response.exception = Value(e.clone());
        } catch (...) {
            response.exception = Value(make_shared<RpcRemoteException>("unknown exception caught."));
        }
        if (broken || !rpc) {
            return;
        }
    }

    shared_ptr<UseStream> streamFromServer;
    if (response.exception.isNull()) {
        streamFromServer = convertUseStream(response.result);
    }
    if (streamFromServer) {
        shared_ptr<qtng::VirtualChannel> subChannelFromServer = channel->makeChannel();
        if (broken || !rpc) {
            return;
        }
        if (!subChannelFromServer) {
            response.exception = Value(make_shared<RpcRemoteException>("bad channel"));
        } else {
            shared_ptr<qtng::SocketLike> rawSocket;
            string connectionId;
            if (streamFromServer->preferRawSocket) {
                rawSocket = rpcPrivate->makeRawSocket(name, connectionId);
            }
            streamFromServer->place = UseStream::ServerSide | UseStream::ValueOfResponse;
            streamFromServer->channel = subChannelFromServer;
            streamFromServer->rawSocket = rawSocket;
            response.channel = subChannelFromServer->channelNumber();
            response.rawSocket = connectionId;
            streamFromServer->ready.set();
        }
    }

    if (!response.exception.isNull()) {
        response.result = Value();
    }

    const string responseBytes = packResponse(response);
    if (responseBytes.empty()) {
        return;
    }
    channel->sendPacket(responseBytes);
}

Value PeerPrivate::lookupAndCall(const string &methodName, const ValueList &args, const ValueMap &kwargs,
                                 const ValueMap &header)
{
    RpcPrivate *rpcPrivate = rpc->d_func();

    // 1) plain functions (whole method name).
    map<string, RpcFunction>::iterator fit = rpcPrivate->functions.find(methodName);
    if (fit != rpcPrivate->functions.end()) {
        rpcPrivate->setCurrentPeerAndHeader(q_ptr->shared_from_this(), header);
        struct Cleaner
        {
            RpcPrivate *d;
            ~Cleaner() { d->deleteCurrentPeerAndHeader(); }
        } cleaner{ rpcPrivate };
        (void)cleaner;
        if (rpcPrivate->loggingCallback) {
            rpcPrivate->loggingCallback->calling(q_ptr, methodName, args, kwargs);
            try {
                Value result = fit->second(args, kwargs);
                rpcPrivate->loggingCallback->success(q_ptr, methodName, args, kwargs, result);
                return result;
            } catch (...) {
                rpcPrivate->loggingCallback->failed(q_ptr, methodName, args, kwargs);
                throw;
            }
        }
        return fit->second(args, kwargs);
    }

    // 2) registered service instances: name.method
    const size_t dot = methodName.find('.');
    if (dot == string::npos) {
        throw RpcRemoteException("service method not found: " + methodName);
    }
    const string serviceName = methodName.substr(0, dot);
    const string method = methodName.substr(dot + 1);
    map<string, shared_ptr<Callable>>::iterator sit = rpcPrivate->services.find(serviceName);
    if (sit == rpcPrivate->services.end()) {
        throw RpcRemoteException("service not found: " + serviceName);
    }

    rpcPrivate->setCurrentPeerAndHeader(q_ptr->shared_from_this(), header);
    struct Cleaner
    {
        RpcPrivate *d;
        ~Cleaner() { d->deleteCurrentPeerAndHeader(); }
    } cleaner{ rpcPrivate };
    (void)cleaner;

    if (rpcPrivate->loggingCallback) {
        rpcPrivate->loggingCallback->calling(q_ptr, methodName, args, kwargs);
        try {
            Value result = sit->second->call(method, args, kwargs);
            rpcPrivate->loggingCallback->success(q_ptr, methodName, args, kwargs, result);
            return result;
        } catch (...) {
            rpcPrivate->loggingCallback->failed(q_ptr, methodName, args, kwargs);
            throw;
        }
    }
    return sit->second->call(method, args, kwargs);
}

Peer::Peer(const string &name, const shared_ptr<qtng::DataChannel> &channel, const shared_ptr<Rpc> &rpc)
    : d_ptr(new PeerPrivate(name, channel, rpc, this))
{
}

Peer::~Peer()
{
    delete d_ptr;
}

void Peer::shutdown()
{
    d_ptr->shutdown();
}

bool Peer::isOk() const
{
    return !d_ptr->broken && d_ptr->rpc != nullptr;
}

bool Peer::isActive() const
{
    return !d_ptr->waiters.empty();
}

string Peer::name() const
{
    return d_ptr->name;
}

void Peer::setName(const string &name)
{
    d_ptr->name = name;
}

string Peer::address() const
{
    return d_ptr->address;
}

void Peer::setAddress(const string &address)
{
    d_ptr->address = address;
}

Value Peer::call(const string &method, const ValueList &args, const ValueMap &kwargs)
{
    return d_ptr->call(method, args, kwargs);
}

Value Peer::call(const string &method, const Value &arg1)
{
    ValueList args;
    args.push_back(arg1);
    return d_ptr->call(method, args, ValueMap());
}

Value Peer::call(const string &method, const Value &arg1, const Value &arg2)
{
    ValueList args;
    args.push_back(arg1);
    args.push_back(arg2);
    return d_ptr->call(method, args, ValueMap());
}

Value Peer::call(const string &method, const Value &arg1, const Value &arg2, const Value &arg3)
{
    ValueList args;
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    return d_ptr->call(method, args, ValueMap());
}

Value Peer::call(const string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4)
{
    ValueList args;
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    args.push_back(arg4);
    return d_ptr->call(method, args, ValueMap());
}

Value Peer::call(const string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
                 const Value &arg5)
{
    ValueList args;
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    args.push_back(arg4);
    args.push_back(arg5);
    return d_ptr->call(method, args, ValueMap());
}

Value Peer::call(const string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
                 const Value &arg5, const Value &arg6)
{
    ValueList args;
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    args.push_back(arg4);
    args.push_back(arg5);
    args.push_back(arg6);
    return d_ptr->call(method, args, ValueMap());
}

Value Peer::call(const string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
                 const Value &arg5, const Value &arg6, const Value &arg7)
{
    ValueList args;
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    args.push_back(arg4);
    args.push_back(arg5);
    args.push_back(arg6);
    args.push_back(arg7);
    return d_ptr->call(method, args, ValueMap());
}

Value Peer::call(const string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
                 const Value &arg5, const Value &arg6, const Value &arg7, const Value &arg8)
{
    ValueList args;
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    args.push_back(arg4);
    args.push_back(arg5);
    args.push_back(arg6);
    args.push_back(arg7);
    args.push_back(arg8);
    return d_ptr->call(method, args, ValueMap());
}

Value Peer::call(const string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
                 const Value &arg5, const Value &arg6, const Value &arg7, const Value &arg8, const Value &arg9)
{
    ValueList args;
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    args.push_back(arg4);
    args.push_back(arg5);
    args.push_back(arg6);
    args.push_back(arg7);
    args.push_back(arg8);
    args.push_back(arg9);
    return d_ptr->call(method, args, ValueMap());
}

shared_ptr<qtng::VirtualChannel> Peer::makeChannel()
{
    if (!isOk()) {
        return shared_ptr<qtng::VirtualChannel>();
    }
    return d_ptr->channel->makeChannel();
}

shared_ptr<qtng::VirtualChannel> Peer::takeChannel(uint32_t channelNumber)
{
    if (!isOk()) {
        return shared_ptr<qtng::VirtualChannel>();
    }
    return d_ptr->channel->takeChannel(channelNumber);
}

void Peer::setProperty(const string &key, const Value &v)
{
    d_ptr->properties[key] = v;
}

Value Peer::property(const string &key) const
{
    map<string, Value>::const_iterator it = d_ptr->properties.find(key);
    return it == d_ptr->properties.end() ? Value() : it->second;
}

}  // namespace rpc
}  // namespace qtng
