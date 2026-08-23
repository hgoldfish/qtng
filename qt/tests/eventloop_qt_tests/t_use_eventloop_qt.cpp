#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

#include <qtnetworkng.h>

using namespace qtng;

// Explicit qtng::useQtEventloop() (== qtng_core::useEventloop(EventLoopQt)) at the very beginning
// of main(): the GUI thread must still get the Qt-backed event loop and startQtLoop() must drive it.
int main(int argc, char **argv)
{
    useQtEventloop();
    QCoreApplication app(argc, argv);
    qDebug() << "== t_use_eventloop_qt ==";

    bool ran = false;
    Coroutine::spawn([&ran]() {
        Coroutine::sleep(0.2f);
        ran = true;
        QCoreApplication::instance()->quit();
    });

    QTimer::singleShot(5000, []() {
        qDebug() << "[main] 5s timeout: coroutine was never scheduled.";
        QCoreApplication::instance()->quit();
    });

    const int rc = startQtLoop();
    const bool ok = ran && rc == 0;
    qDebug() << "[result] ran =" << ran << "startQtLoop() returned" << rc << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
