#include "client.h"

#include <QEventLoop>

using namespace Qt::StringLiterals;

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
        emit disconnected(u"соединение с демоном потеряно"_s);
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
    send(QCborMap{{u"op"_s, u"search"_s},
                  {u"id"_s, id},
                  {u"q"_s, q},
                  {u"filter"_s, o.filter},
                  {u"case"_s, o.case_sensitive},
                  {u"regex"_s, o.regex},
                  {u"path"_s, o.path},
                  {u"sort"_s, o.sort},
                  {u"desc"_s, o.desc}});
    return id;
}

void Client::requestRows(qint64 id, qint64 from, qint64 count) {
    send(QCborMap{{u"op"_s, u"rows"_s},
                  {u"id"_s, id},
                  {u"from"_s, from},
                  {u"count"_s, count}});
}

void Client::requestSum(qint64 id, const QCborArray& ranges, qint64 tag) {
    send(QCborMap{{u"op"_s, u"sum"_s},
                  {u"id"_s, id},
                  {u"tag"_s, tag},
                  {u"ranges"_s, ranges}});
}

void Client::requestStatus() { send(QCborMap{{u"op"_s, u"status"_s}}); }

void Client::on_ready_read() {
    reader_.feed(sock_.readAll());
    QCborMap m;
    bool error = false;
    while (reader_.next(m, error)) {
        emit message(m);
        const QString op = m.value(u"op"_s).toString();
        if (op == u"result"_s)
            emit result(m.value(u"id"_s).toInteger(), m.value(u"total"_s).toInteger(), m.value(u"ms"_s).toDouble(),
                        m.value(u"error"_s).toString(), m);
        else if (op == u"sum"_s)
            emit sum(m.value(u"id"_s).toInteger(), m.value(u"tag"_s).toInteger(), m.value(u"bytes"_s).toInteger(),
                     m.value(u"files"_s).toInteger(), m.value(u"dirs"_s).toInteger());
        else if (op == u"rows"_s)
            emit rows(m.value(u"id"_s).toInteger(), m.value(u"from"_s).toInteger(), m.value(u"rows"_s).toArray());
        else if (op == u"status"_s)
            emit status(m);
        else if (op == u"changed"_s)
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
        if (m.value(u"op"_s).toString() == op && (id < 0 || m.value(u"id"_s).toInteger() == id)) {
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
