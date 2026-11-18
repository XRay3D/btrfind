#pragma once

#include "client.h"

#include <QCborMap>
#include <QMainWindow>
#include <QModelIndexList>
#include <QSet>
#include <QTimer>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;
class QToolButton;

namespace bx {

class ResultModel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Client* client, QWidget* parent = nullptr);

    void setFolder(const QString& dir);  // ограничить поиск папкой ("" — везде)
    void setQuery(const QString& q);
    void bringToFront();

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void runSearch(bool refresh = false);
    void onResult(qint64 id, qint64 total, double ms, const QString& error);
    void onStatus(const QCborMap& m);
    void showContextMenu(const QPoint& pos);
    QStringList selectedPaths() const;
    void openSelected();
    void openFolders();
    void copyPaths();
    void copyNames();
    void copyFiles();
    QString filterText() const;
    void restoreSelection(int first, int last);

    Client* c_;
    ResultModel* model_;
    QLineEdit* edit_;
    QComboBox* filter_;
    QToolButton *case_, *regex_, *path_, *folderChip_;
    QTableView* view_;
    QLabel *count_, *daemon_;
    QTimer debounce_, statusTimer_, refreshTimer_;
    QSet<QString> selPaths_;  // выделение «по файлам», а не по номерам строк
    QString curPath_;
    bool restoring_ = false;
    QString folder_;
    QString sort_ = QStringLiteral("name");
    bool desc_ = false;
    qint64 pendingId_ = -1;
    bool refresh_ = false;
    int keepScroll_ = 0;
    bool allReady_ = false;
};

}  // namespace bx
