#include "fan_watch.h"

#include "btrfs_tree.h"

#include <fcntl.h>
#include <sys/fanotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

namespace bx {
namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// Типы file handle btrfs (fs/btrfs/export.h): objectid u64, root_objectid u64, ...
constexpr int kBtrfsFidMin = 0x4d, kBtrfsFidMax = 0x4f;

std::string unescape_mount(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {  // \040 и т.п. в /proc/self/mountinfo
            r += char((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            r += s[i];
        }
    }
    return r;
}

struct MountRef {
    std::string path;
    int fd = -1;
};

// subvol id -> точка монтирования этого подтома (для open_by_handle_at и красивых путей).
std::map<uint64_t, MountRef> btrfs_mounts_by_subvol() {
    std::map<uint64_t, MountRef> m;
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) continue;
        std::istringstream a(line.substr(0, sep)), b(line.substr(sep + 3));
        std::string id, parent, devno, root, mnt, fstype;
        a >> id >> parent >> devno >> root >> mnt;
        b >> fstype;
        if (fstype != "btrfs") continue;
        mnt = unescape_mount(mnt);
        struct stat stbuf {};
        if (stat(mnt.c_str(), &stbuf) < 0 || stbuf.st_ino != BTRFS_FIRST_FREE_OBJECTID)
            continue;  // берём только монтирования корня подтома
        const int fd = open(mnt.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) continue;
        SubvolInfo si;
        std::string err;
        if (!get_subvol_info(fd, si, err) || m.count(si.treeid)) {
            close(fd);
            continue;
        }
        m[si.treeid] = {mnt, fd};
    }
    return m;
}

struct Resolver {
    std::map<uint64_t, MountRef> mounts;
    uint64_t watched_tree = 0, watched_ino = 0;
    std::string watched_path;
    int top_fd = -1;  // верхний подтом: запасной вариант для подтомов без своей точки монтирования
    uint64_t resolved = 0, unresolved = 0;

    std::string dir_path(const file_handle* fh) {
        uint64_t obj = 0, root = 0;
        const bool btrfs = fh->handle_type >= kBtrfsFidMin && fh->handle_type <= kBtrfsFidMax &&
                           fh->handle_bytes >= 16;
        if (btrfs) {
            std::memcpy(&obj, fh->f_handle, 8);
            std::memcpy(&root, fh->f_handle + 8, 8);
        }
        auto it = btrfs ? mounts.find(root) : mounts.end();
        const int mount_fd = it != mounts.end() ? it->second.fd : (btrfs ? top_fd : -1);
        if (mount_fd >= 0) {
            std::vector<char> h(sizeof(file_handle) + fh->handle_bytes);
            std::memcpy(h.data(), fh, h.size());
            const int dfd = open_by_handle_at(mount_fd, reinterpret_cast<file_handle*>(h.data()),
                                              O_PATH | O_CLOEXEC);
            if (dfd >= 0) {
                char lnk[64], out[PATH_MAX];
                snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", dfd);
                const ssize_t n = readlink(lnk, out, sizeof out - 1);
                close(dfd);
                if (n > 0) {
                    ++resolved;
                    return std::string(out, size_t(n));
                }
            }
        }
        if (btrfs && root == watched_tree && obj == watched_ino) {
            ++resolved;
            return watched_path;
        }
        ++unresolved;
        if (btrfs) return "<subvol " + std::to_string(root) + " ino " + std::to_string(obj) + ">";
        return "<handle type " + std::to_string(fh->handle_type) + ">";
    }
};

// Устройство btrfs, на котором лежит path (самая длинная подходящая точка монтирования).
std::string btrfs_source_for(const std::string& path) {
    char real[PATH_MAX];
    if (!realpath(path.c_str(), real)) return {};
    const std::string rp = real;
    std::ifstream f("/proc/self/mountinfo");
    std::string line, best_src;
    size_t best_len = 0;
    while (std::getline(f, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) continue;
        std::istringstream a(line.substr(0, sep)), b(line.substr(sep + 3));
        std::string id, parent, devno, root, mnt, fstype, src;
        a >> id >> parent >> devno >> root >> mnt;
        b >> fstype >> src;
        mnt = unescape_mount(mnt);
        const bool prefix = rp == mnt || mnt == "/" ||
                            (rp.compare(0, mnt.size(), mnt) == 0 && rp[mnt.size()] == '/');
        if (!prefix || mnt.size() < best_len) continue;
        best_len = mnt.size();
        best_src = fstype == "btrfs" ? src : std::string();
    }
    return best_src;
}

