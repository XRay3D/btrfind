#pragma once
// Низкоуровневые операции с ФС: чтение каталога через getdents64 и statx -> Attrs.

#include "index.h"

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>

namespace bx {

struct DevId {
    uint32_t major = 0, minor = 0;
    bool operator==(const DevId&) const = default;
};

struct FsStat {
    bool exists = false;
    Attrs a;
    DevId dev;
    bool mount_root = false;  // корень смонтированной ФС (точка монтирования)
};

// statx(dirfd, name) без следования по ссылкам. false — ошибка (errno), exists=false при ENOENT/ENOTDIR.
bool stat_at(int dirfd, const char* name, FsStat& out);

// Вызывает cb(name, d_ino, d_type) для каждой записи каталога, кроме "." и "..".
using DirEntryCb = std::function<void(const char* name, uint64_t ino, unsigned char type)>;
bool list_dir(int dirfd, const DirEntryCb& cb);

// Путь, на который указывает дескриптор (readlink /proc/self/fd/N).
std::string fd_path(int fd);

}  // namespace bx
