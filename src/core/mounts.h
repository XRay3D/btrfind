#pragma once
// Таблица монтирования и служебное монтирование верхнего подтома btrfs.

#include <string>
#include <vector>

namespace bx {

struct MountInfo {
    int id = 0, parent = 0;
    std::string root;        // корень внутри ФС (для btrfs — путь подтома, напр. "/@home")
    std::string mountpoint;  // куда смонтировано
    std::string fstype;
    std::string source;      // устройство
};

std::vector<MountInfo> read_mountinfo();

// Устройство btrfs, которому принадлежит path (самая длинная подходящая точка монтирования), или "".
std::string btrfs_source_for(const std::string& path, const std::vector<MountInfo>& mounts);

// Отсоединённое (невидимое) монтирование верхнего подтома (subvolid=5) через fsopen/fsmount.
// Возвращает обычный дескриптор каталога или -1 (err заполняется). Нужен root.
int open_btrfs_toplevel(const std::string& device, std::string& err);

// true, если path лежит внутри dir (или совпадает с ним).
bool path_within(const std::string& path, const std::string& dir);

}  // namespace bx
