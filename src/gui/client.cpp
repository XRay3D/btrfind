#include "client.h"

#include <QEventLoop>

namespace bx {

Client::Client(QString socket_path, QObject* parent) : QObject(parent), path_(std::move(socket_path)) {
    reconnect_.setInterval(2000);
    reconnect_.setSingleShot(true);
    connect(&reconnect_, &QTimer::timeout, this, &Client::connectToDaemon);
    connect(&sock_, &QLocalSocket::connected, this, [this] {
        reader_ = {};
        emit connected();
    });
    connect(&sock_, &QLocalSocket::disconnected, this, [this] {
        emit disconnected(QStringLiteral("соединение с демоном потеряно"));
        reconnect_.start();
    });
    connect(&sock_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (sock_.state() != QLocalSocket::ConnectedState) {
            emit disconnected(sock_.errorString());
            reconnect_.start();
        }
    });
    connect(&sock_, &QLocalSocket::readyRead, this, &Client::on_ready_read);
}

void Client::connectToDaemon() {
    if (sock_.state() != QLocalSocket::UnconnectedState) return;
    sock_.connectToServer(path_);
}

void Client::send(const QCborMap& m) {
    if (isConnected()) proto::send(&sock_, m);
}

qint64 Client::search(const QString& q, const SearchOpts& o) {
    const qint64 id = next_id_++;
    send(QCborMap{{QStringLiteral("op"), QStringLiteral("search")},
                  {QStringLiteral("id"), id},
                  {QStringLiteral("q"), q},
                  {QStringLiteral("filter"), o.filter},
                  {QStringLiteral("case"), o.case_sensitive},
                  {QStringLiteral("regex"), o.regex},
                  {QStringLiteral("path"), o.path},
                  {QStringLiteral("sort"), o.sort},
                  {QStringLiteral("desc"), o.desc}});
    return id;
}

void Client::requestRows(qint64 id, qint64 from, qint64 count) {
    send(QCborMap{{QStringLiteral("op"), QStringLiteral("rows")},
                  {QStringLiteral("id"), id},
                  {QStringLiteral("from"), from},
                  {QStringLiteral("count"), count}});
}

void Client::requestStatus() { send(QCborMap{{QStringLiteral("op"), QStringLiteral("status")}}); }

void Client::on_ready_read() {
    reader_.feed(sock_.readAll());
    QCborMap m;
    bool error = false;
    while (reader_.next(m, error)) {
        emit message(m);
        const QString op = m.value("op").toString();
        if (op == "result")
            emit result(m.value("id").toInteger(), m.value("total").toInteger(), m.value("ms").toDouble(),
                        m.value("error").toString());
        else if (op == "rows")
            emit rows(m.value("id").toInteger(), m.value("from").toInteger(), m.value("rows").toArray());
        else if (op == "status")
            emit status(m);
        else if (op == "changed")
            emit changed();
    }
    if (error) sock_.abort();
}

bool Client::waitConnected(int ms) {
    connectToDaemon();
    return sock_.waitForConnected(ms);
}

QCborMap Client::waitFor(const QString& op, qint64 id, int ms) {
    QCborMap got;
    QEventLoop loop;
    QTimer t;
    t.setSingleShot(true);
    connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    auto c = connect(this, &Client::message, &loop, [&](const QCborMap& m) {
        if (m.value("op").toString() == op && (id < 0 || m.value("id").toInteger() == id)) {
            got = m;
            loop.quit();
        }
    });
    auto d = connect(&sock_, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
    t.start(ms);
    loop.exec();
    disconnect(c);
    disconnect(d);
    return got;
}

}  // namespace bx
