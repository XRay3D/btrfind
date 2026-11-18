#include "daemon.h"

#include "reconcile.h"
#include "server.h"
#include "watcher.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QCborArray>
#include <QDir>
#include <QSocketNotifier>
#include <QtDebug>

#include <mutex>
#include <set>

using namespace Qt::StringLiterals;

namespace bx {

Daemon::Daemon(Options o, QObject* parent) : QObject(parent), o_(std::move(o)) {
    maint_.setMaxThreadCount(1);
    maint_.setExpiryTimeout(-1);
}

Daemon::~Daemon() {
    maint_.waitForDone();
    if (mounts_fd_ >= 0) close(mounts_fd_);
}

void Daemon::log(Volume* v, const std::string& msg) {
    if (v)
        qInfo().noquote() << u"[%1]"_s.arg(QString::fromStdString(v->path)) << QString::fromStdString(msg);
    else
        qInfo().noquote() << QString::fromStdString(msg);
}

bool Daemon::start() {
    if (geteuid() != 0)
        log(nullptr, "предупреждение: демон запущен не от root — индексация обходом каталогов, без живых обновлений "
                     "и догона");
    if (o_.save) QDir().mkpath(QString::fromStdString(o_.state_dir));

    watcher_ = new Watcher([this](const FsUuid& u, uint64_t s) { return find_volume(u, s); }, this);
    server_ = new Server(*this, this);
    connect(watcher_, &Watcher::changed, server_, &Server::notify_changed);
    connect(watcher_, &Watcher::overflow, this, [this](const QByteArray& u) {
        log(nullptr, "переполнение очереди fanotify — догон изменений");
        for (auto& v : volumes)
            if (std::equal(v->uuid.begin(), v->uuid.end(), u.begin())) schedule(v.get(), Task::CatchUp);
    });

    QString err;
    if (!server_->listen(o_.socket, err)) {
        log(nullptr, "не удалось открыть сокет " + o_.socket.toStdString() + ": " + err.toStdString());
        return false;
    }
    log(nullptr, "сокет: " + o_.socket.toStdString());

    // Изменения таблицы монтирования: poll(/proc/self/mountinfo) -> POLLPRI.
    mounts_fd_ = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mounts_fd_ >= 0) {
        mounts_sn_ = new QSocketNotifier(mounts_fd_, QSocketNotifier::Exception, this);
        connect(mounts_sn_, &QSocketNotifier::activated, this, [this] { mounts_timer_.start(); });
    }
    mounts_timer_.setSingleShot(true);
    mounts_timer_.setInterval(500);
    connect(&mounts_timer_, &QTimer::timeout, this, &Daemon::mounts_changed);

    if (o_.save) {
        save_timer_.setInterval(o_.save_interval_min * 60 * 1000);
        connect(&save_timer_, &QTimer::timeout, this, [this] { save_all(false); });
        save_timer_.start();
    }
    mounts_ = read_mountinfo();
    discover();
    return true;
}

Volume* Daemon::find_volume(const FsUuid& uuid, uint64_t subvol) {
    for (auto& v : volumes)
        if (v->subvol == subvol && v->uuid == uuid && v->state != VolState::Removed) return v.get();
    return nullptr;
}

Volume* Daemon::add_volume(const std::string& path, bool from_mount) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    FsStat st;
    SubvolInfo si;
    FsUuid uuid{};
    std::string err;
    if (!stat_at(fd, ".", st) || st.a.ino != BTRFS_FIRST_FREE_OBJECTID || !get_subvol_info(fd, si, err) ||
        !fs_uuid(fd, uuid, err) || find_volume(uuid, si.treeid)) {
        close(fd);
        return nullptr;
    }
    std::string device;
    size_t best = 0;
    for (const auto& m : mounts_)
        if (m.fstype == "btrfs" && path_within(path, m.mountpoint) && m.mountpoint.size() >= best) {
            device = m.source;  // самое глубокое btrfs-монтирование, содержащее путь
            best = m.mountpoint.size();
        }
    Volume* v;
    {
        std::unique_lock lk(volumes_mu);
        volumes.push_back(std::make_unique<Volume>(uint32_t(volumes.size()), path, uuid, si.treeid, device, st.dev, fd));
        v = volumes.back().get();
        from_mount_.push_back(from_mount);
        busy_.push_back(0);
    }
    log(v, "том: подтом '" + si.name + "' (id " + std::to_string(si.treeid) + "), ФС " + uuid_str(uuid));
    schedule(v, Task::Init);
    return v;
}

