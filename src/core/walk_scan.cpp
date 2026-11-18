#include "walk_scan.h"

#include "fsutil.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace bx {
namespace {
struct Task {
    std::string path;
    uint64_t ino;
};
}  // namespace

bool walk_scan(const std::string& root, const WalkOptions& opt, Index& out, WalkStats& st, std::string& err) {
    const auto t0 = std::chrono::steady_clock::now();
    FsStat rs;
    if (!stat_at(AT_FDCWD, root.c_str(), rs) || !rs.exists || !rs.a.dir) {
        err = "statx(" + root + "): " + strerror(errno);
        return false;
    }
    const DevId dev = rs.dev;

    const int nthreads = opt.threads > 0 ? opt.threads : int(std::max(1u, std::thread::hardware_concurrency()));
    std::deque<Task> queue;
    std::mutex mu;
    std::condition_variable cv;
    int active = 0;
    queue.push_back({root, rs.a.ino});

    std::vector<IndexBuilder> builders(nthreads);
    std::vector<WalkStats> stats(nthreads);

    auto worker = [&](int t) {
        IndexBuilder& b = builders[t];
        WalkStats& s = stats[t];
        std::vector<Task> fresh;
        for (;;) {
            Task task;
            {
                std::unique_lock lk(mu);
                cv.wait(lk, [&] { return !queue.empty() || active == 0; });
                if (queue.empty()) {
                    cv.notify_all();
                    return;
                }
                task = std::move(queue.back());
                queue.pop_back();
                ++active;
            }

            const int fd = open(task.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) {
                ++s.errors;
            } else {
                ++s.dirs;
                const std::string prefix = task.path.back() == '/' ? task.path : task.path + '/';
                const bool ok = list_dir(fd, [&](const char* nm, uint64_t d_ino, unsigned char type) {
                    Attrs a;
                    a.ino = d_ino;
                    a.dir = type == DT_DIR;
                    if (opt.stat || a.dir || type == DT_UNKNOWN) {
                        FsStat fs;
                        if (stat_at(fd, nm, fs) && fs.exists) {
                            ++s.stat_calls;
                            if (fs.a.dir && !(fs.dev == dev)) {
                                if (fs.mount_root) {
                                    // Каталог-точка монтирования принадлежит этому тому.
                                    ++s.mountpoints;
                                    Attrs m = fs.a;
                                    m.ino = d_ino;
                                    b.add(task.ino, nm, m);
                                } else {
                                    ++s.nested_subvols;
                                }
                                return;
                            }
                            a = fs.a;
                        } else {
                            ++s.errors;
                            return;
                        }
                    }
                    if (a.dir)
                        fresh.push_back({prefix + nm, a.ino});
                    else
                        ++s.files;
                    b.add(task.ino, nm, a);
                });
                if (!ok) ++s.errors;
                close(fd);
            }

            bool idle;
            {
                std::lock_guard lk(mu);
                for (auto& f : fresh) queue.push_back(std::move(f));
                idle = --active == 0;
            }
            if (!fresh.empty() || idle) cv.notify_all();
            fresh.clear();
        }
    };

    std::vector<std::thread> th;
    for (int t = 0; t < nthreads; ++t) th.emplace_back(worker, t);
    for (auto& x : th) x.join();

    IndexBuilder all = std::move(builders[0]);
    for (int t = 1; t < nthreads; ++t) all.append(std::move(builders[t]));
    all.finish(out, root, rs.a);

    st = WalkStats{};
    st.threads = nthreads;
    for (const auto& s : stats) {
        st.dirs += s.dirs;
        st.files += s.files;
        st.stat_calls += s.stat_calls;
        st.errors += s.errors;
        st.mountpoints += s.mountpoints;
        st.nested_subvols += s.nested_subvols;
    }
    st.sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace bx
