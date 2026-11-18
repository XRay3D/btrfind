#pragma once
// Сверка индекса с реальной ФС. Все операции идемпотентны и не зависят от порядка событий:
// индекс приводится к тому, что сейчас видит statx/getdents.

#include "btrfs_tree.h"
#include "fsutil.h"
#include "index.h"

#include <string>
#include <string_view>
#include <vector>

namespace bx {

class Reconciler {
public:
    struct Stats {
        uint64_t added = 0, removed = 0, moved = 0, updated = 0, dirs_listed = 0;
    };

    Reconciler(Index& ix, DevId dev) : ix_(ix), dev_(dev) {}

    // Сверить одно имя в каталоге dir; dirfd открыт на этот каталог.
    void reconcile_name(uint32_t dir, int dirfd, std::string_view name);
    // Полная сверка содержимого каталогов: сначала добавления/переносы во всех, потом удаления.
    void rescan_dirs(const std::vector<uint32_t>& dirs);
    // Применить изменения btrfs (collect_changes) — догон после простоя.
    void apply_changes(int volfd, const ChangeSet& cs);
    // Поставить/снять метку точки монтирования (дети скрываются / индексируются заново).
    void set_mountpoint(uint32_t dir, bool on);

    Stats stats;

private:
    void add_or_update(uint32_t dir, int dirfd, std::string_view name, const FsStat& st, uint64_t d_ino);
    uint32_t add_mountpoint(uint32_t dir, std::string_view name, const Attrs& a, uint64_t d_ino);
    void fill_dir(uint32_t idx);  // рекурсивно проиндексировать новый каталог
    bool is_ancestor(uint32_t a, uint32_t b) const;
    int open_dir(uint32_t idx) const;

    Index& ix_;
    DevId dev_;
};

}  // namespace bx