void Daemon::discover() {
    for (const auto& m : mounts_) {
        if (m.fstype != "btrfs") continue;
        bool known = false;
        for (auto& v : volumes)
            if (v->path == m.mountpoint && v->state != VolState::Removed) known = true;
        if (!known) add_volume(m.mountpoint, true);
    }
}

void Daemon::schedule(Volume* v, Task t) {
    if (busy_[v->id]) return;  // уже в работе; догон всё равно увидит свежие изменения
    busy_[v->id] = 1;
    const std::string dir = o_.state_dir;
    const bool use_saved = o_.save;
    maint_.start([this, v, t, dir, use_saved] {
        std::string msg;
        bool ok = false;
        if (t == Task::Init) {
            v->state = VolState::Loading;
            std::string lmsg;
            if (use_saved && v->load(dir, lmsg)) {
                log(v, lmsg);
                v->state = VolState::CatchingUp;
                ok = v->catch_up(msg);
            } else {
                if (use_saved) log(v, lmsg);
                v->state = VolState::Scanning;
                ok = v->full_scan(msg);
            }
        } else {
            v->state = VolState::CatchingUp;
            ok = v->catch_up(msg);
        }
        QMetaObject::invokeMethod(this, [this, v, ok, msg] { task_done(v, ok, msg); }, Qt::QueuedConnection);
    });
}

void Daemon::task_done(Volume* v, bool ok, const std::string& msg) {
    busy_[v->id] = 0;
    log(v, msg);
    if (!ok) {
        v->state = VolState::Error;
        return;
    }
    after_ready(v);
}

void Daemon::after_ready(Volume* v) {
    v->state = VolState::Ready;
    apply_mountpoints(v);
    if (v->privileged) add_nested(v);
    if (live_ok_ && geteuid() == 0 && !watcher_->watching(v->uuid)) {
        std::string err;
        if (watcher_->watch_fs(v->uuid, v->device, err))
            log(nullptr, "живые изменения: fanotify на ФС " + uuid_str(v->uuid) + " (" + v->device + ")");
        else {
            live_ok_ = false;
            log(nullptr, "fanotify недоступен: " + err + " — только догон при запуске");
        }
    }
    server_->notify_changed();
}

void Daemon::apply_mountpoints(Volume* v) {
    // Точки монтирования внутри тома, для которых этот том — самый глубокий владелец.
    std::set<std::string> inside;
    for (const auto& m : mounts_) {
        if (m.mountpoint == v->path || !path_within(m.mountpoint, v->path)) continue;
        const Volume* owner = nullptr;
        for (auto& o : volumes)
            if (o->state != VolState::Removed && o->path != m.mountpoint && path_within(m.mountpoint, o->path) &&
                (!owner || o->path.size() > owner->path.size()))
                owner = o.get();
        if (owner != v) continue;
        // Bind-монтирование того же каталога того же подтома (systemd ProtectSystem/ReadWritePaths,
        // ручной bind ro) показывает то же содержимое — ничего не скрывает.
        FsStat st;
        if (m.fstype == "btrfs" && stat_at(AT_FDCWD, m.mountpoint.c_str(), st) && st.exists && st.dev == v->dev) {
            std::shared_lock lk(v->mu);
            const uint32_t idx = v->index.lookup(m.mountpoint);
            if (idx != kNone && v->index.entries[idx].ino == st.a.ino) continue;
        }
        inside.insert(m.mountpoint);
    }
    std::unique_lock lk(v->mu);
    Reconciler rec(v->index, v->dev);
    for (const auto& mp : inside) {
        const uint32_t idx = v->index.lookup(mp);
        if (idx != kNone) rec.set_mountpoint(idx, true);
    }
    for (uint32_t i = 1; i < v->index.entries.size(); ++i) {
        const Entry& e = v->index.entries[i];
        if ((e.flags & (kMountpoint | kDeleted)) == kMountpoint && !inside.count(v->index.path(i)))
            rec.set_mountpoint(i, false);  // отмонтировали — содержимое снова индексируется
    }
}

