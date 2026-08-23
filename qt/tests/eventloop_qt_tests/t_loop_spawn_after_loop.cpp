#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QTimer>

#include <qtnetworkng.h>

using namespace qtng;

namespace {
int qtTicks = 0;
bool coroutineStarted = false;
bool coroutineFinished = false;
bool timeoutQuit = false;
}

// Coroutines spawned AFTER startQtLoop() (the documented usage) must be scheduled on the Qt event
// loop: they run while Qt keeps pumping (the UI stays responsive).
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qDebug() << "== t_loop_spawn_after_loop ==";

    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, []() { ++qtTicks; });
    ticker.start(100);

    QTimer trigger;
    QObject::connect(&trigger, &QTimer::timeout, [&trigger]() {
        trigger.stop();
        qDebug() << "[gui-event] spawning coroutine AFTER startQtLoop()";
        Coroutine::spawn([]() {
            coroutineStarted = true;
            qDebug() << "[coroutine] started";
            Coroutine::sleep(0.5f);
            coroutineFinished = true;
            qDebug() << "[coroutine] finished";
            QCoreApplication::instance()->quit();
        });
    });
    trigger.start(500);

    QTimer::singleShot(3000, []() {
        timeoutQuit = true;
        qDebug() << "[main] 3s timeout. qtTicks =" << qtTicks << "coroutineStarted =" << coroutineStarted
                 << "coroutineFinished =" << coroutineFinished;
        QCoreApplication::instance()->quit();
    });

    qDebug() << "[main] calling startQtLoop() ...";
    int rc = startQtLoop();
    qDebug() << "[main] startQtLoop() returned" << rc;
    qDebug() << "[result] qtTicks =" << qtTicks << "coroutineStarted =" << coroutineStarted
             << "coroutineFinished =" << coroutineFinished;

    const bool ok = qtTicks > 0 && coroutineStarted && coroutineFinished && !timeoutQuit && rc == 0;
    qDebug() << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
