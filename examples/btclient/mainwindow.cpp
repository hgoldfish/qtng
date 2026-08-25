#include "mainwindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "qtng/coroutine.h"
#include "qtng/eventloop.h"

using namespace std;
using namespace qtng;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_workerRunning(false)
    , m_pauseRequested(false)
    , m_stopRequested(false)
{
    setWindowTitle(QStringLiteral("qtng BitTorrent Client"));
    resize(720, 480);

    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout *layout = new QVBoxLayout(central);

    QHBoxLayout *torrentRow = new QHBoxLayout();
    m_torrentEdit = new QLineEdit(central);
    m_torrentEdit->setPlaceholderText(QStringLiteral("Path to .torrent or magnet:?xt=urn:btih:..."));
    QPushButton *torrentBrowse = new QPushButton(QStringLiteral("Browse…"), central);
    torrentRow->addWidget(m_torrentEdit, 1);
    torrentRow->addWidget(torrentBrowse);
    layout->addLayout(torrentRow);

    QHBoxLayout *saveRow = new QHBoxLayout();
    m_saveEdit = new QLineEdit(central);
    m_saveEdit->setText(QDir::homePath() + QStringLiteral("/Downloads"));
    QPushButton *saveBrowse = new QPushButton(QStringLiteral("Save to…"), central);
    saveRow->addWidget(m_saveEdit, 1);
    saveRow->addWidget(saveBrowse);
    layout->addLayout(saveRow);

    QHBoxLayout *btnRow = new QHBoxLayout();
    m_startBtn = new QPushButton(QStringLiteral("Start"), central);
    m_pauseBtn = new QPushButton(QStringLiteral("Pause"), central);
    m_stopBtn = new QPushButton(QStringLiteral("Stop"), central);
    m_pauseBtn->setEnabled(false);
    m_stopBtn->setEnabled(false);
    btnRow->addWidget(m_startBtn);
    btnRow->addWidget(m_pauseBtn);
    btnRow->addWidget(m_stopBtn);
    btnRow->addStretch(1);
    layout->addLayout(btnRow);

    m_progress = new QProgressBar(central);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    layout->addWidget(m_progress);

    m_statusLabel = new QLabel(QStringLiteral("Idle"), central);
    m_peersLabel = new QLabel(QStringLiteral("Peers: 0 / 0"), central);
    m_rateLabel = new QLabel(QStringLiteral("↓ 0 B/s  ↑ 0 B/s"), central);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_peersLabel);
    layout->addWidget(m_rateLabel);

    m_log = new QTextEdit(central);
    m_log->setReadOnly(true);
    layout->addWidget(m_log, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(500);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::refreshStats);
    connect(torrentBrowse, &QPushButton::clicked, this, &MainWindow::browseTorrent);
    connect(saveBrowse, &QPushButton::clicked, this, &MainWindow::browseSaveDir);
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::startDownload);
    connect(m_pauseBtn, &QPushButton::clicked, this, &MainWindow::pauseDownload);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::stopDownload);
}

MainWindow::~MainWindow()
{
    stopDownload();
}

void MainWindow::appendLog(const QString &line)
{
    m_log->append(line);
}

void MainWindow::setUiRunning(bool running)
{
    m_startBtn->setEnabled(!running);
    m_pauseBtn->setEnabled(running);
    m_stopBtn->setEnabled(running);
    m_torrentEdit->setEnabled(!running);
    m_saveEdit->setEnabled(!running);
}

void MainWindow::browseTorrent()
{
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open torrent"), QString(),
                                                QStringLiteral("Torrent (*.torrent);;All (*)"));
    if (!path.isEmpty()) {
        m_torrentEdit->setText(path);
    }
}

void MainWindow::browseSaveDir()
{
    QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Save directory"), m_saveEdit->text());
    if (!path.isEmpty()) {
        m_saveEdit->setText(path);
    }
}

