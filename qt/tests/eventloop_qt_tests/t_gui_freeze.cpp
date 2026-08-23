#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QTimer>

#include <qtnetworkng.h>

using namespace qtng;

namespace {
int qtTicks = 0;
bool coroutineFinished = false;
bool joinReturned = false;
bool qtStalledDuringJoin = false;
}

// The GUI thread waits on a coroutine via join(): runUntil() runs a nested Qt event loop until the
// coroutine finishes. Qt keeps pumping during the join, so the UI must not freeze (this is how a Qt
// event handler can wait on spawned work, e.g. lafdup's peer->join()).
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qDebug() << "== t_gui_freeze ==";
    qDebug() << "[main] spawning coroutine and joining from the GUI thread";

    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, []() { ++qtTicks; });
    ticker.start(100);

    Coroutine *worker = Coroutine::spawn([]() {
        qDebug() << "[coroutine] started. sleeping 0.8s";
        Coroutine::sleep(0.8f);
        coroutineFinished = true;
        qDebug() << "[coroutine] finished";
    });

    const int ticksBefore = qtTicks;
    worker->join();
    joinReturned = true;
    const int ticksAfter = qtTicks;
    if (ticksAfter <= ticksBefore) {
        qtStalledDuringJoin = true;
    }
    qDebug() << "[main] join() returned. qtTicks before/after =" << ticksBefore << "/" << ticksAfter;

    // Let startQtLoop()'s exec() return promptly; the interesting part already happened.
    QTimer::singleShot(0, []() { QCoreApplication::instance()->quit(); });

    qDebug() << "[main] calling startQtLoop() ...";
    int rc = startQtLoop();
    qDebug() << "[main] startQtLoop() returned" << rc;
    qDebug() << "[result] qtTicks =" << qtTicks << "coroutineFinished =" << coroutineFinished
             << "joinReturned =" << joinReturned << "qtStalledDuringJoin =" << qtStalledDuringJoin;

    const bool ok = qtTicks > 0 && coroutineFinished && joinReturned && !qtStalledDuringJoin && rc == 0;
    qDebug() << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
