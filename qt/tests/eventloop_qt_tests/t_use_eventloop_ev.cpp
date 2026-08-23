#include <QCoreApplication>
#include <QDebug>

#include <qtnetworkng.h>

using namespace qtng;

// qtng::useEventloop(Ev) forces the libev backend even on the GUI thread (the former
// qtnetworkng 1.0 preferLibev() behaviour), so startQtLoop() has no Qt loop to run and must
// report failure instead of blocking forever.
int main(int argc, char **argv)
{
    useEventloop(EventLoopType::Ev);
    QCoreApplication app(argc, argv);
    qDebug() << "== t_use_eventloop_ev ==";

    const int rc = startQtLoop();
    const bool ok = rc == -1;
    qDebug() << "[result] startQtLoop() returned" << rc << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
