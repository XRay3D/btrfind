// Тесты группового переименования: шаблоны, проверки, обмен имён, каталоги, отмена, права.
#include "rename.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace bx;

namespace {

void touch(const QString& p, const QByteArray& content = {}) {
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

QString apply(const QString& oldf, const QString& newf, const QString& name, RenameOptions o = {}, int counter = 1) {
    RenamePattern p;
    QString err, out;
    if (!p.compile(oldf, newf, o, err)) return QStringLiteral("ERR:") + err;
    if (!p.apply(name, counter, out)) return QStringLiteral("NOMATCH");
    return out;
}

QStringList ls(const QString& dir) {
    QStringList l = QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    l.sort();
    return l;
}

}  // namespace

class TestRename : public QObject {
    Q_OBJECT
private slots:
    void suggest() {
        QCOMPARE(suggest_old_format({"IMG_0001.jpg", "IMG_0002.jpg", "IMG_0013.jpg"}), QString("IMG_%1.jpg"));
        QCOMPARE(suggest_old_format({"a.txt", "b.txt"}), QString("%1.txt"));
        QCOMPARE(suggest_old_format({"report.pdf", "report.doc"}), QString("report.%1"));
        QCOMPARE(suggest_old_format({"100%.txt", "50%.txt"}), QString("%1%%.txt"));
        QCOMPARE(suggest_old_format({"same", "same"}), QString("same"));
    }

    void patterns() {
        QCOMPARE(apply("IMG_%1.jpg", "Отпуск_%1.jpg", "IMG_0042.jpg"), QString("Отпуск_0042.jpg"));
        QCOMPARE(apply("%1 - %2.mp3", "%2 - %1.mp3", "Artist - Song.mp3"), QString("Song - Artist.mp3"));
        QCOMPARE(apply("%1.%2", "photo_%03n.%2", "DSC1234.JPG", {}, 7), QString("photo_007.JPG"));
        QCOMPARE(apply("%1", "%n_%1", "x", {}, 12), QString("12_x"));
        QCOMPARE(apply("%1", "100%% %1", "x"), QString("100% x"));
        QCOMPARE(apply("img_%1.jpg", "%1.jpg", "IMG_1.JPG"), QString("1.jpg"));  // без учёта регистра
        RenameOptions cs;
        cs.case_sensitive = true;
        QCOMPARE(apply("img_%1.jpg", "%1.jpg", "IMG_1.JPG", cs), QString("NOMATCH"));
        QCOMPARE(apply("IMG_%1.jpg", "%1", "other.png"), QString("NOMATCH"));
        QCOMPARE(apply("%1", "%2", "x").left(4), QString("ERR:"));  // %2 нет в старом формате
        QCOMPARE(apply("%1.txt", "%1.md", "a.b.txt"), QString("a.b.md"));
        QCOMPARE(apply("a(%1)", "b[%1]", "a(x)"), QString("b[x]"));  // спецсимволы regex экранируются
    }

    void regex_mode() {
        RenameOptions rx;
        rx.regex = true;
        QCOMPARE(apply("(\\d{4})-(\\d\\d)-(\\d\\d)_(.*)", "\\3.\\2.\\1 $4", "2026-11-18_отчёт.pdf", rx),
                 QString("18.11.2026 отчёт.pdf"));
        QCOMPARE(apply("(.*)\\.jpeg", "%1.jpg", "a.JPEG", rx), QString("a.jpg"));
        QCOMPARE(apply("(.*", "x", "a", rx).left(4), QString("ERR:"));
        QCOMPARE(apply("(a)", "\\2", "a", rx).left(4), QString("ERR:"));
    }

    void plan_checks() {
        QTemporaryDir tmp;
        const QString R = tmp.path();
        for (const char* n : {"a.txt", "b.txt", "c.txt", "keep.dat", "taken.txt"}) touch(R + "/" + n);
        RenamePattern p;
        QString err;

        // все в одно имя -> дубликаты
        QVERIFY(p.compile("%1.txt", "same.txt", {}, err));
        auto plan = plan_rename({R + "/a.txt", R + "/b.txt"}, p);
        QCOMPARE(int(plan[0].status), int(RenameStatus::Duplicate));
        QCOMPARE(int(plan[1].status), int(RenameStatus::Duplicate));

        // цель уже существует (и не входит в партию)
        QVERIFY(p.compile("a.txt", "taken.txt", {}, err));
        plan = plan_rename({R + "/a.txt", R + "/keep.dat"}, p);
        QCOMPARE(int(plan[0].status), int(RenameStatus::Exists));
        QCOMPARE(int(plan[1].status), int(RenameStatus::NoMatch));

        // недопустимые имена
        QVERIFY(p.compile("%1.txt", "x/%1", {}, err));
        QCOMPARE(int(plan_rename({R + "/a.txt"}, p)[0].status), int(RenameStatus::Invalid));
        QVERIFY(p.compile("%1", QString(300, 'z'), {}, err));
        QCOMPARE(int(plan_rename({R + "/a.txt"}, p)[0].status), int(RenameStatus::Invalid));
        QVERIFY(p.compile("%1", "", {}, err));
        QCOMPARE(int(plan_rename({R + "/a.txt"}, p)[0].status), int(RenameStatus::Invalid));

        // без изменений
        QVERIFY(p.compile("%1", "%1", {}, err));
        int errors = -1;
        QCOMPARE(plan_summary(plan_rename({R + "/a.txt"}, p), &errors), 0);
        QCOMPARE(errors, 0);

        // один файл
        QCOMPARE(int(plan_single(R + "/a.txt", "taken.txt")[0].status), int(RenameStatus::Exists));
        QCOMPARE(int(plan_single(R + "/a.txt", "new.txt")[0].status), int(RenameStatus::Rename));
        QCOMPARE(int(plan_single(R + "/a.txt", "a.txt")[0].status), int(RenameStatus::Unchanged));
    }

