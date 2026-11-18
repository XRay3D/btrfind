#include "btrfs_tree.h"

#include <sys/ioctl.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace bx {
namespace {

// Размеры on-disk структур, которые разбираем вручную.
static_assert(sizeof(btrfs_inode_item) == 160);
static_assert(sizeof(btrfs_inode_ref) == 10);
static_assert(sizeof(btrfs_inode_extref) == 18);
static_assert(sizeof(btrfs_dir_item) == 30);
static_assert(sizeof(btrfs_root_ref) == 18);
static_assert(sizeof(btrfs_ioctl_search_header) == 32);

double now_sec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string errstr(const char* what) {
    std::string s = std::string(what) + ": " + strerror(errno);
    if (errno == EPERM || errno == EACCES) s += " (нужен root / CAP_SYS_ADMIN)";
    return s;
}

template <class T>
T load(const char* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

Attrs attrs_of(uint64_t ino, const btrfs_inode_item& ii) {
    Attrs a;
    a.ino = ino;
    a.size = ii.size;
    a.mtime = int64_t(ii.mtime.sec);
    a.dir = S_ISDIR(ii.mode);
    a.mode = ii.mode;
    a.uid = ii.uid;
    a.gid = ii.gid;
    return a;
}

// Обходит имена в элементе INODE_REF / INODE_EXTREF: cb(parent_ino, name).
template <class F>
void for_each_ref(const btrfs_ioctl_search_header& h, const char* d, F&& cb) {
    uint32_t p = 0;
    if (h.type == BTRFS_INODE_REF_KEY) {
        while (p + sizeof(btrfs_inode_ref) <= h.len) {
            const auto r = load<btrfs_inode_ref>(d + p);
            p += sizeof(btrfs_inode_ref);
            if (p + r.name_len > h.len) break;
            cb(uint64_t(h.offset), std::string_view(d + p, r.name_len));
            p += r.name_len;
        }
    } else if (h.type == BTRFS_INODE_EXTREF_KEY) {
        while (p + sizeof(btrfs_inode_extref) <= h.len) {
            const auto r = load<btrfs_inode_extref>(d + p);
            p += sizeof(btrfs_inode_extref);
            if (p + r.name_len > h.len) break;
            cb(uint64_t(r.parent_objectid), std::string_view(d + p, r.name_len));
            p += r.name_len;
        }
    }
}

}  // namespace

std::string uuid_str(const FsUuid& u) {
    char b[40];
    snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1],
             u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return b;
}

bool get_subvol_info(int fd, SubvolInfo& out, std::string& err) {
    btrfs_ioctl_get_subvol_info_args a{};
    if (ioctl(fd, BTRFS_IOC_GET_SUBVOL_INFO, &a) < 0) {
        err = errstr("BTRFS_IOC_GET_SUBVOL_INFO");
        return false;
    }
    out.treeid = a.treeid;
    out.parent_id = a.parent_id;
    out.generation = a.generation;
    out.name = a.name;
    return true;
}

bool fs_uuid(int fd, FsUuid& out, std::string& err) {
    btrfs_ioctl_fs_info_args a{};
    if (ioctl(fd, BTRFS_IOC_FS_INFO, &a) < 0) {
        err = errstr("BTRFS_IOC_FS_INFO");
        return false;
    }
    std::memcpy(out.data(), a.fsid, 16);
    return true;
}

bool tree_search(int fd, uint64_t tree_id, uint64_t min_transid, const ItemCallback& cb,
                 TreeScanStats& st, std::string& err, uint64_t min_obj, uint64_t max_obj,
                 size_t buf_size, uint8_t min_type, uint8_t max_type) {
    std::vector<uint64_t> mem((sizeof(btrfs_ioctl_search_args_v2) + buf_size + 7) / 8);
    auto* args = reinterpret_cast<btrfs_ioctl_search_args_v2*>(mem.data());
    btrfs_ioctl_search_key& sk = args->key;
    sk.tree_id = tree_id;
    sk.min_objectid = min_obj;
    sk.max_objectid = max_obj;
    sk.min_type = min_type;
    sk.max_type = max_type;
    sk.min_offset = 0;
    sk.max_offset = UINT64_MAX;
    sk.min_transid = min_transid;
    sk.max_transid = UINT64_MAX;
    const char* buf = reinterpret_cast<const char*>(args->buf);

    for (;;) {
        sk.nr_items = UINT32_MAX;
        args->buf_size = buf_size;
        const double t0 = now_sec();
        const int r = ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, args);
        st.ioctl_sec += now_sec() - t0;
        ++st.ioctl_calls;
        if (r < 0) {
            err = errstr("BTRFS_IOC_TREE_SEARCH_V2");
            return false;
        }
        if (sk.nr_items == 0) break;

        size_t off = 0;
        btrfs_ioctl_search_header h{};
        for (uint32_t i = 0; i < sk.nr_items; ++i) {
            std::memcpy(&h, buf + off, sizeof h);
            cb(h, buf + off + sizeof h);
            off += sizeof h + h.len;
        }
        st.items += sk.nr_items;
        st.bytes += off;

        if (h.offset < UINT64_MAX) {
            sk.min_objectid = h.objectid;
            sk.min_type = uint8_t(h.type);
            sk.min_offset = h.offset + 1;
        } else if (h.type < UINT8_MAX) {
            sk.min_objectid = h.objectid;
            sk.min_type = uint8_t(h.type + 1);
            sk.min_offset = 0;
        } else if (h.objectid < sk.max_objectid) {
            sk.min_objectid = h.objectid + 1;
            sk.min_type = 0;
            sk.min_offset = 0;
        } else {
            break;
        }
    }
    return true;
}

