#include "fsutil.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <vector>

namespace bx {
namespace {
struct LinuxDirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};
}  // namespace

bool stat_at(int dirfd, const char* name, FsStat& out) {
    out = FsStat{};
    struct statx sx {};
    if (statx(dirfd, name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC,
              STATX_TYPE | STATX_MODE | STATX_UID | STATX_GID | STATX_INO | STATX_SIZE | STATX_MTIME, &sx) < 0)
        return errno == ENOENT || errno == ENOTDIR;
    out.exists = true;
    out.a.ino = sx.stx_ino;
    out.a.size = sx.stx_size;
    out.a.mtime = sx.stx_mtime.tv_sec;
    out.a.dir = S_ISDIR(sx.stx_mode);
    out.a.mode = sx.stx_mode;
    out.a.uid = sx.stx_uid;
    out.a.gid = sx.stx_gid;
    out.dev = {sx.stx_dev_major, sx.stx_dev_minor};
    out.mount_root = (sx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) && (sx.stx_attributes & STATX_ATTR_MOUNT_ROOT);
    return true;
}

bool list_dir(int dirfd, const DirEntryCb& cb) {
    thread_local std::vector<uint64_t> mem(64 * 1024 / 8);
    char* buf = reinterpret_cast<char*>(mem.data());
    const size_t sz = mem.size() * 8;
    if (lseek(dirfd, 0, SEEK_SET) < 0) return false;
    for (;;) {
        const long n = syscall(SYS_getdents64, dirfd, buf, sz);
        if (n < 0) return false;
        if (n == 0) return true;
        for (long pos = 0; pos < n;) {
            auto* d = reinterpret_cast<LinuxDirent64*>(buf + pos);
            pos += d->d_reclen;
            const char* nm = d->d_name;
            if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
            cb(nm, d->d_ino, d->d_type);
        }
    }
}

std::string fd_path(int fd) {
    char lnk[64], out[PATH_MAX];
    snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
    const ssize_t n = readlink(lnk, out, sizeof out - 1);
    return n > 0 ? std::string(out, size_t(n)) : std::string();
}

}  // namespace bx
