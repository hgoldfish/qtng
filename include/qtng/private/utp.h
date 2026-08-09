#ifndef QTNG_UTP_H
#define QTNG_UTP_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/socket.h"
#include "qtng/utils/platform.h"

namespace qtng {

class DatagramLink;
class DatagramPath;

class UtpStreamPrivate;
class MasterUtpStreamPrivate;
class UtpStream
{
public:
    explicit UtpStream(std::shared_ptr<DatagramLink> link);
    virtual ~UtpStream();
public:
    std::shared_ptr<DatagramLink> link() const;

    void setDelayTarget(float milliseconds);
    float delayTarget() const;
    void setMaxWindow(std::uint32_t bytes);
    std::uint32_t maxWindow() const;
    void setPacketSize(std::uint32_t bytes);
    std::uint32_t packetSize() const;
    std::uint32_t payloadSizeHint() const;
    void setReceiveBufferSize(std::uint32_t bytes);
    std::uint32_t receiveBufferSize() const;
    void setIdleTimeout(float seconds);
    float idleTimeout() const;

    Event busy;
    Event notBusy;
public:
    Socket::SocketError error() const;
    std::string errorString() const;
    bool isValid() const;
    DatagramPath peerPath() const;
    Socket::SocketState state() const;

    UtpStream *accept();
    UtpStream *accept(const DatagramPath &remote);

    bool connect(const DatagramPath &remote);
    bool markBound();
    void close();
    void abort();
    bool listen(int backlog);

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);

    // Used by MasterUtpStreamPrivate demux (private header only).
    bool feedDatagram(const char *data, std::int32_t len, const DatagramPath &remote);
private:
    explicit UtpStream(UtpStreamPrivate *master, const DatagramPath &remote, std::uint16_t synConnId,
                        std::uint16_t synSeq);
    friend class MasterUtpStreamPrivate;
    friend class UtpStreamPrivate;
private:
    UtpStreamPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(UtpStream)
};

}  // namespace qtng

#endif  // QTNG_UTP_H