namespace {

bool scan_range(int fd, uint64_t a, uint64_t b, size_t buf_size, IndexBuilder& out,
                TreeScanStats& st, std::string& err) {
    Attrs cur;
    cur.ino = UINT64_MAX;
    auto cb = [&](const btrfs_ioctl_search_header& h, const char* d) {
        switch (h.type) {
            case BTRFS_INODE_ITEM_KEY:
                ++st.inode_items;
                if (h.len < sizeof(btrfs_inode_item)) break;
                cur = attrs_of(h.objectid, load<btrfs_inode_item>(d));
                if (h.objectid == BTRFS_FIRST_FREE_OBJECTID) {
                    st.root_attrs = cur;
                    st.have_root = true;
                }
                break;
            case BTRFS_INODE_REF_KEY:
            case BTRFS_INODE_EXTREF_KEY:
                (h.type == BTRFS_INODE_REF_KEY ? st.inode_refs : st.inode_extrefs)++;
                if (h.objectid == BTRFS_FIRST_FREE_OBJECTID) break;  // ".." корня подтома
                for_each_ref(h, d, [&](uint64_t parent, std::string_view name) {
                    Attrs at = cur.ino == h.objectid ? cur : Attrs{};
                    at.ino = h.objectid;
                    out.add(parent, name, at);
                });
                break;
            case BTRFS_DIR_INDEX_KEY: {
                ++st.dir_index;
                if (h.len < sizeof(btrfs_dir_item)) break;
                const auto di = load<btrfs_dir_item>(d);
                if (di.location.type == BTRFS_ROOT_ITEM_KEY && sizeof di + di.name_len <= h.len &&
                    st.nested.size() < 10000)
                    st.nested.push_back({h.objectid, di.location.objectid,
                                         std::string(d + sizeof di, di.name_len)});
                break;
            }
            default:
                ++st.other_items;
        }
    };
    return tree_search(fd, 0, 0, cb, st, err, a, b, buf_size);
}

}  // namespace

bool max_objectid(int fd, uint64_t& out, std::string& err) {
    std::vector<uint64_t> mem((sizeof(btrfs_ioctl_search_args_v2) + 256 * 1024) / 8);
    auto* args = reinterpret_cast<btrfs_ioctl_search_args_v2*>(mem.data());
    auto first_from = [&](uint64_t x, uint64_t& found) -> int {
        btrfs_ioctl_search_key& sk = args->key;
        sk = {};
        sk.min_objectid = x;
        sk.max_objectid = BTRFS_LAST_FREE_OBJECTID;
        sk.max_type = UINT8_MAX;
        sk.max_offset = UINT64_MAX;
        sk.max_transid = UINT64_MAX;
        sk.nr_items = 1;
        args->buf_size = 256 * 1024;
        if (ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, args) < 0) return -1;
        if (sk.nr_items == 0) return 0;
        btrfs_ioctl_search_header h;
        std::memcpy(&h, args->buf, sizeof h);
        found = h.objectid;
        return 1;
    };
    uint64_t lo = 0;
    int r = first_from(BTRFS_FIRST_FREE_OBJECTID, lo);
    if (r < 0) {
        err = errstr("BTRFS_IOC_TREE_SEARCH_V2");
        return false;
    }
    if (r == 0) {
        out = BTRFS_FIRST_FREE_OBJECTID;
        return true;
    }
    uint64_t hi = BTRFS_LAST_FREE_OBJECTID + 1;
    while (hi - lo > 1) {
        const uint64_t mid = lo + (hi - lo) / 2;
        uint64_t f = 0;
        r = first_from(mid, f);
        if (r < 0) {
            err = errstr("BTRFS_IOC_TREE_SEARCH_V2");
            return false;
        }
        if (r == 1)
            lo = f;
        else
            hi = mid;
    }
    out = lo;
    return true;
}

