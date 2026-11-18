#pragma once
// Сохранение индекса тома на диск и загрузка (сырые массивы + заголовок).

#include "btrfs_tree.h"
#include "index.h"

#include <string>

namespace bx {

struct StoredMeta {
    FsUuid uuid{};
    uint64_t subvol = 0;
    uint64_t generation = 0;  // все изменения до этой транзакции отражены в индексе
};

bool save_index(const std::string& file, const Index& ix, const StoredMeta& meta, std::string& err);
// expect — ожидаемые uuid/subvol (generation заполняется из файла).
bool load_index(const std::string& file, Index& ix, StoredMeta& meta, const StoredMeta& expect, std::string& err);

}  // namespace bx
