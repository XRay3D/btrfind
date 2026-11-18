// Юнит-тесты ядра: свёртка регистра, glob/regex, разбор запросов, индекс, сверка, поиск, хранилище.
#include "fold.h"
#include "fsutil.h"
#include "index.h"
#include "matcher.h"
#include "query.h"
#include "reconcile.h"
#include "search.h"
#include "storage.h"
#include "walk_scan.h"

#include <QTemporaryDir>
#include <QtTest>

#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <random>
#include <set>

using namespace bx;

namespace {

void touch(const std::string& p, size_t size = 0) {
    std::ofstream f(p);
    f << std::string(size, 'x');
}

std::set<std::string> paths_of(const Index& ix) {
    std::set<std::string> s;
    for (uint32_t i = 1; i < ix.entries.size(); ++i)
        if (ix.alive(i)) s.insert(ix.path(i));
    return s;
}

std::set<std::string> names_found(const Index& ix, const std::string& q, QueryOptions o = {}) {
    const Query qq = parse_query(q, o);
    std::set<std::string> s;
    for (Ref r : run_search(qq, {{&ix, 0}}, nullptr, nullptr, nullptr)) s.insert(std::string(ix.name(ref_idx(r))));
    return s;
}

Attrs file_attrs(uint64_t ino, uint64_t size = 0) {
    Attrs a;
    a.ino = ino;
    a.size = size;
    a.mode = 0100644;
    return a;
}
Attrs dir_attrs(uint64_t ino, uint32_t mode = 040755, uint32_t uid = 0) {
    Attrs a;
    a.ino = ino;
    a.dir = true;
    a.mode = mode;
    a.uid = uid;
    return a;
}

DevId dev_of(const std::string& p) {
    FsStat st;
    stat_at(AT_FDCWD, p.c_str(), st);
    return st.dev;
}

}  // namespace

class TestCore : public QObject {
    Q_OBJECT
private slots:
    void fold() {
        QCOMPARE(fold_case("ReadMe.TXT"), std::string("readme.txt"));
        QCOMPARE(fold_case("Отчёт ДЛЯ Пети.PDF"), std::string("отчёт для пети.pdf"));
        QVERIFY(is_ascii("abc"));
        QVERIFY(!is_ascii("абв"));
    }

    void glob() {
        QVERIFY(glob_match("*.pdf", "report.pdf"));
        QVERIFY(!glob_match("*.pdf", "report.pdf.bak"));
        QVERIFY(glob_match("rep?rt.*", "report.txt"));
        QVERIFY(glob_match("*", ""));
        QVERIFY(glob_match("а?в", "абв"));  // ? = один символ UTF-8
        QVERIFY(glob_match("*a*b*c*", "xxaxxbxxcxx"));
        QVERIFY(!glob_match("*a*b*c", "xxaxxbxxcxx"));
    }

    void regex() {
        std::string err;
        auto re = Regex::compile("^cmake.*\\.txt$", false, err);
        QVERIFY2(re, err.c_str());
        QVERIFY(re->match("CMakeLists.txt"));
        QVERIFY(!re->match("CMakeLists.txt.bak"));
        auto ru = Regex::compile("отчёт", false, err);
        QVERIFY(ru->match("ОТЧЁТ.pdf"));
        auto cs = Regex::compile("ABC", true, err);
        QVERIFY(!cs->match("abc"));
        QVERIFY(!Regex::compile("(", false, err));
        QVERIFY(!err.empty());
    }

    void sizes_and_dates() {
        uint64_t mn, mx;
        QVERIFY(parse_size_filter(">10mb", mn, mx));
        QCOMPARE(mn, 10ull * 1024 * 1024 + 1);
        QVERIFY(parse_size_filter("1kb..5kb", mn, mx));
        QCOMPARE(mn, 1024ull);
        QCOMPARE(mx, 5120ull);
        QVERIFY(parse_size_filter("empty", mn, mx));
        QCOMPARE(mx, 0ull);
        QVERIFY(!parse_size_filter("abc", mn, mx));

        int64_t a, b;
        const time_t now = time(nullptr);
        QVERIFY(parse_date_filter("today", now, a, b));
        QVERIFY(a <= now && b == INT64_MAX);
        QVERIFY(parse_date_filter("2026-01-01", now, a, b));
        QCOMPARE(b - a, int64_t(24 * 3600 - 1));
        QVERIFY(parse_date_filter("2026-01-01..2026-01-31", now, a, b));
        QVERIFY(!parse_date_filter("вчера", now, a, b));
    }

