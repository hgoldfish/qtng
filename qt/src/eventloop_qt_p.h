#include <QtCore/qobject.h>
#include "config.h"

class QTimerEvent;

QTNETWORKNG_NAMESPACE_BEGIN

class QtEventLoopCoroutinePrivate;
class EventLoopCoroutinePrivateQtHelper : public QObject
{
    Q_OBJECT
public:
    explicit EventLoopCoroutinePrivateQtHelper(QtEventLoopCoroutinePrivate *parent);
public slots:
    void timerEvent(QTimerEvent *event) override;
    void callLaterThreadSafeStub(quint32 msecs, void *callback);
    void handleIoEvent(int socket);
private:
    QtEventLoopCoroutinePrivate * const parent;
};

QTNETWORKNG_NAMESPACE_END