void Daemon::add_nested(Volume* v) {
    std::vector<RootRef> refs;
    std::string err;
    if (!root_refs(v->fd, v->subvol, refs, err)) return;
    for (const auto& r : refs) {
        if (find_volume(v->uuid, r.child_id)) continue;
        std::string path;
        {
            std::shared_lock lk(v->mu);
            const uint32_t d = v->index.dir_for_ino(r.dirid);
            if (d == kNone) continue;
            path = v->index.path(d);
        }
        path = (path == "/" ? "" : path) + "/" + r.name;
        if (Volume* nv = add_volume(path, false)) log(nv, "вложенный подтом");
    }
}

void Daemon::save_all(bool final) {
    if (!o_.save) return;
    watcher_->drain();  // всё, что уже случилось, — в индексе
    for (auto& v : volumes) {
        if (v->state != VolState::Ready) continue;
        {
            std::shared_lock lk(v->mu);
            if (v->index.version == v->saved_version && !final) continue;
        }
        // При живом наблюдении индекс актуален на текущую транзакцию; без него — на момент скана.
        const uint64_t gen = (watcher_->watching(v->uuid) && v->privileged) ? v->current_generation() : v->generation.load();
        bool compacted = false;
        std::string msg;
        if (v->save(o_.state_dir, gen, compacted, msg)) v->generation = gen;
        log(v.get(), msg);
        if (compacted) server_->notify_changed();
    }
}

void Daemon::mounts_changed() {
    mounts_ = read_mountinfo();
    // Пропавшие точки монтирования томов
    for (auto& v : volumes) {
        if (!from_mount_[v->id] || v->state == VolState::Removed) continue;
        bool still = false;
        for (const auto& m : mounts_)
            if (m.mountpoint == v->path && m.fstype == "btrfs") still = true;
        if (!still) {
            log(v.get(), "том отмонтирован");
            v->state = VolState::Removed;
        }
    }
    discover();
    for (auto& v : volumes)
        if (v->state == VolState::Ready) apply_mountpoints(v.get());
    server_->notify_changed();
}

void Daemon::shutdown() {
    save_timer_.stop();
    maint_.waitForDone();
    save_all(true);
}

QCborMap Daemon::status() {
    QCborArray vols;
    for (auto& v : volumes) {
        uint64_t entries = 0;
        {
            std::shared_lock lk(v->mu, std::try_to_lock);
            if (lk.owns_lock()) entries = v->index.live;
        }
        vols.append(QCborMap{{u"path"_s, QString::fromStdString(v->path)},
                             {u"subvol"_s, qint64(v->subvol)},
                             {u"uuid"_s, QString::fromStdString(uuid_str(v->uuid))},
                             {u"entries"_s, qint64(entries)},
                             {u"state"_s, QString::fromLatin1(state_name(v->state))},
                             {u"generation"_s, qint64(v->generation.load())},
                             {u"live"_s, watcher_->watching(v->uuid)}});
    }
    return QCborMap{{u"op"_s, u"status"_s},
                    {u"version"_s, QString::fromLatin1(BTRFIND_VERSION)},
                    {u"volumes"_s, vols},
                    {u"events"_s, qint64(watcher_->stats.events)}};
}

}  // namespace bx