    void query_parse() {
        Query q = parse_query("foo bar", {});
        QCOMPARE(int(q.groups.size()), 2);
        q = parse_query("foo | bar baz", {});
        QCOMPARE(int(q.groups.size()), 2);
        QCOMPARE(int(q.groups[0].size()), 2);
        q = parse_query("\"a b\" !c *.pdf src/main", {});
        QCOMPARE(int(q.groups.size()), 4);
        QCOMPARE(q.groups[0][0].text, std::string("a b"));
        QVERIFY(q.groups[1][0].negate);
        QCOMPARE(int(q.groups[2][0].kind), int(Term::Glob));
        QVERIFY(q.groups[3][0].path);
        q = parse_query("ext:pdf;.DOCX size:>1mb dm:today file: folder:", {});
        QVERIFY(q.ok());
        QCOMPARE(q.groups[0][0].exts, (std::vector<std::string>{"pdf", "docx"}));
        q = parse_query("size:huh", {});
        QVERIFY(!q.ok());
        q = parse_query("regex:([", {});
        QVERIFY(!q.ok());
        QueryOptions ro;
        ro.regex = true;
        q = parse_query("^a.*b$", ro);
        QVERIFY(q.ok());
        QCOMPARE(int(q.groups[0][0].kind), int(Term::Regex));
        QVERIFY(parse_query("   ", {}).match_all());
    }

    void index_ops() {
        Index ix;
        ix.reset("/vol", dir_attrs(256));
        const uint32_t a = ix.add(0, "Docs", dir_attrs(300));
        const uint32_t f = ix.add(a, "Report.PDF", file_attrs(301, 5000));
        const uint32_t b = ix.add(0, "other", dir_attrs(302));
        QCOMPARE(ix.path(f), std::string("/vol/Docs/Report.PDF"));
        QCOMPARE(ix.dir_path(f), std::string("/vol/Docs"));
        QCOMPARE(ix.lookup("/vol/Docs/Report.PDF"), f);
        QCOMPARE(ix.lookup("/vol/nope"), kNone);
        QCOMPARE(ix.find_child(a, "Report.PDF"), f);
        QCOMPARE(ix.dir_for_ino(300), a);
        std::string lp;
        ix.append_lpath(f, lp);
        QCOMPARE(lp, std::string("/vol/docs/report.pdf"));

        ix.move(a, b, "Документы");  // перенос каталога с переименованием: поддерево сохраняется
        QCOMPARE(ix.path(f), std::string("/vol/other/Документы/Report.PDF"));
        QCOMPARE(names_found(ix, "документы"), std::set<std::string>{"Документы"});
        QCOMPARE(names_found(ix, "docs"), std::set<std::string>{});  // старое имя мёртвое

        ix.remove_subtree(b);
        QVERIFY(!ix.alive(f));
        QCOMPARE(ix.live, uint64_t(1));
        QCOMPARE(ix.dir_for_ino(300), kNone);
        QVERIFY(names_found(ix, "report").empty());

        for (int i = 0; i < 100; ++i) ix.add(0, "file" + std::to_string(i), file_attrs(1000 + i));
        ix.compact();
        QCOMPARE(ix.entries.size(), size_t(101));
        QCOMPARE(int(names_found(ix, "file9").size()), 11);
        QCOMPARE(ix.lookup("/vol/file42"), ix.find_child(0, "file42"));
    }

