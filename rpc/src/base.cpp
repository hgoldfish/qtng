#include "qtng/rpc/base.h"
#include "qtng/rpc/value.h"

namespace qtng {
namespace rpc {

Value RpcRemoteException::saveState() const
{
    ValueMap m;
    m["message"] = Value::str(message);
    return Value(std::move(m));
}

bool RpcRemoteException::restoreState(const Value &state)
{
    if (!state.isNull() && state.hasKey("message")) {
        message = state.at("message").asStr();
        return true;
    }
    return false;
}

UseStream::UseStream()
    : place(ServerSide | ValueOfResponse)
    , preferRawSocket(false)
{
}

UseStream::~UseStream() = default;

}  // namespace rpc
}  // namespace qtng
