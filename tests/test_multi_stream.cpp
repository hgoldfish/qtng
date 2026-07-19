#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include "qtng/coroutine.h"
#include "qtng/coroutine_utils.h"
#include "qtng/hostaddress.h"
#include "qtng/multi_stream.h"
#include "qtng/socket.h"

using namespace std;
using namespace qtng;

namespace {

struct ConnectedPair {
    shared_ptr<Socket> serverSide;
    shared_ptr<Socket> clientSide;
};

ConnectedPair makeConnectedPair()
{
    unique_ptr<Socket> listener(Socket::createServer(HostAddress::LocalHost, 0, 1));
    REQUIRE(listener);
    uint16_t port = listener->localPort();
    REQUIRE(port != 0);

    shared_ptr<Socket> client;
    shared_ptr<Socket> accepted;
    shared_ptr<Coroutine> acceptor(Coroutine::spawn([&] {
        accepted.reset(listener->accept());
    }));
    client.reset(Socket::createConnection(HostAddress::LocalHost, port));
    REQUIRE(client);
    acceptor->join();
    REQUIRE(accepted);

    ConnectedPair pair;
    pair.serverSide = accepted;
    pair.clientSide = client;
    return pair;
}

}  // namespace

TEST_CASE("makeSlave/takeSlave pairs a stream", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> remote;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        remote = negative.takeSlave();
    }));

    shared_ptr<MultiStreamSlave> local = positive.makeSlave();
    REQUIRE(local);
    taker->join();
    REQUIRE(remote);
    REQUIRE(local->streamNumber() == remote->streamNumber());
    REQUIRE_FALSE(local->isBroken());
    REQUIRE_FALSE(remote->isBroken());
}

TEST_CASE("both sides can create slaves concurrently", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> positiveOwned;
    shared_ptr<MultiStreamSlave> negativeTaken;
    shared_ptr<MultiStreamSlave> negativeOwned;
    shared_ptr<MultiStreamSlave> positiveTaken;

    shared_ptr<Coroutine> negativeTaker(Coroutine::spawn([&] {
        negativeTaken = negative.takeSlave();
    }));
    shared_ptr<Coroutine> positiveTaker(Coroutine::spawn([&] {
        positiveTaken = positive.takeSlave();
    }));

    positiveOwned = positive.makeSlave();
    negativeOwned = negative.makeSlave();
    REQUIRE(positiveOwned);
    REQUIRE(negativeOwned);

    negativeTaker->join();
    positiveTaker->join();
    REQUIRE(negativeTaken);
    REQUIRE(positiveTaken);
    REQUIRE(positiveOwned->streamNumber() == negativeTaken->streamNumber());
    REQUIRE(negativeOwned->streamNumber() == positiveTaken->streamNumber());
    REQUIRE(positiveOwned->streamNumber() != negativeOwned->streamNumber());
}

TEST_CASE("multiple slaves keep packet boundaries and isolation", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    vector<shared_ptr<MultiStreamSlave>> remotes(3);
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        for (int i = 0; i < 3; ++i) {
            remotes[static_cast<size_t>(i)] = negative.takeSlave();
        }
    }));

    vector<shared_ptr<MultiStreamSlave>> locals;
    for (int i = 0; i < 3; ++i) {
        shared_ptr<MultiStreamSlave> slave = positive.makeSlave();
        REQUIRE(slave);
        locals.push_back(slave);
    }
    taker->join();

    for (int i = 0; i < 3; ++i) {
        REQUIRE(remotes[static_cast<size_t>(i)]);
        string payload = "packet-" + to_string(i);
        REQUIRE(locals[static_cast<size_t>(i)]->sendPacket(payload));
        REQUIRE(remotes[static_cast<size_t>(i)]->recvPacket() == payload);
    }

    REQUIRE(locals[0]->sendPacket("alpha"));
    REQUIRE(locals[2]->sendPacket("gamma"));
    REQUIRE(remotes[2]->recvPacket() == "gamma");
    REQUIRE(remotes[0]->recvPacket() == "alpha");
}

TEST_CASE("asSocketLike supports byte-stream send/recv", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> remote;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        remote = negative.takeSlave();
    }));
    shared_ptr<MultiStreamSlave> local = positive.makeSlave();
    taker->join();
    REQUIRE(local);
    REQUIRE(remote);

    shared_ptr<SocketLike> localSock = asSocketLike(local);
    shared_ptr<SocketLike> remoteSock = asSocketLike(remote);
    REQUIRE(localSock);
    REQUIRE(remoteSock);

    string message(4000, 'x');
    shared_ptr<Coroutine> reader(Coroutine::spawn([&] {
        string got = remoteSock->recvall(static_cast<int32_t>(message.size()));
        REQUIRE(got == message);
    }));
    REQUIRE(localSock->sendall(message) == static_cast<int32_t>(message.size()));
    reader->join();
}