bool scan_subvolume(int fd, const std::string& root_path, int threads, Index& out,
                    TreeScanStats& st, std::string& err) {
    const double t0 = now_sec();
    if (threads <= 0) threads = int(std::max(1u, std::thread::hardware_concurrency()));
    uint64_t maxo = BTRFS_LAST_FREE_OBJECTID;
    if (threads > 1 && !max_objectid(fd, maxo, err)) return false;
    st.max_ino = maxo;
    st.threads = threads;

    const int nchunks = threads == 1 ? 1 : threads * 8;
    const size_t buf = threads == 1 ? 16u << 20 : 2u << 20;
    const uint64_t first = BTRFS_FIRST_FREE_OBJECTID;
    const unsigned __int128 span = (unsigned __int128)(maxo - first) + 1;
    std::vector<IndexBuilder> builders(nchunks);
    std::vector<TreeScanStats> stats(nchunks);
    std::vector<std::string> errs(nchunks);
    std::vector<char> ok(nchunks, 1);
    std::atomic<int> next{0};

    auto worker = [&] {
        for (int c; (c = next.fetch_add(1)) < nchunks;) {
            const uint64_t a = first + uint64_t(span * c / nchunks);
            const uint64_t b = first + uint64_t(span * (c + 1) / nchunks) - 1;
            if (a > b) continue;
            ok[c] = scan_range(fd, a, b, buf, builders[c], stats[c], errs[c]);
        }
    };
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t) th.emplace_back(worker);
    for (auto& t : th) t.join();

    for (int c = 0; c < nchunks; ++c) {
        if (!ok[c]) {
            err = errs[c];
            return false;
        }
        const auto& s = stats[c];
        st.ioctl_calls += s.ioctl_calls;
        st.items += s.items;
        st.bytes += s.bytes;
        st.inode_items += s.inode_items;
        st.inode_refs += s.inode_refs;
        st.inode_extrefs += s.inode_extrefs;
        st.dir_index += s.dir_index;
        st.other_items += s.other_items;
        st.ioctl_sec += s.ioctl_sec;
        st.nested.insert(st.nested.end(), s.nested.begin(), s.nested.end());
        if (s.have_root) {
            st.root_attrs = s.root_attrs;
            st.have_root = true;
        }
    }
    IndexBuilder all = std::move(builders[0]);
    for (int c = 1; c < nchunks; ++c) all.append(std::move(builders[c]));
    const double t1 = now_sec();
    Attrs ra = st.root_attrs;
    ra.ino = BTRFS_FIRST_FREE_OBJECTID;
    ra.dir = true;
    st.orphans = all.finish(out, root_path, ra);
    st.total_sec = t1 - t0;
    st.build_sec = now_sec() - t1;
    return true;
}

bool collect_changes(int fd, uint64_t since_gen, ChangeSet& out, std::string& err) {
    const double t0 = now_sec();
    TreeScanStats st;
    uint64_t changed_ino = UINT64_MAX;
    auto cb = [&](const btrfs_ioctl_search_header& h, const char* d) {
        if (h.type == BTRFS_INODE_ITEM_KEY) {
            if (h.len < sizeof(btrfs_inode_item)) return;
            const auto ii = load<btrfs_inode_item>(d);
            // В лист с новой generation попадают и неизменённые соседи — фильтруем по inode.
            if (ii.transid > since_gen) {
                out.inodes.push_back({h.objectid, ii.transid, ii.size, int64_t(ii.mtime.sec),
                                      S_ISDIR(ii.mode), ii.generation > since_gen});
                changed_ino = h.objectid;
            }
        } else if ((h.type == BTRFS_INODE_REF_KEY || h.type == BTRFS_INODE_EXTREF_KEY) &&
                   h.objectid == changed_ino) {
            for_each_ref(h, d, [&](uint64_t parent, std::string_view name) {
                out.refs.emplace(h.objectid, ChangeSet::Ref{parent, std::string(name)});
            });
        }
    };
    const bool ok = tree_search(fd, 0, since_gen + 1, cb, st, err);
    out.items = st.items;
    out.sec = now_sec() - t0;
    return ok;
}

std::vector<std::string> ino_paths(int fd, uint64_t ino) {
    std::vector<uint64_t> mem(64 * 1024 / 8);
    auto* dc = reinterpret_cast<btrfs_data_container*>(mem.data());
    btrfs_ioctl_ino_path_args a{};
    a.inum = ino;
    a.size = mem.size() * 8;
    a.fspath = reinterpret_cast<uintptr_t>(dc);
    std::vector<std::string> r;
    if (ioctl(fd, BTRFS_IOC_INO_PATHS, &a) < 0) return r;
    const char* base = reinterpret_cast<const char*>(dc->val);
    for (uint32_t i = 0; i < dc->elem_cnt; ++i) r.emplace_back(base + dc->val[i]);
    return r;
}

bool root_refs(int fd, uint64_t subvol_id, std::vector<RootRef>& out, std::string& err) {
    TreeScanStats st;
    auto cb = [&](const btrfs_ioctl_search_header& h, const char* d) {
        if (h.type != BTRFS_ROOT_REF_KEY || h.objectid != subvol_id || h.len < sizeof(btrfs_root_ref)) return;
        const auto rr = load<btrfs_root_ref>(d);
        if (sizeof rr + rr.name_len > h.len) return;
        out.push_back({h.offset, rr.dirid, std::string(d + sizeof rr, rr.name_len)});
    };
    return tree_search(fd, BTRFS_ROOT_TREE_OBJECTID, 0, cb, st, err, subvol_id, subvol_id, 64 * 1024,
                       BTRFS_ROOT_REF_KEY, BTRFS_ROOT_REF_KEY);
}

}  // namespace bx
