#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QTimer>

#include <qtnetworkng.h>

using namespace qtng;

namespace {
int qtTicks = 0;
bool eventLoopCallbackRan = false;
bool joinReturned = false;
bool watchdogFired = false;
}

// A coroutine that blocks on something the Qt event loop must deliver (callInEventLoop posts to the
// Qt loop and waits) must not deadlock: on the Qt-backed core loop the coroutine yields and Qt keeps
// pumping, so the callback runs and the coroutine resumes.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qDebug() << "== t_coroutine_await_qt ==";

    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, []() { ++qtTicks; });
    ticker.start(100);

    QTimer trigger;
    QObject::connect(&trigger, &QTimer::timeout, [&trigger]() {
        trigger.stop();
        qDebug() << "[gui-event] spawning coroutine and joining from Qt event handler";
        Coroutine *worker = Coroutine::spawn([]() {
            qDebug() << "[coroutine] started. calling callInEventLoop() (posts to Qt event loop + waits)";
            callInEventLoop([]() {
                eventLoopCallbackRan = true;
                qDebug() << "[qt-event] callInEventLoop callback executed";
            });
            qDebug() << "[coroutine] callInEventLoop() returned";
        });
        worker->join();
        joinReturned = true;
        qDebug() << "[gui-event] join() returned";
        QCoreApplication::instance()->quit();
    });
    trigger.start(500);

    QTimer watchdog;
    QObject::connect(&watchdog, &QTimer::timeout, []() {
        watchdogFired = true;
        qDebug() << "[main] WATCHDOG fired after 5s: GUI thread is stuck, Qt loop no longer pumps."
                 << "qtTicks =" << qtTicks << "joinReturned =" << joinReturned
                 << "eventLoopCallbackRan =" << eventLoopCallbackRan;
        qDebug() << "FAIL";
        QCoreApplication::instance()->quit();
    });
    watchdog.start(5000);

    qDebug() << "[main] calling startQtLoop() ...";
    int rc = startQtLoop();
    qDebug() << "[main] startQtLoop() returned" << rc;
    qDebug() << "[result] qtTicks =" << qtTicks << "joinReturned =" << joinReturned
             << "eventLoopCallbackRan =" << eventLoopCallbackRan << "watchdogFired =" << watchdogFired;

    const bool ok = !watchdogFired && joinReturned && eventLoopCallbackRan && qtTicks > 0 && rc == 0;
    qDebug() << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
