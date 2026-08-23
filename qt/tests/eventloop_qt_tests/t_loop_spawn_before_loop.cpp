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
bool qtStalledDuringCoroutine = false;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qDebug() << "== t_loop_spawn_before_loop ==";
    qDebug() << "[main] spawning coroutine BEFORE startQtLoop, coroutine sleeps 1s x3";

    Coroutine::spawn([]() {
        coroutineStarted = true;
        qDebug() << "[coroutine] started (thread" << QThread::currentThreadId() << ")";
        for (int i = 0; i < 3; ++i) {
            const int before = qtTicks;
            Coroutine::sleep(1.0f);
            const int after = qtTicks;
            qDebug() << "[coroutine] sleep #" << i + 1 << "done. qtTicks before/after =" << before << "/" << after;
            if (before >= 0 && after == before) {
                // The Qt event loop stopped ticking while this coroutine slept: it is blocked.
                qtStalledDuringCoroutine = true;
            }
        }
        coroutineFinished = true;
    });

    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, []() {
        ++qtTicks;
        qDebug() << "[qt] tick" << qtTicks;
    });
    ticker.start(200);

    QTimer::singleShot(6000, []() {
        qDebug() << "[main] 6s elapsed. qtTicks =" << qtTicks << "coroutineFinished =" << coroutineFinished;
        QCoreApplication::instance()->quit();
    });

    qDebug() << "[main] calling startQtLoop() ...";
    int rc = startQtLoop();
    qDebug() << "[main] startQtLoop() returned" << rc;
    qDebug() << "[result] qtTicks =" << qtTicks << "coroutineStarted =" << coroutineStarted
             << "coroutineFinished =" << coroutineFinished << "qtStalledDuringCoroutine =" << qtStalledDuringCoroutine;

    // Qt loop must keep running while the coroutine sleeps; if it stalled, the GUI would freeze.
    const bool ok = (qtTicks > 0) && coroutineStarted && coroutineFinished && !qtStalledDuringCoroutine && rc == 0;
    qDebug() << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
