#include "server.h"

#include "daemon.h"
#include "query.h"

#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <QCborArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QThreadPool>

#include <chrono>
#include <mutex>
#include <shared_mutex>

namespace bx {
namespace {

Access peer_access(QLocalSocket* s) {
    Access a;
    ucred cr{};
    socklen_t len = sizeof cr;
    if (getsockopt(int(s->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &cr, &len) < 0) {
        a.uid = UINT32_MAX - 1;  // неизвестный — видит только общедоступное
        return a;
    }
    a.uid = cr.uid;
    a.gids.push_back(cr.gid);
    passwd pw{}, *res = nullptr;
    char buf[4096];
    if (getpwuid_r(cr.uid, &pw, buf, sizeof buf, &res) == 0 && res) {
        int n = 64;
        std::vector<gid_t> g(n);
        if (getgrouplist(pw.pw_name, pw.pw_gid, g.data(), &n) < 0) {
            g.resize(n);
            getgrouplist(pw.pw_name, pw.pw_gid, g.data(), &n);
        }
        for (int i = 0; i < n; ++i) a.gids.push_back(g[i]);
    }
    return a;
}

SortKey sort_key(const QString& s) {
    if (s == "path") return SortKey::Path;
    if (s == "size") return SortKey::Size;
    if (s == "mtime") return SortKey::Mtime;
    return SortKey::Name;
}

}  // namespace

// ---------------------------------------------------------------- Session

Session::Session(Daemon& d, QLocalSocket* sock, QObject* parent) : QObject(parent), d_(d), sock_(sock) {
    sock_->setParent(this);
    access_ = peer_access(sock_);
    connect(sock_, &QLocalSocket::readyRead, this, &Session::on_ready_read);
    connect(sock_, &QLocalSocket::disconnected, this, &QObject::deleteLater);
}

Session::~Session() {
    if (cancel_) cancel_->store(true);
}

void Session::push(const QCborMap& m) {
    if (sock_->state() == QLocalSocket::ConnectedState) proto::send(sock_, m);
}

void Session::on_ready_read() {
    reader_.feed(sock_->readAll());
    QCborMap m;
    bool error = false;
    while (reader_.next(m, error)) handle(m);
    if (error) sock_->disconnectFromServer();
}

void Session::handle(const QCborMap& m) {
    const QString op = m.value("op").toString();
    if (op == "search")
        start_search(m);
    else if (op == "rows")
        send_rows(m);
    else if (op == "status")
        push(d_.status());
}

void Session::start_search(const QCborMap& m) {
    const qint64 id = m.value("id").toInteger();
    latest_id_ = id;
    if (cancel_) cancel_->store(true);  // предыдущий поиск больше не нужен
    cancel_ = std::make_shared<std::atomic<bool>>(false);

    QueryOptions qo;
    qo.case_sensitive = m.value("case").toBool();
    qo.regex = m.value("regex").toBool();
    qo.match_path = m.value("path").toBool();
    const std::string text = m.value("q").toString().toStdString();
    const std::string filter = m.value("filter").toString().toStdString();  // доп. условия (пресет GUI)
    const SortKey key = sort_key(m.value("sort").toString());
    const bool desc = m.value("desc").toBool();
    const Access access = access_;
    auto cancel = cancel_;
    QPointer<Session> self(this);
    Daemon* d = &d_;

    QThreadPool::globalInstance()->start([=] {
        const auto t0 = std::chrono::steady_clock::now();
        QCborMap reply{{QStringLiteral("op"), QStringLiteral("result")}, {QStringLiteral("id"), id}};
        std::vector<Ref> refs;
        std::vector<uint64_t> epochs;
        {
            std::shared_lock vl(d->volumes_mu);
            std::vector<std::shared_lock<std::shared_mutex>> locks;
            std::vector<SearchVolume> svs;
            epochs.assign(d->volumes.size(), 0);
            for (auto& v : d->volumes) {
                const VolState st = v->state.load();
                if (st == VolState::Pending || st == VolState::Loading || st == VolState::Removed ||
                    st == VolState::Error)
                    continue;
                locks.emplace_back(v->mu);
                if (v->index.entries.empty()) continue;
                svs.push_back({&v->index, v->id});
                epochs[v->id] = v->index.epoch;
            }
            Query q = parse_query(text, qo);
            if (q.ok() && !filter.empty()) {
                QueryOptions fo;
                fo.case_sensitive = qo.case_sensitive;
                Query f = parse_query(filter, fo);
                if (!f.ok()) q.error = f.error;
                for (auto& g : f.groups) q.groups.push_back(std::move(g));
            }
            if (!q.ok()) {
                reply.insert(QStringLiteral("error"), QString::fromStdString(q.error));
            } else {
                refs = run_search(q, svs, &access, cancel.get(), nullptr);
                if (!cancel->load()) sort_results(refs, svs, key, desc);
            }
        }
        if (cancel->load()) return;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        reply.insert(QStringLiteral("total"), qint64(refs.size()));
        reply.insert(QStringLiteral("ms"), ms);
        QMetaObject::invokeMethod(
            d,  // контекст — демон (живёт всегда); сессия могла уже закрыться
            [self, id, reply, refs = std::move(refs), epochs = std::move(epochs)]() mutable {
                if (!self || id != self->latest_id_) return;
                self->result_id_ = id;
                self->results_ = std::move(refs);
                self->epochs_ = std::move(epochs);
                self->push(reply);
            },
            Qt::QueuedConnection);
    });
}

void Session::send_rows(const QCborMap& m) {
    const qint64 id = m.value("id").toInteger();
    const qint64 from = std::max<qint64>(0, m.value("from").toInteger());
    const qint64 count = std::clamp<qint64>(m.value("count").toInteger(), 0, 5000);
    QCborMap reply{{QStringLiteral("op"), QStringLiteral("rows")}, {QStringLiteral("id"), id},
                   {QStringLiteral("from"), from}};
    QCborArray rows;
    if (id == result_id_) {
        std::shared_lock vl(d_.volumes_mu);
        const qint64 to = std::min<qint64>(from + count, qint64(results_.size()));
        Volume* locked = nullptr;
        std::shared_lock<std::shared_mutex> lk;
        for (qint64 k = from; k < to; ++k) {
            const Ref r = results_[size_t(k)];
            const uint32_t vid = ref_vol(r), idx = ref_idx(r);
            Volume* v = vid < d_.volumes.size() ? d_.volumes[vid].get() : nullptr;
            if (v != locked) {
                lk = v ? std::shared_lock<std::shared_mutex>(v->mu) : std::shared_lock<std::shared_mutex>();
                locked = v;
            }
            const Index* ix = v ? &v->index : nullptr;
            if (!ix || ix->epoch != epochs_[vid] || idx >= ix->entries.size()) {
                rows.append(QCborArray{QString(), QString(), 0, 0, false});  // запись устарела
                continue;
            }
            const std::string_view nm = ix->name(idx);
            const Entry& e = ix->entries[idx];
            rows.append(QCborArray{QString::fromUtf8(nm.data(), qsizetype(nm.size())),
                                   QString::fromStdString(ix->dir_path(idx)), qint64(e.size), qint64(e.mtime),
                                   bool(e.flags & kDir)});
        }
    }
    reply.insert(QStringLiteral("rows"), rows);
    push(reply);
}

// ---------------------------------------------------------------- Server

Server::Server(Daemon& d, QObject* parent) : QObject(parent), d_(d) {
    changed_timer_.setSingleShot(true);
    changed_timer_.setInterval(300);
    connect(&changed_timer_, &QTimer::timeout, this, [this] {
        const QCborMap m{{QStringLiteral("op"), QStringLiteral("changed")}};
        for (auto& s : sessions_)
            if (s) s->push(m);
        std::erase_if(sessions_, [](const QPointer<Session>& s) { return s.isNull(); });
    });
    connect(&srv_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* sock = srv_.nextPendingConnection()) sessions_.emplace_back(new Session(d_, sock, this));
    });
}

bool Server::listen(const QString& path, QString& err) {
    if (path.toLocal8Bit().size() >= 108) {  // sizeof(sockaddr_un::sun_path)
        err = QStringLiteral("путь сокета длиннее 107 байт");
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QLocalServer::removeServer(path);
    srv_.setSocketOptions(QLocalServer::WorldAccessOption);
    if (!srv_.listen(path)) {
        err = srv_.errorString();
        return false;
    }
    return true;
}

void Server::notify_changed() {
    if (!changed_timer_.isActive()) changed_timer_.start();
}

}  // namespace bx
