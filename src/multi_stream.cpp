#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef QTNG_NO_CRYPTO
#  include "qtng/ssl.h"
#endif

#include "qtng/coroutine_utils.h"
#include "qtng/udp.h"
#include "qtng/multi_stream.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/string_utils.h"

using namespace std;

NG_LOGGER("qtng.multi_stream");

namespace qtng {

namespace {

const uint8_t MAKE_SLAVE_REQUEST = 1;
const uint8_t SLAVE_MADE_REQUEST = 2;
const uint8_t RESET_SLAVE_REQUEST = 3;
const uint8_t WINDOW_UPDATE_REQUEST = 4;
// 5 reserved (was GO_THROUGH)
const uint8_t KEEPALIVE_REQUEST = 6;

const uint32_t CommandStreamNumber = 0;
const uint32_t FrameHeaderSize = sizeof(uint32_t) + sizeof(uint32_t);  // payloadSize + streamNumber
const uint32_t DefaultPacketSize = 1024 * 64;
const uint32_t DefaultPayloadSize = 1400;
const uint32_t DefaultSlaveReceivingCapacity = 128 * 1024;
const uint32_t DefaultSlaveSendingCapacity = 64 * 1024;
const uint32_t WindowUpdateMinBytes = 8 * 1024;

enum class BlockFlag {
    NonBlock,
    Block_And_Not_Wait_Sent,
    Block_Until_Sent,
};

string packMakeSlaveRequest(uint32_t streamNumber, uint32_t initialWindow)
{
    uint8_t buf[sizeof(uint8_t) + sizeof(uint32_t) * 2];
    ngToBigEndian(MAKE_SLAVE_REQUEST, buf);
    ngToBigEndian(streamNumber, buf + sizeof(uint8_t));
    ngToBigEndian(initialWindow, buf + sizeof(uint8_t) + sizeof(uint32_t));
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

string packSlaveMadeRequest(uint32_t streamNumber, uint32_t initialWindow)
{
    uint8_t buf[sizeof(uint8_t) + sizeof(uint32_t) * 2];
    ngToBigEndian(SLAVE_MADE_REQUEST, buf);
    ngToBigEndian(streamNumber, buf + sizeof(uint8_t));
    ngToBigEndian(initialWindow, buf + sizeof(uint8_t) + sizeof(uint32_t));
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

string packResetSlaveRequest(uint32_t streamNumber, MultiStreamResetCode resetCode)
{
    uint8_t buf[sizeof(uint8_t) + sizeof(uint32_t) * 2];
    ngToBigEndian(RESET_SLAVE_REQUEST, buf);
    ngToBigEndian(streamNumber, buf + sizeof(uint8_t));
    ngToBigEndian(static_cast<uint32_t>(resetCode), buf + sizeof(uint8_t) + sizeof(uint32_t));
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

string packWindowUpdateRequest(uint32_t streamNumber, uint32_t credit)
{
    uint8_t buf[sizeof(uint8_t) + sizeof(uint32_t) * 2];
    ngToBigEndian(WINDOW_UPDATE_REQUEST, buf);
    ngToBigEndian(streamNumber, buf + sizeof(uint8_t));
    ngToBigEndian(credit, buf + sizeof(uint8_t) + sizeof(uint32_t));
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

string packKeepaliveRequest()
{
    uint8_t buf[sizeof(uint8_t)];
    ngToBigEndian(KEEPALIVE_REQUEST, buf);
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

bool unpackCommand(const string &data, uint8_t *command, uint32_t *streamNumber, uint32_t *arg)
{
    if (data.size() == (sizeof(uint8_t) + sizeof(uint32_t) * 2)) {
        *command = ngFromBigEndian<uint8_t>(data.data());
        if (*command != MAKE_SLAVE_REQUEST && *command != SLAVE_MADE_REQUEST && *command != RESET_SLAVE_REQUEST
            && *command != WINDOW_UPDATE_REQUEST) {
            return false;
        }
        *streamNumber = ngFromBigEndian<uint32_t>(data.data() + sizeof(uint8_t));
        *arg = ngFromBigEndian<uint32_t>(data.data() + sizeof(uint8_t) + sizeof(uint32_t));
        return true;
    }
    if (data.size() == sizeof(uint8_t)) {
        *command = ngFromBigEndian<uint8_t>(data.data());
        if (*command != KEEPALIVE_REQUEST) {
            return false;
        }
        *streamNumber = 0;
        *arg = 0;
        return true;
    }
    return false;
}

string errorToString(MultiStreamMaster::StreamError error)
{
    switch (error) {
    case MultiStreamMaster::RemotePeerClosedError:
        return "The remote peer closed the connection";
    case MultiStreamMaster::KeepaliveTimeoutError:
        return "The remote peer didn't send keepalive packet for a long time.";
    case MultiStreamMaster::ReceivingError:
        return "Can not receive packet from remote peer";
    case MultiStreamMaster::SendingError:
        return "Can not send packet to remote peer.";
    case MultiStreamMaster::InvalidPacket:
        return "Can not parse packet header.";
    case MultiStreamMaster::InvalidCommand:
        return "Can not parse command or unknown command.";
    case MultiStreamMaster::UserShutdown:
        return "Programmer shutdown stream manually.";
    case MultiStreamMaster::PacketTooLarge:
        return "The packet is too large.";
    case MultiStreamMaster::UnknownError:
        return "Caught unknown error.";
    case MultiStreamMaster::ProgrammingError:
        return "The QtNetwork programmer do a stupid thing.";
    case MultiStreamMaster::NoError:
        return string();
    default:
        NG_UNREACHABLE();
    }
}

string resetCodeToString(MultiStreamResetCode code)
{
    switch (code) {
    case MultiStreamResetNormalClose:
        return "normal close";
    case MultiStreamResetAbort:
        return "abort";
    case MultiStreamResetProtocolError:
        return "protocol error";
    case MultiStreamResetRefused:
        return "refused";
    default:
        return "unknown reset";
    }
}

// Empty packet marks a graceful-close barrier: all prior data for this slave
// has been drained from the sending queue; doSend appends RESET into the current
// batch, then processCloseBarrier() tears down local slave state.
// Real payloads are never empty (sendPacketRaw rejects empty input).
class WritingPacket
{
public:
    WritingPacket() = default;
    WritingPacket(string packet, shared_ptr<ValueEvent<bool>> done)
        : packet(std::move(packet))
        , done(done)
    {
    }
    static WritingPacket makeCloseBarrier(shared_ptr<ValueEvent<bool>> done)
    {
        WritingPacket writing;
        writing.done = done;
        return writing;
    }

    string packet;
    shared_ptr<ValueEvent<bool>> done;
    uint32_t size() const
    {
        // Close barrier still occupies one unit of queue capacity.
        if (packet.empty()) {
            return 1;
        }
        return static_cast<uint32_t>(packet.size());
    }
    bool isValid() const
    {
        return !packet.empty() || static_cast<bool>(done);
    }
    bool isCloseBarrier() const
    {
        return packet.empty() && static_cast<bool>(done);
    }
};

}  // namespace

class MultiStreamSlavePrivate
{
public:
    MultiStreamSlavePrivate(MultiStreamMaster *master, MultiStreamPole pole, uint32_t streamNumber,
                            MultiStreamSlave *parent);
    ~MultiStreamSlavePrivate();

    void abort(MultiStreamMaster::StreamError reason);
    void handleIncomingPacket(string payload);
    void wakeReceivers()
    {
        for (uint32_t i = 0; i < activeReceivers; ++i) {
            receivingQueue.putForcedly(string());
        }
        creditEvent.set();
    }
    void finish(MultiStreamMaster::StreamError reason, bool discardPendingSends);
    void addSendCredit(uint32_t credit);
    bool waitForSendCredit(uint32_t bytes, BlockFlag blocking);
    void releaseSendCredit(uint32_t bytes);
    void cleanSendingQueue();
    void maybeSendWindowUpdate(uint32_t consumedBytes);
    int wrrWeight() const { return priority >= 0 ? priority + 1 : 1; }

    MultiStreamMaster *master;
    MultiStreamPole pole;
    uint32_t streamNumber;
    string name;
    SizedQueue<string> receivingQueue;
    SizedQueue<WritingPacket> sendingQueue;
    uint32_t activeReceivers = 0;
    uint32_t sendCredit = 0;
    uint32_t windowUpdatePending = 0;
    Event creditEvent;
    bool closing = false;
    int priority = 0;
    int wrrQuantum = 0;
    MultiStreamMaster::StreamError error;
    MultiStreamResetCode resetCode;
    shared_ptr<ValueEvent<bool>> closeDone;

    MultiStreamSlave * const q_ptr;
    NG_DECLARE_PUBLIC(MultiStreamSlave)

    static MultiStreamSlavePrivate *getPrivateHelper(const shared_ptr<MultiStreamSlave> &slave)
    {
        return slave ? slave->d_func() : nullptr;
    }
};

class MultiStreamMasterPrivate
{
public:
    MultiStreamMasterPrivate(shared_ptr<SocketLike> connection, MultiStreamPole pole, MultiStreamMaster *parent);
    ~MultiStreamMasterPrivate();

    shared_ptr<MultiStreamSlave> makeSlaveInternal(MultiStreamPole pole, uint32_t streamNumber);
    shared_ptr<MultiStreamSlave> peekSlave(uint32_t streamNumber);

    void abort(MultiStreamMaster::StreamError reason);
    bool isBroken() const { return error != MultiStreamMaster::NoError || !connection->isValid(); }
    bool sendPacketRaw(uint32_t streamNumber, string packet, BlockFlag blocking);
    bool enqueueCloseBarrier(uint32_t streamNumber, shared_ptr<ValueEvent<bool>> done);
    void enqueueCommand(string packet);
    bool enqueueSlavePacket(MultiStreamSlavePrivate *slavePrivate, WritingPacket packet, BlockFlag blocking);
    void notifyHasData() { hasData.set(); }
    bool hasSendableData();
    shared_ptr<MultiStreamSlave> findNextSlaveWithData(uint32_t afterStreamNumber);
    void cleanSlave(uint32_t streamNumber, bool sendResetPacket,
                    MultiStreamResetCode resetCode = MultiStreamResetAbort);
    void notifySlaveClose(uint32_t streamNumber, MultiStreamResetCode resetCode = MultiStreamResetAbort)
    {
        if (error != MultiStreamMaster::NoError) {
            return;
        }
        enqueueCommand(packResetSlaveRequest(streamNumber, resetCode));
    }
    // Remove slave from the master map and finalize local state after RESET is queued in buf.
    void processCloseBarrier(uint32_t streamNumber);
    bool handleCommand(const string &packet);
    MultiStreamMaster::StreamError handleIncomingPacket(uint32_t streamNumber, string payload);

    void doSend();
    void doReceive();
    void doKeepalive();

    const shared_ptr<SocketLike> connection;
    MultiStreamPole pole;
    uint32_t nextStreamNumber;
    map<uint32_t, weak_ptr<MultiStreamSlave>> slaves;
    deque<shared_ptr<MultiStreamSlave>> pendingSlaves;
    Condition pendingSlavesNotEmpty;
    SizedQueue<string> commandQueue;
    Event hasData;
    uint32_t lastRrStreamNumber;
    CoroutineGroup *operations;
    uint32_t _maxPayloadSize;
    uint32_t _payloadSizeHint;
    uint32_t _slaveReceivingCapacity;
    uint32_t _slaveSendingCapacity;
    int64_t lastActiveTimestamp;
    int64_t lastKeepaliveTimestamp;
    int64_t keepaliveTimeout;
    int64_t keepaliveInterval;
    string name;
    MultiStreamMaster::StreamError error;

    MultiStreamMaster * const q_ptr;
    NG_DECLARE_PUBLIC(MultiStreamMaster)

    inline static MultiStreamMasterPrivate *getPrivateHelper(MultiStreamMaster *master)
    {
        return master ? master->d_func() : nullptr;
    }
};

MultiStreamSlavePrivate::MultiStreamSlavePrivate(MultiStreamMaster *master, MultiStreamPole pole, uint32_t streamNumber,
                                                 MultiStreamSlave *parent)
    : master(master)
    , pole(pole)
    , streamNumber(streamNumber)
    , receivingQueue(MultiStreamMasterPrivate::getPrivateHelper(master)->_slaveReceivingCapacity)
    , sendingQueue(MultiStreamMasterPrivate::getPrivateHelper(master)->_slaveSendingCapacity)
    , sendCredit(0)
    , error(MultiStreamMaster::NoError)
    , resetCode(MultiStreamResetNormalClose)
    , q_ptr(parent)
{
}

MultiStreamSlavePrivate::~MultiStreamSlavePrivate()
{
    abort(MultiStreamMaster::UserShutdown);
}

void MultiStreamSlavePrivate::finish(MultiStreamMaster::StreamError reason, bool discardPendingSends)
{
    if (error == MultiStreamMaster::NoError) {
        error = reason;
    }
    closing = false;
    master = nullptr;
    if (discardPendingSends) {
        cleanSendingQueue();
    }
    receivingQueue.clear();
    wakeReceivers();
    if (closeDone) {
        shared_ptr<ValueEvent<bool>> done = closeDone;
        closeDone.reset();
        if (!done->isSet()) {
            done->send(false);
        }
    }
}

void MultiStreamSlavePrivate::abort(MultiStreamMaster::StreamError reason)
{
    if (error != MultiStreamMaster::NoError) {
        return;
    }
    if (master) {
        MultiStreamMasterPrivate::getPrivateHelper(master)->cleanSlave(streamNumber, true, MultiStreamResetAbort);
    }
    finish(reason, true);
}

void MultiStreamSlavePrivate::addSendCredit(uint32_t credit)
{
    if (credit == 0) {
        return;
    }
    sendCredit += credit;
    creditEvent.set();
}

bool MultiStreamSlavePrivate::waitForSendCredit(uint32_t bytes, BlockFlag blocking)
{
    while (sendCredit < bytes) {
        if (blocking == BlockFlag::NonBlock) {
            return false;
        }
        if (closing || error != MultiStreamMaster::NoError || !master) {
            return false;
        }
        creditEvent.clear();
        if (sendCredit >= bytes) {
            break;
        }
        if (!creditEvent.tryWait()) {
            return false;
        }
    }
    return true;
}

void MultiStreamSlavePrivate::releaseSendCredit(uint32_t bytes)
{
    sendCredit += bytes;
    creditEvent.set();
}

void MultiStreamSlavePrivate::cleanSendingQueue()
{
    while (!sendingQueue.isEmpty()) {
        WritingPacket writingPacket = sendingQueue.get();
        if (!writingPacket.done) {
            continue;
        }
        if (writingPacket.done == closeDone) {
            closeDone.reset();
        }
        if (!writingPacket.done->isSet()) {
            writingPacket.done->send(false);
        }
    }
    // Wake senders waiting for credit; they will observe closing/error/master.
    creditEvent.set();
}

void MultiStreamSlavePrivate::maybeSendWindowUpdate(uint32_t consumedBytes)
{
    if (consumedBytes == 0 || !master || error != MultiStreamMaster::NoError) {
        return;
    }
    windowUpdatePending += consumedBytes;
    const uint32_t threshold = max(WindowUpdateMinBytes, receivingQueue.capacity() / 8);
    if (windowUpdatePending >= threshold || receivingQueue.isEmpty()) {
        uint32_t credit = windowUpdatePending;
        windowUpdatePending = 0;
        MultiStreamMasterPrivate::getPrivateHelper(master)->enqueueCommand(
                packWindowUpdateRequest(streamNumber, credit));
    }
}

void MultiStreamSlavePrivate::handleIncomingPacket(string payload)
{
    if (error != MultiStreamMaster::NoError) {
        return;
    }
    receivingQueue.put(std::move(payload));
}

MultiStreamMasterPrivate::MultiStreamMasterPrivate(shared_ptr<SocketLike> connection, MultiStreamPole pole,
                                                   MultiStreamMaster *parent)
    : connection(connection)
    , pole(pole)
    , commandQueue(64 * 1024)
    , lastRrStreamNumber(0)
    , operations(new CoroutineGroup())
    , _maxPayloadSize(DefaultPacketSize - FrameHeaderSize)
    , _payloadSizeHint(DefaultPayloadSize)
    , _slaveReceivingCapacity(DefaultSlaveReceivingCapacity)
    , _slaveSendingCapacity(DefaultSlaveSendingCapacity)
    , lastActiveTimestamp(utils::DateTime::currentMSecsSinceEpoch())
    , lastKeepaliveTimestamp(lastActiveTimestamp)
    , keepaliveTimeout(-1)
    , keepaliveInterval(1000 * 2)
    , error(MultiStreamMaster::NoError)
    , q_ptr(parent)
{
    if (pole == MultiStreamNegativePole) {
        nextStreamNumber = 0xffffffff;
    } else {
        nextStreamNumber = 1;
    }
    operations->spawnWithName("receiving", [this] { this->doReceive(); });
    operations->spawnWithName("sending", [this] { this->doSend(); });
    operations->spawnWithName("keepalive", [this] { this->doKeepalive(); });
}

MultiStreamMasterPrivate::~MultiStreamMasterPrivate()
{
    abort(MultiStreamMaster::UserShutdown);
    delete operations;
}

shared_ptr<MultiStreamSlave> MultiStreamMasterPrivate::makeSlaveInternal(MultiStreamPole pole, uint32_t streamNumber)
{
    shared_ptr<MultiStreamSlave> slave(new MultiStreamSlave(q_ptr, pole, streamNumber));
    slaves[streamNumber] = slave;
    return slave;
}

shared_ptr<MultiStreamSlave> MultiStreamMasterPrivate::peekSlave(uint32_t streamNumber)
{
    if (isBroken()) {
        return shared_ptr<MultiStreamSlave>();
    }
    for (size_t i = 0; i < pendingSlaves.size(); ++i) {
        shared_ptr<MultiStreamSlave> slave = pendingSlaves[i];
        if (slave && slave->streamNumber() == streamNumber) {
            return slave;
        }
    }
    return shared_ptr<MultiStreamSlave>();
}

bool MultiStreamMasterPrivate::hasSendableData()
{
    if (!commandQueue.isEmpty()) {
        return true;
    }
    for (const auto &item : slaves) {
        shared_ptr<MultiStreamSlave> slave = item.second.lock();
        if (slave && !slave->d_func()->sendingQueue.isEmpty()) {
            return true;
        }
    }
    return false;
}

shared_ptr<MultiStreamSlave> MultiStreamMasterPrivate::findNextSlaveWithData(uint32_t afterStreamNumber)
{
    if (slaves.empty()) {
        return shared_ptr<MultiStreamSlave>();
    }

    auto hasWork = [](MultiStreamSlavePrivate *sp) {
        return sp && !sp->sendingQueue.isEmpty();
    };

    // Refill WRR quanta when every busy slave has exhausted its quantum.
    bool anyBusy = false;
    bool anyWithQuantum = false;
    int maxPriority = INT_MIN;
    for (const auto &item : slaves) {
        shared_ptr<MultiStreamSlave> slave = item.second.lock();
        if (!slave) {
            continue;
        }
        MultiStreamSlavePrivate *sp = slave->d_func();
        if (!hasWork(sp)) {
            continue;
        }
        anyBusy = true;
        if (sp->wrrQuantum > 0) {
            anyWithQuantum = true;
            maxPriority = max(maxPriority, sp->priority);
        }
    }
    if (!anyBusy) {
        return shared_ptr<MultiStreamSlave>();
    }
    if (!anyWithQuantum) {
        maxPriority = INT_MIN;
        for (const auto &item : slaves) {
            shared_ptr<MultiStreamSlave> slave = item.second.lock();
            if (!slave) {
                continue;
            }
            MultiStreamSlavePrivate *sp = slave->d_func();
            if (!hasWork(sp)) {
                continue;
            }
            sp->wrrQuantum = sp->wrrWeight();
            maxPriority = max(maxPriority, sp->priority);
        }
    }

    auto tryFrom = [this, maxPriority, &hasWork](
                           map<uint32_t, weak_ptr<MultiStreamSlave>>::iterator begin,
                           map<uint32_t, weak_ptr<MultiStreamSlave>>::iterator end) -> shared_ptr<MultiStreamSlave> {
        for (auto it = begin; it != end; ++it) {
            shared_ptr<MultiStreamSlave> slave = it->second.lock();
            if (!slave) {
                continue;
            }
            MultiStreamSlavePrivate *sp = slave->d_func();
            if (hasWork(sp) && sp->priority == maxPriority && sp->wrrQuantum > 0) {
                return slave;
            }
        }
        return shared_ptr<MultiStreamSlave>();
    };

    shared_ptr<MultiStreamSlave> found = tryFrom(slaves.upper_bound(afterStreamNumber), slaves.end());
    if (found) {
        return found;
    }
    return tryFrom(slaves.begin(), slaves.upper_bound(afterStreamNumber));
}

void MultiStreamMasterPrivate::enqueueCommand(string packet)
{
    if (error != MultiStreamMaster::NoError || packet.empty()) {
        return;
    }
    if (static_cast<uint32_t>(packet.size()) > _maxPayloadSize) {
        return;
    }
    commandQueue.putForcedly(std::move(packet));
    notifyHasData();
}

bool MultiStreamMasterPrivate::enqueueSlavePacket(MultiStreamSlavePrivate *slavePrivate, WritingPacket packet,
                                                  BlockFlag blocking)
{
    if (!slavePrivate) {
        return false;
    }
    shared_ptr<ValueEvent<bool>> done = packet.done;
    switch (blocking) {
    case BlockFlag::NonBlock:
        slavePrivate->sendingQueue.putForcedly(std::move(packet));
        notifyHasData();
        return true;
    case BlockFlag::Block_And_Not_Wait_Sent:
        if (!slavePrivate->sendingQueue.put(std::move(packet))) {
            return false;
        }
        notifyHasData();
        return true;
    case BlockFlag::Block_Until_Sent: {
        if (!done) {
            return false;
        }
        if (!slavePrivate->sendingQueue.put(std::move(packet))) {
            return false;
        }
        notifyHasData();
        return done->tryWait();
    }
    default:
        NG_UNREACHABLE();
        break;
    }
}

bool MultiStreamMasterPrivate::sendPacketRaw(uint32_t streamNumber, string packet, BlockFlag blocking)
{
    if (error != MultiStreamMaster::NoError || packet.empty()) {
        return false;
    }
    if (static_cast<uint32_t>(packet.size()) > _maxPayloadSize) {
        return false;
    }
    if (streamNumber == CommandStreamNumber) {
        return false;
    }

    auto it = slaves.find(streamNumber);
    if (it == slaves.end()) {
        return false;
    }
    shared_ptr<MultiStreamSlave> slave = it->second.lock();
    if (!slave) {
        slaves.erase(it);
        return false;
    }
    MultiStreamSlavePrivate *slavePrivate = slave->d_func();
    const uint32_t bytes = static_cast<uint32_t>(packet.size());
    if (!slavePrivate->waitForSendCredit(bytes, blocking)) {
        return false;
    }
    slavePrivate->sendCredit -= bytes;
    shared_ptr<ValueEvent<bool>> done;
    if (blocking == BlockFlag::Block_Until_Sent) {
        done = make_shared<ValueEvent<bool>>();
    }
    if (!enqueueSlavePacket(slavePrivate, WritingPacket(std::move(packet), done), blocking)) {
        slavePrivate->releaseSendCredit(bytes);
        return false;
    }
    return true;
}

bool MultiStreamMasterPrivate::enqueueCloseBarrier(uint32_t streamNumber, shared_ptr<ValueEvent<bool>> done)
{
    if (error != MultiStreamMaster::NoError || !done) {
        return false;
    }
    auto it = slaves.find(streamNumber);
    if (it == slaves.end()) {
        return false;
    }
    shared_ptr<MultiStreamSlave> slave = it->second.lock();
    if (!slave) {
        slaves.erase(it);
        return false;
    }
    return enqueueSlavePacket(slave->d_func(), WritingPacket::makeCloseBarrier(done),
                              BlockFlag::Block_And_Not_Wait_Sent);
}

void MultiStreamMasterPrivate::processCloseBarrier(uint32_t streamNumber)
{
    shared_ptr<MultiStreamSlave> strong;
    auto it = slaves.find(streamNumber);
    if (it != slaves.end()) {
        strong = it->second.lock();
        slaves.erase(it);
    }
    if (!strong) {
        return;
    }
    MultiStreamSlavePrivate *slavePrivate = strong->d_func();
    // close() waits via the barrier's done in doSend's dones list (after RESET is sent).
    slavePrivate->closeDone.reset();
    slavePrivate->finish(MultiStreamMaster::UserShutdown, false);
}

void MultiStreamMasterPrivate::doSend()
{
    const int headSize = static_cast<int>(FrameHeaderSize);
    string buf;
    while (true) {
        try {
            while (error == MultiStreamMaster::NoError && !hasSendableData()) {
                hasData.clear();
                if (hasSendableData()) {
                    break;
                }
                if (!hasData.tryWait()) {
                    return;
                }
            }
        } catch (CoroutineExitException) {
            assert(error != MultiStreamMaster::NoError);
            return;
        } catch (...) {
            return abort(MultiStreamMaster::UnknownError);
        }
        if (error != MultiStreamMaster::NoError) {
            return;
        }

        const int maxSendSize = static_cast<int>(q_ptr->maxPacketSize());
        if (static_cast<int>(buf.size()) != maxSendSize) {
            buf.resize(static_cast<size_t>(maxSendSize), '\0');
        }

        vector<shared_ptr<ValueEvent<bool>>> dones;
        bool sendSucceeded = false;
        int count = 0;
        auto clean = shared_ptr<void>(nullptr, [&dones, &sendSucceeded](void *) {
            if (!sendSucceeded) {
                for (shared_ptr<ValueEvent<bool>> &done : dones) {
                    if (done) {
                        done->send(false);
                    }
                }
            }
        });

        auto appendFrame = [&](uint32_t streamNumber, const string &packet) {
            char *p = &buf[static_cast<size_t>(count)];
            ngToBigEndian<uint32_t>(static_cast<uint32_t>(packet.size()), p);
            p += sizeof(uint32_t);
            ngToBigEndian<uint32_t>(streamNumber, p);
            p += sizeof(uint32_t);
            memcpy(p, packet.data(), packet.size());
            count += headSize + static_cast<int>(packet.size());
        };

        try {
            // Commands first. Graceful RESET is appended directly when a close barrier is
            // reached (count == 0); it can share this batch with later frames.
            while (count < maxSendSize) {
                if (!commandQueue.isEmpty()) {
                    string next = commandQueue.get();
                    const int packetWireSize = headSize + static_cast<int>(next.size());
                    if (count > 0 && count + packetWireSize > maxSendSize) {
                        commandQueue.returnsForcely(std::move(next));
                        break;
                    }
                    appendFrame(CommandStreamNumber, next);
                    continue;
                }

                shared_ptr<MultiStreamSlave> slave = findNextSlaveWithData(lastRrStreamNumber);
                if (!slave) {
                    break;
                }
                MultiStreamSlavePrivate *slavePrivate = slave->d_func();
                WritingPacket next = slavePrivate->sendingQueue.get();
                if (!next.isValid()) {
                    continue;
                }
                if (next.isCloseBarrier()) {
                    const string resetPacket =
                            packResetSlaveRequest(slave->streamNumber(), MultiStreamResetNormalClose);
                    const int resetWireSize = headSize + static_cast<int>(resetPacket.size());
                    if (count > 0 && count + resetWireSize > maxSendSize) {
                        // Preserve the barrier until RESET fits in the next batch.
                        slavePrivate->sendingQueue.returnsForcely(std::move(next));
                        break;
                    }
                    lastRrStreamNumber = slave->streamNumber();
                    if (slavePrivate->wrrQuantum > 0) {
                        --slavePrivate->wrrQuantum;
                    }
                    if (error == MultiStreamMaster::NoError) {
                        if (next.done) {
                            dones.push_back(next.done);
                        }
                        appendFrame(CommandStreamNumber, resetPacket);
                    } else if (next.done) {
                        next.done->send(false);
                    }
                    processCloseBarrier(slave->streamNumber());
                    continue;
                }
                const int packetWireSize = headSize + static_cast<int>(next.packet.size());
                if (count > 0 && count + packetWireSize > maxSendSize) {
                    slavePrivate->sendingQueue.returnsForcely(std::move(next));
                    break;
                }
                lastRrStreamNumber = slave->streamNumber();
                if (slavePrivate->wrrQuantum > 0) {
                    --slavePrivate->wrrQuantum;
                }
                if (next.done) {
                    dones.push_back(next.done);
                }
                appendFrame(slave->streamNumber(), next.packet);
            }
        } catch (CoroutineExitException) {
            assert(error != MultiStreamMaster::NoError);
            return;
        } catch (...) {
            return abort(MultiStreamMaster::UnknownError);
        }

        if (count == 0) {
            continue;
        }
        if (error != MultiStreamMaster::NoError) {
            return;
        }

        int sentBytes;
        try {
            sentBytes = connection->sendall(buf.data(), count);
        } catch (CoroutineExitException) {
            assert(error != MultiStreamMaster::NoError);
            return;
        } catch (...) {
            return abort(MultiStreamMaster::UnknownError);
        }

        if (sentBytes == count) {
            sendSucceeded = true;
            for (shared_ptr<ValueEvent<bool>> &done : dones) {
                if (done) {
                    done->send(true);
                }
            }
            lastKeepaliveTimestamp = utils::DateTime::currentMSecsSinceEpoch();
        } else {
            return abort(MultiStreamMaster::SendingError);
        }
    }
}

void MultiStreamMasterPrivate::doReceive()
{
    const size_t headerSize = FrameHeaderSize;
    while (true) {
        uint32_t payloadSize;
        uint32_t streamNumber;
        string payload;
        try {
            const string &header = connection->recvall(headerSize);
            if (header.size() != headerSize) {
                return abort(MultiStreamMaster::ReceivingError);
            }
            payloadSize = ngFromBigEndian<uint32_t>(header.data());
            streamNumber = ngFromBigEndian<uint32_t>(header.data() + sizeof(uint32_t));
            if (payloadSize > _maxPayloadSize) {
                return abort(MultiStreamMaster::PacketTooLarge);
            }
            payload = connection->recvall(static_cast<int32_t>(payloadSize));
            if (payload.size() != static_cast<size_t>(payloadSize)) {
                return abort(MultiStreamMaster::InvalidPacket);
            }
        } catch (CoroutineExitException) {
            assert(error != MultiStreamMaster::NoError);
            return;
        } catch (...) {
            return abort(MultiStreamMaster::UnknownError);
        }
        MultiStreamMaster::StreamError handlePacketResult =
                handleIncomingPacket(streamNumber, std::move(payload));
        if (handlePacketResult != MultiStreamMaster::NoError) {
            return abort(handlePacketResult);
        }
        lastActiveTimestamp = utils::DateTime::currentMSecsSinceEpoch();
    }
}

void MultiStreamMasterPrivate::doKeepalive()
{
    while (true) {
        Coroutine::sleep(0.5f);
        int64_t now = utils::DateTime::currentMSecsSinceEpoch();
        if (keepaliveTimeout > 0 && now > lastActiveTimestamp && (now - lastActiveTimestamp > keepaliveTimeout)) {
            return abort(MultiStreamMaster::KeepaliveTimeoutError);
        }
        if (now > lastKeepaliveTimestamp && (now - lastKeepaliveTimestamp > keepaliveInterval)
            && !hasSendableData()) {
            lastKeepaliveTimestamp = now;
            enqueueCommand(packKeepaliveRequest());
        }
    }
}

void MultiStreamMasterPrivate::abort(MultiStreamMaster::StreamError reason)
{
    if (error != MultiStreamMaster::NoError) {
        return;
    }
    error = reason;
    Coroutine *current = Coroutine::current();
    connection->abort();

    while (!commandQueue.isEmpty()) {
        commandQueue.get();
    }
    notifyHasData();

    if (operations->get("receiving").get() != current) {
        operations->kill("receiving");
    }
    if (operations->get("sending").get() != current) {
        operations->kill("sending");
    }
    if (operations->get("keepalive").get() != current) {
        operations->kill("keepalive");
    }

    for (uint32_t i = 0; i < pendingSlavesNotEmpty.getting(); ++i) {
        pendingSlaves.push_back(shared_ptr<MultiStreamSlave>());
    }
    pendingSlavesNotEmpty.notifyAll();

    for (const auto &item : slaves) {
        shared_ptr<MultiStreamSlave> strong = item.second.lock();
        if (strong) {
            strong->d_func()->master = nullptr;
            strong->d_func()->abort(this->error);
        }
    }
    slaves.clear();
}

void MultiStreamMasterPrivate::cleanSlave(uint32_t streamNumber, bool sendResetPacket,
                                          MultiStreamResetCode resetCode)
{
    auto it = slaves.find(streamNumber);
    if (it == slaves.end()) {
        return;
    }
    slaves.erase(it);
    if (sendResetPacket) {
        notifySlaveClose(streamNumber, resetCode);
    }
}

MultiStreamMaster::StreamError MultiStreamMasterPrivate::handleIncomingPacket(uint32_t streamNumber, string payload)
{
    if (streamNumber == CommandStreamNumber) {
        if (!handleCommand(payload)) {
            return MultiStreamMaster::InvalidCommand;
        }
        return MultiStreamMaster::NoError;
    }

    auto it = slaves.find(streamNumber);
    if (it == slaves.end()) {
        return MultiStreamMaster::NoError;
    }
    shared_ptr<MultiStreamSlave> slave = it->second.lock();
    if (!slave) {
        slaves.erase(it);
        return MultiStreamMaster::NoError;
    }
    slave->d_func()->handleIncomingPacket(std::move(payload));
    return MultiStreamMaster::NoError;
}

bool MultiStreamMasterPrivate::handleCommand(const string &packet)
{
    uint8_t command;
    uint32_t streamNumber;
    uint32_t arg;
    if (!unpackCommand(packet, &command, &streamNumber, &arg)) {
        ngWarning() << "invalid multi stream command.";
        return false;
    }
    if (command == MAKE_SLAVE_REQUEST) {
        if (slaves.find(streamNumber) != slaves.end()) {
            return false;
        }
        shared_ptr<MultiStreamSlave> slave = makeSlaveInternal(MultiStreamNegativePole, streamNumber);
        slave->d_func()->sendCredit = arg;
        slave->d_func()->creditEvent.set();
        enqueueCommand(packSlaveMadeRequest(streamNumber, slave->receivingCapacity()));
        pendingSlaves.push_back(slave);
        pendingSlavesNotEmpty.notify();
        return true;
    }
    if (command == SLAVE_MADE_REQUEST) {
        auto it = slaves.find(streamNumber);
        if (it != slaves.end()) {
            shared_ptr<MultiStreamSlave> slave = it->second.lock();
            if (slave) {
                slave->d_func()->sendCredit = arg;
                slave->d_func()->creditEvent.set();
                return true;
            }
            slaves.erase(it);
        }
        enqueueCommand(packResetSlaveRequest(streamNumber, MultiStreamResetRefused));
        return true;
    }
    if (command == RESET_SLAVE_REQUEST) {
        shared_ptr<MultiStreamSlave> strong = peekSlave(streamNumber);
        auto it = slaves.find(streamNumber);
        if (it != slaves.end()) {
            if (!strong) {
                strong = it->second.lock();
            }
            cleanSlave(streamNumber, false);
        }
        if (strong) {
            MultiStreamSlavePrivate *slavePrivate = strong->d_func();
            slavePrivate->resetCode = static_cast<MultiStreamResetCode>(arg);
            slavePrivate->finish(MultiStreamMaster::RemotePeerClosedError, true);
        }
        return true;
    }
    if (command == WINDOW_UPDATE_REQUEST) {
        auto it = slaves.find(streamNumber);
        if (it != slaves.end()) {
            shared_ptr<MultiStreamSlave> slave = it->second.lock();
            if (slave) {
                slave->d_func()->addSendCredit(arg);
            }
        }
        return true;
    }
    if (command == KEEPALIVE_REQUEST) {
        return true;
    }
    if (command < 32) {
        ngWarning() << "unknown multi stream command.";
        return false;
    }
    ngInfo() << "unknown optional multi stream command, you might upgrade your qtng.";
    return true;
}

MultiStreamMaster::MultiStreamMaster(shared_ptr<Socket> connection, MultiStreamPole pole)
    : d_ptr(new MultiStreamMasterPrivate(asSocketLike(connection), pole, this))
{
}

#ifndef QTNG_NO_CRYPTO
MultiStreamMaster::MultiStreamMaster(shared_ptr<SslSocket> connection, MultiStreamPole pole)
    : d_ptr(new MultiStreamMasterPrivate(asSocketLike(connection), pole, this))
{
}
#endif

MultiStreamMaster::MultiStreamMaster(shared_ptr<KcpSocket> connection, MultiStreamPole pole)
    : d_ptr(new MultiStreamMasterPrivate(asSocketLike(connection), pole, this))
{
}

MultiStreamMaster::MultiStreamMaster(shared_ptr<SocketLike> connection, MultiStreamPole pole)
    : d_ptr(new MultiStreamMasterPrivate(connection, pole, this))
{
}

MultiStreamMaster::~MultiStreamMaster()
{
    delete d_ptr;
}

MultiStreamMaster::StreamError MultiStreamMaster::error() const
{
    NG_D(const MultiStreamMaster);
    return d->error;
}

string MultiStreamMaster::errorString() const
{
    NG_D(const MultiStreamMaster);
    return errorToString(d->error);
}

string MultiStreamMaster::toString() const
{
    NG_D(const MultiStreamMaster);
    return utils::formatMessage("<MultiStreamMaster (name = %1, error = %2, slaves = %3)>",
                                {d->name.empty() ? "unnamed" : d->name, errorToString(d->error),
                                 utils::number(static_cast<int>(d->slaves.size()))});
}

MultiStreamPole MultiStreamMaster::pole() const
{
    NG_D(const MultiStreamMaster);
    return d->pole;
}

void MultiStreamMaster::setName(const string &name)
{
    NG_D(MultiStreamMaster);
    d->name = name;
}

string MultiStreamMaster::name() const
{
    NG_D(const MultiStreamMaster);
    return d->name;
}

bool MultiStreamMaster::isBroken() const
{
    NG_D(const MultiStreamMaster);
    return d->isBroken();
}

void MultiStreamMaster::abort()
{
    NG_D(MultiStreamMaster);
    d->abort(UserShutdown);
}

shared_ptr<MultiStreamSlave> MultiStreamMaster::makeSlave()
{
    NG_D(MultiStreamMaster);
    if (d->isBroken()) {
        return shared_ptr<MultiStreamSlave>();
    }
    uint32_t streamNumber = d->nextStreamNumber;
    d->nextStreamNumber += static_cast<uint32_t>(static_cast<int32_t>(d->pole));
    shared_ptr<MultiStreamSlave> slave = d->makeSlaveInternal(MultiStreamPositivePole, streamNumber);
    d->enqueueCommand(packMakeSlaveRequest(streamNumber, slave->receivingCapacity()));
    return slave;
}

shared_ptr<MultiStreamSlave> MultiStreamMaster::takeSlave()
{
    NG_D(MultiStreamMaster);
    if (d->isBroken()) {
        return shared_ptr<MultiStreamSlave>();
    }
    while (true) {
        if (!d->pendingSlaves.empty()) {
            shared_ptr<MultiStreamSlave> slave = d->pendingSlaves.front();
            d->pendingSlaves.pop_front();
            return slave;
        }
        if (!d->pendingSlavesNotEmpty.wait()) {
            return shared_ptr<MultiStreamSlave>();
        }
    }
}

shared_ptr<MultiStreamSlave> MultiStreamMaster::takeSlave(uint32_t streamNumber)
{
    NG_D(MultiStreamMaster);
    if (d->isBroken()) {
        return shared_ptr<MultiStreamSlave>();
    }
    for (size_t i = 0; i < d->pendingSlaves.size(); ++i) {
        shared_ptr<MultiStreamSlave> slave = d->pendingSlaves[i];
        if (slave && slave->streamNumber() == streamNumber) {
            d->pendingSlaves.erase(d->pendingSlaves.begin() + static_cast<ptrdiff_t>(i));
            return slave;
        }
    }
    return shared_ptr<MultiStreamSlave>();
}

void MultiStreamMaster::setMaxPacketSize(uint32_t size)
{
    NG_D(MultiStreamMaster);
    if (size == 0) {
        size = DefaultPacketSize;
    } else if (size < 64) {
        ngWarning() << "the max packet size of MultiStreamMaster should not lesser than 64.";
        return;
    }
    if (size <= FrameHeaderSize) {
        ngWarning() << "the max packet size of MultiStreamMaster must be greater than frame header.";
        return;
    }
    d->_maxPayloadSize = size - FrameHeaderSize;
    d->_payloadSizeHint = min(d->_payloadSizeHint, d->_maxPayloadSize);
}

uint32_t MultiStreamMaster::maxPacketSize() const
{
    NG_D(const MultiStreamMaster);
    return d->_maxPayloadSize + FrameHeaderSize;
}

uint32_t MultiStreamMaster::maxPayloadSize() const
{
    NG_D(const MultiStreamMaster);
    return d->_maxPayloadSize;
}

void MultiStreamMaster::setPayloadSizeHint(uint32_t payloadSizeHint)
{
    NG_D(MultiStreamMaster);
    if (payloadSizeHint == 0) {
        payloadSizeHint = DefaultPayloadSize;
    } else if (payloadSizeHint < 64) {
        ngWarning() << "the payload size hint of MultiStreamMaster should not lesser than 64.";
        return;
    }
    d->_payloadSizeHint = min(payloadSizeHint, d->_maxPayloadSize);
}

uint32_t MultiStreamMaster::payloadSizeHint() const
{
    NG_D(const MultiStreamMaster);
    return d->_payloadSizeHint;
}

void MultiStreamMaster::setSlaveReceivingCapacity(uint32_t bytes)
{
    NG_D(MultiStreamMaster);
    d->_slaveReceivingCapacity = bytes;
}

uint32_t MultiStreamMaster::slaveReceivingCapacity() const
{
    NG_D(const MultiStreamMaster);
    return d->_slaveReceivingCapacity;
}

void MultiStreamMaster::setSlaveSendingCapacity(uint32_t bytes)
{
    NG_D(MultiStreamMaster);
    d->_slaveSendingCapacity = bytes;
}

uint32_t MultiStreamMaster::slaveSendingCapacity() const
{
    NG_D(const MultiStreamMaster);
    return d->_slaveSendingCapacity;
}

void MultiStreamMaster::setKeepaliveTimeout(float timeout)
{
    NG_D(MultiStreamMaster);
    if (timeout > 0) {
        d->keepaliveTimeout = static_cast<int64_t>(timeout * 1000);
        if (d->keepaliveTimeout < 1000) {
            d->keepaliveTimeout = 1000;
        }
    } else {
        d->keepaliveTimeout = -1;
    }
}

float MultiStreamMaster::keepaliveTimeout() const
{
    NG_D(const MultiStreamMaster);
    return static_cast<float>(d->keepaliveTimeout) / 1000;
}

void MultiStreamMaster::setKeepaliveInterval(float keepaliveInterval)
{
    NG_D(MultiStreamMaster);
    d->keepaliveInterval = static_cast<int64_t>(keepaliveInterval * 1000);
    if (d->keepaliveInterval < 200) {
        d->keepaliveInterval = 200;
    }
}

float MultiStreamMaster::keepaliveInterval() const
{
    NG_D(const MultiStreamMaster);
    return static_cast<float>(d->keepaliveInterval) / 1000;
}

uint32_t MultiStreamMaster::sendingQueueSize() const
{
    NG_D(const MultiStreamMaster);
    uint32_t total = d->commandQueue.size();
    for (const auto &item : d->slaves) {
        shared_ptr<MultiStreamSlave> slave = item.second.lock();
        if (slave) {
            total += MultiStreamSlavePrivate::getPrivateHelper(slave)->sendingQueue.size();
        }
    }
    return total;
}

shared_ptr<SocketLike> MultiStreamMaster::connection() const
{
    NG_D(const MultiStreamMaster);
    return d->connection;
}

MultiStreamSlave::MultiStreamSlave(MultiStreamMaster *master, MultiStreamPole pole, uint32_t streamNumber)
    : d_ptr(new MultiStreamSlavePrivate(master, pole, streamNumber, this))
{
}

MultiStreamSlave::~MultiStreamSlave()
{
    delete d_ptr;
}

uint32_t MultiStreamSlave::streamNumber() const
{
    NG_D(const MultiStreamSlave);
    return d->streamNumber;
}

MultiStreamMaster::StreamError MultiStreamSlave::error() const
{
    NG_D(const MultiStreamSlave);
    return d->error;
}

string MultiStreamSlave::errorString() const
{
    NG_D(const MultiStreamSlave);
    if (d->error == MultiStreamMaster::RemotePeerClosedError) {
        return utils::formatMessage("The remote peer reset the stream (%1)", {resetCodeToString(d->resetCode)});
    }
    return errorToString(d->error);
}

string MultiStreamSlave::toString() const
{
    NG_D(const MultiStreamSlave);
    return utils::formatMessage(
            "<MultiStreamSlave (name = %1, stream = %2, error = %3, reset = %4, closing = %5, priority = %6, "
            "capacity = %7, queue_size = %8)>",
            {d->name.empty() ? "unnamed" : d->name, utils::number(static_cast<long long>(d->streamNumber)),
             errorToString(d->error), resetCodeToString(d->resetCode),
             d->closing ? string("true") : string("false"), utils::number(d->priority),
             utils::number(static_cast<long long>(d->receivingQueue.capacity())),
             utils::number(static_cast<int>(d->receivingQueue.size()))});
}

MultiStreamPole MultiStreamSlave::pole() const
{
    NG_D(const MultiStreamSlave);
    return d->pole;
}

void MultiStreamSlave::setName(const string &name)
{
    NG_D(MultiStreamSlave);
    d->name = name;
}

string MultiStreamSlave::name() const
{
    NG_D(const MultiStreamSlave);
    return d->name;
}

bool MultiStreamSlave::isBroken() const
{
    NG_D(const MultiStreamSlave);
    return d->error != MultiStreamMaster::NoError || !d->master || d->master->isBroken();
}

bool MultiStreamSlave::isClosing() const
{
    NG_D(const MultiStreamSlave);
    return d->closing && d->error == MultiStreamMaster::NoError;
}

void MultiStreamSlave::close()
{
    NG_D(MultiStreamSlave);
    if (d->error != MultiStreamMaster::NoError) {
        return;
    }
    if (d->closing) {
        if (d->closeDone) {
            d->closeDone->tryWait();
        }
        return;
    }
    if (!d->master) {
        d->finish(MultiStreamMaster::UserShutdown, false);
        return;
    }
    d->closing = true;
    d->closeDone = make_shared<ValueEvent<bool>>();
    shared_ptr<ValueEvent<bool>> done = d->closeDone;
    if (!MultiStreamMasterPrivate::getPrivateHelper(d->master)->enqueueCloseBarrier(d->streamNumber, done)) {
        d->abort(MultiStreamMaster::UserShutdown);
        return;
    }
    done->tryWait();
}

void MultiStreamSlave::abort()
{
    NG_D(MultiStreamSlave);
    d->abort(MultiStreamMaster::UserShutdown);
}

bool MultiStreamSlave::sendPacket(const string &packet, bool waitSent)
{
    NG_D(MultiStreamSlave);
    if (d->closing || d->error != MultiStreamMaster::NoError || !d->master || packet.empty()) {
        return false;
    }
    return MultiStreamMasterPrivate::getPrivateHelper(d->master)->sendPacketRaw(
            d->streamNumber, packet, waitSent ? BlockFlag::Block_Until_Sent : BlockFlag::Block_And_Not_Wait_Sent);
}

bool MultiStreamSlave::sendPacket(string &&packet, bool waitSent)
{
    NG_D(MultiStreamSlave);
    if (d->closing || d->error != MultiStreamMaster::NoError || !d->master || packet.empty()) {
        return false;
    }
    return MultiStreamMasterPrivate::getPrivateHelper(d->master)->sendPacketRaw(
            d->streamNumber, std::move(packet),
            waitSent ? BlockFlag::Block_Until_Sent : BlockFlag::Block_And_Not_Wait_Sent);
}

bool MultiStreamSlave::sendPacketAsync(const string &packet)
{
    NG_D(MultiStreamSlave);
    if (d->closing || d->error != MultiStreamMaster::NoError || !d->master || packet.empty()) {
        return false;
    }
    return MultiStreamMasterPrivate::getPrivateHelper(d->master)->sendPacketRaw(d->streamNumber, packet,
                                                                                BlockFlag::NonBlock);
}

bool MultiStreamSlave::sendPacketAsync(string &&packet)
{
    NG_D(MultiStreamSlave);
    if (d->closing || d->error != MultiStreamMaster::NoError || !d->master || packet.empty()) {
        return false;
    }
    return MultiStreamMasterPrivate::getPrivateHelper(d->master)->sendPacketRaw(
            d->streamNumber, std::move(packet), BlockFlag::NonBlock);
}

string MultiStreamSlave::recvPacket()
{
    NG_D(MultiStreamSlave);
    if (d->receivingQueue.isEmpty() && d->error != MultiStreamMaster::NoError) {
        return string();
    }
    ++d->activeReceivers;
    auto receiverGuard = shared_ptr<void>(nullptr, [d](void *) {
        --d->activeReceivers;
    });
    string packet = d->receivingQueue.get();
    if (packet.empty()) {
        return string();
    }
    d->maybeSendWindowUpdate(static_cast<uint32_t>(packet.size()));
    return packet;
}

uint32_t MultiStreamSlave::maxPacketSize() const
{
    NG_D(const MultiStreamSlave);
    if (!d->master) {
        return DefaultPacketSize;
    }
    return d->master->maxPacketSize();
}

uint32_t MultiStreamSlave::maxPayloadSize() const
{
    NG_D(const MultiStreamSlave);
    if (!d->master) {
        return DefaultPayloadSize;
    }
    return d->master->maxPayloadSize();
}

uint32_t MultiStreamSlave::payloadSizeHint() const
{
    NG_D(const MultiStreamSlave);
    if (!d->master) {
        return DefaultPayloadSize;
    }
    return d->master->payloadSizeHint();
}

void MultiStreamSlave::setReceivingCapacity(uint32_t bytes)
{
    NG_D(MultiStreamSlave);
    uint32_t oldCapacity = d->receivingQueue.capacity();
    d->receivingQueue.setCapacity(bytes);
    if (bytes > oldCapacity && d->master && d->error == MultiStreamMaster::NoError) {
        MultiStreamMasterPrivate::getPrivateHelper(d->master)->enqueueCommand(
                packWindowUpdateRequest(d->streamNumber, bytes - oldCapacity));
    }
}

uint32_t MultiStreamSlave::receivingCapacity() const
{
    NG_D(const MultiStreamSlave);
    return d->receivingQueue.capacity();
}

uint32_t MultiStreamSlave::receivingQueueSize() const
{
    NG_D(const MultiStreamSlave);
    return d->receivingQueue.size();
}

MultiStreamResetCode MultiStreamSlave::resetCode() const
{
    NG_D(const MultiStreamSlave);
    return d->resetCode;
}

void MultiStreamSlave::setPriority(int priority)
{
    NG_D(MultiStreamSlave);
    d->priority = priority;
    if (d->master) {
        MultiStreamMasterPrivate::getPrivateHelper(d->master)->notifyHasData();
    }
}

int MultiStreamSlave::priority() const
{
    NG_D(const MultiStreamSlave);
    return d->priority;
}

namespace {

class MultiStreamSlaveSocketLikeImpl : public SocketLike
{
public:
    MultiStreamSlaveSocketLikeImpl(shared_ptr<MultiStreamSlave> slave);
public:
    virtual Socket::SocketError error() const override;
    virtual string errorString() const override;
    virtual bool isValid() const override;
    virtual HostAddress localAddress() const override;
    virtual uint16_t localPort() const override;
    virtual HostAddress peerAddress() const override;
    virtual string peerName() const override;
    virtual uint16_t peerPort() const override;
    virtual intptr_t fileno() const override;
    virtual Socket::SocketType type() const override;
    virtual Socket::SocketState state() const override;
    virtual HostAddress::NetworkLayerProtocol protocol() const override;
    virtual string localAddressURI() const override;
    virtual string peerAddressURI() const override;

    virtual Socket *acceptRaw() override;
    virtual shared_ptr<SocketLike> accept() override;
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override;
    virtual bool bind(uint16_t port, Socket::BindMode mode) override;
    virtual bool connect(const HostAddress &addr, uint16_t port) override;
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;
    virtual void abort() override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, int value) override;
    virtual int option(Socket::SocketOption option) const override;
public:
    virtual int32_t peek(char *data, int32_t size) override;
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t recv(char *data, int32_t size) override;
    virtual int32_t recvall(char *data, int32_t size) override;
    virtual int32_t send(const char *data, int32_t size) override;
    virtual int32_t sendall(const char *data, int32_t size) override;
    virtual string recv(int32_t size) override;
    virtual string recvall(int32_t size) override;
    virtual int32_t send(const string &data) override;
    virtual int32_t sendall(const string &data) override;
    virtual void close() override;
public:
    shared_ptr<SocketLike> getBackend() const;
public:
    string buf;
    shared_ptr<MultiStreamSlave> slave;
};

MultiStreamSlaveSocketLikeImpl::MultiStreamSlaveSocketLikeImpl(shared_ptr<MultiStreamSlave> slave)
    : slave(slave)
{
}

shared_ptr<SocketLike> MultiStreamSlaveSocketLikeImpl::getBackend() const
{
    MultiStreamSlavePrivate *d = MultiStreamSlavePrivate::getPrivateHelper(slave);
    if (!d || !d->master) {
        return shared_ptr<SocketLike>();
    }
    return d->master->connection();
}

Socket::SocketError MultiStreamSlaveSocketLikeImpl::error() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return Socket::UnknownSocketError;
    }
    return backend->error();
}

string MultiStreamSlaveSocketLikeImpl::errorString() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return string();
    }
    return backend->errorString();
}

bool MultiStreamSlaveSocketLikeImpl::isValid() const
{
    return !slave->isBroken();
}

HostAddress MultiStreamSlaveSocketLikeImpl::localAddress() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->localAddress() : HostAddress();
}

uint16_t MultiStreamSlaveSocketLikeImpl::localPort() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->localPort() : 0;
}