TEST_CASE("aborting one slave does not break siblings", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> remoteA;
    shared_ptr<MultiStreamSlave> remoteB;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        remoteA = negative.takeSlave();
        remoteB = negative.takeSlave();
    }));
    shared_ptr<MultiStreamSlave> localA = positive.makeSlave();
    shared_ptr<MultiStreamSlave> localB = positive.makeSlave();
    taker->join();
    REQUIRE(remoteA);
    REQUIRE(remoteB);

    localA->abort();
    Coroutine::sleep(0.05f);
    REQUIRE(localA->isBroken());

    REQUIRE(localB->sendPacket("still-alive"));
    REQUIRE(remoteB->recvPacket() == "still-alive");
    REQUIRE_FALSE(localB->isBroken());
    REQUIRE_FALSE(positive.isBroken());
    REQUIRE_FALSE(negative.isBroken());
}

TEST_CASE("aborting master closes all slaves", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> remote;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        remote = negative.takeSlave();
    }));
    shared_ptr<MultiStreamSlave> local = positive.makeSlave();
    taker->join();
    REQUIRE(local);
    REQUIRE(remote);

    positive.abort();
    Coroutine::sleep(0.05f);
    REQUIRE(positive.isBroken());
    REQUIRE(local->isBroken());
    REQUIRE(remote->recvPacket().empty());
    REQUIRE(remote->isBroken());
}

TEST_CASE("slave capacity backpressure eventually recovers", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);
    negative.setSlaveReceivingCapacity(256);

    shared_ptr<MultiStreamSlave> remote;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        remote = negative.takeSlave();
    }));
    shared_ptr<MultiStreamSlave> local = positive.makeSlave();
    taker->join();
    REQUIRE(local);
    REQUIRE(remote);
    REQUIRE(remote->receivingCapacity() == 256);

    string chunk(64, 'b');
    for (int i = 0; i < 4; ++i) {
        REQUIRE(local->sendPacket(chunk));
    }
    Coroutine::sleep(0.05f);

    for (int i = 0; i < 4; ++i) {
        REQUIRE(remote->recvPacket() == chunk);
    }
    REQUIRE(local->sendPacket("after-drain"));
    REQUIRE(remote->recvPacket() == "after-drain");
}

TEST_CASE("close flushes sends and discards local receives", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> remote;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        remote = negative.takeSlave();
    }));
    shared_ptr<MultiStreamSlave> local = positive.makeSlave();
    taker->join();
    REQUIRE(local);
    REQUIRE(remote);

    vector<string> received;
    shared_ptr<Coroutine> reader(Coroutine::spawn([&] {
        while (true) {
            string packet = remote->recvPacket();
            if (packet.empty()) {
                break;
            }
            received.push_back(std::move(packet));
        }
    }));
    Coroutine::sleep(0.01f);

    REQUIRE(remote->sendPacket("discard-on-close"));
    Coroutine::sleep(0.05f);
    REQUIRE(local->receivingQueueSize() > 0);

    REQUIRE(local->sendPacketAsync("one"));
    REQUIRE(local->sendPacketAsync("two"));
    REQUIRE(local->sendPacketAsync("three"));
    local->close();
    REQUIRE(local->isBroken());
    REQUIRE_FALSE(local->isClosing());
    REQUIRE_FALSE(local->sendPacket("after-close"));
    REQUIRE(local->receivingQueueSize() == 0);
    REQUIRE(local->recvPacket().empty());

    Coroutine::sleep(0.05f);
    REQUIRE(remote->isBroken());
    reader->join();
    REQUIRE(received.empty());
    REQUIRE(remote->isBroken());
    REQUIRE(remote->error() == MultiStreamMaster::RemotePeerClosedError);
    REQUIRE(remote->resetCode() == MultiStreamResetNormalClose);
}

