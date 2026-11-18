#pragma once
// Модель результатов: строк столько, сколько нашёл демон; данные подгружаются страницами
// по мере прокрутки (видимые строки), поэтому миллион результатов не тормозит.

#include <QAbstractTableModel>
#include <QCborArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QVector>

namespace bx {

class Client;

class ResultModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Path, Size, Modified, ColumnCount };
    enum Role { FullPathRole = Qt::UserRole + 1, IsDirRole, DirRole };

    explicit ResultModel(Client* client, QObject* parent = nullptr);

    // Новый результат: полный сброс модели (новый запрос пользователя).
    void setResult(qint64 id, qint64 total);
    // Фоновое обновление того же запроса: без сброса — старые строки видны, пока не придут новые.
    void refreshResult(qint64 id, qint64 total);
    void clear();
    qint64 resultId() const { return id_; }

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    void sort(int column, Qt::SortOrder order) override;

    QString fullPath(int row) const;

signals:
    void sortRequested(const QString& key, bool desc);
    void rowsLoaded(int first, int last);  // строки получены от демона

private:
    struct Row {
        QString name, dir;
        qint64 size = 0, mtime = 0;
        bool isdir = false;
    };
    static constexpr int kPage = 256;
    void onRows(qint64 id, qint64 from, const QCborArray& rows);
    const Row* row(int r) const;  // nullptr — ещё не загружена (запрос отправлен)

    Client* client_;
    qint64 id_ = -1;
    qint64 total_ = 0;
    mutable QHash<int, QVector<Row>> pages_;
    mutable QList<int> lru_;
    mutable QSet<int> pending_;
    mutable QSet<int> stale_;  // страницы от предыдущего результата (показываются до обновления)
};

}  // namespace bx