HostAddress MultiStreamSlaveSocketLikeImpl::peerAddress() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->peerAddress() : HostAddress();
}

string MultiStreamSlaveSocketLikeImpl::peerName() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->peerName() : string();
}

uint16_t MultiStreamSlaveSocketLikeImpl::peerPort() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->peerPort() : 0;
}

intptr_t MultiStreamSlaveSocketLikeImpl::fileno() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->fileno() : 0;
}

Socket::SocketType MultiStreamSlaveSocketLikeImpl::type() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->type() : Socket::UnknownSocketType;
}

Socket::SocketState MultiStreamSlaveSocketLikeImpl::state() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->state() : Socket::UnconnectedState;
}

HostAddress::NetworkLayerProtocol MultiStreamSlaveSocketLikeImpl::protocol() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->protocol() : HostAddress::UnknownNetworkLayerProtocol;
}

string MultiStreamSlaveSocketLikeImpl::localAddressURI() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? ("multistream+" + backend->localAddressURI()) : string();
}

string MultiStreamSlaveSocketLikeImpl::peerAddressURI() const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? ("multistream+" + backend->peerAddressURI()) : string();
}

Socket *MultiStreamSlaveSocketLikeImpl::acceptRaw()
{
    return nullptr;
}

shared_ptr<SocketLike> MultiStreamSlaveSocketLikeImpl::accept()
{
    return shared_ptr<SocketLike>();
}

