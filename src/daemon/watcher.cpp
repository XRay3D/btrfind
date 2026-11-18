#include "watcher.h"

#include "mounts.h"
#include "reconcile.h"

#include <fcntl.h>
#include <sys/fanotify.h>
#include <unistd.h>

#include <QByteArray>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>

namespace bx {
namespace {

constexpr int kBtrfsFidMin = 0x4d, kBtrfsFidMax = 0x4f;  // FILEID_BTRFS_*: objectid, root_objectid, ...
constexpr uint64_t kMask = FAN_CREATE | FAN_DELETE | FAN_RENAME | FAN_CLOSE_WRITE | FAN_ATTRIB | FAN_ONDIR;

struct Item {
    Volume* vol;
    uint64_t dir_ino;
    std::vector<char> handle;  // struct file_handle + f_handle
    std::string name;
};

}  // namespace

Watcher::Watcher(Resolver r, QObject* parent) : QObject(parent), resolve_(std::move(r)) {}

Watcher::~Watcher() {
    for (auto& g : groups_) {
        if (g->fan >= 0) close(g->fan);
        if (g->top >= 0) close(g->top);
    }
}

bool Watcher::watching(const FsUuid& uuid) const {
    for (auto& g : groups_)
        if (g->uuid == uuid) return true;
    return false;
}

bool Watcher::watch_fs(const FsUuid& uuid, const std::string& device, std::string& err) {
    if (watching(uuid)) return true;
    auto g = std::make_unique<Group>();
    g->uuid = uuid;
    g->fan = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK | FAN_REPORT_DFID_NAME | FAN_UNLIMITED_QUEUE,
                           O_RDONLY | O_LARGEFILE);
    if (g->fan < 0) {
        err = std::string("fanotify_init: ") + strerror(errno);
        return false;
    }
    g->top = open_btrfs_toplevel(device, err);
    if (g->top < 0) {
        close(g->fan);
        return false;
    }
    if (fanotify_mark(g->fan, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, kMask, g->top, nullptr) < 0) {
        err = std::string("fanotify_mark: ") + strerror(errno);
        close(g->fan);
        close(g->top);
        return false;
    }
    Group* raw = g.get();
    g->sn = new QSocketNotifier(g->fan, QSocketNotifier::Read, this);
    connect(g->sn, &QSocketNotifier::activated, this, [this, raw] { read_events(*raw); });
    groups_.push_back(std::move(g));
    return true;
}

void Watcher::drain() {
    for (auto& g : groups_) read_events(*g);
}

void Watcher::read_events(Group& g) {
    alignas(fanotify_event_metadata) static char buf[256 * 1024];
    std::vector<Item> items;
    bool overflowed = false;

    for (;;) {
        const ssize_t n = read(g.fan, buf, sizeof buf);
        if (n <= 0) break;  // EAGAIN — очередь пуста
        ssize_t len = n;
        for (auto* md = reinterpret_cast<fanotify_event_metadata*>(buf); FAN_EVENT_OK(md, len);
             md = FAN_EVENT_NEXT(md, len)) {
            ++stats.events;
            if (md->mask & FAN_Q_OVERFLOW) {
                overflowed = true;
                continue;
            }
            const char* p = reinterpret_cast<const char*>(md) + md->metadata_len;
            const char* end = reinterpret_cast<const char*>(md) + md->event_len;
            std::vector<Item> ev;  // для RENAME: сначала новое имя, потом старое
            while (p + sizeof(fanotify_event_info_header) <= end) {
                const auto* ih = reinterpret_cast<const fanotify_event_info_header*>(p);
                if (ih->len == 0) break;
                const uint8_t t = ih->info_type;
                if (t == FAN_EVENT_INFO_TYPE_DFID_NAME || t == FAN_EVENT_INFO_TYPE_OLD_DFID_NAME ||
                    t == FAN_EVENT_INFO_TYPE_NEW_DFID_NAME) {
                    const auto* fi = reinterpret_cast<const fanotify_event_info_fid*>(ih);
                    const auto* fh = reinterpret_cast<const file_handle*>(fi->handle);
                    if (fh->handle_type >= kBtrfsFidMin && fh->handle_type <= kBtrfsFidMax && fh->handle_bytes >= 16) {
                        uint64_t obj = 0, root = 0;
                        std::memcpy(&obj, fh->f_handle, 8);
                        std::memcpy(&root, fh->f_handle + 8, 8);
                        if (Volume* v = resolve_(g.uuid, root)) {
                            Item it{v, obj, {}, reinterpret_cast<const char*>(fh->f_handle) + fh->handle_bytes};
                            const char* hb = reinterpret_cast<const char*>(fh);
                            it.handle.assign(hb, hb + sizeof(file_handle) + fh->handle_bytes);
                            if (t == FAN_EVENT_INFO_TYPE_NEW_DFID_NAME)
                                ev.insert(ev.begin(), std::move(it));
                            else
                                ev.push_back(std::move(it));
                        }
                    }
                }
                p += ih->len;
            }
            for (auto& it : ev) items.push_back(std::move(it));
        }
    }

    // Применение: подряд идущие события одного тома — под одной блокировкой.
    bool any = false;
    size_t i = 0;
    while (i < items.size()) {
        Volume* v = items[i].vol;
        const VolState st = v->state.load();
        size_t j = i;
        while (j < items.size() && items[j].vol == v) ++j;
        if (st == VolState::Pending || st == VolState::Loading || st == VolState::Removed) {
            i = j;
            continue;
        }
        std::unique_lock lk(v->mu);
        if (v->index.entries.empty()) {  // первый скан ещё идёт — изменения подхватит догон после него
            i = j;
            continue;
        }
        Reconciler rec(v->index, v->dev);
        for (; i < j; ++i) {
            Item& it = items[i];
            if (i > 0 && items[i - 1].vol == v && items[i - 1].dir_ino == it.dir_ino && items[i - 1].name == it.name &&
                items[i - 1].handle == it.handle)
                continue;  // повтор (например, серия CLOSE_WRITE)
            const int dirfd = open_by_handle_at(v->fd, reinterpret_cast<file_handle*>(it.handle.data()),
                                                O_PATH | O_DIRECTORY | O_CLOEXEC);
            if (dirfd < 0) continue;  // каталог уже удалён — его удаление придёт отдельным событием
            const uint32_t dir = v->index.dir_for_ino(it.dir_ino);
            if (dir != kNone) {
                rec.reconcile_name(dir, dirfd, it.name);
                ++stats.applied;
                any = true;
            } else {
                // Каталог неизвестен индексу: найти ближайшего известного предка и сверить от него.
                std::string pth = fd_path(dirfd), child;
                uint32_t anc = kNone;
                while (path_within(pth, v->path)) {
                    anc = v->index.lookup(pth);
                    if (anc != kNone || pth == v->path) break;
                    const size_t slash = pth.rfind('/');
                    child = pth.substr(slash + 1);
                    pth = slash == 0 ? "/" : pth.substr(0, slash);
                }
                if (anc != kNone && !child.empty()) {
                    const int afd = open(pth.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
                    if (afd >= 0) {
                        rec.reconcile_name(anc, afd, child);
                        close(afd);
                        any = true;
                    }
                } else {
                    ++stats.unknown;
                }
            }
            close(dirfd);
        }
    }
    if (overflowed) {
        ++stats.overflows;
        emit overflow(QByteArray(reinterpret_cast<const char*>(g.uuid.data()), 16));
    }
    if (any) emit changed();
}

}  // namespace bx
