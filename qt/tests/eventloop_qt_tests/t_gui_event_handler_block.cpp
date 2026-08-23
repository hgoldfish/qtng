#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QTimer>

#include <qtnetworkng.h>

using namespace qtng;

namespace {
int qtTicks = 0;
int stalls = 0;
bool coroutineFinished = false;
bool joinReturned = false;
}

// A Qt event handler (e.g. a tray click or clipboard change in lafdup) fires on the GUI thread and
// calls into coroutine APIs. Joining a coroutine from the handler must run a nested Qt event loop,
// so Qt keeps pumping (ticker keeps ticking) while the handler waits.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qDebug() << "== t_gui_event_handler_block ==";

    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, []() { ++qtTicks; });
    ticker.start(100);

    QTimer trigger;
    QObject::connect(&trigger, &QTimer::timeout, [&trigger]() {
        trigger.stop();
        qDebug() << "[gui-event] spawning coroutine and joining from Qt event handler";
        Coroutine *worker = Coroutine::spawn([]() {
            qDebug() << "[coroutine] started";
            Coroutine::sleep(0.6f);
            coroutineFinished = true;
            qDebug() << "[coroutine] finished";
        });
        const int before = qtTicks;
        worker->join();
        joinReturned = true;
        const int after = qtTicks;
        qDebug() << "[gui-event] join() returned. qtTicks before/after =" << before << "/" << after;
        if (after == before) {
            ++stalls;
            qDebug() << "[gui-event] *** Qt loop stalled while coroutine API was blocking ***";
        }
        QCoreApplication::instance()->quit();
    });
    trigger.start(500);

    QTimer::singleShot(4000, []() {
        qDebug() << "[main] 4s elapsed. qtTicks =" << qtTicks << "stalls =" << stalls;
        QCoreApplication::instance()->quit();
    });

    qDebug() << "[main] calling startQtLoop() ...";
    int rc = startQtLoop();
    qDebug() << "[main] startQtLoop() returned" << rc;
    qDebug() << "[result] qtTicks =" << qtTicks << "stalls =" << stalls << "coroutineFinished =" << coroutineFinished
             << "joinReturned =" << joinReturned;

    const bool ok = qtTicks > 0 && stalls == 0 && coroutineFinished && joinReturned && rc == 0;
    qDebug() << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
