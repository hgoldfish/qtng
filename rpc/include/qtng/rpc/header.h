#ifndef QTNG_RPC_HEADER_H
#define QTNG_RPC_HEADER_H

#include <memory>
#include <string>

#include "qtng/rpc/base.h"
#include "qtng/rpc/value.h"

BEGIN_QTNG_RPC_NAMESPACE

class Peer;

// Called on every outgoing call to attach a header, and on the serving side to
// authenticate the incoming call. The pbook PC client sends its `revision` here.
struct HeaderCallback
{
    virtual ~HeaderCallback() = default;
    virtual ValueMap make(Peer *peer, const std::string &methodName) = 0;
    virtual bool auth(Peer *peer, const std::string &methodName, const ValueMap &header) = 0;
};

struct LoggingCallback
{
    virtual ~LoggingCallback() = default;
    virtual void calling(Peer *peer, const std::string &methodName, const ValueList &args, const ValueMap &kwargs) = 0;
    virtual void success(Peer *peer, const std::string &methodName, const ValueList &args, const ValueMap &kwargs,
                         const Value &result) = 0;
    virtual void failed(Peer *peer, const std::string &methodName, const ValueList &args, const ValueMap &kwargs) = 0;
};

END_QTNG_RPC_NAMESPACE

#endif  // QTNG_RPC_HEADER_H