    void execute_and_undo() {
        QTemporaryDir tmp;
        const QString R = tmp.path();
        for (int i = 1; i <= 3; ++i) touch(R + QStringLiteral("/IMG_000%1.jpg").arg(i), QByteArray::number(i));
        RenamePattern p;
        QString err;
        QVERIFY(p.compile("IMG_%1.jpg", "Отпуск_%03n.jpg", {}, err));
        const QStringList paths{R + "/IMG_0001.jpg", R + "/IMG_0002.jpg", R + "/IMG_0003.jpg"};
        const auto plan = plan_rename(paths, p);
        QCOMPARE(plan_summary(plan), 3);
        const RenameResult r = execute_rename(plan);
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join("; ")));
        QCOMPARE(ls(R), (QStringList{"Отпуск_001.jpg", "Отпуск_002.jpg", "Отпуск_003.jpg"}));
        QFile f(R + "/Отпуск_002.jpg");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("2"));  // содержимое переехало с файлом

        const RenameResult u = undo_rename(r.done);
        QVERIFY2(u.errors.isEmpty(), qPrintable(u.errors.join("; ")));
        QCOMPARE(ls(R), (QStringList{"IMG_0001.jpg", "IMG_0002.jpg", "IMG_0003.jpg"}));
    }

    void swap_and_cycle() {
        QTemporaryDir tmp;
        const QString R = tmp.path();
        touch(R + "/a", "A");
        touch(R + "/b", "B");
        touch(R + "/c", "C");
        // цикл a->b, b->c, c->a (обмен — частный случай)
        RenamePattern p;
        QString err;
        QVERIFY(p.compile("%1", "%1", {}, err));
        std::vector<PlanItem> plan = {
            {R, "a", "b", RenameStatus::Rename, {}},
            {R, "b", "c", RenameStatus::Rename, {}},
            {R, "c", "a", RenameStatus::Rename, {}},
        };
        // plan_rename сочтёт это допустимым: цели заняты файлами этой же партии
        RenameResult r = execute_rename(plan);
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join("; ")));
        auto content = [&](const char* n) {
            QFile f(R + "/" + n);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        QCOMPARE(content("b"), QByteArray("A"));
        QCOMPARE(content("c"), QByteArray("B"));
        QCOMPARE(content("a"), QByteArray("C"));
        QCOMPARE(ls(R), (QStringList{"a", "b", "c"}));  // временных имён не осталось

        // та же партия через plan_rename (подстановка по шаблону regex)
        RenameOptions rx;
        rx.regex = true;
        QVERIFY(p.compile("x_(.*)", "y_\\1", rx, err));
        touch(R + "/x_1");
        touch(R + "/y_1");  // занято файлом вне партии
        QCOMPARE(int(plan_rename({R + "/x_1"}, p)[0].status), int(RenameStatus::Exists));
    }

    void directory_with_contents() {
        QTemporaryDir tmp;
        const QString R = tmp.path();
        QVERIFY(QDir(R).mkpath("photos/2026"));
        touch(R + "/photos/2026/img.jpg");
        // в партии и каталог, и файл внутри него
        std::vector<PlanItem> plan = {
            {R, "photos", "Фото", RenameStatus::Rename, {}},
            {R + "/photos/2026", "img.jpg", "pic.jpg", RenameStatus::Rename, {}},
        };
        RenameResult r = execute_rename(plan);
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join("; ")));
        QVERIFY(QFile::exists(R + "/Фото/2026/pic.jpg"));
        RenameResult u = undo_rename(r.done);
        QVERIFY2(u.errors.isEmpty(), qPrintable(u.errors.join("; ")));
        QVERIFY(QFile::exists(R + "/photos/2026/img.jpg"));
    }

    void no_permission_and_noreplace() {
        if (geteuid() == 0) QSKIP("от root права не ограничивают");
        QTemporaryDir tmp;
        const QString R = tmp.path();
        QVERIFY(QDir(R).mkpath("ro"));
        touch(R + "/ro/f.txt");
        touch(R + "/ok.txt");
        chmod(QFile::encodeName(R + "/ro").constData(), 0555);
        std::vector<PlanItem> plan = {
            {R + "/ro", "f.txt", "g.txt", RenameStatus::Rename, {}},
            {R, "ok.txt", "done.txt", RenameStatus::Rename, {}},
        };
        RenameResult r = execute_rename(plan);
        chmod(QFile::encodeName(R + "/ro").constData(), 0755);
        QCOMPARE(int(r.errors.size()), 1);  // ошибка одного файла не мешает остальным
        QCOMPARE(int(r.done.size()), 1);
        QVERIFY(QFile::exists(R + "/done.txt"));

        // существующий файл никогда не перезаписывается, даже если план устарел
        touch(R + "/x", "X");
        touch(R + "/y", "Y");
        std::vector<PlanItem> stale = {{R, "x", "y", RenameStatus::Rename, {}}};
        r = execute_rename(stale);
        QCOMPARE(int(r.errors.size()), 1);
        QFile f(R + "/y");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("Y"));
    }
};

QTEST_GUILESS_MAIN(TestRename)
#include "test_rename.moc"