    void empty_index() {
        Index ix;  // том до первого скана
        QCOMPARE(ix.lookup("/home/x"), kNone);
        QCOMPARE(ix.find_child(0, "x"), kNone);
        QCOMPARE(ix.dir_for_ino(256), kNone);
        QVERIFY(!ix.alive(0));
        QVERIFY(run_search(parse_query("x", {}), {{&ix, 0}}, nullptr, nullptr, nullptr).empty());
        Reconciler rec(ix, DevId{});
        rec.reconcile_name(0, AT_FDCWD, "x");
        rec.rescan_dirs({0});
    }

    void builder_orphans() {
        IndexBuilder b;
        b.add(256, "a", dir_attrs(300));
        b.add(300, "x", file_attrs(301));
        b.add(999, "orphan", file_attrs(302));  // родителя нет
        Index ix;
        QCOMPARE(b.finish(ix, "/", dir_attrs(256)), size_t(1));
        QCOMPARE(paths_of(ix), (std::set<std::string>{"/a", "/a/x"}));
    }

    void search_vs_bruteforce() {
        Index ix;
        ix.reset("/r", dir_attrs(256));
        std::mt19937 rng(42);
        const char* parts[] = {"main", "Test", "readme", "Отчёт", "data", "CMake", "lists", ".txt", ".cpp", ".pdf"};
        std::vector<uint32_t> dirs{0};
        for (int i = 0; i < 3000; ++i) {
            std::string n = std::string(parts[rng() % 6]) + std::to_string(rng() % 50) + parts[6 + rng() % 4];
            const uint32_t parent = dirs[rng() % dirs.size()];
            if (ix.find_child(parent, n) != kNone) continue;
            if (rng() % 5 == 0)
                dirs.push_back(ix.add(parent, n, dir_attrs(10000 + i)));
            else
                ix.add(parent, n, file_attrs(10000 + i, rng() % 100000));
        }
        // переименования, чтобы проверить мёртвые имена в пуле
        for (int i = 0; i < 200; ++i) {
            const uint32_t idx = 1 + rng() % (ix.entries.size() - 1);
            if (ix.alive(idx) && !ix.is_dir(idx)) ix.move(idx, ix.entries[idx].parent, "renamed" + std::to_string(i) + ".txt");
        }
        for (const char* q : {"main", "MAIN", "отчёт", "a", ".pdf", "renamed1", "*.cpp", "test !.txt",
                              "main | data", "ext:pdf;txt", "size:>50000", "file: lists", "folder:", "r/test",
                              "regex:^cmake\\d+\\.", "test cpp"}) {
            const Query qq = parse_query(q, {});
            QVERIFY2(qq.ok(), q);
            std::set<uint32_t> got, want;
            for (Ref r : run_search(qq, {{&ix, 0}}, nullptr, nullptr, nullptr)) got.insert(ref_idx(r));
            Query brute = qq;  // полный проход без префильтра по пулу
            brute.no_pool_scan = true;
            for (Ref r : run_search(brute, {{&ix, 0}}, nullptr, nullptr, nullptr)) want.insert(ref_idx(r));
            // и честный перебор для простых подстрок
            if (qq.groups.size() == 1 && qq.groups[0].size() == 1 && qq.groups[0][0].kind == Term::Substr &&
                !qq.groups[0][0].path) {
                std::set<uint32_t> naive;
                for (uint32_t i = 1; i < ix.entries.size(); ++i)
                    if (ix.alive(i) && ix.lname(i).find(qq.groups[0][0].ftext) != std::string_view::npos) naive.insert(i);
                QVERIFY2(naive == want, q);
            }
            QVERIFY2(got == want, q);
        }
        // сортировка по имени
        auto refs = run_search(parse_query("main", {}), {{&ix, 0}}, nullptr, nullptr, nullptr);
        sort_results(refs, {{&ix, 0}}, SortKey::Name, false);
        for (size_t i = 1; i < refs.size(); ++i) QVERIFY(ix.lname(ref_idx(refs[i - 1])) <= ix.lname(ref_idx(refs[i])));
        sort_results(refs, {{&ix, 0}}, SortKey::Path, true);
        sort_results(refs, {{&ix, 0}}, SortKey::Size, false);
        for (size_t i = 1; i < refs.size(); ++i)
            QVERIFY(ix.entries[ref_idx(refs[i - 1])].size <= ix.entries[ref_idx(refs[i])].size);
    }