bool MultiStreamSlaveSocketLikeImpl::bind(const HostAddress &, uint16_t, Socket::BindMode)
{
    return false;
}

bool MultiStreamSlaveSocketLikeImpl::bind(uint16_t, Socket::BindMode)
{
    return false;
}

bool MultiStreamSlaveSocketLikeImpl::connect(const HostAddress &, uint16_t)
{
    return false;
}

bool MultiStreamSlaveSocketLikeImpl::connect(const string &, uint16_t, shared_ptr<SocketDnsCache>)
{
    return false;
}

void MultiStreamSlaveSocketLikeImpl::abort()
{
    slave->abort();
}

bool MultiStreamSlaveSocketLikeImpl::listen(int)
{
    return false;
}

bool MultiStreamSlaveSocketLikeImpl::setOption(Socket::SocketOption option, int value)
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->setOption(option, value) : false;
}

int MultiStreamSlaveSocketLikeImpl::option(Socket::SocketOption option) const
{
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->option(option) : -1;
}

int32_t MultiStreamSlaveSocketLikeImpl::peek(char *data, int32_t size)
{
    if (size <= 0) {
        return -1;
    }
    int32_t len = min(size, static_cast<int32_t>(buf.size()));
    memcpy(data, buf.data(), static_cast<size_t>(len));
    return len;
}