TEST_CASE("abort drops receive queue and still notifies peer", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> remote;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        remote = negative.takeSlave();
    }));
    shared_ptr<MultiStreamSlave> local = positive.makeSlave();
    taker->join();
    REQUIRE(local);
    REQUIRE(remote);

    REQUIRE(remote->sendPacket("buffered"));
    Coroutine::sleep(0.05f);
    REQUIRE(local->receivingQueueSize() > 0);

    local->abort();
    REQUIRE(local->isBroken());
    REQUIRE(local->receivingQueueSize() == 0);
    REQUIRE(local->recvPacket().empty());

    REQUIRE(remote->recvPacket().empty());
    REQUIRE(remote->isBroken());
    REQUIRE(remote->resetCode() == MultiStreamResetAbort);
}

TEST_CASE("higher priority slave is preferred over a busy low-priority flood", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> remoteLow;
    shared_ptr<MultiStreamSlave> remoteHigh;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        remoteLow = negative.takeSlave();
        remoteHigh = negative.takeSlave();
    }));
    shared_ptr<MultiStreamSlave> localLow = positive.makeSlave();
    shared_ptr<MultiStreamSlave> localHigh = positive.makeSlave();
    taker->join();
    REQUIRE(remoteLow);
    REQUIRE(remoteHigh);

    localLow->setPriority(0);
    localHigh->setPriority(10);
    REQUIRE(localHigh->priority() == 10);

    string highSeen;
    shared_ptr<Coroutine> highReader(Coroutine::spawn([&] {
        highSeen = remoteHigh->recvPacket();
    }));

    for (int i = 0; i < 200; ++i) {
        REQUIRE(localLow->sendPacketAsync(string(200, 'L')));
    }
    REQUIRE(localHigh->sendPacket("high-hello"));
    highReader->join();
    REQUIRE(highSeen == "high-hello");
}

TEST_CASE("equal priority weighted round-robin avoids starvation", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> remoteBusy;
    shared_ptr<MultiStreamSlave> remoteQuiet;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        remoteBusy = negative.takeSlave();
        remoteQuiet = negative.takeSlave();
    }));
    shared_ptr<MultiStreamSlave> localBusy = positive.makeSlave();
    shared_ptr<MultiStreamSlave> localQuiet = positive.makeSlave();
    taker->join();
    REQUIRE(remoteBusy);
    REQUIRE(remoteQuiet);

    string quietSeen;
    shared_ptr<Coroutine> quietReader(Coroutine::spawn([&] {
        quietSeen = remoteQuiet->recvPacket();
    }));

    for (int i = 0; i < 200; ++i) {
        REQUIRE(localBusy->sendPacketAsync(string(200, 'B')));
    }
    REQUIRE(localQuiet->sendPacket("quiet-hello"));
    quietReader->join();
    REQUIRE(quietSeen == "quiet-hello");
}

TEST_CASE("takeSlave by stream number", "[multistream]")
{
    ConnectedPair sockets = makeConnectedPair();
    MultiStreamMaster positive(sockets.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative(sockets.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> first = positive.makeSlave();
    shared_ptr<MultiStreamSlave> second = positive.makeSlave();
    REQUIRE(first);
    REQUIRE(second);

    shared_ptr<MultiStreamSlave> takenFirst;
    shared_ptr<MultiStreamSlave> takenSecond;
    shared_ptr<Coroutine> taker(Coroutine::spawn([&] {
        // Drain into pending, then pick by number out of order.
        takenFirst = negative.takeSlave();
        takenSecond = negative.takeSlave();
    }));
    taker->join();
    REQUIRE(takenFirst);
    REQUIRE(takenSecond);

    // Recreate and exercise takeSlave(streamNumber) against pending queue.
    ConnectedPair sockets2 = makeConnectedPair();
    MultiStreamMaster positive2(sockets2.clientSide, MultiStreamPositivePole);
    MultiStreamMaster negative2(sockets2.serverSide, MultiStreamNegativePole);

    shared_ptr<MultiStreamSlave> a = positive2.makeSlave();
    shared_ptr<MultiStreamSlave> b = positive2.makeSlave();
    REQUIRE(a);
    REQUIRE(b);

    // Wait until both MAKE requests are processed into pending.
    for (int i = 0; i < 50; ++i) {
        shared_ptr<MultiStreamSlave> maybe = negative2.takeSlave(b->streamNumber());
        if (maybe) {
            REQUIRE(maybe->streamNumber() == b->streamNumber());
            shared_ptr<MultiStreamSlave> other = negative2.takeSlave();
            REQUIRE(other);
            REQUIRE(other->streamNumber() == a->streamNumber());
            return;
        }
        Coroutine::sleep(0.01f);
    }
    FAIL("timed out waiting for pending slave");
}
