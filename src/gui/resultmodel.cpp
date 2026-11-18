#include "resultmodel.h"

#include "client.h"
#include "icons.h"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>

#include <QDateTime>
#include <QFont>
#include <QFile>
#include <QLocale>
#include <QMimeData>
#include <QUrl>

namespace bx {
namespace {

QString user_name(quint32 uid) {
    static QHash<quint32, QString> cache;
    auto it = cache.constFind(uid);
    if (it != cache.constEnd()) return *it;
    passwd pw{}, *res = nullptr;
    char buf[4096];
    const QString n = getpwuid_r(uid, &pw, buf, sizeof buf, &res) == 0 && res ? QString::fromLocal8Bit(pw.pw_name)
                                                                              : QString::number(uid);
    cache.insert(uid, n);
    return n;
}

QString group_name(quint32 gid) {
    static QHash<quint32, QString> cache;
    auto it = cache.constFind(gid);
    if (it != cache.constEnd()) return *it;
    group gr{}, *res = nullptr;
    char buf[4096];
    const QString n = getgrgid_r(gid, &gr, buf, sizeof buf, &res) == 0 && res ? QString::fromLocal8Bit(gr.gr_name)
                                                                              : QString::number(gid);
    cache.insert(gid, n);
    return n;
}

QString perm_string(quint32 m) {
    QString s(10, QLatin1Char('-'));
    if (S_ISDIR(m)) s[0] = QLatin1Char('d');
    else if (S_ISLNK(m)) s[0] = QLatin1Char('l');
    else if (S_ISSOCK(m)) s[0] = QLatin1Char('s');
    else if (S_ISFIFO(m)) s[0] = QLatin1Char('p');
    else if (S_ISCHR(m)) s[0] = QLatin1Char('c');
    else if (S_ISBLK(m)) s[0] = QLatin1Char('b');
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        if (m & (0400 >> i)) s[i + 1] = QLatin1Char(rwx[i]);
    if (m & S_ISUID) s[3] = (m & S_IXUSR) ? QLatin1Char('s') : QLatin1Char('S');
    if (m & S_ISGID) s[6] = (m & S_IXGRP) ? QLatin1Char('s') : QLatin1Char('S');
    if (m & S_ISVTX) s[9] = (m & S_IXOTH) ? QLatin1Char('t') : QLatin1Char('T');
    return s;
}

QString date_text(qint64 t) {
    return t > 0 ? QDateTime::fromSecsSinceEpoch(t).toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
}

QString size_text(qint64 n) { return QLocale().formattedDataSize(n, 1, QLocale::DataSizeTraditionalFormat); }

}  // namespace

ResultModel::ResultModel(Client* client, QObject* parent) : QAbstractTableModel(parent), client_(client) {
    connect(client_, &Client::rows, this, &ResultModel::onRows);
}

void ResultModel::setResult(qint64 id, qint64 total) {
    beginResetModel();
    id_ = id;
    total_ = std::min<qint64>(total, INT_MAX);
    pages_.clear();
    lru_.clear();
    pending_.clear();
    stale_.clear();
    endResetModel();
}

void ResultModel::refreshResult(qint64 id, qint64 total) {
    total = std::min<qint64>(total, INT_MAX);
    id_ = id;
    pending_.clear();
    if (total > total_) {
        beginInsertRows({}, int(total_), int(total) - 1);
        total_ = total;
        endInsertRows();
    } else if (total < total_) {
        beginRemoveRows({}, int(total), int(total_) - 1);
        total_ = total;
        endRemoveRows();
    }
    // Последние просмотренные страницы оставить как «устаревшие» и перезапросить, остальные забыть.
    const QList<int> keep = lru_.mid(std::max<qsizetype>(0, lru_.size() - 16));
    for (auto it = pages_.begin(); it != pages_.end();)
        it = keep.contains(it.key()) && qint64(it.key()) * kPage < total_ ? std::next(it) : pages_.erase(it);
    lru_.clear();
    stale_.clear();
    for (int p : keep)
        if (pages_.contains(p)) {
            lru_.append(p);
            stale_.insert(p);
            pending_.insert(p);
            client_->requestRows(id_, qint64(p) * kPage, kPage);
        }
}

void ResultModel::clear() { setResult(-1, 0); }

int ResultModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(total_); }
int ResultModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

