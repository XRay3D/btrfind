#pragma once
// Изменяемый индекс одного тома (подтома btrfs): записи с деревом родитель/дети
// и пулы имён только на добавление (оригинал и свёрнутый регистр).

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bx {

constexpr uint32_t kNone = UINT32_MAX;

enum EntryFlags : uint16_t {
    kDir = 1,
    kDeleted = 2,
    kMountpoint = 4,  // каталог, поверх которого смонтирована другая ФС/подтом; дети не индексируются
};

struct Entry {
    uint64_t ino;
    uint64_t size;
    int64_t mtime;
    uint32_t parent;
    uint32_t first_child;
    uint32_t next_sibling;
    uint32_t name_off;   // в names
    uint32_t lname_off;  // в lnames
    uint16_t name_len;
    uint16_t lname_len;
    uint16_t flags;
    uint16_t reserved;
    uint32_t dir_slot;  // индекс в dir_meta (только каталоги)
};
static_assert(sizeof(Entry) == 56);

struct DirMeta {
    uint32_t mode, uid, gid;
};

struct NameStart {
    uint32_t off;  // смещение имени в lnames
    uint32_t idx;  // запись, которой имя принадлежало при добавлении
};

struct Attrs {
    uint64_t ino = 0, size = 0;
    int64_t mtime = 0;
    bool dir = false;
    uint32_t mode = 0, uid = 0, gid = 0;
};

class Index {
public:
    std::string root_path;   // точка монтирования тома
    std::string lroot_path;  // root_path в свёрнутом регистре ("" для "/")
    std::vector<Entry> entries;
    std::vector<char> names;   // каждое имя + '\0'
    std::vector<char> lnames;  // свёрнутые имена + '\0'
    std::vector<NameStart> lname_starts;  // по возрастанию off
    std::vector<DirMeta> dir_meta;
    std::unordered_map<uint64_t, uint32_t> dir_by_ino;
    uint64_t live = 0;             // живых записей
    uint64_t dead_bytes = 0;       // байты мёртвых имён в пулах
    uint64_t epoch = 0;            // меняется при сжатии (индексы записей становятся другими)
    uint64_t version = 0;          // меняется при каждом изменении

    void reset(std::string root, const Attrs& root_attrs);

    std::string_view name(uint32_t i) const { return {names.data() + entries[i].name_off, entries[i].name_len}; }
    std::string_view lname(uint32_t i) const { return {lnames.data() + entries[i].lname_off, entries[i].lname_len}; }
    bool is_dir(uint32_t i) const { return entries[i].flags & kDir; }
    bool alive(uint32_t i) const { return i < entries.size() && !(entries[i].flags & kDeleted); }

    uint32_t add(uint32_t parent, std::string_view name, const Attrs& a);
    void remove_subtree(uint32_t idx);
    void remove_children(uint32_t dir);
    void move(uint32_t idx, uint32_t new_parent, std::string_view new_name);
    void set_attrs(uint32_t idx, const Attrs& a);
    uint32_t find_child(uint32_t parent, std::string_view name) const;
    uint32_t dir_for_ino(uint64_t ino) const;
    // Запись по абсолютному пути (внутри root_path), kNone если нет.
    uint32_t lookup(std::string_view abs_path) const;

    std::string path(uint32_t i) const;      // полный путь
    std::string dir_path(uint32_t i) const;  // путь родительского каталога
    void append_lpath(uint32_t i, std::string& out) const;  // свёрнутый путь (для поиска по пути)

    bool needs_compaction() const;
    void compact();
    size_t memory_bytes() const;
    size_t dir_count() const;

private:
    friend class IndexBuilder;
    uint32_t push_name(std::string_view name, uint32_t idx, Entry& e);
    void link(uint32_t idx, uint32_t parent);
    void unlink(uint32_t idx);
    void kill_subtree(uint32_t idx);  // пометить удалёнными без отвязки от родителя
};

// Накопитель записей с родителями по номеру inode (результат скана), связывается в finish().
class IndexBuilder {
public:
    void add(uint64_t parent_ino, std::string_view name, const Attrs& a);
    void append(IndexBuilder&& other);
    size_t size() const { return recs_.size(); }
    // Записи без найденного родителя отбрасываются; возвращает их число.
    size_t finish(Index& out, std::string root_path, const Attrs& root_attrs);

private:
    struct Rec {
        uint64_t parent_ino;
        Attrs a;
        uint32_t name_off;
        uint16_t name_len;
    };
    std::vector<Rec> recs_;
    std::vector<char> names_;
};

}  // namespace bx
