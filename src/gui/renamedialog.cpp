#include "renamedialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace bx {
namespace {

constexpr int kPreviewLimit = 10000;

void show_errors(QWidget* parent, const RenameResult& r) {
    if (r.errors.isEmpty()) return;
    QMessageBox box(QMessageBox::Warning, QObject::tr("Переименование"),
                    QObject::tr("Переименовано: %1, ошибок: %2.").arg(r.done.size()).arg(r.errors.size()),
                    QMessageBox::Ok, parent);
    box.setDetailedText(r.errors.join(QLatin1Char('\n')));
    box.exec();
}

}  // namespace

RenameDialog::RenameDialog(const QStringList& paths, QWidget* parent) : QDialog(parent), paths_(paths) {
    setWindowTitle(tr("Групповое переименование — %n объект(ов)", nullptr, int(paths.size())));
    auto* lay = new QVBoxLayout(this);

    QStringList names;
    for (const QString& p : paths) names << p.mid(p.lastIndexOf(QLatin1Char('/')) + 1);
    const QString fmt = suggest_old_format(names);

    auto* form = new QFormLayout;
    old_ = new QLineEdit(fmt, this);
    new_ = new QLineEdit(fmt, this);
    old_->setToolTip(tr("%1…%9 — переменные части имени (имя должно совпасть целиком), %% — знак процента.\n"
                        "В режиме регулярного выражения — PCRE на всё имя."));
    new_->setToolTip(tr("%1…%9 — подстановка частей (в режиме regex также \\1 и $1),\n"
                        "%n — порядковый номер, %03n — номер с ведущими нулями (001), %% — знак процента."));
    form->addRow(tr("Старый формат:"), old_);
    form->addRow(tr("Новый формат:"), new_);
    lay->addLayout(form);

    auto* opts = new QHBoxLayout;
    regex_ = new QCheckBox(tr("Регулярное выражение"), this);
    case_ = new QCheckBox(tr("С учётом регистра"), this);
    QSettings s;
    regex_->setChecked(s.value(QStringLiteral("rename/regex")).toBool());
    case_->setChecked(s.value(QStringLiteral("rename/case")).toBool());
    auto* help = new QLabel(tr("<small>%1…%9 — части имени · %n, %03n — номер · %% — знак %</small>"), this);
    opts->addWidget(regex_);
    opts->addWidget(case_);
    opts->addStretch(1);
    opts->addWidget(help);
    lay->addLayout(opts);

    preview_ = new QTreeWidget(this);
    preview_->setRootIsDecorated(false);
    preview_->setUniformRowHeights(true);
    preview_->setAlternatingRowColors(true);
    preview_->setTextElideMode(Qt::ElideMiddle);  // у длинных путей видна конечная папка
    preview_->setHeaderLabels({tr("Было"), tr("Станет"), tr("Папка"), tr("Статус")});
    preview_->header()->setStretchLastSection(true);
    preview_->setColumnWidth(0, 260);
    preview_->setColumnWidth(1, 260);
    preview_->setColumnWidth(2, 280);
    preview_->setColumnWidth(3, 200);
    lay->addWidget(preview_, 1);

    summary_ = new QLabel(this);
    lay->addWidget(summary_);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = bb->button(QDialogButtonBox::Ok);
    ok_->setText(tr("Переименовать"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);

    timer_.setSingleShot(true);
    timer_.setInterval(80);
    connect(&timer_, &QTimer::timeout, this, &RenameDialog::updatePreview);
    for (QLineEdit* e : {old_, new_}) connect(e, &QLineEdit::textChanged, &timer_, qOverload<>(&QTimer::start));
    for (QCheckBox* c : {regex_, case_}) connect(c, &QCheckBox::toggled, &timer_, qOverload<>(&QTimer::start));

    resize(1060, 560);
    updatePreview();
    new_->setFocus();
    new_->selectAll();
}

void RenameDialog::updatePreview() {
    RenamePattern p;
    QString err;
    RenameOptions o;
    o.regex = regex_->isChecked();
    o.case_sensitive = case_->isChecked();
    preview_->clear();
    if (!p.compile(old_->text(), new_->text(), o, err)) {
        plan_.clear();
        summary_->setText(QStringLiteral("<span style='color:#d33'>%1</span>").arg(err.toHtmlEscaped()));
        ok_->setEnabled(false);
        return;
    }
    plan_ = plan_rename(paths_, p);
    int errors = 0, nomatch = 0;
    const int n = plan_summary(plan_, &errors);
    QList<QTreeWidgetItem*> items;
    const QBrush red(QColor(0xd3, 0x33, 0x33)), grey(palette().color(QPalette::Disabled, QPalette::Text));
    for (size_t k = 0; k < plan_.size() && k < size_t(kPreviewLimit); ++k) {
        const PlanItem& it = plan_[k];
        nomatch += it.status == RenameStatus::NoMatch;
        auto* w = new QTreeWidgetItem({it.old_name, it.new_name, it.dir, status_text(it)});
        if (it.is_error())
            for (int c = 0; c < 4; ++c) w->setForeground(c, red);
        else if (it.status != RenameStatus::Rename)
            for (int c = 0; c < 4; ++c) w->setForeground(c, grey);
        items << w;
    }
    preview_->addTopLevelItems(items);
    QString text = tr("Будет переименовано: %1").arg(n);
    if (nomatch) text += tr(" · не подходят под шаблон: %1").arg(nomatch);
    if (errors) text += tr(" · <span style='color:#d33'>ошибок: %1 — исправьте формат</span>").arg(errors);
    summary_->setText(text);
    ok_->setEnabled(n > 0 && errors == 0);
}

void RenameDialog::accept() {
    timer_.stop();
    updatePreview();  // проверить ещё раз: на диске могло что-то измениться
    int errors = 0;
    if (plan_summary(plan_, &errors) == 0 || errors) return;
    QSettings s;
    s.setValue(QStringLiteral("rename/regex"), regex_->isChecked());
    s.setValue(QStringLiteral("rename/case"), case_->isChecked());
    result_ = execute_rename(plan_);
    show_errors(this, result_);
    QDialog::accept();
}

bool rename_single(QWidget* parent, const QString& path, bool is_dir, RenameResult& out) {
    const QString old = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    QDialog d(parent);
    d.setWindowTitle(is_dir ? QObject::tr("Переименовать папку") : QObject::tr("Переименовать файл"));
    auto* lay = new QVBoxLayout(&d);
    lay->addWidget(new QLabel(QObject::tr("Новое имя для «%1»:").arg(old), &d));
    auto* edit = new QLineEdit(old, &d);
    lay->addWidget(edit);
    auto* status = new QLabel(&d);
    lay->addWidget(status);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    QPushButton* ok = bb->button(QDialogButtonBox::Ok);
    ok->setText(QObject::tr("Переименовать"));
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    lay->addWidget(bb);

    auto check = [&] {
        const auto plan = plan_single(path, edit->text());
        const PlanItem& it = plan[0];
        status->setText(it.is_error() ? QStringLiteral("<span style='color:#d33'>%1</span>").arg(status_text(it))
                                      : QString());
        ok->setEnabled(it.status == RenameStatus::Rename);
    };
    QObject::connect(edit, &QLineEdit::textChanged, &d, check);
    check();
    // Выделить имя без расширения (как в файловых менеджерах); у папок и «.bashrc» — всё имя.
    const qsizetype dot = old.lastIndexOf(QLatin1Char('.'));
    edit->setSelection(0, (!is_dir && dot > 0) ? dot : old.size());
    d.resize(480, d.sizeHint().height());

    if (d.exec() != QDialog::Accepted) return false;
    const auto plan = plan_single(path, edit->text());
    if (plan[0].status != RenameStatus::Rename) return false;
    out = execute_rename(plan);
    show_errors(parent, out);
    return !out.done.empty();
}

}  // namespace bx
