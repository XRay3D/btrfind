// Тесты удаления: корзина и восстановление, безвозвратное удаление, защита точек монтирования и ссылок.
// Корзина изолирована: XDG_DATA_HOME и временные файлы — в каталоге сборки (та же ФС, что $HOME).
#include "remove.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace bx;

namespace {
void touch(const QString& p) {
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
}
}  // namespace

class TestRemove : public QObject {
    Q_OBJECT
    QTemporaryDir base_{QDir::currentPath() + u"/test-remove-XXXXXX"_s};

private slots:
    void initTestCase() {
        QVERIFY(base_.isValid());
        QVERIFY(QDir().mkpath(base_.path() + u"/data"_s));
        qputenv("XDG_DATA_HOME", QFile::encodeName(base_.path() + u"/data"_s));  // своя корзина
    }

    void top_level() {
        QCOMPARE(top_level_paths({u"/a/b"_s, u"/a"_s, u"/a/b/c"_s, u"/ab"_s, u"/a"_s}),
                 (QStringList{u"/a"_s, u"/ab"_s}));
    }

    void mount_guard() {
        std::vector<MountInfo> mounts(2);
        mounts[0].mountpoint = "/";
        mounts[1].mountpoint = "/x/home";
        QVERIFY(!removal_problem(u"/x/home"_s, mounts).isEmpty());   // сама точка монтирования
        QVERIFY(!removal_problem(u"/x"_s, mounts).isEmpty());        // внутри смонтировано
        QVERIFY(removal_problem(u"/x/other"_s, mounts).isEmpty());
        QVERIFY(!removal_problem(u"/"_s, mounts).isEmpty());
        QVERIFY(!removal_problem(u"relative"_s, mounts).isEmpty());
    }

    void trash_and_restore() {
        QTemporaryDir tmp(base_.path() + u"/work-XXXXXX"_s);
        const QString R = tmp.path();
        touch(R + u"/a.txt"_s);
        QVERIFY(QDir(R).mkpath(u"dir/sub"_s));
        touch(R + u"/dir/sub/f.txt"_s);

        // папка и файл внутри неё: удаляется только папка (вместе с содержимым)
        RemoveResult r = move_to_trash({R + u"/a.txt"_s, R + u"/dir"_s, R + u"/dir/sub/f.txt"_s}, {});
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(u"; "_s)));
        QCOMPARE(int(r.trashed.size()), 2);
        QVERIFY(!QFile::exists(R + u"/a.txt"_s));
        QVERIFY(!QFile::exists(R + u"/dir"_s));
        for (const auto& t : r.trashed) {
            QVERIFY(t.in_trash.startsWith(base_.path() + u"/data/Trash/files/"_s));
            QVERIFY(QFile::exists(t.in_trash));
        }

        const RemoveResult u = restore_from_trash(r.trashed);
        QVERIFY2(u.errors.isEmpty(), qPrintable(u.errors.join(u"; "_s)));
        QVERIFY(QFile::exists(R + u"/a.txt"_s));
        QVERIFY(QFile::exists(R + u"/dir/sub/f.txt"_s));
        QVERIFY(QDir(base_.path() + u"/data/Trash/info"_s).isEmpty(QDir::Files));  // .trashinfo убраны

        // восстановление не перезаписывает новый файл с тем же именем
        r = move_to_trash({R + u"/a.txt"_s}, {});
        QCOMPARE(int(r.trashed.size()), 1);
        touch(R + u"/a.txt"_s);
        QCOMPARE(int(restore_from_trash(r.trashed).errors.size()), 1);
    }

    void permanent_and_symlinks() {
        QTemporaryDir tmp(base_.path() + u"/work-XXXXXX"_s);
        const QString R = tmp.path();
        QVERIFY(QDir(R).mkpath(u"keep"_s));
        touch(R + u"/keep/precious.txt"_s);
        QVERIFY(QDir(R).mkpath(u"tree/a/b"_s));
        touch(R + u"/tree/a/b/f.txt"_s);
        // ссылка на каталог — и снаружи, и внутри удаляемого дерева
        QVERIFY(QFile::link(R + u"/keep"_s, R + u"/link"_s));
        QVERIFY(QFile::link(R + u"/keep"_s, R + u"/tree/a/inner-link"_s));

        RemoveResult r = delete_permanently({R + u"/tree"_s, R + u"/link"_s}, {});
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(u"; "_s)));
        QVERIFY(!QFile::exists(R + u"/tree"_s));
        QVERIFY(!QFileInfo(R + u"/link"_s).isSymLink());
        QVERIFY(QFile::exists(R + u"/keep/precious.txt"_s));  // по ссылкам не ходили

        // защищённый путь не удаляется
        std::vector<MountInfo> mounts(1);
        mounts[0].mountpoint = (R + u"/keep"_s).toStdString();
        r = delete_permanently({R + u"/keep"_s}, mounts);
        QCOMPARE(int(r.errors.size()), 1);
        QVERIFY(QFile::exists(R + u"/keep/precious.txt"_s));
    }
};

QTEST_GUILESS_MAIN(TestRemove)
#include "test_remove.moc"
