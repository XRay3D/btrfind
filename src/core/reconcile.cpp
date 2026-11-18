#include "reconcile.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <unordered_set>

namespace bx {

int Reconciler::open_dir(uint32_t idx) const {
    return open(ix_.path(idx).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

bool Reconciler::is_ancestor(uint32_t a, uint32_t b) const {
    for (uint32_t c = b; c != kNone; c = ix_.entries[c].parent)
        if (c == a) return true;
    return false;
}

uint32_t Reconciler::add_mountpoint(uint32_t dir, std::string_view name, const Attrs& a, uint64_t d_ino) {
    Attrs m = a;
    m.ino = d_ino;  // inode каталога в нашем томе (statx видит корень смонтированной ФС)
    m.dir = true;
    const uint32_t idx = ix_.add(dir, name, m);
    ix_.entries[idx].flags |= kMountpoint;
    ++stats.added;
    return idx;
}

void Reconciler::reconcile_name(uint32_t dir, int dirfd, std::string_view name) {
    if (!ix_.alive(dir) || !ix_.is_dir(dir) || (ix_.entries[dir].flags & kMountpoint)) return;
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos) return;
    const std::string nm(name);
    FsStat st;
    if (!stat_at(dirfd, nm.c_str(), st)) return;  // ошибка доступа и т.п. — индекс не трогаем
    if (!st.exists) {
        const uint32_t child = ix_.find_child(dir, name);
        if (child != kNone) {
            ix_.remove_subtree(child);
            ++stats.removed;
        }
        return;
    }
    add_or_update(dir, dirfd, name, st, 0);
}

void Reconciler::add_or_update(uint32_t dir, int dirfd, std::string_view name, const FsStat& st, uint64_t d_ino) {
    uint32_t child = ix_.find_child(dir, name);

    if (st.a.dir && !(st.dev == dev_)) {
        if (st.mount_root) {  // точка монтирования другой ФС/подтома
            if (child != kNone && !(ix_.entries[child].flags & kMountpoint)) {
                ix_.remove_children(child);
                ix_.entries[child].flags |= kMountpoint;
            } else if (child == kNone) {
                add_mountpoint(dir, name, st.a, d_ino);
            }
        } else if (child != kNone) {  // вложенный подтом — это отдельный том
            ix_.remove_subtree(child);
            ++stats.removed;
        }
        return;
    }

    if (child != kNone) {
        Entry& e = ix_.entries[child];
        if (e.flags & kMountpoint) {  // ФС отмонтировали — содержимое снова видно
            e.flags &= uint16_t(~kMountpoint);
            ix_.set_attrs(child, st.a);
            fill_dir(child);
            return;
        }
        if (e.ino == st.a.ino && bool(e.flags & kDir) == st.a.dir) {
            ix_.set_attrs(child, st.a);
            ++stats.updated;
            return;
        }
        ix_.remove_subtree(child);
        ++stats.removed;
    }

    if (st.a.dir) {
        const uint32_t moved = ix_.dir_for_ino(st.a.ino);
        if (moved != kNone && moved != 0 && ix_.alive(moved) && !is_ancestor(moved, dir)) {
            ix_.move(moved, dir, name);  // переименование/перенос каталога: поддерево сохраняется
            ix_.set_attrs(moved, st.a);
            ++stats.moved;
            return;
        }
        const uint32_t idx = ix_.add(dir, name, st.a);
        ++stats.added;
        (void)dirfd;
        fill_dir(idx);
    } else {
        ix_.add(dir, name, st.a);
        ++stats.added;
    }
}

void Reconciler::fill_dir(uint32_t root) {
    std::vector<uint32_t> stack{root};
    while (!stack.empty()) {
        const uint32_t idx = stack.back();
        stack.pop_back();
        if (!ix_.alive(idx)) continue;
        const int fd = open_dir(idx);
        if (fd < 0) continue;
        ++stats.dirs_listed;
        list_dir(fd, [&](const char* nm, uint64_t d_ino, unsigned char) {
            FsStat st;
            if (!stat_at(fd, nm, st) || !st.exists) return;
            if (ix_.find_child(idx, nm) != kNone) return;  // уже есть (повторный обход)
            if (st.a.dir && !(st.dev == dev_)) {
                if (st.mount_root) add_mountpoint(idx, nm, st.a, d_ino);
                return;
            }
            const uint32_t c = ix_.add(idx, nm, st.a);
            ++stats.added;
            if (st.a.dir) stack.push_back(c);
        });
        close(fd);
    }
}

void Reconciler::rescan_dirs(const std::vector<uint32_t>& dirs) {
    struct Listing {
        uint32_t dir;
        std::unordered_set<std::string> names;
    };
    std::vector<Listing> listings;

    // Фаза 1: добавления, обновления, переносы.
    for (uint32_t d : dirs) {
        if (!ix_.alive(d) || !ix_.is_dir(d) || (ix_.entries[d].flags & kMountpoint)) continue;
        const int fd = open_dir(d);
        if (fd < 0) continue;  // каталог исчез — его удалит сверка родителя
        ++stats.dirs_listed;
        FsStat self;
        if (stat_at(fd, ".", self) && self.exists) ix_.set_attrs(d, self.a);
        Listing l{d, {}};
        const bool ok = list_dir(fd, [&](const char* nm, uint64_t d_ino, unsigned char) {
            l.names.emplace(nm);
            FsStat st;
            if (stat_at(fd, nm, st) && st.exists) add_or_update(d, fd, nm, st, d_ino);
        });
        close(fd);
        if (ok) listings.push_back(std::move(l));
    }
    // Фаза 2: удаления.
    for (const Listing& l : listings) {
        if (!ix_.alive(l.dir)) continue;
        std::vector<uint32_t> gone;
        for (uint32_t c = ix_.entries[l.dir].first_child; c != kNone; c = ix_.entries[c].next_sibling)
            if (!l.names.count(std::string(ix_.name(c)))) gone.push_back(c);
        for (uint32_t c : gone) {
            ix_.remove_subtree(c);
            ++stats.removed;
        }
    }
}

void Reconciler::apply_changes(int volfd, const ChangeSet& cs) {
    std::vector<uint32_t> dirs;
    std::unordered_set<uint32_t> dir_set;
    for (const auto& in : cs.inodes) {
        if (!in.dir) continue;
        const uint32_t d = ix_.dir_for_ino(in.ino);
        if (d != kNone && dir_set.insert(d).second) dirs.push_back(d);
        // Неизвестный (новый) каталог появится при сверке родителя: его inode тоже изменился.
    }
    rescan_dirs(dirs);

    // Изменённые файлы в неизменённых каталогах (запись содержимого, атрибуты).
    for (const auto& in : cs.inodes) {
        if (in.dir) continue;
        std::vector<std::pair<uint32_t, std::string>> targets;
        auto [b, e] = cs.refs.equal_range(in.ino);
        for (auto it = b; it != e; ++it) {
            const uint32_t p = ix_.dir_for_ino(it->second.parent);
            if (p != kNone) targets.emplace_back(p, it->second.name);
        }
        if (b == e && volfd >= 0) {
            for (const std::string& rel : ino_paths(volfd, in.ino)) {
                const std::string abs = (ix_.root_path == "/" ? "" : ix_.root_path) + "/" + rel;
                const size_t slash = abs.rfind('/');
                const uint32_t p = ix_.lookup(abs.substr(0, slash == 0 ? 1 : slash));
                if (p != kNone) targets.emplace_back(p, abs.substr(slash + 1));
            }
        }
        for (auto& [p, name] : targets) {
            if (dir_set.count(p) || !ix_.alive(p)) continue;  // уже сверено целиком
            const int fd = open_dir(p);
            if (fd < 0) continue;
            reconcile_name(p, fd, name);
            close(fd);
        }
    }
}

void Reconciler::set_mountpoint(uint32_t dir, bool on) {
    if (!ix_.alive(dir) || !ix_.is_dir(dir) || dir == 0) return;
    Entry& e = ix_.entries[dir];
    if (on) {
        if (!(e.flags & kMountpoint)) {
            ix_.remove_children(dir);
            ix_.entries[dir].flags |= kMountpoint;
        }
    } else if (e.flags & kMountpoint) {
        e.flags &= uint16_t(~kMountpoint);
        fill_dir(dir);
    }
}

}  // namespace bx
