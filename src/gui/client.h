#pragma once
// Клиент демона btrfindd: асинхронный (сигналы) с автоматическим переподключением.

#include "protocol.h"

#include <QCborArray>
#include <QCborMap>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

namespace bx {

using namespace Qt::StringLiterals;

struct SearchOpts {
    QString filter;
    bool case_sensitive = false;
    bool regex = false;
    bool path = false;
    QString sort = u"name"_s;
    bool desc = false;
};

class Client : public QObject {
    Q_OBJECT
public:
    explicit Client(QString socket_path, QObject* parent = nullptr);

    void connectToDaemon();
    bool isConnected() const { return sock_.state() == QLocalSocket::ConnectedState; }
    QString socketPath() const { return path_; }

    qint64 search(const QString& q, const SearchOpts& o);
    void requestRows(qint64 id, qint64 from, qint64 count);
    void requestStatus();
    // Сумма по диапазонам строк результата id: [[from, to), ...]; tag возвращается в ответе.
    void requestSum(qint64 id, const QCborArray& ranges, qint64 tag);

    // Синхронное ожидание ответа (для консольного режима).
    bool waitConnected(int ms);
    QCborMap waitFor(const QString& op, qint64 id, int ms);

signals:
    void connected();
    void disconnected(const QString& reason);
    void result(qint64 id, qint64 total, double ms, const QString& error, const QCborMap& m);
    void sum(qint64 id, qint64 tag, qint64 bytes, qint64 files, qint64 dirs);
    void rows(qint64 id, qint64 from, const QCborArray& rows);
    void status(const QCborMap& m);
    void changed();
    void message(const QCborMap& m);

private:
    void on_ready_read();
    void send(const QCborMap& m);

    QString path_;
    QLocalSocket sock_;
    proto::FrameReader reader_;
    QTimer reconnect_;
    qint64 next_id_ = 1;
};

}  // namespace bx
