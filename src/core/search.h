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
    bool allowed(const DirMeta& m, unsigned bit /*4 = r, 1 = x*/) const;
};

struct SearchStats {
    double ms = 0;
    uint64_t candidates = 0;
    bool used_pool_scan = false;
};

// access == nullptr — без проверки прав. Отмена через cancel (может быть nullptr).
std::vector<Ref> run_search(const Query& q, const std::vector<SearchVolume>& vols, const Access* access,
                            const std::atomic<bool>* cancel, SearchStats* stats);

enum class SortKey { Name, Path, Size, Mtime };
void sort_results(std::vector<Ref>& refs, const std::vector<SearchVolume>& vols, SortKey key, bool desc);

int search_threads();

}  // namespace bx
