#include "search.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <grp.h>
#include <pwd.h>
#include <memory>
#include <parallel/algorithm>
#include <string>
#include <map>
#include <unordered_map>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace bx {

int search_threads() {
#ifdef _OPENMP
    return omp_get_max_threads();
#else
    return 1;
#endif
}

bool Access::allowed(const Entry& m, unsigned bit) const {
    if (uid == 0) return true;
    if (m.uid == uid) return (m.mode >> 6) & bit;
    if (std::find(gids.begin(), gids.end(), m.gid) != gids.end()) return (m.mode >> 3) & bit;
    return m.mode & bit;
}

namespace {

// Ленивая проверка видимости записи пользователю: r+x у родителя и x у всех предков.
class Visibility {
public:
    Visibility(const Index& ix, const Access& acc)
        : ix_(ix), acc_(acc), trav_(ix.entries.size()), read_(ix.entries.size()) {}

    bool visible(uint32_t i) {
        const uint32_t p = ix_.entries[i].parent;
        return p != kNone && readable(p) && traversable(p);
    }

private:
    bool check(std::vector<std::atomic<uint8_t>>& memo, uint32_t d, unsigned bit) {
        if (d >= memo.size()) return false;
        const uint8_t s = memo[d].load(std::memory_order_relaxed);
        if (s) return s == 1;
        bool r = acc_.allowed(ix_.entries[d], bit);
        if (r && bit == 1) {
            const uint32_t p = ix_.entries[d].parent;
            r = p == kNone || traversable(p);
        }
        memo[d].store(r ? 1 : 2, std::memory_order_relaxed);
        return r;
    }
    bool traversable(uint32_t d) { return check(trav_, d, 1); }
    bool readable(uint32_t d) { return check(read_, d, 4); }

    const Index& ix_;
    const Access& acc_;
    std::vector<std::atomic<uint8_t>> trav_, read_;
};

class Evaluator {
public:
    Evaluator(const Query& q, const Index& ix) : q_(q), ix_(ix) {}

    bool all(uint32_t i) {
        for (const auto& group : q_.groups) {
            bool any = false;
            for (const Term& t : group)
                if (term(t, i)) {
                    any = true;
                    break;
                }
            if (!any) return false;
        }
        return true;
    }

private:
    std::string_view path(uint32_t i) {
        if (path_for_ != i) {
            path_ = ix_.path(i);
            path_for_ = i;
        }
        return path_;
    }
    std::string_view lpath(uint32_t i) {
        if (lpath_for_ != i) {
            lpath_.clear();
            ix_.append_lpath(i, lpath_);
            lpath_for_ = i;
        }
        return lpath_;
    }
    std::string_view hay(const Term& t, uint32_t i) {
        if (t.path) return t.case_sensitive ? path(i) : lpath(i);
        return t.case_sensitive ? ix_.name(i) : ix_.lname(i);
    }

    bool term(const Term& t, uint32_t i) {
        const Entry& e = ix_.entries[i];
        const bool dir = e.flags & kDir;
        bool r = false;
        switch (t.kind) {
            case Term::Substr: {
                const std::string_view h = hay(t, i);
                r = t.text.empty() || memmem(h.data(), h.size(), t.text.data(), t.text.size()) != nullptr;
                break;
            }
            case Term::Glob:
                r = glob_match(t.text, hay(t, i));
                break;
            case Term::Regex:
                r = t.re && t.re->match(t.path ? path(i) : ix_.name(i));
                break;
            case Term::Ext:
                if (!dir) {
                    const std::string_view ln = ix_.lname(i);
                    const size_t dot = ln.rfind('.');
                    if (dot != std::string_view::npos && dot + 1 < ln.size()) {
                        const std::string_view ext = ln.substr(dot + 1);
                        for (const auto& x : t.exts)
                            if (x == ext) {
                                r = true;
                                break;
                            }
                    }
                }
                break;
            case Term::Size:
                r = !dir && e.size >= t.min && e.size <= t.max;
                break;
            case Term::Date:
                r = e.mtime >= t.tmin && e.mtime <= t.tmax;
                break;
            case Term::Created:
                r = e.btime != 0 && e.btime >= t.tmin && e.btime <= t.tmax;
                break;
            case Term::Uid:
                r = e.uid == t.min;
                break;
            case Term::Gid:
                r = e.gid == t.min;
                break;
            case Term::IsFile:
                r = !dir;
                break;
            case Term::IsDir:
                r = dir;
                break;
        }
        return t.negate ? !r : r;
    }

