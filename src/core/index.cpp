#include "index.h"

#include "fold.h"

#include <algorithm>

namespace bx {

// ---------------------------------------------------------------- Index

void Index::reset(std::string root, const Attrs& ra) {
    const uint64_t next_epoch = epoch + 1;
    *this = Index{};
    epoch = next_epoch;
    root_path = std::move(root);
    lroot_path = fold_case(root_path == "/" ? std::string() : root_path);
    Entry e{};
    e.ino = ra.ino;
    e.size = ra.size;
    e.mtime = ra.mtime;
    e.parent = e.first_child = e.next_sibling = kNone;
    e.btime = ra.btime;
    e.flags = kDir;
    e.mode = ra.mode;
    e.uid = ra.uid;
    e.gid = ra.gid;
    entries.push_back(e);
    push_name("", 0, entries[0]);
    dir_by_ino[ra.ino] = 0;
    live = 1;
}

uint32_t Index::push_name(std::string_view name, uint32_t idx, Entry& e) {
    e.name_off = uint32_t(names.size());
    e.name_len = uint16_t(name.size());
    names.insert(names.end(), name.begin(), name.end());
    names.push_back('\0');

    thread_local std::string folded;
    folded.clear();
    fold_case_append(name, folded);
    e.lname_off = uint32_t(lnames.size());
    e.lname_len = uint16_t(folded.size());
    lnames.insert(lnames.end(), folded.begin(), folded.end());
    lnames.push_back('\0');
    lname_starts.push_back({e.lname_off, idx});
    return e.name_off;
}

void Index::link(uint32_t idx, uint32_t parent) {
    Entry& e = entries[idx];
    e.parent = parent;
    e.next_sibling = entries[parent].first_child;
    entries[parent].first_child = idx;
}

void Index::unlink(uint32_t idx) {
    const uint32_t p = entries[idx].parent;
    if (p == kNone) return;
    uint32_t* link = &entries[p].first_child;
    while (*link != kNone && *link != idx) link = &entries[*link].next_sibling;
    if (*link == idx) *link = entries[idx].next_sibling;
    entries[idx].parent = kNone;
    entries[idx].next_sibling = kNone;
}

uint32_t Index::add(uint32_t parent, std::string_view name, const Attrs& a) {
    const uint32_t idx = uint32_t(entries.size());
    Entry e{};
    e.ino = a.ino;
    e.size = a.size;
    e.mtime = a.mtime;
    e.parent = e.first_child = e.next_sibling = kNone;
    e.btime = a.btime;
    e.flags = a.dir ? kDir : 0;
    e.mode = a.mode;
    e.uid = a.uid;
    e.gid = a.gid;
    if (a.dir) dir_by_ino[a.ino] = idx;
    entries.push_back(e);
    push_name(name, idx, entries.back());
    if (parent != kNone) link(idx, parent);
    ++live;
    ++version;
    return idx;
}

void Index::kill_subtree(uint32_t idx) {
    std::vector<uint32_t> stack{idx};
    while (!stack.empty()) {
        const uint32_t n = stack.back();
        stack.pop_back();
        Entry& e = entries[n];
        if (e.flags & kDeleted) continue;
        e.flags |= kDeleted;
        --live;
        dead_bytes += e.name_len + e.lname_len + 2u;
        if (e.flags & kDir) {
            auto it = dir_by_ino.find(e.ino);
            if (it != dir_by_ino.end() && it->second == n) dir_by_ino.erase(it);
        }
        for (uint32_t c = e.first_child; c != kNone; c = entries[c].next_sibling) stack.push_back(c);
    }
}

void Index::remove_subtree(uint32_t idx) {
    if (idx == 0 || !alive(idx)) return;
    unlink(idx);
    kill_subtree(idx);
    ++version;
}

void Index::remove_children(uint32_t dir) {
    for (uint32_t c = entries[dir].first_child; c != kNone;) {
        const uint32_t next = entries[c].next_sibling;
        kill_subtree(c);
        c = next;
    }
    entries[dir].first_child = kNone;
    ++version;
}

void Index::move(uint32_t idx, uint32_t new_parent, std::string_view new_name) {
    if (idx == 0 || !alive(idx)) return;
    unlink(idx);
    if (name(idx) != new_name) {
        dead_bytes += entries[idx].name_len + entries[idx].lname_len + 2u;
        push_name(new_name, idx, entries[idx]);
    }
    link(idx, new_parent);
    ++version;
}

void Index::set_attrs(uint32_t idx, const Attrs& a) {
    Entry& e = entries[idx];
    const bool changed = e.size != a.size || e.mtime != a.mtime || e.btime != a.btime || e.mode != a.mode ||
                         e.uid != a.uid || e.gid != a.gid;
    e.size = a.size;
    e.mtime = a.mtime;
    e.btime = a.btime;
    e.mode = a.mode;
    e.uid = a.uid;
    e.gid = a.gid;
    if (changed) ++version;
}

uint32_t Index::find_child(uint32_t parent, std::string_view nm) const {
    if (parent >= entries.size()) return kNone;
    for (uint32_t c = entries[parent].first_child; c != kNone; c = entries[c].next_sibling)
        if (name(c) == nm) return c;
    return kNone;
}

uint32_t Index::dir_for_ino(uint64_t ino) const {
    auto it = dir_by_ino.find(ino);
    return it == dir_by_ino.end() ? kNone : it->second;
}

uint32_t Index::lookup(std::string_view p) const {
    if (entries.empty()) return kNone;  // том ещё не проиндексирован
    std::string_view rest;
    if (root_path == "/") {
        if (p.empty() || p[0] != '/') return kNone;
        rest = p.substr(1);
    } else {
        if (p.substr(0, root_path.size()) != root_path) return kNone;
        rest = p.substr(root_path.size());
        if (!rest.empty() && rest[0] != '/') return kNone;
        if (!rest.empty()) rest.remove_prefix(1);
    }
    uint32_t cur = 0;
    while (!rest.empty()) {
        const size_t slash = rest.find('/');
        const std::string_view comp = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        if (comp.empty()) continue;
        cur = find_child(cur, comp);
        if (cur == kNone) return kNone;
    }
    return cur;
}

std::string Index::path(uint32_t i) const {
    if (i == 0) return root_path;
    uint32_t chain[1024];
    int n = 0;
    for (uint32_t c = i; c != 0 && c != kNone && n < 1024; c = entries[c].parent) chain[n++] = c;
    std::string p = root_path == "/" ? std::string() : root_path;
    for (int k = n - 1; k >= 0; --k) {
        p += '/';
        p += name(chain[k]);
    }
    return p;
}

std::string Index::dir_path(uint32_t i) const {
    const uint32_t p = entries[i].parent;
    return p == kNone ? root_path : path(p);
}

void Index::append_lpath(uint32_t i, std::string& out) const {
    uint32_t chain[1024];
    int n = 0;
    for (uint32_t c = i; c != 0 && c != kNone && n < 1024; c = entries[c].parent) chain[n++] = c;
    out += lroot_path;
    for (int k = n - 1; k >= 0; --k) {
        out += '/';
        out += lname(chain[k]);
    }
}

bool Index::needs_compaction() const {
    const uint64_t dead_entries = entries.size() - live;
    return (dead_bytes > (1u << 20) && dead_bytes * 4 > names.size() + lnames.size()) ||
           (dead_entries > 10000 && dead_entries * 4 > entries.size());
}

void Index::compact() {
    std::vector<uint32_t> remap(entries.size(), kNone);
    uint32_t n = 0;
    for (uint32_t i = 0; i < entries.size(); ++i)
        if (!(entries[i].flags & kDeleted)) remap[i] = n++;

    Index nx;
    nx.root_path = root_path;
    nx.lroot_path = lroot_path;
    nx.epoch = epoch + 1;
    nx.version = version + 1;
    nx.entries.reserve(n);
    nx.names.reserve(names.size() - dead_bytes / 2);
    nx.lnames.reserve(lnames.size() - dead_bytes / 2);
    nx.lname_starts.reserve(n);
    nx.dir_by_ino.reserve(dir_by_ino.size());
    auto rm = [&](uint32_t v) { return v == kNone ? kNone : remap[v]; };

    for (uint32_t i = 0; i < entries.size(); ++i) {
        const Entry& o = entries[i];
        if (o.flags & kDeleted) continue;
        const uint32_t ni = uint32_t(nx.entries.size());
        Entry e = o;
        e.parent = rm(o.parent);
        e.first_child = rm(o.first_child);
        e.next_sibling = rm(o.next_sibling);
        if (o.flags & kDir) nx.dir_by_ino[o.ino] = ni;
        nx.entries.push_back(e);
        Entry& ne = nx.entries.back();
        ne.name_off = uint32_t(nx.names.size());
        nx.names.insert(nx.names.end(), names.begin() + o.name_off, names.begin() + o.name_off + o.name_len + 1);
        ne.lname_off = uint32_t(nx.lnames.size());
        nx.lnames.insert(nx.lnames.end(), lnames.begin() + o.lname_off,
                         lnames.begin() + o.lname_off + o.lname_len + 1);
        nx.lname_starts.push_back({ne.lname_off, ni});
    }
    nx.live = n;
    *this = std::move(nx);
}

size_t Index::memory_bytes() const {
    return entries.capacity() * sizeof(Entry) + names.capacity() + lnames.capacity() +
           lname_starts.capacity() * sizeof(NameStart) +
           dir_by_ino.bucket_count() * sizeof(void*) + dir_by_ino.size() * 32;
}

size_t Index::dir_count() const {
    size_t n = 0;
    for (const Entry& e : entries)
        if ((e.flags & (kDir | kDeleted)) == kDir) ++n;
    return n;
}

// ---------------------------------------------------------------- IndexBuilder

void IndexBuilder::add(uint64_t parent_ino, std::string_view name, const Attrs& a) {
    const uint16_t len = uint16_t(std::min<size_t>(name.size(), UINT16_MAX));
    recs_.push_back(Rec{parent_ino, a, uint32_t(names_.size()), len});
    names_.insert(names_.end(), name.begin(), name.begin() + len);
}

void IndexBuilder::append(IndexBuilder&& o) {
    const uint32_t base = uint32_t(names_.size());
    names_.insert(names_.end(), o.names_.begin(), o.names_.end());
    recs_.reserve(recs_.size() + o.recs_.size());
    for (Rec r : o.recs_) {
        r.name_off += base;
        recs_.push_back(r);
    }
    o.recs_ = {};
    o.names_ = {};
}

size_t IndexBuilder::finish(Index& ix, std::string root_path, const Attrs& ra) {
    ix.reset(std::move(root_path), ra);
    ix.entries.reserve(recs_.size() + 1);
    ix.lname_starts.reserve(recs_.size() + 1);
    ix.names.reserve(names_.size() + recs_.size() + 1);
    ix.lnames.reserve(names_.size() + recs_.size() + 1);
    ix.dir_by_ino.reserve(recs_.size() / 6 + 16);

    // 1) записи без связей
    for (const Rec& r : recs_)
        ix.add(kNone, std::string_view(names_.data() + r.name_off, r.name_len), r.a);

    // 2) связи по номеру inode родителя
    size_t orphans = 0;
    for (size_t k = 0; k < recs_.size(); ++k) {
        const uint32_t idx = uint32_t(k + 1);
        const uint32_t p = ix.dir_for_ino(recs_[k].parent_ino);
        if (p != kNone && p != idx)
            ix.link(idx, p);
        else
            ++orphans;
    }
    // 3) всё недостижимое от корня — удалить
    if (orphans) {
        std::vector<char> seen(ix.entries.size(), 0);
        std::vector<uint32_t> stack{0};
        while (!stack.empty()) {
            const uint32_t n = stack.back();
            stack.pop_back();
            seen[n] = 1;
            for (uint32_t c = ix.entries[n].first_child; c != kNone; c = ix.entries[c].next_sibling)
                stack.push_back(c);
        }
        for (uint32_t i = 0; i < ix.entries.size(); ++i)
            if (!seen[i] && !(ix.entries[i].flags & kDeleted)) {
                ix.entries[i].parent = kNone;
                ix.kill_subtree(i);
            }
    }
    recs_ = {};
    names_ = {};
    return orphans;
}

}  // namespace bx
