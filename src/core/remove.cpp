#include "remove.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

using namespace Qt::StringLiterals;

namespace bx {
namespace {

QString err_text(const QString& path) {
    return u"%1: %2"_s.arg(path, QString::fromLocal8Bit(strerror(errno)));
}

bool is_symlink(const QString& path) {
    struct stat st {};
    return lstat(QFile::encodeName(path).constData(), &st) == 0 && S_ISLNK(st.st_mode);
}

}  // namespace

QStringList top_level_paths(const QStringList& paths) {
    QStringList sorted = paths;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    QStringList out;
    for (const QString& p : sorted)
        if (out.isEmpty() || !path_within(p.toStdString(), out.last().toStdString())) out << p;
    return out;
}

QString removal_problem(const QString& path, const std::vector<MountInfo>& mounts) {
    if (path.isEmpty() || path == u"/"_s || !path.startsWith(u'/')) return u"недопустимый путь"_s;
    if (is_symlink(path)) return {};  // удаляется сама ссылка
    const std::string p = path.toStdString();
    for (const MountInfo& m : mounts) {
        if (m.mountpoint == p) return u"это точка монтирования"_s;
        if (path_within(m.mountpoint, p))
            return u"внутри смонтирована ФС «%1»"_s.arg(QString::fromStdString(m.mountpoint));
    }
    return {};
}

RemoveResult move_to_trash(const QStringList& paths, const std::vector<MountInfo>& mounts) {
    RemoveResult r;
    for (const QString& p : top_level_paths(paths)) {
        if (const QString why = removal_problem(p, mounts); !why.isEmpty()) {
            r.errors << u"%1: %2"_s.arg(p, why);
            continue;
        }
        QFile f(p);
        if (f.moveToTrash())  // после успеха fileName() — путь внутри корзины
            r.trashed.push_back({p, f.fileName()});
        else if (QStorageInfo(p).rootPath() != QStorageInfo(QDir::homePath()).rootPath())
            r.errors << u"%1: корзина недоступна — файл не на том же томе, что домашняя папка (удалите безвозвратно, Shift+Del)"_s
                            .arg(p);
        else
            r.errors << u"%1: не удалось переместить в корзину (%2)"_s.arg(p, f.errorString());
    }
    return r;
}

RemoveResult delete_permanently(const QStringList& paths, const std::vector<MountInfo>& mounts) {
    RemoveResult r;
    for (const QString& p : top_level_paths(paths)) {
        if (const QString why = removal_problem(p, mounts); !why.isEmpty()) {
            r.errors << u"%1: %2"_s.arg(p, why);
            continue;
        }
        const QByteArray enc = QFile::encodeName(p);
        struct stat st {};
        if (lstat(enc.constData(), &st) < 0) {
            r.errors << err_text(p);
            continue;
        }
        bool ok;
        if (S_ISDIR(st.st_mode)) {
            // QDir::removeRecursively не следует по ссылкам внутри: ссылки удаляются как файлы.
            ok = QDir(p).removeRecursively();
            if (!ok) r.errors << u"%1: удалено не полностью (нет прав на часть содержимого?)"_s.arg(p);
        } else {
            ok = ::unlink(enc.constData()) == 0;
            if (!ok) r.errors << err_text(p);
        }
        if (ok) r.removed << p;
    }
    return r;
}

RemoveResult restore_from_trash(const std::vector<TrashedItem>& items) {
    RemoveResult r;
    // В обратном порядке: последним удалённое — первым на место.
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        const QByteArray from = QFile::encodeName(it->in_trash), to = QFile::encodeName(it->original);
        if (renameat2(AT_FDCWD, from.constData(), AT_FDCWD, to.constData(), RENAME_NOREPLACE) < 0) {
            r.errors << (errno == EEXIST ? u"%1: на этом месте уже есть файл"_s.arg(it->original) : err_text(it->original));
            continue;
        }
        // …/Trash/files/имя -> …/Trash/info/имя.trashinfo
        const QFileInfo fi(it->in_trash);
        QFile::remove(fi.dir().absoluteFilePath(u"../info/%1.trashinfo"_s.arg(fi.fileName())));
        r.removed << it->original;
    }
    return r;
}

}  // namespace bx