    const Query& q_;
    const Index& ix_;
    std::string path_, lpath_;
    uint32_t path_for_ = kNone, lpath_for_ = kNone;
};

// Самое длинное простое слово (подстрока в имени, не НЕ, не ИЛИ) — по нему идёт memmem по пулу.
const Term* pick_driver(const Query& q) {
    const Term* best = nullptr;
    if (q.no_pool_scan) return nullptr;
    for (const auto& g : q.groups) {
        if (g.size() != 1) continue;
        const Term& t = g[0];
        if (t.kind != Term::Substr || t.negate || t.path || t.ftext.empty()) continue;
        if (!best || t.ftext.size() > best->ftext.size()) best = &t;
    }
    return best;
}

void search_volume(const Query& q, const SearchVolume& sv, const Access* access, const std::atomic<bool>* cancel,
                   const Term* driver, std::vector<Ref>& out, uint64_t& candidates) {
    const Index& ix = *sv.ix;
    std::unique_ptr<Visibility> vis;
    if (access && access->uid != 0) vis = std::make_unique<Visibility>(ix, *access);

    const int nchunks = std::max(1, search_threads() * 8);
    std::vector<std::vector<Ref>> parts(nchunks);
    std::vector<uint64_t> cand(nchunks, 0);
    auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };

    auto accept = [&](Evaluator& ev, uint32_t idx, int c) {
        ++cand[c];
        if (ev.all(idx) && (!vis || vis->visible(idx))) parts[c].push_back(make_ref(sv.vol, idx));
    };

    if (driver) {
        const auto& starts = ix.lname_starts;
        const size_t n = starts.size();
        const char* base = ix.lnames.data();
        const std::string& pat = driver->ftext;
#pragma omp parallel for schedule(dynamic, 1)
        for (int c = 0; c < nchunks; ++c) {
            if (cancelled()) continue;
            const size_t a = n * c / nchunks, b = n * (c + 1) / nchunks;
            if (a >= b) continue;
            Evaluator ev(q, ix);
            const char* s = base + starts[a].off;
            const char* end = b < n ? base + starts[b].off : base + ix.lnames.size();
            while (s < end) {
                const char* hit = static_cast<const char*>(memmem(s, size_t(end - s), pat.data(), pat.size()));
                if (!hit) break;
                const uint32_t off = uint32_t(hit - base);
                const auto it = std::upper_bound(starts.begin() + a, starts.begin() + b, off,
                                                 [](uint32_t v, const NameStart& ns) { return v < ns.off; });
                const size_t k = size_t(it - starts.begin()) - 1;
                const NameStart ns = starts[k];
                const Entry& e = ix.entries[ns.idx];
                if (ns.idx != 0 && e.lname_off == ns.off && !(e.flags & kDeleted)) accept(ev, ns.idx, c);
                s = base + (k + 1 < n ? starts[k + 1].off : ix.lnames.size());
            }
        }
    } else {
        const size_t n = ix.entries.size();
#pragma omp parallel for schedule(dynamic, 1)
        for (int c = 0; c < nchunks; ++c) {
            if (cancelled()) continue;
            const size_t a = std::max<size_t>(1, n * c / nchunks), b = n * (c + 1) / nchunks;
            Evaluator ev(q, ix);
            for (size_t i = a; i < b; ++i) {
                if (ix.entries[i].flags & kDeleted) continue;
                accept(ev, uint32_t(i), c);
                if ((i & 0xFFFF) == 0 && cancelled()) break;
            }
        }
    }
    for (int c = 0; c < nchunks; ++c) {
        out.insert(out.end(), parts[c].begin(), parts[c].end());
        candidates += cand[c];
    }
}

}  // namespace

std::vector<Ref> run_search(const Query& q, const std::vector<SearchVolume>& vols, const Access* access,
                            const std::atomic<bool>* cancel, SearchStats* stats) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Ref> out;
    uint64_t candidates = 0;
    const Term* driver = q.ok() ? pick_driver(q) : nullptr;
    if (q.ok())
        for (const auto& sv : vols) {
            if (cancel && cancel->load()) break;
            search_volume(q, sv, access, cancel, driver, out, candidates);
        }
    if (stats) {
        stats->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        stats->candidates = candidates;
        stats->used_pool_scan = driver != nullptr;
    }
    return out;
}

Totals sum_results(const std::vector<Ref>& refs, const std::vector<const Index*>& by_vol, size_t from, size_t to) {
    Totals t;
    to = std::min(to, refs.size());
    for (size_t k = from; k < to; ++k) {
        const uint32_t v = ref_vol(refs[k]), i = ref_idx(refs[k]);
        const Index* ix = v < by_vol.size() ? by_vol[v] : nullptr;
        if (!ix || i >= ix->entries.size()) continue;
        const Entry& e = ix->entries[i];
        if (e.flags & kDir) {
            ++t.dirs;
        } else {
            ++t.files;
            t.bytes += e.size;
        }
    }
    return t;
}

SortKey sort_key_from(std::string_view s) {
    if (s == "path") return SortKey::Path;
    if (s == "size") return SortKey::Size;
    if (s == "mtime") return SortKey::Mtime;
    if (s == "btime") return SortKey::Btime;
    if (s == "ext" || s == "type") return SortKey::Ext;
    if (s == "owner") return SortKey::Owner;
    if (s == "group") return SortKey::Group;
    if (s == "mode") return SortKey::Mode;
    return SortKey::Name;
}

std::string_view extension_of(std::string_view ln, bool dir) {
    if (dir) return {};
    const size_t dot = ln.rfind('.');
    return (dot == std::string_view::npos || dot == 0 || dot + 1 == ln.size()) ? std::string_view() : ln.substr(dot + 1);
}

