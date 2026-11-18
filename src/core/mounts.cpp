#include "mounts.h"

#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace bx {
namespace {
std::string unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {  // \040 и т.п.
            r += char((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            r += s[i];
        }
    }
    return r;
}
}  // namespace

std::vector<MountInfo> read_mountinfo() {
    std::vector<MountInfo> out;
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) continue;
        std::istringstream a(line.substr(0, sep)), b(line.substr(sep + 3));
        MountInfo m;
        std::string devno;
        a >> m.id >> m.parent >> devno >> m.root >> m.mountpoint;
        b >> m.fstype >> m.source;
        m.root = unescape(m.root);
        m.mountpoint = unescape(m.mountpoint);
        m.source = unescape(m.source);
        out.push_back(std::move(m));
    }
    return out;
}

bool path_within(const std::string& path, const std::string& dir) {
    if (dir == "/") return !path.empty() && path[0] == '/';
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/');
}

std::string btrfs_source_for(const std::string& path, const std::vector<MountInfo>& mounts) {
    char real[PATH_MAX];
    if (!realpath(path.c_str(), real)) return {};
    const std::string rp = real;
    std::string best;
    size_t best_len = 0;
    bool found = false;
    for (const auto& m : mounts) {
        if (!path_within(rp, m.mountpoint)) continue;
        if (found && m.mountpoint.size() < best_len) continue;
        found = true;
        best_len = m.mountpoint.size();
        best = m.fstype == "btrfs" ? m.source : std::string();
    }
    return best;
}

int open_btrfs_toplevel(const std::string& dev, std::string& err) {
    auto fail = [&](const char* what) {
        err = std::string(what) + ": " + strerror(errno);
        return -1;
    };
    const int fsfd = fsopen("btrfs", FSOPEN_CLOEXEC);
    if (fsfd < 0) return fail("fsopen(btrfs)");
    if (fsconfig(fsfd, FSCONFIG_SET_STRING, "source", dev.c_str(), 0) < 0 ||
        fsconfig(fsfd, FSCONFIG_SET_STRING, "subvolid", "5", 0) < 0 ||
        fsconfig(fsfd, FSCONFIG_CMD_CREATE, nullptr, nullptr, 0) < 0) {
        const int r = fail("fsconfig(subvolid=5)");
        close(fsfd);
        return r;
    }
    const int mfd = fsmount(fsfd, FSMOUNT_CLOEXEC, MOUNT_ATTR_RDONLY);
    close(fsfd);
    if (mfd < 0) return fail("fsmount");
    // fsmount даёт O_PATH-дескриптор; fanotify_mark/open_by_handle_at нужен обычный.
    const int dfd = openat(mfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(mfd);
    if (dfd < 0) return fail("openat(toplevel)");
    return dfd;
}

}  // namespace bx
