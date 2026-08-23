#ifndef QTNG_QT_BRIDGE_EVENTLOOP_QT_CORE_P_H
#define QTNG_QT_BRIDGE_EVENTLOOP_QT_CORE_P_H

// QObject helper for the Qt-backed core event loop (qtng_core::QtEventLoopCoroutine). It lives in
// its own header so AUTOMOC can generate its meta object. Do NOT include qtng core headers from
// here: qt/src/*.cpp include this after bridge/core_access.h, which has already expanded the core
// headers (into namespace qtng_core) and dropped their include guards.

#include <QtCore/qobject.h>
#include <QtCore/qcoreevent.h>

namespace qtng_core {

class QtEventLoopCoroutinePrivate;

class QtEventLoopHelper : public QObject
{
    Q_OBJECT
public:
    explicit QtEventLoopHelper(QtEventLoopCoroutinePrivate *parent);
public slots:
    void timerEvent(QTimerEvent *event) override;
    void callLaterThreadSafeStub(quint32 msecs, void *callback);
    void handleIoEvent(int socket);
private:
    QtEventLoopCoroutinePrivate * const parent;
};

}  // namespace qtng_core

#endif  // QTNG_QT_BRIDGE_EVENTLOOP_QT_CORE_P_H
