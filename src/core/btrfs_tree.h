#pragma once
// Прямое чтение B-дерева подтома btrfs через BTRFS_IOC_TREE_SEARCH_V2 — аналог чтения MFT.
// Функции поиска по дереву требуют CAP_SYS_ADMIN (root); get_subvol_info и fs_uuid — нет.

#include "index.h"

#include <linux/btrfs.h>
#include <linux/btrfs_tree.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bx {

using FsUuid = std::array<uint8_t, 16>;
std::string uuid_str(const FsUuid& u);

struct SubvolInfo {
    uint64_t treeid = 0;
    uint64_t parent_id = 0;
    uint64_t generation = 0;  // транзакция последнего изменения дерева подтома
    std::string name;
};

bool get_subvol_info(int fd, SubvolInfo& out, std::string& err);
bool fs_uuid(int fd, FsUuid& out, std::string& err);  // BTRFS_IOC_FS_INFO

struct NestedSubvol {
    uint64_t parent_dir_ino;
    uint64_t subvol_id;
    std::string name;
};

struct TreeScanStats {
    uint64_t ioctl_calls = 0, items = 0, bytes = 0;
    uint64_t inode_items = 0, inode_refs = 0, inode_extrefs = 0, dir_index = 0, other_items = 0;
    std::vector<NestedSubvol> nested;  // вложенные подтома (DIR_INDEX -> ROOT_ITEM)
    Attrs root_attrs;                  // INODE_ITEM корня подтома (ino 256)
    bool have_root = false;
    size_t orphans = 0;
    int threads = 1;
    uint64_t max_ino = 0;
    double ioctl_sec = 0;  // время внутри ioctl (сумма по потокам)
    double total_sec = 0;  // ioctl + разбор
    double build_sec = 0;  // построение индекса
};

using ItemCallback = std::function<void(const btrfs_ioctl_search_header&, const char* data)>;

// Перебирает элементы дерева tree_id (0 — подтом fd) с objectid в [min_obj, max_obj]
// и типом в [min_type, max_type] (тип фильтруется только на границах диапазона objectid!)
// в листьях с generation >= min_transid.
bool tree_search(int fd, uint64_t tree_id, uint64_t min_transid, const ItemCallback& cb,
                 TreeScanStats& st, std::string& err,
                 uint64_t min_obj = BTRFS_FIRST_FREE_OBJECTID,
                 uint64_t max_obj = BTRFS_LAST_FREE_OBJECTID,
                 size_t buf_size = 16 * 1024 * 1024, uint8_t min_type = 0, uint8_t max_type = 255);

bool max_objectid(int fd, uint64_t& out, std::string& err);

// Полный индекс подтома. fd — корень подтома (ino 256).
bool scan_subvolume(int fd, const std::string& root_path, int threads, Index& out,
                    TreeScanStats& st, std::string& err);

struct ChangeSet {
    struct Inode {
        uint64_t ino, transid, size;
        int64_t mtime;
        bool dir, created;
    };
    struct Ref {
        uint64_t parent;
        std::string name;
    };
    std::vector<Inode> inodes;                        // изменённые после since_gen
    std::unordered_multimap<uint64_t, Ref> refs;      // имена изменённых inode из тех же листьев
    uint64_t items = 0;
    double sec = 0;
};

// Изменения в транзакциях > since_gen (аналог догона USN-журнала).
bool collect_changes(int fd, uint64_t since_gen, ChangeSet& out, std::string& err);

// Все пути inode относительно корня подтома (BTRFS_IOC_INO_PATHS, root).
std::vector<std::string> ino_paths(int fd, uint64_t ino);

struct RootRef {
    uint64_t child_id;  // id вложенного подтома
    uint64_t dirid;     // inode каталога в родительском подтоме
    std::string name;
};
// Подтома, вложенные в subvol_id (ROOT_REF в дереве корней), root.
bool root_refs(int fd, uint64_t subvol_id, std::vector<RootRef>& out, std::string& err);

}  // namespace bx
