#include "bridge/core_access.h"
#include "bridge/udp_access.h"
#include "utp.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class UtpStreamPrivate
{
public:
    explicit UtpStreamPrivate(shared_ptr<qtng_core::UtpStream> coreStream)
        : core(std::move(coreStream))
    {
    }

    static UtpStream *fromCore(qtng_core::UtpStream *adopted)
    {
        if (!adopted) {
            return nullptr;
        }
        UtpStream *stream = new UtpStream(new UtpStreamPrivate(shared_ptr<qtng_core::UtpStream>(adopted)));
        stream->attachEvents();
        return stream;
    }

    shared_ptr<qtng_core::UtpStream> core;
};

UtpStream::UtpStream(QSharedPointer<DatagramLink> link)
    : d_ptr(new UtpStreamPrivate(make_shared<qtng_core::UtpStream>(toCoreDatagramLink(link))))
{
    attachEvents();
}

UtpStream::UtpStream(UtpStreamPrivate *d)
    : d_ptr(d)
{
}

UtpStream::~UtpStream()
{
    busy.attachCore(nullptr);
    notBusy.attachCore(nullptr);
    delete d_ptr;
}

void UtpStream::attachEvents()
{
    busy.attachCore(&d_ptr->core->busy);
    notBusy.attachCore(&d_ptr->core->notBusy);
}

QSharedPointer<DatagramLink> UtpStream::link() const
{
    return toQtDatagramLink(d_ptr->core->link());
}

void UtpStream::setDelayTarget(float milliseconds)
{
    d_ptr->core->setDelayTarget(milliseconds);
}

float UtpStream::delayTarget() const
{
    return d_ptr->core->delayTarget();
}

void UtpStream::setMaxWindow(quint32 bytes)
{
    d_ptr->core->setMaxWindow(bytes);
}

quint32 UtpStream::maxWindow() const
{
    return d_ptr->core->maxWindow();
}

void UtpStream::setPacketSize(quint32 bytes)
{
    d_ptr->core->setPacketSize(bytes);
}

quint32 UtpStream::packetSize() const
{
    return d_ptr->core->packetSize();
}

quint32 UtpStream::payloadSizeHint() const
{
    return d_ptr->core->payloadSizeHint();
}

void UtpStream::setReceiveBufferSize(quint32 bytes)
{
    d_ptr->core->setReceiveBufferSize(bytes);
}

quint32 UtpStream::receiveBufferSize() const
{
    return d_ptr->core->receiveBufferSize();
}

void UtpStream::setIdleTimeout(float seconds)
{
    d_ptr->core->setIdleTimeout(seconds);
}

float UtpStream::idleTimeout() const
{
    return d_ptr->core->idleTimeout();
}

Socket::SocketError UtpStream::error() const
{
    return static_cast<Socket::SocketError>(d_ptr->core->error());
}

QString UtpStream::errorString() const
{
    return toQString(d_ptr->core->errorString());
}

bool UtpStream::isValid() const
{
    return d_ptr->core->isValid();
}

DatagramPath UtpStream::peerPath() const
{
    return toQtDatagramPath(d_ptr->core->peerPath());
}

Socket::SocketState UtpStream::state() const
{
    return static_cast<Socket::SocketState>(d_ptr->core->state());
}

UtpStream *UtpStream::accept()
{
    return UtpStreamPrivate::fromCore(d_ptr->core->accept());
}

UtpStream *UtpStream::accept(const DatagramPath &remote)
{
    return UtpStreamPrivate::fromCore(d_ptr->core->accept(toCoreDatagramPath(remote)));
}

bool UtpStream::connect(const DatagramPath &remote)
{
    return d_ptr->core->connect(toCoreDatagramPath(remote));
}

bool UtpStream::markBound()
{
    return d_ptr->core->markBound();
}

void UtpStream::close()
{
    d_ptr->core->close();
}

void UtpStream::abort()
{
    d_ptr->core->abort();
}

bool UtpStream::listen(int backlog)
{
    return d_ptr->core->listen(backlog);
}

qint32 UtpStream::peek(char *data, qint32 size)
{
    return d_ptr->core->peek(data, size);
}

qint32 UtpStream::recv(char *data, qint32 size)
{
    return d_ptr->core->recv(data, size);
}

qint32 UtpStream::recvall(char *data, qint32 size)
{
    return d_ptr->core->recvall(data, size);
}

qint32 UtpStream::send(const char *data, qint32 size)
{
    return d_ptr->core->send(data, size);
}

qint32 UtpStream::sendall(const char *data, qint32 size)
{
    return d_ptr->core->sendall(data, size);
}

QByteArray UtpStream::recv(qint32 size)
{
    return toQByteArray(d_ptr->core->recv(size));
}

QByteArray UtpStream::recvall(qint32 size)
{
    return toQByteArray(d_ptr->core->recvall(size));
}

qint32 UtpStream::send(const QByteArray &data)
{
    return d_ptr->core->send(toStdString(data));
}

qint32 UtpStream::sendall(const QByteArray &data)
{
    return d_ptr->core->sendall(toStdString(data));
}

bool UtpStream::feedDatagram(const char *data, qint32 len, const DatagramPath &remote)
{
    return d_ptr->core->feedDatagram(data, len, toCoreDatagramPath(remote));
}

}  // namespace QTNETWORKNG_NAMESPACE
