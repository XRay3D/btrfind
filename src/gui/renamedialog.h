#pragma once
// Диалоги переименования: групповое (старый/новый формат с предпросмотром) и одного файла.

#include "rename.h"

#include <QDialog>
#include <QTimer>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace bx {

class RenameDialog : public QDialog {
    Q_OBJECT
public:
    RenameDialog(const QStringList& paths, QWidget* parent = nullptr);
    const RenameResult& result() const { return result_; }

protected:
    void accept() override;

private:
    void updatePreview();

    QStringList paths_;
    QLineEdit *old_, *new_;
    QCheckBox *regex_, *case_;
    QTreeWidget* preview_;
    QLabel* summary_;
    QPushButton* ok_;
    QTimer timer_;
    std::vector<PlanItem> plan_;
    RenameResult result_;
};

// Переименовать один файл. true — переименован (out заполнен).
bool rename_single(QWidget* parent, const QString& path, bool is_dir, RenameResult& out);

}  // namespace bx
