#include "volume.h"

#include "fold.h"
#include "reconcile.h"
#include "storage.h"
#include "walk_scan.h"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace bx {
namespace {
double now_sec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string fmt(const char* f, auto... args) {
    char b[1024];
    snprintf(b, sizeof b, f, args...);
    return b;
}
}  // namespace

const char* state_name(VolState s) {
    switch (s) {
        case VolState::Pending: return "pending";
        case VolState::Loading: return "loading";
        case VolState::Scanning: return "scanning";
        case VolState::CatchingUp: return "catching-up";
        case VolState::Ready: return "ready";
        case VolState::Error: return "error";
        case VolState::Removed: return "removed";
    }
    return "?";
}

Volume::Volume(uint32_t id_, std::string path_, FsUuid uuid_, uint64_t subvol_, std::string device_, DevId dev_, int fd_)
    : id(id_), path(std::move(path_)), uuid(uuid_), subvol(subvol_), device(std::move(device_)), dev(dev_), fd(fd_) {}

Volume::~Volume() {
    if (fd >= 0) close(fd);
}

std::string Volume::state_file(const std::string& dir) const {
    return dir + "/" + uuid_str(uuid) + "-" + std::to_string(subvol) + ".idx";
}

uint64_t Volume::current_generation() const {
    SubvolInfo si;
    std::string err;
    return get_subvol_info(fd, si, err) ? si.generation : 0;
}

bool Volume::load(const std::string& dir, std::string& log) {
    const double t0 = now_sec();
    Index nx;
    StoredMeta meta, expect;
    expect.uuid = uuid;
    expect.subvol = subvol;
    std::string err;
    if (!load_index(state_file(dir), nx, meta, expect, err)) {
        log = "индекс не загружен: " + err;
        return false;
    }
    if (nx.root_path != path) {  // том смонтирован в другое место — пути внутри индекса относительные
        nx.root_path = path;
        nx.lroot_path = fold_case(path == "/" ? std::string() : path);
    }
    {
        std::unique_lock lk(mu);
        nx.epoch = index.epoch + 1;
        index = std::move(nx);
        saved_version = index.version;
    }
    generation = meta.generation;
    log = fmt("загружен индекс: %llu записей за %.2f с, generation %llu", (unsigned long long)index.live,
              now_sec() - t0, (unsigned long long)meta.generation);
    return true;
}

bool Volume::full_scan(std::string& log) {
    const double t0 = now_sec();
    const uint64_t g0 = current_generation();
    Index nx;
    std::string err;
    TreeScanStats st;
    const char* method = "ioctl";
    if (scan_subvolume(fd, path, 0, nx, st, err)) {
        privileged = true;
        nested = st.nested;
    } else {
        privileged = false;  // без root: обход каталогов
        method = "обход (нет прав на ioctl)";
        WalkStats ws;
        std::string werr;
        if (!walk_scan(path, {}, nx, ws, werr)) {
            log = "скан не удался: " + err + "; " + werr;
            return false;
        }
    }
    {
        std::unique_lock lk(mu);
        nx.epoch = index.epoch + 1;
        index = std::move(nx);
    }
    generation = g0;
    log = fmt("полный скан (%s): %llu записей за %.2f с", method, (unsigned long long)index.live, now_sec() - t0);
    if (privileged) {  // изменения, случившиеся во время скана
        std::string clog;
        catch_up(clog);
        log += "; " + clog;
    }
    return true;
}

bool Volume::catch_up(std::string& log) {
    if (!privileged) return full_scan(log);
    const double t0 = now_sec();
    const uint64_t g0 = current_generation();
    const uint64_t since = generation;
    ChangeSet cs;
    std::string err;
    if (!collect_changes(fd, since, cs, err)) {
        if (errno == EPERM || errno == EACCES) {
            privileged = false;
            return full_scan(log);
        }
        log = "догон не удался: " + err;
        return false;
    }
    Reconciler::Stats rs;
    {
        std::unique_lock lk(mu);
        Reconciler rec(index, dev);
        rec.apply_changes(fd, cs);
        rs = rec.stats;
    }
    generation = g0;
    log = fmt("догон с generation %llu: изменённых inode %zu, +%llu −%llu ↻%llu перенесено %llu, %.3f с",
              (unsigned long long)since, cs.inodes.size(), (unsigned long long)rs.added,
              (unsigned long long)rs.removed, (unsigned long long)rs.updated, (unsigned long long)rs.moved,
              now_sec() - t0);
    return true;
}

bool Volume::save(const std::string& dir, uint64_t gen, bool& compacted, std::string& log) {
    const double t0 = now_sec();
    compacted = false;
    {
        std::unique_lock lk(mu);
        if (index.needs_compaction()) {
            index.compact();
            compacted = true;
        }
    }
    StoredMeta meta;
    meta.uuid = uuid;
    meta.subvol = subvol;
    meta.generation = gen;
    std::string err;
    std::shared_lock lk(mu);
    if (!save_index(state_file(dir), index, meta, err)) {
        log = "сохранение не удалось: " + err;
        return false;
    }
    saved_version = index.version;
    log = fmt("сохранено: %llu записей, generation %llu, %.2f с%s", (unsigned long long)index.live,
              (unsigned long long)gen, now_sec() - t0, compacted ? " (после сжатия)" : "");
    return true;
}

}  // namespace bx