    void access_filter() {
        Index ix;
        ix.reset("/r", dir_attrs(256, 040755));
        const uint32_t priv = ix.add(0, "private", dir_attrs(300, 040700, 1000));
        ix.add(priv, "secret.txt", file_attrs(301));
        const uint32_t pub = ix.add(0, "public", dir_attrs(302, 040755, 1000));
        ix.add(pub, "secret2.txt", file_attrs(303));
        const Query q = parse_query("secret", {});
        Access other{2000, {2000}};
        QCOMPARE(int(run_search(q, {{&ix, 0}}, &other, nullptr, nullptr).size()), 1);
        Access owner{1000, {1000}};
        QCOMPARE(int(run_search(q, {{&ix, 0}}, &owner, nullptr, nullptr).size()), 2);
        Access root{0, {}};
        QCOMPARE(int(run_search(q, {{&ix, 0}}, &root, nullptr, nullptr).size()), 2);
    }

    void attributes_filters_sorting() {
        QTemporaryDir tmp;
        const std::string R = tmp.path().toStdString();
        mkdir((R + "/dir").c_str(), 0750);
        touch(R + "/b.txt", 30);
        touch(R + "/a.pdf", 10);
        touch(R + "/c.txt", 20);
        chmod((R + "/c.txt").c_str(), 0600);
        Index ix;
        WalkStats ws;
        std::string err;
        QVERIFY(walk_scan(R, {}, ix, ws, err));
        const Entry& c = ix.entries[ix.lookup(R + "/c.txt")];
        QCOMPARE(c.uid, uint32_t(getuid()));
        QCOMPARE(c.mode & 07777, 0600u);
        QVERIFY(S_ISREG(c.mode));
        QVERIFY(S_ISDIR(ix.entries[ix.lookup(R + "/dir")].mode));

        const std::vector<SearchVolume> vols{{&ix, 0}};
        auto names = [&](const std::vector<Ref>& refs) {
            std::vector<std::string> v;
            for (Ref r : refs) v.emplace_back(ix.name(ref_idx(r)));
            return v;
        };
        auto all = run_search(parse_query("", {}), vols, nullptr, nullptr, nullptr);
        sort_results(all, vols, SortKey::Ext, false);  // папки первыми, затем по расширению
        QCOMPARE(names(all), (std::vector<std::string>{"dir", "a.pdf", "b.txt", "c.txt"}));
        sort_results(all, vols, SortKey::Mode, false);
        QCOMPARE(names(all).front(), std::string("c.txt"));  // 0600 — наименьшие права
        sort_results(all, vols, SortKey::Owner, false);
        sort_results(all, vols, SortKey::Btime, true);
        QCOMPARE(int(all.size()), 4);

        // итоги: размер только файлов
        std::vector<const Index*> by_vol{&ix};
        const Totals t = sum_results(all, by_vol, 0, all.size());
        QCOMPARE(t.bytes, uint64_t(60));
        QCOMPARE(t.files, uint64_t(3));
        QCOMPARE(t.dirs, uint64_t(1));
        QCOMPARE(sum_results(all, by_vol, 1, 2).files + sum_results(all, by_vol, 1, 2).dirs, uint64_t(1));

        // фильтры по владельцу и дате создания
        const std::string me = std::to_string(getuid());
        QCOMPARE(int(run_search(parse_query("owner:" + me + " .txt", {}), vols, nullptr, nullptr, nullptr).size()), 2);
        QCOMPARE(int(run_search(parse_query("owner:999999 .txt", {}), vols, nullptr, nullptr, nullptr).size()), 0);
        QVERIFY(!parse_query("owner:нет_такого_пользователя", {}).ok());
        QVERIFY(parse_query("group:" + std::to_string(getgid()), {}).ok());
        const bool has_btime = ix.entries[ix.lookup(R + "/a.pdf")].btime != 0;
        if (has_btime)  // ФС с временем создания (btrfs, ext4, xfs, tmpfs)
            QCOMPARE(int(run_search(parse_query("dc:today", {}), vols, nullptr, nullptr, nullptr).size()), 4);
        QCOMPARE(int(run_search(parse_query("dc:<2000-01-01", {}), vols, nullptr, nullptr, nullptr).size()), 0);
    }

