#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <qtnetworkng.h>

using namespace qtng;

// Verify that network I/O coroutines are driven by the Qt event loop backend:
// a server and a client coroutine exchange data over a local TCP loopback
// connection, both running on the GUI thread under startQtLoop().
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    Coroutine::spawn([]() {
        Socket server;
        if (!server.bind(quint16(0)) || !server.listen(10)) {
            qFatal("server bind/listen failed: %s", qPrintable(server.errorString()));
        }
        const quint16 port = server.localPort();

        Coroutine::spawn([port]() {
            Socket *client = Socket::createConnection(QStringLiteral("127.0.0.1"), port);
            if (!client || !client->isValid()) {
                qFatal("client connect failed");
            }
            const QByteArray payload = QByteArrayLiteral("ping-from-client");
            client->sendall(payload);
            const QByteArray reply = client->recvall(payload.size());
            qDebug() << "client received:" << reply;
            if (reply != QByteArrayLiteral("pong-from-server")) {
                qFatal("client got wrong reply: %s", reply.constData());
            }
            client->close();
        });

        Socket *conn = server.accept();
        if (!conn) {
            qFatal("server accept failed");
        }
        const QByteArray data = conn->recvall(16);
        qDebug() << "server received:" << data;
        if (data != QByteArrayLiteral("ping-from-client")) {
            qFatal("server got wrong data: %s", data.constData());
        }
        conn->sendall(QByteArrayLiteral("pong-from-server"));
        conn->close();
        server.close();
        qDebug() << "network io coroutines OK";
        qApp->quit();
    });

    QTimer::singleShot(10000, &app, []() {
        qFatal("TIMEOUT: network io coroutines did not complete under Qt event loop");
    });

    const int result = startQtLoop();
    return result;
}
