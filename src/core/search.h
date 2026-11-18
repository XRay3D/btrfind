#pragma once
// Поиск по индексам томов: параллельный проход (OpenMP), фильтр прав доступа, сортировка.

#include "index.h"
#include "query.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace bx {

using Ref = uint64_t;  // (номер тома << 32) | индекс записи
inline Ref make_ref(uint32_t vol, uint32_t idx) { return (uint64_t(vol) << 32) | idx; }
inline uint32_t ref_vol(Ref r) { return uint32_t(r >> 32); }
inline uint32_t ref_idx(Ref r) { return uint32_t(r); }

struct SearchVolume {
    const Index* ix;
    uint32_t vol;
};

// Права пользователя, от имени которого ищем. uid 0 видит всё.
struct Access {
    uint32_t uid = 0;
    std::vector<uint32_t> gids;
    bool allowed(const Entry& e, unsigned bit /*4 = r, 1 = x*/) const;
};

struct SearchStats {
    double ms = 0;
    uint64_t candidates = 0;
    bool used_pool_scan = false;
};

// access == nullptr — без проверки прав. Отмена через cancel (может быть nullptr).
std::vector<Ref> run_search(const Query& q, const std::vector<SearchVolume>& vols, const Access* access,
                            const std::atomic<bool>* cancel, SearchStats* stats);

enum class SortKey { Name, Path, Size, Mtime, Btime, Ext, Owner, Group, Mode };
SortKey sort_key_from(std::string_view s);  // "name", "path", "size", "mtime", "btime", "ext", "owner", "group", "mode"
// Расширение имени (после последней точки; у каталогов и имён без точки — пусто).
std::string_view extension_of(std::string_view lname, bool dir);
void sort_results(std::vector<Ref>& refs, const std::vector<SearchVolume>& vols, SortKey key, bool desc);

// Суммарный размер файлов и число файлов/папок для refs[from, to).
struct Totals {
    uint64_t bytes = 0, files = 0, dirs = 0;
};
Totals sum_results(const std::vector<Ref>& refs, const std::vector<const Index*>& by_vol, size_t from, size_t to);

int search_threads();

}  // namespace bx