const ResultModel::Row* ResultModel::row(int r) const {
    const int page = r / kPage;
    auto it = pages_.find(page);
    if (it == pages_.end()) {
        if (!pending_.contains(page)) {
            pending_.insert(page);
            client_->requestRows(id_, qint64(page) * kPage, kPage);
        }
        return nullptr;
    }
    if (lru_.isEmpty() || lru_.last() != page) {
        lru_.removeOne(page);
        lru_.append(page);
    }
    const int k = r % kPage;
    return k < it->size() ? &(*it)[k] : nullptr;
}

void ResultModel::onRows(qint64 id, qint64 from, const QCborArray& rows) {
    if (id != id_) return;
    const int page = int(from / kPage);
    pending_.remove(page);
    stale_.remove(page);
    QVector<Row> v;
    v.reserve(rows.size());
    for (const QCborValue& rv : rows) {
        const QCborArray a = rv.toArray();
        Row x;
        x.name = a.at(0).toString();
        x.dir = a.at(1).toString();
        x.size = a.at(2).toInteger();
        x.mtime = a.at(3).toInteger();
        x.isdir = a.at(4).toBool();
        x.btime = a.at(5).toInteger();  // старый демон этих полей не присылает — будут нули
        x.mode = quint32(a.at(6).toInteger());
        x.uid = quint32(a.at(7).toInteger());
        x.gid = quint32(a.at(8).toInteger());
        v.push_back(std::move(x));
    }
    pages_.insert(page, std::move(v));
    lru_.append(page);
    while (lru_.size() > 200) pages_.remove(lru_.takeFirst());  // ~50 тыс. строк в памяти
    const int first = page * kPage;
    const int last = std::min<int>(first + kPage, int(total_)) - 1;
    if (last >= first) {
        emit dataChanged(index(first, 0), index(last, ColumnCount - 1));
        emit rowsLoaded(first, last);
    }
}

void ResultModel::loadStat(const Row& x) const {
    struct statx sx {};
    const QByteArray p = QFile::encodeName(x.dir == QLatin1String("/") ? QLatin1Char('/') + x.name
                                                                     : x.dir + QLatin1Char('/') + x.name);
    if (statx(AT_FDCWD, p.constData(), AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC, STATX_ATIME | STATX_BLOCKS, &sx) == 0) {
        x.atime = (sx.stx_mask & STATX_ATIME) ? qint64(sx.stx_atime.tv_sec) : 0;
        x.disk = (sx.stx_mask & STATX_BLOCKS) ? qint64(sx.stx_blocks) * 512 : 0;
    } else {
        x.atime = x.disk = 0;  // нет доступа или файла уже нет
    }
}

QString ResultModel::fullPath(int r) const {
    const Row* x = row(r);
    if (!x || x->name.isEmpty()) return {};
    return x->dir == QLatin1String("/") ? QLatin1Char('/') + x->name : x->dir + QLatin1Char('/') + x->name;
}