namespace {
std::string user_name(uint32_t uid) {
    passwd pw{}, *res = nullptr;
    char buf[4096];
    return getpwuid_r(uid, &pw, buf, sizeof buf, &res) == 0 && res ? std::string(pw.pw_name) : std::to_string(uid);
}
std::string group_name(uint32_t gid) {
    group gr{}, *res = nullptr;
    char buf[4096];
    return getgrgid_r(gid, &gr, buf, sizeof buf, &res) == 0 && res ? std::string(gr.gr_name) : std::to_string(gid);
}
}  // namespace

void sort_results(std::vector<Ref>& refs, const std::vector<SearchVolume>& vols, SortKey key, bool desc) {
    uint32_t maxv = 0;
    for (const auto& v : vols) maxv = std::max(maxv, v.vol);
    std::vector<const Index*> ixs(maxv + 1, nullptr);
    for (const auto& v : vols) ixs[v.vol] = v.ix;
    auto E = [&](Ref r) -> const Entry& { return ixs[ref_vol(r)]->entries[ref_idx(r)]; };
    auto LN = [&](Ref r) { return ixs[ref_vol(r)]->lname(ref_idx(r)); };
    auto by_name = [&](Ref a, Ref b) {
        const int c = LN(a).compare(LN(b));
        return c != 0 ? c < 0 : a < b;
    };

    std::function<bool(Ref, Ref)> less;
    std::unordered_map<Ref, uint32_t> rank;  // для сортировки по пути: ранг родительского каталога
    switch (key) {
        case SortKey::Name:
            less = by_name;
            break;
        case SortKey::Size:
            less = [&](Ref a, Ref b) {
                const uint64_t sa = (E(a).flags & kDir) ? 0 : E(a).size, sb = (E(b).flags & kDir) ? 0 : E(b).size;
                return sa != sb ? sa < sb : by_name(a, b);
            };
            break;
        case SortKey::Mtime:
            less = [&](Ref a, Ref b) { return E(a).mtime != E(b).mtime ? E(a).mtime < E(b).mtime : by_name(a, b); };
            break;
        case SortKey::Btime:
            less = [&](Ref a, Ref b) { return E(a).btime != E(b).btime ? E(a).btime < E(b).btime : by_name(a, b); };
            break;
        case SortKey::Mode:
            less = [&](Ref a, Ref b) {
                const uint32_t ma = E(a).mode & 07777, mb = E(b).mode & 07777;
                return ma != mb ? ma < mb : by_name(a, b);
            };
            break;
        case SortKey::Ext:
            less = [&](Ref a, Ref b) {
                const bool da = E(a).flags & kDir, db = E(b).flags & kDir;
                if (da != db) return da;  // папки — вместе, первыми
                const int c = extension_of(LN(a), da).compare(extension_of(LN(b), db));
                return c != 0 ? c < 0 : by_name(a, b);
            };
            break;
        case SortKey::Owner:
        case SortKey::Group: {
            // Ранг по имени пользователя/группы (имён немного — разрешаются один раз).
            const bool owner = key == SortKey::Owner;
            std::map<uint32_t, std::string> names;
            for (Ref r : refs) names.emplace(owner ? E(r).uid : E(r).gid, std::string());
            std::vector<std::pair<std::string, uint32_t>> sorted;
            for (auto& [id, n] : names) sorted.emplace_back(owner ? user_name(id) : group_name(id), id);
            std::sort(sorted.begin(), sorted.end());
            for (uint32_t k = 0; k < sorted.size(); ++k) rank[sorted[k].second] = k;
            const auto& crank = rank;
            less = [&, owner](Ref a, Ref b) {
                const uint32_t ra = crank.at(owner ? E(a).uid : E(a).gid), rb = crank.at(owner ? E(b).uid : E(b).gid);
                return ra != rb ? ra < rb : by_name(a, b);
            };
            break;
        }
        case SortKey::Path: {
            std::vector<std::pair<std::string, Ref>> dirs;
            for (Ref r : refs) {
                const Ref pr = make_ref(ref_vol(r), E(r).parent);
                if (rank.emplace(pr, 0).second) {
                    std::string p;
                    ixs[ref_vol(r)]->append_lpath(E(r).parent, p);
                    dirs.emplace_back(std::move(p), pr);
                }
            }
            __gnu_parallel::sort(dirs.begin(), dirs.end());
            for (uint32_t k = 0; k < dirs.size(); ++k) rank[dirs[k].second] = k;
            const auto& crank = rank;  // только чтение — безопасно из потоков сортировки
            less = [&](Ref a, Ref b) {
                const uint32_t ra = crank.at(make_ref(ref_vol(a), E(a).parent));
                const uint32_t rb = crank.at(make_ref(ref_vol(b), E(b).parent));
                return ra != rb ? ra < rb : by_name(a, b);
            };
            break;
        }
    }
    if (desc)
        __gnu_parallel::sort(refs.begin(), refs.end(), [&](Ref a, Ref b) { return less(b, a); });
    else
        __gnu_parallel::sort(refs.begin(), refs.end(), less);
}

}  // namespace bx