// Невидимое (отсоединённое) монтирование верхнего подтома (id 5) через новый mount API.
// FAN_MARK_FILESYSTEM на btrfs разрешён только через подтом, чей fsid совпадает с корнем ФС.
int open_btrfs_toplevel(const std::string& dev) {
    const int fsfd = fsopen("btrfs", FSOPEN_CLOEXEC);
    if (fsfd < 0) {
        perror("fsopen(btrfs)");
        return -1;
    }
    if (fsconfig(fsfd, FSCONFIG_SET_STRING, "source", dev.c_str(), 0) < 0 ||
        fsconfig(fsfd, FSCONFIG_SET_STRING, "subvolid", "5", 0) < 0 ||
        fsconfig(fsfd, FSCONFIG_CMD_CREATE, nullptr, nullptr, 0) < 0) {
        perror("fsconfig(subvolid=5)");
        close(fsfd);
        return -1;
    }
    const int mfd = fsmount(fsfd, FSMOUNT_CLOEXEC, MOUNT_ATTR_RDONLY);
    close(fsfd);
    if (mfd < 0) {
        perror("fsmount");
        return -1;
    }
    // fsmount даёт O_PATH-дескриптор; fanotify_mark/open_by_handle_at нужен обычный.
    const int dfd = openat(mfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(mfd);
    if (dfd < 0) perror("openat(toplevel)");
    return dfd;
}

std::string mask_str(uint64_t m) {
    std::string s;
    auto add = [&](uint64_t bit, const char* name) {
        if (m & bit) {
            if (!s.empty()) s += '|';
            s += name;
        }
    };
    add(FAN_CREATE, "CREATE");
    add(FAN_DELETE, "DELETE");
    add(FAN_RENAME, "RENAME");
    add(FAN_MOVED_FROM, "MOVED_FROM");
    add(FAN_MOVED_TO, "MOVED_TO");
    add(FAN_CLOSE_WRITE, "CLOSE_WRITE");
    add(FAN_ATTRIB, "ATTRIB");
    add(FAN_ONDIR, "DIR");
    return s;
}

std::string timestamp() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm t{};
    localtime_r(&ts.tv_sec, &t);
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d:%02d.%03ld", t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000);
    return b;
}

}  // namespace

