#pragma once
// Многопоточный обход каталогов (getdents64 + statx) — запасной способ индексации без root
// и для ФС, отличных от btrfs. Не выходит за пределы тома (другой st_dev).

#include "index.h"

#include <cstdint>
#include <string>

namespace bx {

struct WalkOptions {
    bool stat = true;  // false — statx только для каталогов (имена без размеров/дат)
    int threads = 0;   // 0 — по числу ядер
};

struct WalkStats {
    double sec = 0;
    uint64_t dirs = 0, files = 0, stat_calls = 0, errors = 0;
    uint64_t mountpoints = 0;
    uint64_t nested_subvols = 0;
    int threads = 0;
};

bool walk_scan(const std::string& root, const WalkOptions& opt, Index& out, WalkStats& st, std::string& err);

}  // namespace bx