QVariant ResultModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid()) return {};
    const Row* x = row(idx.row());
    if (!x) return role == Qt::DisplayRole && idx.column() == Name ? QStringLiteral("…") : QVariant();
    switch (role) {
        case Qt::DisplayRole:
            switch (idx.column()) {
                case Name: return x->name;
                case Path: return x->dir;
                case FullPath: return fullPath(idx.row());
                case Size: return x->isdir ? QString() : size_text(x->size);
                case Modified: return date_text(x->mtime);
                case Created: return date_text(x->btime);
                case Extension: {
                    const qsizetype dot = x->name.lastIndexOf(QLatin1Char('.'));
                    return x->isdir || dot <= 0 ? QString() : x->name.mid(dot + 1);
                }
                case Type: return x->name.isEmpty() ? QString() : type_for(x->name, x->isdir);
                case Owner: return x->mode ? user_name(x->uid) : QString();
                case Group: return x->mode ? group_name(x->gid) : QString();
                case Permissions: return x->mode ? perm_string(x->mode) : QString();
                case Accessed:
                    if (x->atime < 0) loadStat(*x);
                    return date_text(x->atime);
                case DiskSize:
                    if (x->disk < 0) loadStat(*x);
                    return x->isdir ? QString() : size_text(x->disk);
            }
            break;
        case Qt::DecorationRole:
            if (idx.column() == Name && !x->name.isEmpty()) return icon_for(x->name, x->isdir);
            break;
        case Qt::TextAlignmentRole:
            if (idx.column() == Size || idx.column() == DiskSize) return int(Qt::AlignRight | Qt::AlignVCenter);
            break;
        case Qt::FontRole:
            if (idx.column() == Permissions) {
                static const QFont mono(QStringLiteral("monospace"));
                return mono;
            }
            break;
        case Qt::ToolTipRole:
        case FullPathRole:
            return fullPath(idx.row());
        case IsDirRole:
            return x->isdir;
        case DirRole:
            return x->dir;
        case MtimeRole:
            return x->mtime;
        case NameRole:
            return x->name;
    }
    return {};
}

QString ResultModel::columnTitle(int c) {
    switch (c) {
        case Name: return tr("Имя");
        case Path: return tr("Путь");
        case Size: return tr("Размер");
        case Modified: return tr("Изменён");
        case Created: return tr("Создан");
        case Extension: return tr("Расширение");
        case Type: return tr("Тип");
        case Owner: return tr("Владелец");
        case Group: return tr("Группа");
        case Permissions: return tr("Права");
        case Accessed: return tr("Открыт");
        case DiskSize: return tr("На диске");
        case FullPath: return tr("Полный путь");
    }
    return {};
}

QString ResultModel::sortKey(int c) {
    switch (c) {
        case Name: return QStringLiteral("name");
        case Path:
        case FullPath: return QStringLiteral("path");
        case Size: return QStringLiteral("size");
        case Modified: return QStringLiteral("mtime");
        case Created: return QStringLiteral("btime");
        case Extension:
        case Type: return QStringLiteral("ext");
        case Owner: return QStringLiteral("owner");
        case Group: return QStringLiteral("group");
        case Permissions: return QStringLiteral("mode");
    }
    return {};
}

bool ResultModel::sortable(int c) { return !sortKey(c).isEmpty(); }

QVariant ResultModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal) return {};
    if (role == Qt::DisplayRole) return columnTitle(section);
    if (role == Qt::ToolTipRole && !sortable(section))
        return tr("Читается с диска для видимых строк; сортировка по этому столбцу недоступна");
    return {};
}

Qt::ItemFlags ResultModel::flags(const QModelIndex& idx) const {
    return idx.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled : Qt::NoItemFlags;
}

QStringList ResultModel::mimeTypes() const { return {QStringLiteral("text/uri-list")}; }

QMimeData* ResultModel::mimeData(const QModelIndexList& indexes) const {
    QList<QUrl> urls;
    QSet<int> seen;
    for (const QModelIndex& i : indexes)
        if (!seen.contains(i.row())) {
            seen.insert(i.row());
            const QString p = fullPath(i.row());
            if (!p.isEmpty()) urls << QUrl::fromLocalFile(p);
        }
    auto* md = new QMimeData;
    md->setUrls(urls);
    return md;
}

void ResultModel::sort(int column, Qt::SortOrder order) {
    if (column < 0 || column >= ColumnCount) return;
    if (!sortable(column)) {
        emit sortUnavailable(column);
        return;
    }
    emit sortRequested(sortKey(column), order == Qt::DescendingOrder);
}

}  // namespace bx