    void walk_and_reconcile() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::string R = tmp.path().toStdString();
        mkdir((R + "/a").c_str(), 0755);
        mkdir((R + "/a/sub").c_str(), 0755);
        touch(R + "/a/sub/deep.txt");
        touch(R + "/a/one.txt", 10);
        touch(R + "/two.txt");

        Index ix;
        WalkStats ws;
        std::string err;
        QVERIFY2(walk_scan(R, {}, ix, ws, err), err.c_str());
        QCOMPARE(paths_of(ix), (std::set<std::string>{R + "/a", R + "/a/sub", R + "/a/sub/deep.txt", R + "/a/one.txt",
                                                       R + "/two.txt"}));
        Reconciler rec(ix, dev_of(R));
        auto reconcile = [&](const std::string& dir, const std::string& name) {
            const uint32_t d = ix.lookup(dir);
            QVERIFY(d != kNone);
            const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
            rec.reconcile_name(d, fd, name);
            close(fd);
        };

        // перенос каталога с содержимым: событие RENAME (сначала новое имя, потом старое)
        const uint32_t sub = ix.lookup(R + "/a/sub");
        QVERIFY(rename((R + "/a/sub").c_str(), (R + "/moved").c_str()) == 0);
        reconcile(R, "moved");
        reconcile(R + "/a", "sub");
        QCOMPARE(ix.lookup(R + "/moved"), sub);  // та же запись — поддерево сохранено
        QVERIFY(ix.lookup(R + "/moved/deep.txt") != kNone);
        QCOMPARE(rec.stats.moved, uint64_t(1));

        // создание, изменение размера, удаление, O_TMPFILE-имя
        touch(R + "/new.txt", 5);
        reconcile(R, "new.txt");
        QCOMPARE(ix.entries[ix.lookup(R + "/new.txt")].size, uint64_t(5));
        touch(R + "/new.txt", 50);
        reconcile(R, "new.txt");
        QCOMPARE(ix.entries[ix.lookup(R + "/new.txt")].size, uint64_t(50));
        unlink((R + "/two.txt").c_str());
        reconcile(R, "two.txt");
        reconcile(R, "#123456");
        QCOMPARE(ix.lookup(R + "/two.txt"), kNone);

        // новый каталог с содержимым (перенесён извне) — обходится целиком
        mkdir((R + "/fresh").c_str(), 0755);
        mkdir((R + "/fresh/x").c_str(), 0755);
        touch(R + "/fresh/x/y.txt");
        reconcile(R, "fresh");
        QVERIFY(ix.lookup(R + "/fresh/x/y.txt") != kNone);

        // догон «вслепую»: изменения без событий, потом полная сверка каталогов
        touch(R + "/a/late.txt");
        unlink((R + "/a/one.txt").c_str());
        rec.rescan_dirs({ix.lookup(R), ix.lookup(R + "/a")});
        Index fresh;
        QVERIFY(walk_scan(R, {}, fresh, ws, err));
        QCOMPARE(paths_of(ix), paths_of(fresh));

        // сохранение и загрузка
        StoredMeta meta;
        meta.subvol = 257;
        meta.generation = 12345;
        const std::string file = R + "/index.bin";
        QVERIFY2(save_index(file, ix, meta, err), err.c_str());
        Index loaded;
        StoredMeta got;
        QVERIFY2(load_index(file, loaded, got, meta, err), err.c_str());
        QCOMPARE(got.generation, uint64_t(12345));
        QCOMPARE(paths_of(loaded), paths_of(ix));
        QCOMPARE(loaded.dir_for_ino(ix.entries[sub].ino), sub);
        StoredMeta wrong = meta;
        wrong.subvol = 1;
        QVERIFY(!load_index(file, loaded, got, wrong, err));
    }
};

QTEST_GUILESS_MAIN(TestCore)
#include "test_core.moc"
