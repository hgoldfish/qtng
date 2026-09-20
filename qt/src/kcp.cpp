#include "bridge/core_access.h"
#include "bridge/udp_access.h"
#include "kcp.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class KcpStreamPrivate
{
public:
    explicit KcpStreamPrivate(shared_ptr<qtng_core::KcpStream> coreStream)
        : core(std::move(coreStream))
    {
    }

    static KcpStream *fromCore(qtng_core::KcpStream *adopted)
    {
        if (!adopted) {
            return nullptr;
        }
        KcpStream *stream = new KcpStream(new KcpStreamPrivate(shared_ptr<qtng_core::KcpStream>(adopted)));
        stream->attachEvents();
        return stream;
    }

    shared_ptr<qtng_core::KcpStream> core;
};

static KcpStreamStats toQtStats(const qtng_core::KcpStreamStats &s)
{
    KcpStreamStats out;
    out.sndWnd = s.sndWnd;
    out.sendBudgetSegs = s.sendBudgetSegs;
    out.memoryCapSegs = s.memoryCapSegs;
    out.rtoResends = s.rtoResends;
    out.fastResends = s.fastResends;
    out.fastresend = s.fastresend;
    out.lossRate = s.lossRate;
    out.deliveryBps = s.deliveryBps;
    out.waitsnd = s.waitsnd;
    out.sndUna = s.sndUna;
    out.sndNxt = s.sndNxt;
    out.sentSegs = s.sentSegs;
    out.rmtWnd = s.rmtWnd;
    out.rxSrtt = s.rxSrtt;
    out.srttMin = s.srttMin;
    out.mss = s.mss;
    return out;
}

KcpStream::KcpStream(QSharedPointer<DatagramLink> link, quint32 sessionId)
    : d_ptr(new KcpStreamPrivate(make_shared<qtng_core::KcpStream>(toCoreDatagramLink(link), sessionId)))
{
    attachEvents();
}

KcpStream::KcpStream(KcpStreamPrivate *d)
    : d_ptr(d)
{
}

KcpStream::~KcpStream()
{
    busy.attachCore(nullptr);
    notBusy.attachCore(nullptr);
    delete d_ptr;
}

void KcpStream::attachEvents()
{
    busy.attachCore(&d_ptr->core->busy);
    notBusy.attachCore(&d_ptr->core->notBusy);
}

QSharedPointer<DatagramLink> KcpStream::link() const
{
    return toQtDatagramLink(d_ptr->core->link());
}

quint32 KcpStream::sessionId() const
{
    return d_ptr->core->sessionId();
}

void KcpStream::setSessionId(quint32 id)
{
    d_ptr->core->setSessionId(id);
}

void KcpStream::setProtocolVersion(quint8 version)
{
    d_ptr->core->setProtocolVersion(version);
}

quint8 KcpStream::protocolVersion() const
{
    return d_ptr->core->protocolVersion();
}

void KcpStream::setTunerEnabled(bool enabled)
{
    d_ptr->core->setTunerEnabled(enabled);
}

bool KcpStream::tunerEnabled() const
{
    return d_ptr->core->tunerEnabled();
}

bool KcpStream::setSendBudgetSegs(quint32 segs)
{
    return d_ptr->core->setSendBudgetSegs(segs);
}

void KcpStream::setSendBufferLimit(quint64 bytes)
{
    d_ptr->core->setSendBufferLimit(bytes);
}

quint64 KcpStream::sendBufferLimit() const
{
    return d_ptr->core->sendBufferLimit();
}

void KcpStream::setPacketSize(quint32 packetSize)
{
    d_ptr->core->setPacketSize(packetSize);
}

quint32 KcpStream::packetSize() const
{
    return d_ptr->core->packetSize();
}

quint32 KcpStream::payloadSizeHint() const
{
    return d_ptr->core->payloadSizeHint();
}

void KcpStream::setTearDownTime(float secs)
{
    d_ptr->core->setTearDownTime(secs);
}

float KcpStream::tearDownTime() const
{
    return d_ptr->core->tearDownTime();
}

KcpStreamStats KcpStream::stats() const
{
    return toQtStats(d_ptr->core->stats());
}

Socket::SocketError KcpStream::error() const
{
    return static_cast<Socket::SocketError>(d_ptr->core->error());
}

QString KcpStream::errorString() const
{
    return toQString(d_ptr->core->errorString());
}

bool KcpStream::isValid() const
{
    return d_ptr->core->isValid();
}

DatagramPath KcpStream::peerPath() const
{
    return toQtDatagramPath(d_ptr->core->peerPath());
}

Socket::SocketState KcpStream::state() const
{
    return static_cast<Socket::SocketState>(d_ptr->core->state());
}

KcpStream *KcpStream::accept()
{
    return KcpStreamPrivate::fromCore(d_ptr->core->accept());
}

KcpStream *KcpStream::accept(const DatagramPath &remote)
{
    return KcpStreamPrivate::fromCore(d_ptr->core->accept(toCoreDatagramPath(remote)));
}

bool KcpStream::connect(const DatagramPath &remote)
{
    return d_ptr->core->connect(toCoreDatagramPath(remote));
}

bool KcpStream::markBound()
{
    return d_ptr->core->markBound();
}

void KcpStream::close()
{
    d_ptr->core->close();
}

void KcpStream::abort()
{
    d_ptr->core->abort();
}

bool KcpStream::listen(int backlog)
{
    return d_ptr->core->listen(backlog);
}

qint32 KcpStream::peek(char *data, qint32 size)
{
    return d_ptr->core->peek(data, size);
}

qint32 KcpStream::recv(char *data, qint32 size)
{
    return d_ptr->core->recv(data, size);
}

qint32 KcpStream::recvall(char *data, qint32 size)
{
    return d_ptr->core->recvall(data, size);
}

qint32 KcpStream::send(const char *data, qint32 size)
{
    return d_ptr->core->send(data, size);
}

qint32 KcpStream::sendall(const char *data, qint32 size)
{
    return d_ptr->core->sendall(data, size);
}

QByteArray KcpStream::recv(qint32 size)
{
    return toQByteArray(d_ptr->core->recv(size));
}

QByteArray KcpStream::recvall(qint32 size)
{
    return toQByteArray(d_ptr->core->recvall(size));
}

qint32 KcpStream::send(const QByteArray &data)
{
    return d_ptr->core->send(toStdString(data));
}

qint32 KcpStream::sendall(const QByteArray &data)
{
    return d_ptr->core->sendall(toStdString(data));
}

}  // namespace QTNETWORKNG_NAMESPACE
