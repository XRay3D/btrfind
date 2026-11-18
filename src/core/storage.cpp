#include "storage.h"

#include "fold.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

namespace bx {
namespace {

constexpr char kMagic[8] = {'B', 'T', 'R', 'F', 'I', 'N', 'D', '1'};
constexpr uint32_t kVersion = 2;  // 2: btime/mode/uid/gid у всех записей

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t entry_size;
    uint8_t uuid[16];
    uint64_t subvol, generation;
    uint64_t live, dead_bytes;
    uint64_t n_entries, n_names, n_lnames, n_starts;
    uint32_t root_len, reserved;
};

struct FileCloser {
    void operator()(FILE* f) const { if (f) fclose(f); }
};

template <class T>
bool write_vec(FILE* f, const std::vector<T>& v) {
    return v.empty() || fwrite(v.data(), sizeof(T), v.size(), f) == v.size();
}
template <class T>
bool read_vec(FILE* f, std::vector<T>& v, uint64_t n) {
    v.resize(n);
    return n == 0 || fread(v.data(), sizeof(T), n, f) == n;
}

}  // namespace

bool save_index(const std::string& file, const Index& ix, const StoredMeta& meta, std::string& err) {
    const std::string tmp = file + ".tmp";
    // 0600: в индексе имена всех файлов системы, включая закрытые каталоги
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    std::unique_ptr<FILE, FileCloser> f(fd >= 0 ? fdopen(fd, "wb") : nullptr);
    if (!f) {
        if (fd >= 0) close(fd);
        err = "fopen(" + tmp + "): " + strerror(errno);
        return false;
    }
    Header h{};
    std::memcpy(h.magic, kMagic, 8);
    h.version = kVersion;
    h.entry_size = sizeof(Entry);
    std::memcpy(h.uuid, meta.uuid.data(), 16);
    h.subvol = meta.subvol;
    h.generation = meta.generation;
    h.live = ix.live;
    h.dead_bytes = ix.dead_bytes;
    h.n_entries = ix.entries.size();
    h.n_names = ix.names.size();
    h.n_lnames = ix.lnames.size();
    h.n_starts = ix.lname_starts.size();
    h.root_len = uint32_t(ix.root_path.size());
    const bool ok = fwrite(&h, sizeof h, 1, f.get()) == 1 &&
                    fwrite(ix.root_path.data(), 1, h.root_len, f.get()) == h.root_len &&
                    write_vec(f.get(), ix.entries) && write_vec(f.get(), ix.names) && write_vec(f.get(), ix.lnames) &&
                    write_vec(f.get(), ix.lname_starts) && fflush(f.get()) == 0 &&
                    fsync(fileno(f.get())) == 0;
    f.reset();
    if (!ok) {
        err = "запись " + tmp + ": " + strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), file.c_str()) < 0) {
        err = "rename(" + file + "): " + strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool load_index(const std::string& file, Index& ix, StoredMeta& meta, const StoredMeta& expect, std::string& err) {
    std::unique_ptr<FILE, FileCloser> f(fopen(file.c_str(), "rbe"));
    if (!f) {
        err = "fopen(" + file + "): " + strerror(errno);
        return false;
    }
    Header h{};
    if (fread(&h, sizeof h, 1, f.get()) != 1 || std::memcmp(h.magic, kMagic, 8) != 0 || h.version != kVersion ||
        h.entry_size != sizeof(Entry)) {
        err = "другой формат файла индекса (будет полный скан)";
        return false;
    }
    if (std::memcmp(h.uuid, expect.uuid.data(), 16) != 0 || h.subvol != expect.subvol) {
        err = "индекс от другой ФС/подтома";
        return false;
    }
    if (h.n_entries == 0 || h.n_entries > UINT32_MAX || h.n_names > UINT32_MAX || h.n_lnames > UINT32_MAX ||
        h.root_len > 4096) {
        err = "повреждённый заголовок";
        return false;
    }
    Index nx;
    nx.root_path.resize(h.root_len);
    if (fread(nx.root_path.data(), 1, h.root_len, f.get()) != h.root_len || !read_vec(f.get(), nx.entries, h.n_entries) ||
        !read_vec(f.get(), nx.names, h.n_names) || !read_vec(f.get(), nx.lnames, h.n_lnames) ||
        !read_vec(f.get(), nx.lname_starts, h.n_starts)) {
        err = "файл индекса обрезан";
        return false;
    }
    // Проверка целостности ссылок, чтобы битый файл не уронил демон.
    const uint32_t n = uint32_t(h.n_entries);
    auto bad = [&](uint32_t v) { return v != kNone && v >= n; };
    for (uint32_t i = 0; i < n; ++i) {
        const Entry& e = nx.entries[i];
        if (bad(e.parent) || bad(e.first_child) || bad(e.next_sibling) ||
            uint64_t(e.name_off) + e.name_len >= h.n_names || uint64_t(e.lname_off) + e.lname_len >= h.n_lnames) {
            err = "повреждённая запись " + std::to_string(i);
            return false;
        }
        if ((e.flags & (kDir | kDeleted)) == kDir) nx.dir_by_ino[e.ino] = i;
    }
    for (const NameStart& ns : nx.lname_starts)
        if (ns.idx >= n || ns.off >= h.n_lnames) {
            err = "повреждённый список имён";
            return false;
        }
    nx.lroot_path = fold_case(nx.root_path == "/" ? std::string() : nx.root_path);
    nx.live = h.live;
    nx.dead_bytes = h.dead_bytes;
    nx.epoch = ix.epoch + 1;
    nx.version = 1;
    ix = std::move(nx);
    meta = expect;
    meta.generation = h.generation;
    return true;
}

}  // namespace bx
