#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QTimer>

#include <qtnetworkng.h>

using namespace qtng;

namespace {
bool coroutineRan = false;
}

// On the GUI thread the first coroutine use must install the Qt-backed core event loop, so spawned
// coroutines are scheduled by QCoreApplication::exec() instead of a separate libev loop. startQtLoop()
// returns 0 only when the current core loop is the Qt backend; a libev/Win loop makes it return -1.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qDebug() << "== t_loop_identity ==";

    EventLoopCoroutine *loop = EventLoopCoroutine::get();
    const bool loopCreated = loop != nullptr;
    qDebug() << "[main] qt binding loop objectName =" << (loop ? loop->objectName() : QString("null"));

    Coroutine::spawn([]() {
        qDebug() << "[coroutine] running. thread =" << QThread::currentThreadId();
        Coroutine::sleep(0.3f);
        coroutineRan = true;
        qDebug() << "[coroutine] after sleep. done.";
        QCoreApplication::instance()->quit();
    });

    QTimer::singleShot(5000, []() {
        qDebug() << "[main] 5s timeout: coroutine was never scheduled.";
        QCoreApplication::instance()->quit();
    });

    qDebug() << "[main] calling startQtLoop() ...";
    int rc = startQtLoop();
    qDebug() << "[main] startQtLoop() returned" << rc;

    const bool ok = loopCreated && coroutineRan && rc == 0;
    qDebug() << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
