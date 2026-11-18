#pragma once

#include "client.h"
#include "history.h"
#include "rename.h"

#include <QCborMap>
#include <QMainWindow>
#include <QModelIndexList>
#include <QSet>
#include <QTimer>

class QAbstractItemView;
class QAction;
class QActionGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QStackedWidget;
class QTableView;
class QToolButton;

namespace bx {

class ResultModel;
class ThumbDelegate;
class ThumbnailProvider;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Client* client, QWidget* parent = nullptr);

    void setFolder(const QString& dir);  // ограничить поиск папкой ("" — везде)
    void setQuery(const QString& q);
    void bringToFront();
    void setVisibleColumns(const QList<int>& columns);  // остальные скрыть

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void buildMenus();
    void runSearch(bool refresh = false);
    void onResult(qint64 id, qint64 total, double ms, const QString& error, const QCborMap& m);
    void onStatus(const QCborMap& m);
    void showContextMenu(QAbstractItemView* v, const QPoint& pos);
    QStringList selectedPaths() const;
    void openSelected();
    void openFolders();
    void copyPaths();
    void copyNames();
    void copyFiles();
    QString filterText() const;
    void restoreSelection(int first, int last);
    void renameSelected();
    void undoRename();
    void applyRenamed(const std::vector<RenameDone>& done);  // перенести выделение на новые пути

    // Вид: 0 — таблица, 1…5 — плитка эскизов разного размера
    void setViewMode(int mode);
    QAbstractItemView* currentView() const;
    void fillColumnsMenu(QMenu* m);
    void fillSortMenu(QMenu* m);
    // История
    void commitHistory();
    void fillHistoryMenu(QMenu* m);
    void showHistoryPopup();
    // Строка состояния: всего / выделено
    void updateCountLabel();
    void requestSelectionSum();

    Client* c_;
    ResultModel* model_;
    QLineEdit* edit_;
    QComboBox* filter_;
    QToolButton* folderChip_;
    QAction *aCase_, *aRegex_, *aPath_, *aNoHistory_;
    QStackedWidget* stack_;
    QTableView* view_;
    QListView* grid_;
    ThumbnailProvider* thumbs_;
    ThumbDelegate* tdel_;
    QActionGroup* viewGroup_;
    QList<QAction*> viewActs_;
    QLabel *count_, *daemon_;
    QTimer debounce_, statusTimer_, refreshTimer_, historyTimer_, sumTimer_;
    SearchHistory history_;
    QSet<QString> selPaths_;  // выделение «по файлам», а не по номерам строк
    QString curPath_;
    bool restoring_ = false;
    std::vector<RenameDone> lastRename_;  // последняя партия — для отмены
    QString folder_;
    QString sort_ = QStringLiteral("name");
    bool desc_ = false;
    int sortCol_ = 0;
    bool sortDescCol_ = false;
    qint64 pendingId_ = -1;
    bool refresh_ = false;
    bool allReady_ = false;
    int viewMode_ = -1;
    int wheelAcc_ = 0;
    // итоги текущего результата и выделения
    qint64 total_ = 0, totalBytes_ = 0, totalFiles_ = 0, totalDirs_ = 0;
    double ms_ = 0;
    QString error_;
    qint64 sumTag_ = 0;
    qint64 selCount_ = 0, selBytes_ = -1, selFiles_ = 0, selDirs_ = 0;
};

}  // namespace bx
