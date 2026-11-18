#pragma once
// Том = один подтом btrfs, смонтированный в path: его индекс, состояние и операции обновления.

#include "btrfs_tree.h"
#include "fsutil.h"
#include "index.h"

#include <atomic>
#include <shared_mutex>
#include <string>
#include <vector>

namespace bx {

enum class VolState : int { Pending, Loading, Scanning, CatchingUp, Ready, Error, Removed };
const char* state_name(VolState s);

class Volume {
public:
    Volume(uint32_t id, std::string path, FsUuid uuid, uint64_t subvol, std::string device, DevId dev, int fd);
    ~Volume();
    Volume(const Volume&) = delete;
    Volume& operator=(const Volume&) = delete;

    const uint32_t id;
    const std::string path;  // точка монтирования (или путь вложенного подтома)
    const FsUuid uuid;
    const uint64_t subvol;
    const std::string device;
    const DevId dev;
    const int fd;  // корень подтома

    Index index;                      // защищён mu
    mutable std::shared_mutex mu;
    std::atomic<VolState> state{VolState::Pending};
    std::atomic<uint64_t> generation{0};  // все изменения до этой транзакции отражены в индексе
    std::atomic<bool> privileged{true};   // доступны ioctl дерева (root)
    uint64_t saved_version = 0;           // index.version при последнем сохранении
    std::vector<NestedSubvol> nested;     // вложенные подтома из последнего скана

    std::string state_file(const std::string& dir) const;
    uint64_t current_generation() const;

    // Выполняются в рабочем потоке; индекс подменяется/меняется под эксклюзивной блокировкой.
    bool load(const std::string& dir, std::string& log);
    bool full_scan(std::string& log);
    bool catch_up(std::string& log);
    // Сохранение (сжатие при необходимости). compacted=true — индексы записей изменились.
    bool save(const std::string& dir, uint64_t gen, bool& compacted, std::string& log);
};

}  // namespace bx
