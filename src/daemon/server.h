#pragma once
// Сервер запросов: QLocalServer на unix-сокете, по сессии на клиента.

#include "protocol.h"
#include "search.h"

#include <QCborMap>
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <memory>
#include <vector>

class QLocalSocket;

namespace bx {

class Daemon;

class Session : public QObject {
    Q_OBJECT
public:
    Session(Daemon& d, QLocalSocket* sock, QObject* parent);
    ~Session() override;
    void push(const QCborMap& m);

private:
    void on_ready_read();
    void handle(const QCborMap& m);
    void start_search(const QCborMap& m);
    void send_rows(const QCborMap& m);
    void send_sum(const QCborMap& m);

    Daemon& d_;
    QLocalSocket* sock_;
    proto::FrameReader reader_;
    Access access_;
    qint64 latest_id_ = -1;
    std::shared_ptr<std::atomic<bool>> cancel_;
    // Результат последнего завершённого поиска
    qint64 result_id_ = -1;
    std::vector<Ref> results_;
    std::vector<uint64_t> epochs_;  // epoch индекса тома на момент поиска (по id тома)
};

class Server : public QObject {
    Q_OBJECT
public:
    explicit Server(Daemon& d, QObject* parent = nullptr);
    bool listen(const QString& path, QString& err);
    void notify_changed();  // с задержкой разослать {op:"changed"}

private:
    Daemon& d_;
    QLocalServer srv_;
    QTimer changed_timer_;
    std::vector<QPointer<Session>> sessions_;
};

}  // namespace bx
