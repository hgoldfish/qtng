#include <qtnetworkng.h>

#include <QCoreApplication>
#include <QDebug>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    qtng::HttpSession session;
    qtng::HttpResponse response = session.get(QStringLiteral("http://example.com/"));
    if (response.isOk()) {
        qDebug().noquote() << response.text();
    } else {
        qWarning() << "HTTP request failed, status:" << response.statusCode();
    }

    return qtng::startQtLoop();
}
