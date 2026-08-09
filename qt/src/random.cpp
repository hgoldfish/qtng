#include <QtCore/qsharedpointer.h>
#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "random.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {
QByteArray randomBytes(int i) { return toQByteArray(qtng_core::randomBytes(i)); }
}  // namespace QTNETWORKNG_NAMESPACE
