#ifndef BTCLIENT_MAINWINDOW_H
#define BTCLIENT_MAINWINDOW_H

#include <QMainWindow>
#include <QMutex>
#include <QString>
#include <memory>

#include "qtng/bt.h"

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTextEdit;
class QTimer;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void browseTorrent();
    void browseSaveDir();
    void startDownload();
    void pauseDownload();
    void stopDownload();
    void refreshStats();

private:
    void appendLog(const QString &line);
    void setUiRunning(bool running);

    QLineEdit *m_torrentEdit;
    QLineEdit *m_saveEdit;
    QPushButton *m_startBtn;
    QPushButton *m_pauseBtn;
    QPushButton *m_stopBtn;
    QProgressBar *m_progress;
    QLabel *m_statusLabel;
    QLabel *m_peersLabel;
    QLabel *m_rateLabel;
    QTextEdit *m_log;
    QTimer *m_timer;

    std::unique_ptr<qtng::TorrentSession> m_session;
    qtng::TorrentHandle m_handle;
    QMutex m_mutex;
    bool m_workerRunning;
    bool m_pauseRequested;
    bool m_stopRequested;
};

#endif
