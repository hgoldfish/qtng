#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QTimer>

#include <qtnetworkng.h>

using namespace qtng;

namespace {
int qtTicks = 0;
bool coroutineStarted = false;
bool coroutineResumed = false;
bool coroutineFinished = false;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qDebug() << "== t_loop_basic ==";
    qDebug() << "[main] spawn coroutine BEFORE startQtLoop (this is what lafdup does in the window ctor)";

    Coroutine::spawn([]() {
        coroutineStarted = true;
        qDebug() << "[coroutine] started (thread" << QThread::currentThreadId() << ")";
        Coroutine::sleep(1.0f);
        coroutineResumed = true;
        qDebug() << "[coroutine] resumed after 1s sleep (thread" << QThread::currentThreadId() << ")";
        coroutineFinished = true;
    });

    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, []() {
        ++qtTicks;
        qDebug() << "[qt] tick" << qtTicks;
    });
    ticker.start(200);

    QTimer::singleShot(3000, []() {
        qDebug() << "[main] 3s elapsed. qtTicks =" << qtTicks
                 << "coroutineStarted =" << coroutineStarted
                 << "coroutineFinished =" << coroutineFinished;
        QCoreApplication::instance()->quit();
    });

    qDebug() << "[main] calling startQtLoop() ...";
    int rc = startQtLoop();
    qDebug() << "[main] startQtLoop() returned" << rc;
    qDebug() << "[result] qtTicks =" << qtTicks << "coroutineStarted =" << coroutineStarted
             << "coroutineResumed =" << coroutineResumed << "coroutineFinished =" << coroutineFinished;

    // A healthy integration must keep pumping the Qt event loop (qtTicks > 0) AND schedule the
    // spawned coroutine (it starts, sleeps, resumes and finishes).
    const bool ok = (qtTicks > 0) && coroutineStarted && coroutineResumed && coroutineFinished && rc == 0;
    qDebug() << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
