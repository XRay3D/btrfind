// Тесты группового переименования: шаблоны, проверки, обмен имён, каталоги, отмена, права.
#include "rename.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

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
    if (!p.compile(oldf, newf, o, err)) return u"ERR:"_s + err;
    if (!p.apply(name, counter, out)) return u"NOMATCH"_s;
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
        QCOMPARE(suggest_old_format({u"IMG_0001.jpg"_s, u"IMG_0002.jpg"_s, u"IMG_0013.jpg"_s}), QString(u"IMG_%1.jpg"_s));
        QCOMPARE(suggest_old_format({u"a.txt"_s, u"b.txt"_s}), QString(u"%1.txt"_s));
        QCOMPARE(suggest_old_format({u"report.pdf"_s, u"report.doc"_s}), QString(u"report.%1"_s));
        QCOMPARE(suggest_old_format({u"100%.txt"_s, u"50%.txt"_s}), QString(u"%1%%.txt"_s));
        QCOMPARE(suggest_old_format({u"same"_s, u"same"_s}), QString(u"same"_s));
    }

    void patterns() {
        QCOMPARE(apply(u"IMG_%1.jpg"_s, u"Отпуск_%1.jpg"_s, u"IMG_0042.jpg"_s), QString(u"Отпуск_0042.jpg"_s));
        QCOMPARE(apply(u"%1 - %2.mp3"_s, u"%2 - %1.mp3"_s, u"Artist - Song.mp3"_s), QString(u"Song - Artist.mp3"_s));
        QCOMPARE(apply(u"%1.%2"_s, u"photo_%03n.%2"_s, u"DSC1234.JPG"_s, {}, 7), QString(u"photo_007.JPG"_s));
        QCOMPARE(apply(u"%1"_s, u"%n_%1"_s, u"x"_s, {}, 12), QString(u"12_x"_s));
        QCOMPARE(apply(u"%1"_s, u"100%% %1"_s, u"x"_s), QString(u"100% x"_s));
        QCOMPARE(apply(u"img_%1.jpg"_s, u"%1.jpg"_s, u"IMG_1.JPG"_s), QString(u"1.jpg"_s));  // без учёта регистра
        RenameOptions cs;
        cs.case_sensitive = true;
        QCOMPARE(apply(u"img_%1.jpg"_s, u"%1.jpg"_s, u"IMG_1.JPG"_s, cs), QString(u"NOMATCH"_s));
        QCOMPARE(apply(u"IMG_%1.jpg"_s, u"%1"_s, u"other.png"_s), QString(u"NOMATCH"_s));
        QCOMPARE(apply(u"%1"_s, u"%2"_s, u"x"_s).left(4), QString(u"ERR:"_s));  // %2 нет в старом формате
        QCOMPARE(apply(u"%1.txt"_s, u"%1.md"_s, u"a.b.txt"_s), QString(u"a.b.md"_s));
        QCOMPARE(apply(u"a(%1)"_s, u"b[%1]"_s, u"a(x)"_s), QString(u"b[x]"_s));  // спецсимволы regex экранируются
    }

    void regex_mode() {
        RenameOptions rx;
        rx.regex = true;
        QCOMPARE(apply(u"(\\d{4})-(\\d\\d)-(\\d\\d)_(.*)"_s, u"\\3.\\2.\\1 $4"_s, u"2026-11-18_отчёт.pdf"_s, rx),
                 QString(u"18.11.2026 отчёт.pdf"_s));
        QCOMPARE(apply(u"(.*)\\.jpeg"_s, u"%1.jpg"_s, u"a.JPEG"_s, rx), QString(u"a.jpg"_s));
        QCOMPARE(apply(u"(.*"_s, u"x"_s, u"a"_s, rx).left(4), QString(u"ERR:"_s));
        QCOMPARE(apply(u"(a)"_s, u"\\2"_s, u"a"_s, rx).left(4), QString(u"ERR:"_s));
    }

    void plan_checks() {
        QTemporaryDir tmp;
        const QString R = tmp.path();
        for (const QString& n : {u"a.txt"_s, u"b.txt"_s, u"c.txt"_s, u"keep.dat"_s, u"taken.txt"_s}) touch(R + u"/"_s + n);
        RenamePattern p;
        QString err;

        // все в одно имя -> дубликаты
        QVERIFY(p.compile(u"%1.txt"_s, u"same.txt"_s, {}, err));
        auto plan = plan_rename({R + u"/a.txt"_s, R + u"/b.txt"_s}, p);
        QCOMPARE(int(plan[0].status), int(RenameStatus::Duplicate));
        QCOMPARE(int(plan[1].status), int(RenameStatus::Duplicate));

        // цель уже существует (и не входит в партию)
        QVERIFY(p.compile(u"a.txt"_s, u"taken.txt"_s, {}, err));
        plan = plan_rename({R + u"/a.txt"_s, R + u"/keep.dat"_s}, p);
        QCOMPARE(int(plan[0].status), int(RenameStatus::Exists));
        QCOMPARE(int(plan[1].status), int(RenameStatus::NoMatch));

        // недопустимые имена
        QVERIFY(p.compile(u"%1.txt"_s, u"x/%1"_s, {}, err));
        QCOMPARE(int(plan_rename({R + u"/a.txt"_s}, p)[0].status), int(RenameStatus::Invalid));
        QVERIFY(p.compile(u"%1"_s, QString(300, u'z'), {}, err));
        QCOMPARE(int(plan_rename({R + u"/a.txt"_s}, p)[0].status), int(RenameStatus::Invalid));
        QVERIFY(p.compile(u"%1"_s, u""_s, {}, err));
        QCOMPARE(int(plan_rename({R + u"/a.txt"_s}, p)[0].status), int(RenameStatus::Invalid));

        // без изменений
        QVERIFY(p.compile(u"%1"_s, u"%1"_s, {}, err));
        int errors = -1;
        QCOMPARE(plan_summary(plan_rename({R + u"/a.txt"_s}, p), &errors), 0);
        QCOMPARE(errors, 0);

        // один файл
        QCOMPARE(int(plan_single(R + u"/a.txt"_s, u"taken.txt"_s)[0].status), int(RenameStatus::Exists));
        QCOMPARE(int(plan_single(R + u"/a.txt"_s, u"new.txt"_s)[0].status), int(RenameStatus::Rename));
        QCOMPARE(int(plan_single(R + u"/a.txt"_s, u"a.txt"_s)[0].status), int(RenameStatus::Unchanged));
    }

    void execute_and_undo() {
        QTemporaryDir tmp;
        const QString R = tmp.path();
        for (int i = 1; i <= 3; ++i) touch(R + u"/IMG_000%1.jpg"_s.arg(i), QByteArray::number(i));
        RenamePattern p;
        QString err;
        QVERIFY(p.compile(u"IMG_%1.jpg"_s, u"Отпуск_%03n.jpg"_s, {}, err));
        const QStringList paths{R + u"/IMG_0001.jpg"_s, R + u"/IMG_0002.jpg"_s, R + u"/IMG_0003.jpg"_s};
        const auto plan = plan_rename(paths, p);
        QCOMPARE(plan_summary(plan), 3);
        const RenameResult r = execute_rename(plan);
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(u"; "_s)));
        QCOMPARE(ls(R), (QStringList{u"Отпуск_001.jpg"_s, u"Отпуск_002.jpg"_s, u"Отпуск_003.jpg"_s}));
        QFile f(R + u"/Отпуск_002.jpg"_s);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("2"));  // содержимое переехало с файлом

        const RenameResult u = undo_rename(r.done);
        QVERIFY2(u.errors.isEmpty(), qPrintable(u.errors.join(u"; "_s)));
        QCOMPARE(ls(R), (QStringList{u"IMG_0001.jpg"_s, u"IMG_0002.jpg"_s, u"IMG_0003.jpg"_s}));
    }

    void swap_and_cycle() {
        QTemporaryDir tmp;
        const QString R = tmp.path();
        touch(R + u"/a"_s, "A");
        touch(R + u"/b"_s, "B");
        touch(R + u"/c"_s, "C");
        // цикл a->b, b->c, c->a (обмен — частный случай)
        RenamePattern p;
        QString err;
        QVERIFY(p.compile(u"%1"_s, u"%1"_s, {}, err));
        std::vector<PlanItem> plan = {
            {R, u"a"_s, u"b"_s, RenameStatus::Rename, {}},
            {R, u"b"_s, u"c"_s, RenameStatus::Rename, {}},
            {R, u"c"_s, u"a"_s, RenameStatus::Rename, {}},
        };
        // plan_rename сочтёт это допустимым: цели заняты файлами этой же партии
        RenameResult r = execute_rename(plan);
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(u"; "_s)));
        auto content = [&](const QString& n) {
            QFile f(R + u"/"_s + n);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        QCOMPARE(content(u"b"_s), QByteArray("A"));
        QCOMPARE(content(u"c"_s), QByteArray("B"));
        QCOMPARE(content(u"a"_s), QByteArray("C"));
        QCOMPARE(ls(R), (QStringList{u"a"_s, u"b"_s, u"c"_s}));  // временных имён не осталось

        // та же партия через plan_rename (подстановка по шаблону regex)
        RenameOptions rx;
        rx.regex = true;
        QVERIFY(p.compile(u"x_(.*)"_s, u"y_\\1"_s, rx, err));
        touch(R + u"/x_1"_s);
        touch(R + u"/y_1"_s);  // занято файлом вне партии
        QCOMPARE(int(plan_rename({R + u"/x_1"_s}, p)[0].status), int(RenameStatus::Exists));
    }

    void directory_with_contents() {
        QTemporaryDir tmp;
        const QString R = tmp.path();
        QVERIFY(QDir(R).mkpath(u"photos/2026"_s));
        touch(R + u"/photos/2026/img.jpg"_s);
        // в партии и каталог, и файл внутри него
        std::vector<PlanItem> plan = {
            {R, u"photos"_s, u"Фото"_s, RenameStatus::Rename, {}},
            {R + u"/photos/2026"_s, u"img.jpg"_s, u"pic.jpg"_s, RenameStatus::Rename, {}},
        };
        RenameResult r = execute_rename(plan);
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(u"; "_s)));
        QVERIFY(QFile::exists(R + u"/Фото/2026/pic.jpg"_s));
        RenameResult u = undo_rename(r.done);
        QVERIFY2(u.errors.isEmpty(), qPrintable(u.errors.join(u"; "_s)));
        QVERIFY(QFile::exists(R + u"/photos/2026/img.jpg"_s));
    }

    void no_permission_and_noreplace() {
        if (geteuid() == 0) QSKIP("от root права не ограничивают");
        QTemporaryDir tmp;
        const QString R = tmp.path();
        QVERIFY(QDir(R).mkpath(u"ro"_s));
        touch(R + u"/ro/f.txt"_s);
        touch(R + u"/ok.txt"_s);
        chmod(QFile::encodeName(R + u"/ro"_s).constData(), 0555);
        std::vector<PlanItem> plan = {
            {R + u"/ro"_s, u"f.txt"_s, u"g.txt"_s, RenameStatus::Rename, {}},
            {R, u"ok.txt"_s, u"done.txt"_s, RenameStatus::Rename, {}},
        };
        RenameResult r = execute_rename(plan);
        chmod(QFile::encodeName(R + u"/ro"_s).constData(), 0755);
        QCOMPARE(int(r.errors.size()), 1);  // ошибка одного файла не мешает остальным
        QCOMPARE(int(r.done.size()), 1);
        QVERIFY(QFile::exists(R + u"/done.txt"_s));

        // существующий файл никогда не перезаписывается, даже если план устарел
        touch(R + u"/x"_s, "X");
        touch(R + u"/y"_s, "Y");
        std::vector<PlanItem> stale = {{R, u"x"_s, u"y"_s, RenameStatus::Rename, {}}};
        r = execute_rename(stale);
        QCOMPARE(int(r.errors.size()), 1);
        QFile f(R + u"/y"_s);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("Y"));
    }
};

QTEST_GUILESS_MAIN(TestRename)
#include "test_rename.moc"