int32_t MultiStreamSlaveSocketLikeImpl::peekRaw(char *data, int32_t size)
{
    if (size <= 0) {
        return -1;
    }
    shared_ptr<SocketLike> backend = getBackend();
    return backend ? backend->peekRaw(data, size) : -1;
}

int32_t MultiStreamSlaveSocketLikeImpl::recv(char *data, int32_t size)
{
    if (size <= 0) {
        return -1;
    }
    if (buf.empty()) {
        buf = slave->recvPacket();
        if (buf.empty()) {
            return 0;
        }
    }
    int32_t len = min(size, static_cast<int32_t>(buf.size()));
    memcpy(data, buf.data(), static_cast<size_t>(len));
    buf.erase(0, static_cast<size_t>(len));
    return len;
}

int32_t MultiStreamSlaveSocketLikeImpl::recvall(char *data, int32_t size)
{
    if (size <= 0) {
        return -1;
    }
    while (static_cast<int32_t>(buf.size()) < size) {
        const string &packet = slave->recvPacket();
        if (packet.empty()) {
            break;
        }
        buf.append(packet);
    }
    int32_t len = min(size, static_cast<int32_t>(buf.size()));
    if (len > 0) {
        memcpy(data, buf.data(), static_cast<size_t>(len));
        buf.erase(0, static_cast<size_t>(len));
    }
    return len;
}

