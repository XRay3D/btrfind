// btrfs-poc — диагностика механизмов «Everything на btrfs»: скан, поиск, догон, fanotify.
#include "btrfs_tree.h"
#include "fan_watch.h"
#include "index.h"
#include "query.h"
#include "search.h"
#include "walk_scan.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

using namespace bx;

namespace {

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string num(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), " ");
    return s;
}

std::string mb(uint64_t bytes) {
    char b[32];
    snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0);
    return b;
}

long maxrss_kb() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

void drop_caches() {
    sync();
    const int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd < 0 || write(fd, "3", 1) != 1) fprintf(stderr, "предупреждение: не удалось сбросить кэш (нужен root)\n");
    if (fd >= 0) close(fd);
}

int open_dir(const std::string& p) {
    const int fd = open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) fprintf(stderr, "open(%s): %s\n", p.c_str(), strerror(errno));
    return fd;
}

std::string normalize(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

void usage() {
    fprintf(stderr,
            "Использование:\n"
            "  btrfs-poc scan <mnt> [--walk|--walk-only] [--no-stat] [--verify] [--threads N] [--cold] [--show N]\n"
            "  btrfs-poc search <mnt> [--walk] [--iters N] [--show N] [--case] [--regex] [--path] <запрос>\n"
            "  btrfs-poc gen <mnt>\n"
            "  btrfs-poc changes <mnt> <generation> [--show N]\n"
            "  btrfs-poc watch <путь> [--dir] [--modify] [--quiet]\n");
}

struct Args {
    std::vector<std::string> pos;
    bool walk = false, walk_only = false, no_stat = false, verify = false, cold = false;
    bool dir = false, modify = false, quiet = false;
    QueryOptions qo;
    int threads = 0, iters = 20, show = 10;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        auto intarg = [&](int& v) {
            if (i + 1 >= argc) return false;
            v = atoi(argv[++i]);
            return true;
        };
        if (s == "--walk") a.walk = true;
        else if (s == "--walk-only") a.walk = a.walk_only = true;
        else if (s == "--no-stat") a.no_stat = true;
        else if (s == "--verify") a.verify = a.walk = true;
        else if (s == "--cold") a.cold = true;
        else if (s == "--dir") a.dir = true;
        else if (s == "--modify") a.modify = true;
        else if (s == "--quiet") a.quiet = true;
        else if (s == "--case") a.qo.case_sensitive = true;
        else if (s == "--regex") a.qo.regex = true;
        else if (s == "--path") a.qo.match_path = true;
        else if (s == "--threads") { if (!intarg(a.threads)) return false; }
        else if (s == "--iters") { if (!intarg(a.iters)) return false; }
        else if (s == "--show") { if (!intarg(a.show)) return false; }
        else if (s.rfind("--", 0) == 0) { fprintf(stderr, "неизвестный флаг %s\n", s.c_str()); return false; }
        else a.pos.push_back(s);
    }
    return true;
}

bool print_subvol(int fd, SubvolInfo& si) {
    std::string err;
    if (!get_subvol_info(fd, si, err)) {
        fprintf(stderr, "%s (это btrfs?)\n", err.c_str());
        return false;
    }
    printf("Подтом: '%s' (id %llu, родитель %llu), generation %llu\n", si.name.c_str(), (unsigned long long)si.treeid,
           (unsigned long long)si.parent_id, (unsigned long long)si.generation);
    return true;
}

void print_index(const char* tag, const Index& ix) {
    printf("%s записей: %s (каталогов %s), пул имён: %s\n", tag, num(ix.live).c_str(), num(ix.dir_count()).c_str(),
           mb(ix.names.size()).c_str());
    printf("%s память индекса: %s\n", tag, mb(ix.memory_bytes()).c_str());
}