int fan_watch(const std::string& path, const WatchOptions& opt) {
    unsigned init_flags = FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_REPORT_DFID_NAME;
    if (opt.filesystem) init_flags |= FAN_UNLIMITED_QUEUE;
    const int fan = fanotify_init(init_flags, O_RDONLY | O_LARGEFILE);
    if (fan < 0) {
        perror("fanotify_init");
        return 1;
    }

    uint64_t mask = FAN_CREATE | FAN_DELETE | FAN_RENAME | FAN_ONDIR;
    if (opt.modify) mask |= FAN_CLOSE_WRITE | FAN_ATTRIB;
    if (!opt.filesystem) mask |= FAN_EVENT_ON_CHILD;
    const unsigned mark_flags = FAN_MARK_ADD | (opt.filesystem ? FAN_MARK_FILESYSTEM : 0);
    int rc = fanotify_mark(fan, mark_flags, mask, AT_FDCWD, path.c_str());
    if (rc < 0 && errno == EINVAL) {  // ядро без FAN_RENAME (< 5.17)
        mask = (mask & ~uint64_t(FAN_RENAME)) | FAN_MOVED_FROM | FAN_MOVED_TO;
        rc = fanotify_mark(fan, mark_flags, mask, AT_FDCWD, path.c_str());
    }
    int top_fd = -1;
    if (rc < 0 && errno == EXDEV && opt.filesystem) {
        // Метка через подтом (@, @home) отклонена: у него свой fsid. Ставим её через верхний подтом.
        const std::string dev = btrfs_source_for(path);
        fprintf(stderr, "fanotify_mark: EXDEV (подтом btrfs) — метка через верхний подтом %s, subvolid=5\n",
                dev.c_str());
        if (!dev.empty() && (top_fd = open_btrfs_toplevel(dev)) >= 0)
            rc = fanotify_mark(fan, mark_flags, mask, top_fd, nullptr);
        else
            errno = EXDEV;
    }
    if (rc < 0) {
        const int e = errno;
        perror("fanotify_mark");
        if (e == EPERM)
            fprintf(stderr, "FAN_MARK_FILESYSTEM требует root. Без root: --dir (метка на каталог).\n");
        return 1;
    }

    Resolver res;
    res.mounts = btrfs_mounts_by_subvol();
    res.top_fd = top_fd;
    res.watched_path = path;
    {
        struct stat stbuf {};
        if (stat(path.c_str(), &stbuf) == 0) res.watched_ino = stbuf.st_ino;
        const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        SubvolInfo si;
        std::string err;
        if (fd >= 0 && get_subvol_info(fd, si, err)) res.watched_tree = si.treeid;
        if (fd >= 0) close(fd);
    }

    fprintf(stderr, "fanotify: %s на %s, события: %s\n",
            opt.filesystem ? "FAN_MARK_FILESYSTEM" : "метка каталога", path.c_str(),
            mask_str(mask).c_str());
    fprintf(stderr, "Подтома btrfs, для которых известны точки монтирования:\n");
    for (auto& [id, m] : res.mounts) fprintf(stderr, "  subvol %llu -> %s\n", (unsigned long long)id, m.path.c_str());
    if (top_fd >= 0) fprintf(stderr, "  остальные подтома -> через отсоединённое монтирование subvolid=5\n");
    fprintf(stderr, "Ctrl+C — остановить и показать статистику.\n\n");

    struct sigaction sa {};
    sa.sa_handler = on_signal;  // без SA_RESTART: read() прервётся с EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    alignas(fanotify_event_metadata) static char buf[256 * 1024];
    std::map<std::string, uint64_t> counts;
    uint64_t total = 0, overflows = 0;
    const auto t0 = std::chrono::steady_clock::now();

    while (!g_stop) {
        const ssize_t n = read(fan, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read(fanotify)");
            break;
        }
        ssize_t len = n;
        for (auto* md = reinterpret_cast<fanotify_event_metadata*>(buf); FAN_EVENT_OK(md, len);
             md = FAN_EVENT_NEXT(md, len)) {
            if (md->vers != FANOTIFY_METADATA_VERSION) {
                fprintf(stderr, "несовместимая версия fanotify metadata\n");
                return 1;
            }
            if (md->mask & FAN_Q_OVERFLOW) {
                ++overflows;
                printf("%s !! Q_OVERFLOW — события потеряны, нужен пересканирование\n", timestamp().c_str());
                continue;
            }
            std::string p1, p_old, p_new;
            const char* p = reinterpret_cast<const char*>(md) + md->metadata_len;
            const char* end = reinterpret_cast<const char*>(md) + md->event_len;
            while (p + sizeof(fanotify_event_info_header) <= end) {
                const auto* ih = reinterpret_cast<const fanotify_event_info_header*>(p);
                if (ih->len == 0) break;
                switch (ih->info_type) {
                    case FAN_EVENT_INFO_TYPE_FID:
                    case FAN_EVENT_INFO_TYPE_DFID:
                    case FAN_EVENT_INFO_TYPE_DFID_NAME:
                    case FAN_EVENT_INFO_TYPE_OLD_DFID_NAME:
                    case FAN_EVENT_INFO_TYPE_NEW_DFID_NAME: {
                        const auto* fi = reinterpret_cast<const fanotify_event_info_fid*>(ih);
                        const auto* fh = reinterpret_cast<const file_handle*>(fi->handle);
                        std::string full = res.dir_path(fh);
                        if (ih->info_type != FAN_EVENT_INFO_TYPE_FID &&
                            ih->info_type != FAN_EVENT_INFO_TYPE_DFID) {
                            const char* nm = reinterpret_cast<const char*>(fh->f_handle) + fh->handle_bytes;
                            if (*nm && strcmp(nm, ".") != 0) {
                                if (full != "/") full += '/';
                                full += nm;
                            }
                        }
                        if (ih->info_type == FAN_EVENT_INFO_TYPE_OLD_DFID_NAME)
                            p_old = full;
                        else if (ih->info_type == FAN_EVENT_INFO_TYPE_NEW_DFID_NAME)
                            p_new = full;
                        else
                            p1 = full;
                        break;
                    }
                    default:
                        break;
                }
                p += ih->len;
            }
            const std::string ms = mask_str(md->mask);
            ++counts[ms];
            ++total;
            if (!opt.quiet) {
                if (!p_old.empty() || !p_new.empty())
                    printf("%s %-16s %s -> %s\n", timestamp().c_str(), ms.c_str(),
                           p_old.empty() ? "<каталог вне метки>" : p_old.c_str(),
                           p_new.empty() ? "<каталог вне метки>" : p_new.c_str());
                else
                    printf("%s %-16s %s\n", timestamp().c_str(), ms.c_str(), p1.c_str());
                fflush(stdout);
            }
        }
    }

    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "\nСобытий: %llu за %.1f с (%.1f/с), переполнений очереди: %llu\n",
            (unsigned long long)total, sec, sec > 0 ? total / sec : 0.0, (unsigned long long)overflows);
    fprintf(stderr, "Пути разрешены: %llu, не разрешены: %llu\n", (unsigned long long)res.resolved,
            (unsigned long long)res.unresolved);
    for (auto& [k, v] : counts) fprintf(stderr, "  %-24s %llu\n", k.c_str(), (unsigned long long)v);
    for (auto& [id, m] : res.mounts) close(m.fd);
    if (top_fd >= 0) close(top_fd);
    close(fan);
    return 0;
}

}  // namespace bx
