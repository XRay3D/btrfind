#include "resultmodel.h"

#include "client.h"
#include "icons.h"

#include <QDateTime>
#include <QLocale>
#include <QMimeData>
#include <QUrl>

namespace bx {

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
        v.push_back({a.at(0).toString(), a.at(1).toString(), a.at(2).toInteger(), a.at(3).toInteger(), a.at(4).toBool()});
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
                case Size: return x->isdir ? QString() : QLocale().formattedDataSize(x->size, 1, QLocale::DataSizeTraditionalFormat);
                case Modified:
                    return x->mtime ? QDateTime::fromSecsSinceEpoch(x->mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                    : QString();
            }
            break;
        case Qt::DecorationRole:
            if (idx.column() == Name && !x->name.isEmpty()) return icon_for(x->name, x->isdir);
            break;
        case Qt::TextAlignmentRole:
            if (idx.column() == Size) return int(Qt::AlignRight | Qt::AlignVCenter);
            break;
        case Qt::ToolTipRole:
        case FullPathRole:
            return fullPath(idx.row());
        case IsDirRole:
            return x->isdir;
        case DirRole:
            return x->dir;
    }
    return {};
}

QVariant ResultModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case Name: return tr("Имя");
        case Path: return tr("Путь");
        case Size: return tr("Размер");
        case Modified: return tr("Изменён");
    }
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
    static const char* keys[] = {"name", "path", "size", "mtime"};
    if (column >= 0 && column < ColumnCount) emit sortRequested(QString::fromLatin1(keys[column]), order == Qt::DescendingOrder);
}

}  // namespace bx