int32_t MultiStreamSlaveSocketLikeImpl::send(const char *data, int32_t size)
{
    int32_t len = min<int32_t>(size, static_cast<int32_t>(slave->maxPayloadSize()));
    bool ok = slave->sendPacket(string(data, static_cast<size_t>(len)));
    return ok ? len : -1;
}

int32_t MultiStreamSlaveSocketLikeImpl::sendall(const char *data, int32_t size)
{
    int32_t count = 0;
    int32_t maxPayloadSize = static_cast<int32_t>(slave->maxPayloadSize());
    while (count < size) {
        int32_t len = min(size - count, maxPayloadSize);
        bool ok = slave->sendPacket(string(data + count, static_cast<size_t>(len)));
        if (!ok) {
            break;
        }
        count += len;
    }
    return count;
}

string MultiStreamSlaveSocketLikeImpl::recv(int32_t size)
{
    string t(static_cast<size_t>(size), '\0');
    int32_t len = recv(&t[0], size);
    if (len <= 0) {
        return string();
    }
    t.resize(static_cast<size_t>(len));
    return t;
}

string MultiStreamSlaveSocketLikeImpl::recvall(int32_t size)
{
    string t(static_cast<size_t>(size), '\0');
    int32_t len = recvall(&t[0], size);
    if (len <= 0) {
        return string();
    }
    t.resize(static_cast<size_t>(len));
    return t;
}

int32_t MultiStreamSlaveSocketLikeImpl::send(const string &data)
{
    return send(data.data(), static_cast<int32_t>(data.size()));
}

int32_t MultiStreamSlaveSocketLikeImpl::sendall(const string &data)
{
    return sendall(data.data(), static_cast<int32_t>(data.size()));
}

void MultiStreamSlaveSocketLikeImpl::close()
{
    slave->close();
}

}  // namespace

shared_ptr<SocketLike> asSocketLike(shared_ptr<MultiStreamSlave> slave)
{
    return make_shared<MultiStreamSlaveSocketLikeImpl>(slave);
}

}  // namespace qtng