static QString formatRate(double bytesPerSec)
{
    if (bytesPerSec > 1024.0 * 1024.0) {
        return QStringLiteral("%1 MiB/s").arg(bytesPerSec / (1024.0 * 1024.0), 0, 'f', 2);
    }
    if (bytesPerSec > 1024.0) {
        return QStringLiteral("%1 KiB/s").arg(bytesPerSec / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B/s").arg(bytesPerSec, 0, 'f', 0);
}

static QString stateName(TorrentStats::State st)
{
    switch (st) {
    case TorrentStats::Stopped:
        return QStringLiteral("Stopped");
    case TorrentStats::Checking:
        return QStringLiteral("Checking");
    case TorrentStats::Metadata:
        return QStringLiteral("Metadata");
    case TorrentStats::Downloading:
        return QStringLiteral("Downloading");
    case TorrentStats::Seeding:
        return QStringLiteral("Seeding");
    case TorrentStats::Error:
        return QStringLiteral("Error");
    }
    return QStringLiteral("Unknown");
}

void MainWindow::startDownload()
{
    if (m_workerRunning) {
        return;
    }
    const QString torrentPath = m_torrentEdit->text().trimmed();
    const QString saveDir = m_saveEdit->text().trimmed();
    if (torrentPath.isEmpty() || saveDir.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("btclient"),
                             QStringLiteral("Select a torrent/magnet and save directory."));
        return;
    }

    m_pauseRequested = false;
    m_stopRequested = false;
    m_workerRunning = true;
    setUiRunning(true);
    appendLog(QStringLiteral("Starting %1").arg(torrentPath));

    QThread *thread = QThread::create([this, torrentPath, saveDir]() {
        try {
            shared_ptr<Coroutine> root(Coroutine::spawn([this, torrentPath, saveDir]() {
                unique_ptr<TorrentSession> session(new TorrentSession());
                session->setDownloadDir(saveDir.toStdString());
                session->setListenPort(0);
                session->setDhtEnabled(true);
                session->setUtpEnabled(true);

                TorrentHandle handle;
                const QString lower = torrentPath.left(7).toLower();
                if (lower == QStringLiteral("magnet:")) {
                    MagnetLink magnet = MagnetLink::parse(torrentPath.toStdString());
                    if (!magnet.isValid()) {
                        QMetaObject::invokeMethod(this, [this, magnet]() {
                            appendLog(QStringLiteral("Invalid magnet: %1")
                                              .arg(QString::fromStdString(magnet.errorString())));
                        }, Qt::QueuedConnection);
                        return;
                    }
                    QMetaObject::invokeMethod(this, [this, magnet]() {
                        appendLog(QStringLiteral("Magnet infohash %1")
                                          .arg(QString::fromStdString(magnet.infoHash().toHex())));
                    }, Qt::QueuedConnection);
                    handle = session->addMagnet(magnet);
                } else {
                    TorrentMeta meta = TorrentMeta::fromFile(torrentPath.toStdString());
                    if (!meta.isValid()) {
                        QMetaObject::invokeMethod(this, [this, meta]() {
                            appendLog(QStringLiteral("Invalid torrent: %1")
                                              .arg(QString::fromStdString(meta.errorString())));
                        }, Qt::QueuedConnection);
                        return;
                    }
                    QMetaObject::invokeMethod(this, [this, meta]() {
                        appendLog(QStringLiteral("Infohash %1  size %2")
                                          .arg(QString::fromStdString(meta.infoHash().toHex()))
                                          .arg(meta.totalSize()));
                    }, Qt::QueuedConnection);
                    handle = session->addTorrent(meta);
                }
                if (!handle.isValid()) {
                    string err = session->errorString();
                    QMetaObject::invokeMethod(this, [this, err]() {
                        appendLog(QStringLiteral("Add failed: %1").arg(QString::fromStdString(err)));
                    }, Qt::QueuedConnection);
                    return;
                }

                session->start();
                string sessionErr = session->errorString();
                if (!sessionErr.empty()) {
                    QMetaObject::invokeMethod(this, [this, sessionErr]() {
                        appendLog(QStringLiteral("Session warning: %1").arg(QString::fromStdString(sessionErr)));
                    }, Qt::QueuedConnection);
                }

                {
                    QMutexLocker lock(&m_mutex);
                    m_session = std::move(session);
                    m_handle = handle;
                }

                while (true) {
                    {
                        QMutexLocker lock(&m_mutex);
                        if (m_stopRequested) {
                            if (m_session) {
                                m_session->stop();
                            }
                            break;
                        }
                        if (m_pauseRequested && m_handle.isValid()) {
                            m_handle.pause();
                        } else if (m_handle.isValid()) {
                            m_handle.resume();
                        }
                    }
                    if (handle.isFinished()) {
                        QMetaObject::invokeMethod(this, [this]() { appendLog(QStringLiteral("Download finished.")); },
                                                  Qt::QueuedConnection);
                        break;
                    }
                    TorrentStats st = handle.stats();
                    if (st.state() == TorrentStats::Error) {
                        QMetaObject::invokeMethod(this, [this, st]() {
                            appendLog(QStringLiteral("Error: %1").arg(QString::fromStdString(st.errorString())));
                        }, Qt::QueuedConnection);
                        break;
                    }
                    Coroutine::sleep(0.5f);
                }

                {
                    QMutexLocker lock(&m_mutex);
                    if (m_session) {
                        m_session->stop();
                        m_session.reset();
                    }
                    m_handle = TorrentHandle();
                    m_workerRunning = false;
                }
                QMetaObject::invokeMethod(this, [this]() {
                    setUiRunning(false);
                    m_timer->stop();
                    appendLog(QStringLiteral("Session stopped."));
                }, Qt::QueuedConnection);
            }));
            root->join();
        } catch (...) {
            QMutexLocker lock(&m_mutex);
            m_workerRunning = false;
            QMetaObject::invokeMethod(this, [this]() {
                setUiRunning(false);
                appendLog(QStringLiteral("Worker crashed."));
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    m_timer->start();
}

void MainWindow::pauseDownload()
{
    QMutexLocker lock(&m_mutex);
    m_pauseRequested = !m_pauseRequested;
    appendLog(m_pauseRequested ? QStringLiteral("Pause requested") : QStringLiteral("Resume requested"));
    m_pauseBtn->setText(m_pauseRequested ? QStringLiteral("Resume") : QStringLiteral("Pause"));
}

void MainWindow::stopDownload()
{
    {
        QMutexLocker lock(&m_mutex);
        m_stopRequested = true;
        if (m_session) {
            // stop from UI thread is unsafe for coroutines; worker watches flag
        }
    }
    for (int i = 0; i < 50 && m_workerRunning; ++i) {
        QThread::msleep(100);
        QCoreApplication::processEvents();
    }
    m_timer->stop();
    setUiRunning(false);
    m_pauseBtn->setText(QStringLiteral("Pause"));
}

void MainWindow::refreshStats()
{
    TorrentHandle handle;
    {
        QMutexLocker lock(&m_mutex);
        handle = m_handle;
    }
    if (!handle.isValid()) {
        return;
    }
    TorrentStats st = handle.stats();
    m_progress->setValue(static_cast<int>(st.progress() * 1000.0));
    m_statusLabel->setText(QStringLiteral("State: %1  progress %2%")
                                   .arg(stateName(st.state()))
                                   .arg(st.progress() * 100.0, 0, 'f', 1));
    m_peersLabel->setText(QStringLiteral("Peers: %1 connected / %2 known")
                                  .arg(st.peersConnected())
                                  .arg(st.peersTotal()));
    m_rateLabel->setText(QStringLiteral("↓ %1  ↑ %2  left %3")
                                 .arg(formatRate(st.downloadRate()))
                                 .arg(formatRate(st.uploadRate()))
                                 .arg(st.left()));
    if (!st.errorString().empty()) {
        appendLog(QString::fromStdString(st.errorString()));
    }
}