bool run_tree_scan(int fd, const std::string& mnt, int threads, Index& ix) {
    struct stat stbuf {};
    fstat(fd, &stbuf);
    if (stbuf.st_ino != BTRFS_FIRST_FREE_OBJECTID) {
        fprintf(stderr, "%s — не корень подтома (ino %llu, ожидался 256)\n", mnt.c_str(), (unsigned long long)stbuf.st_ino);
        return false;
    }
    TreeScanStats st;
    std::string err;
    if (!scan_subvolume(fd, mnt, threads, ix, st, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    printf("[ioctl] чтение дерева: %.3f с, потоков %d (ioctl суммарно %.3f с), вызовов %s, получено %s",
           st.total_sec, st.threads, st.ioctl_sec, num(st.ioctl_calls).c_str(), mb(st.bytes).c_str());
    if (st.threads > 1) printf(", max ino %s", num(st.max_ino).c_str());
    printf("\n[ioctl] элементов %s: INODE_ITEM %s, INODE_REF %s, EXTREF %s, DIR_INDEX %s, прочие %s\n",
           num(st.items).c_str(), num(st.inode_items).c_str(), num(st.inode_refs).c_str(),
           num(st.inode_extrefs).c_str(), num(st.dir_index).c_str(), num(st.other_items).c_str());
    printf("[ioctl] построение индекса: %.3f с, итого %.3f с, сирот %s\n", st.build_sec, st.total_sec + st.build_sec,
           num(st.orphans).c_str());
    for (auto& n : st.nested)
        printf("[ioctl] вложенный подтом: dir ino %llu / %s -> subvol %llu\n", (unsigned long long)n.parent_dir_ino,
               n.name.c_str(), (unsigned long long)n.subvol_id);
    print_index("[ioctl]", ix);
    return true;
}

bool run_walk(const std::string& mnt, const Args& a, Index& ix) {
    WalkOptions wo;
    wo.stat = !a.no_stat;
    wo.threads = a.threads;
    WalkStats ws;
    std::string err;
    if (!walk_scan(mnt, wo, ix, ws, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    printf("[walk]  обход: %.3f с, потоков %d: каталогов %s, файлов %s, statx %s, ошибок %s, точек монт. %s, "
           "вложенных подтомов %s\n",
           ws.sec, ws.threads, num(ws.dirs).c_str(), num(ws.files).c_str(), num(ws.stat_calls).c_str(),
           num(ws.errors).c_str(), num(ws.mountpoints).c_str(), num(ws.nested_subvols).c_str());
    print_index("[walk] ", ix);
    return true;
}

void verify(const Index& a, const Index& b, int show) {
    std::hash<std::string> H;
    std::unordered_set<size_t> ha, hb;
    for (uint32_t i = 0; i < a.entries.size(); ++i)
        if (a.alive(i)) ha.insert(H(a.path(i)));
    for (uint32_t i = 0; i < b.entries.size(); ++i)
        if (b.alive(i)) hb.insert(H(b.path(i)));
    auto diff = [&](const Index& x, const std::unordered_set<size_t>& other, const char* tag) {
        uint64_t cnt = 0;
        for (uint32_t i = 0; i < x.entries.size(); ++i) {
            if (!x.alive(i)) continue;
            const std::string p = x.path(i);
            if (other.count(H(p))) continue;
            if (int(cnt) < show) printf("          %s\n", p.c_str());
            ++cnt;
        }
        printf("[verify] только в %s: %s\n", tag, num(cnt).c_str());
    };
    diff(a, hb, "ioctl");
    diff(b, ha, "walk");
}

int cmd_scan(const Args& a) {
    if (a.pos.size() != 1) return usage(), 2;
    const std::string mnt = normalize(a.pos[0]);
    const int fd = open_dir(mnt);
    if (fd < 0) return 1;
    SubvolInfo si;
    if (!print_subvol(fd, si)) return 1;
    Index tix, wix;
    if (!a.walk_only) {
        if (a.cold) drop_caches();
        if (!run_tree_scan(fd, mnt, a.threads, tix)) return 1;
    }
    if (a.walk) {
        if (a.cold) drop_caches();
        if (!run_walk(mnt, a, wix)) return 1;
    }
    if (a.verify && !a.walk_only) verify(tix, wix, a.show);
    printf("Пиковый RSS процесса: %s\n", mb(uint64_t(maxrss_kb()) * 1024).c_str());
    close(fd);
    return 0;
}

int cmd_search(const Args& a) {
    if (a.pos.size() < 2) return usage(), 2;
    const std::string mnt = normalize(a.pos[0]);
    std::string qtext;
    for (size_t i = 1; i < a.pos.size(); ++i) qtext += (i > 1 ? " " : "") + a.pos[i];
    const int fd = open_dir(mnt);
    if (fd < 0) return 1;
    SubvolInfo si;
    if (!print_subvol(fd, si)) return 1;
    Index ix;
    if (a.walk ? !run_walk(mnt, a, ix) : !run_tree_scan(fd, mnt, a.threads, ix)) return 1;
    close(fd);

    const Query q = parse_query(qtext, a.qo);
    if (!q.ok()) {
        fprintf(stderr, "Ошибка запроса: %s\n", q.error.c_str());
        return 1;
    }
    const std::vector<SearchVolume> vols{{&ix, 0}};
    std::vector<double> ts;
    std::vector<Ref> hits;
    SearchStats ss;
    for (int k = 0; k < std::max(1, a.iters); ++k) {
        hits = run_search(q, vols, nullptr, nullptr, &ss);
        ts.push_back(ss.ms);
    }
    std::sort(ts.begin(), ts.end());
    const double t0 = now();
    sort_results(hits, vols, SortKey::Name, false);
    const double sort_ms = (now() - t0) * 1000;
    printf("\nЗапрос «%s»: совпадений %s, кандидатов %s, %s; min %.2f / медиана %.2f / max %.2f мс; "
           "сортировка %.1f мс (потоков %d)\n",
           qtext.c_str(), num(hits.size()).c_str(), num(ss.candidates).c_str(),
           ss.used_pool_scan ? "memmem по пулу" : "полный проход", ts.front(), ts[ts.size() / 2], ts.back(), sort_ms,
           search_threads());
    for (int k = 0; k < std::min<int>(a.show, int(hits.size())); ++k)
        printf("      %s%s\n", ix.path(ref_idx(hits[k])).c_str(), ix.is_dir(ref_idx(hits[k])) ? "/" : "");
    return 0;
}

int cmd_gen(const Args& a) {
    if (a.pos.size() != 1) return usage(), 2;
    const int fd = open_dir(normalize(a.pos[0]));
    if (fd < 0) return 1;
    syncfs(fd);
    SubvolInfo si;
    std::string err;
    if (!get_subvol_info(fd, si, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    printf("%llu\n", (unsigned long long)si.generation);
    close(fd);
    return 0;
}

int cmd_changes(const Args& a) {
    if (a.pos.size() != 2) return usage(), 2;
    const std::string mnt = normalize(a.pos[0]);
    const uint64_t since = strtoull(a.pos[1].c_str(), nullptr, 10);
    const int fd = open_dir(mnt);
    if (fd < 0) return 1;
    syncfs(fd);
    SubvolInfo si;
    if (!print_subvol(fd, si)) return 1;
    ChangeSet cs;
    std::string err;
    if (!collect_changes(fd, since, cs, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    printf("[changes] generation %llu -> %llu: %.3f с, просмотрено элементов %s; изменённых inode %s, имён %s\n",
           (unsigned long long)since, (unsigned long long)si.generation, cs.sec, num(cs.items).c_str(),
           num(cs.inodes.size()).c_str(), num(cs.refs.size()).c_str());
    const std::string prefix = mnt == "/" ? "" : mnt;
    for (int k = 0; k < std::min<int>(a.show, int(cs.inodes.size())); ++k) {
        const auto& c = cs.inodes[k];
        const auto paths = ino_paths(fd, c.ino);
        const char* kind = c.created ? "новый " : "измен.";
        if (paths.empty())
            printf("   %s %s ino %llu — путь не найден (удалён?)\n", kind, c.dir ? "dir " : "file",
                   (unsigned long long)c.ino);
        for (auto& p : paths) printf("   %s %s %s/%s\n", kind, c.dir ? "dir " : "file", prefix.c_str(), p.c_str());
    }
    close(fd);
    return 0;
}

int cmd_watch(const Args& a) {
    if (a.pos.size() != 1) return usage(), 2;
    WatchOptions wo;
    wo.filesystem = !a.dir;
    wo.modify = a.modify;
    wo.quiet = a.quiet;
    return fan_watch(normalize(a.pos[0]), wo);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) return usage(), 2;
    Args a;
    if (!parse(argc, argv, a)) return usage(), 2;
    const std::string cmd = argv[1];
    if (cmd == "scan") return cmd_scan(a);
    if (cmd == "search") return cmd_search(a);
    if (cmd == "gen") return cmd_gen(a);
    if (cmd == "changes") return cmd_changes(a);
    if (cmd == "watch") return cmd_watch(a);
    usage();
    return 2;
}
