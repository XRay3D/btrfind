#include "mainwindow.h"

#include "resultmodel.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QTableView>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace bx {
namespace {

struct Preset {
    const char* title;
    const char* filter;
};
const Preset kPresets[] = {
    {"Все", ""},
    {"Файлы", "file:"},
    {"Папки", "folder:"},
    {"Документы", "ext:doc;docx;odt;ods;odp;pdf;txt;rtf;xls;xlsx;ppt;pptx;md;csv;djvu;epub;fb2"},
    {"Изображения", "ext:png;jpg;jpeg;gif;bmp;svg;webp;tif;tiff;ico;heic;avif;raw;cr2;nef;xcf;kra;psd"},
    {"Аудио", "ext:mp3;flac;ogg;opus;wav;m4a;aac;wma;ape"},
    {"Видео", "ext:mp4;mkv;avi;mov;webm;wmv;flv;m4v;mpg;mpeg;ts"},
    {"Архивы", "ext:zip;rar;7z;tar;gz;bz2;xz;zst;tgz;iso;cab;rpm;deb"},
    {"Исходники", "ext:c;h;cpp;hpp;cc;cxx;qml;py;rs;go;js;ts;java;kt;cs;sh;cmake;json;xml;yaml;yml;toml"},
};

QString human_count(qint64 n) {
    if (n >= 1000000) return QLocale().toString(n / 1e6, 'f', 2) + QObject::tr(" млн");
    if (n >= 10000) return QLocale().toString(n / 1000) + QObject::tr(" тыс.");
    return QLocale().toString(n);
}

QToolButton* toggle(const QString& text, const QString& tip, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setAutoRaise(true);
    QFont f = b->font();
    f.setBold(true);
    b->setFont(f);
    return b;
}

}  // namespace

MainWindow::MainWindow(Client* client, QWidget* parent) : QMainWindow(parent), c_(client) {
    setWindowTitle(QStringLiteral("btrfind"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("system-search")));
    model_ = new ResultModel(c_, this);

    auto* central = new QWidget(this);
    auto* lay = new QVBoxLayout(central);
    lay->setContentsMargins(6, 6, 6, 0);
    lay->setSpacing(4);

    auto* top = new QHBoxLayout;
    edit_ = new QLineEdit(central);
    edit_->setPlaceholderText(tr("Поиск файлов и папок…  (*.pdf, ext:docx, size:>10mb, dm:today, regex:…)"));
    edit_->setClearButtonEnabled(true);
    edit_->installEventFilter(this);
    folderChip_ = new QToolButton(central);
    folderChip_->setIcon(QIcon::fromTheme(QStringLiteral("folder")));
    folderChip_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    folderChip_->setAutoRaise(true);
    folderChip_->setToolTip(tr("Поиск только в этой папке — нажмите, чтобы искать везде"));
    folderChip_->hide();
    connect(folderChip_, &QToolButton::clicked, this, [this] { setFolder({}); });
    case_ = toggle(QStringLiteral("Aa"), tr("С учётом регистра"), central);
    regex_ = toggle(QStringLiteral(".*"), tr("Регулярное выражение (весь запрос, PCRE2)"), central);
    path_ = toggle(QStringLiteral("/"), tr("Искать по полному пути, а не только по имени"), central);
    filter_ = new QComboBox(central);
    for (const Preset& p : kPresets) filter_->addItem(tr(p.title), QString::fromUtf8(p.filter));
    top->addWidget(folderChip_);
    top->addWidget(edit_, 1);
    top->addWidget(case_);
    top->addWidget(regex_);
    top->addWidget(path_);
    top->addWidget(filter_);
    lay->addLayout(top);

    view_ = new QTableView(central);
    view_->setModel(model_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setShowGrid(false);
    view_->setWordWrap(false);
    view_->setAlternatingRowColors(true);
    view_->setDragEnabled(true);
    view_->setDragDropMode(QAbstractItemView::DragOnly);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->setTextElideMode(Qt::ElideMiddle);
    view_->verticalHeader()->hide();
    view_->verticalHeader()->setDefaultSectionSize(view_->fontMetrics().height() + 6);
    view_->horizontalHeader()->setStretchLastSection(false);
    view_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view_->horizontalHeader()->setHighlightSections(false);
    view_->setColumnWidth(ResultModel::Name, 320);
    view_->setColumnWidth(ResultModel::Path, 480);
    view_->setColumnWidth(ResultModel::Size, 90);
    view_->setColumnWidth(ResultModel::Modified, 130);
    view_->setSortingEnabled(true);
    view_->horizontalHeader()->setSortIndicator(ResultModel::Name, Qt::AscendingOrder);
    view_->installEventFilter(this);
    lay->addWidget(view_, 1);
    setCentralWidget(central);

    count_ = new QLabel(this);
    daemon_ = new QLabel(this);
    statusBar()->addWidget(count_, 1);
    statusBar()->addPermanentWidget(daemon_);

    // Действия с горячими клавишами (работают и из контекстного меню)
    auto act = [this](const QString& text, const QString& icon, const QKeySequence& ks, auto fn) {
        auto* a = new QAction(QIcon::fromTheme(icon), text, this);
        if (!ks.isEmpty()) a->setShortcut(ks);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
        return a;
    };
    act(tr("Найти"), {}, QKeySequence::Find, [this] { edit_->setFocus(); edit_->selectAll(); });
    act(tr("Обновить"), {}, QKeySequence::Refresh, [this] { runSearch(); });
    act(tr("Выход"), {}, QKeySequence::Quit, [this] { close(); });

    // Поиск при вводе
    debounce_.setSingleShot(true);
    debounce_.setInterval(30);
    connect(&debounce_, &QTimer::timeout, this, [this] { runSearch(); });
    connect(edit_, &QLineEdit::textChanged, &debounce_, qOverload<>(&QTimer::start));
    for (QToolButton* b : {case_, regex_, path_}) connect(b, &QToolButton::toggled, this, [this] { runSearch(); });
    connect(filter_, &QComboBox::currentIndexChanged, this, [this] { runSearch(); });
    connect(model_, &ResultModel::sortRequested, this, [this](const QString& key, bool desc) {
        sort_ = key;
        desc_ = desc;
        runSearch();
    });
    connect(view_, &QTableView::doubleClicked, this, [this] { openSelected(); });
    connect(view_, &QTableView::customContextMenuRequested, this, &MainWindow::showContextMenu);

    // Связь с демоном
    connect(c_, &Client::result, this, &MainWindow::onResult);
    connect(c_, &Client::status, this, &MainWindow::onStatus);
    // Индекс изменился — тихо обновить результат, не чаще раза в секунду.
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(1000);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { runSearch(true); });
    connect(c_, &Client::changed, this, [this] {
        if (!refreshTimer_.isActive()) refreshTimer_.start();
    });
    // Выделение следует за файлами при обновлениях результата.
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (restoring_) return;
        if (view_->selectionModel()->selectedRows().size() > 5000) {  // Ctrl+A на огромном результате
            selPaths_.clear();
            return;
        }
        const QStringList ps = selectedPaths();
        selPaths_ = QSet<QString>(ps.begin(), ps.end());
    });
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& cur) {
        if (!restoring_) curPath_ = cur.isValid() ? model_->fullPath(cur.row()) : QString();
    });
    connect(model_, &ResultModel::rowsLoaded, this, &MainWindow::restoreSelection);
    connect(c_, &Client::connected, this, [this] {
        c_->requestStatus();
        runSearch();
    });
    connect(c_, &Client::disconnected, this, [this](const QString&) {
        daemon_->setText(tr("<span style='color:#d33'>Демон btrfindd недоступен</span> — "
                            "<code>sudo systemctl start btrfindd</code>"));
        count_->clear();
    });
    statusTimer_.setInterval(3000);
    connect(&statusTimer_, &QTimer::timeout, c_, &Client::requestStatus);
    statusTimer_.start();

    QSettings s;
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    view_->horizontalHeader()->restoreState(s.value(QStringLiteral("header")).toByteArray());
    case_->setChecked(s.value(QStringLiteral("case")).toBool());
    regex_->setChecked(s.value(QStringLiteral("regex")).toBool());
    path_->setChecked(s.value(QStringLiteral("path")).toBool());
    filter_->setCurrentIndex(s.value(QStringLiteral("filter")).toInt());
    if (!s.contains(QStringLiteral("geometry"))) resize(1100, 700);
    const auto order = view_->horizontalHeader()->sortIndicatorOrder();
    model_->sort(view_->horizontalHeader()->sortIndicatorSection(), order);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s;
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    s.setValue(QStringLiteral("header"), view_->horizontalHeader()->saveState());
    s.setValue(QStringLiteral("case"), case_->isChecked());
    s.setValue(QStringLiteral("regex"), regex_->isChecked());
    s.setValue(QStringLiteral("path"), path_->isChecked());
    s.setValue(QStringLiteral("filter"), filter_->currentIndex());
    e->accept();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    if (e->type() != QEvent::KeyPress) return QMainWindow::eventFilter(o, e);
    auto* k = static_cast<QKeyEvent*>(e);
    if (o == edit_) {
        if (k->key() == Qt::Key_Escape && !edit_->text().isEmpty()) {
            edit_->clear();
            return true;
        }
        if ((k->key() == Qt::Key_Down || k->key() == Qt::Key_PageDown) && model_->rowCount() > 0) {
            view_->setFocus();
            view_->selectRow(0);
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            if (model_->rowCount() > 0) {
                view_->setFocus();
                view_->selectRow(0);
            }
            return true;
        }
    } else if (o == view_) {
        const bool ctrl = k->modifiers() & Qt::ControlModifier, shift = k->modifiers() & Qt::ShiftModifier;
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            ctrl ? openFolders() : openSelected();
            return true;
        }
        if (k->key() == Qt::Key_C && ctrl) {
            shift ? copyPaths() : copyFiles();
            return true;
        }
        if (k->key() == Qt::Key_Up && view_->currentIndex().row() <= 0) {
            edit_->setFocus();
            return true;
        }
        if (!k->text().isEmpty() && k->text().at(0).isPrint() && !ctrl) {  // печать — обратно в строку поиска
            edit_->setFocus();
            edit_->insert(k->text());
            return true;
        }
    }
    return QMainWindow::eventFilter(o, e);
}

QString MainWindow::filterText() const {
    QString f = filter_->currentData().toString();
    if (!folder_.isEmpty() && folder_ != QLatin1String("/")) {
        QString d = folder_;
        d.replace(QLatin1Char('"'), QString());
        f += QStringLiteral(" path:\"%1/\"").arg(d);
    }
    return f.trimmed();
}

void MainWindow::setFolder(const QString& dir) {
    QString d = dir;
    while (d.size() > 1 && d.endsWith(QLatin1Char('/'))) d.chop(1);
    folder_ = d;
    folderChip_->setVisible(!d.isEmpty() && d != QLatin1String("/"));
    folderChip_->setText(d + QStringLiteral("  ✕"));
    runSearch();
}

void MainWindow::setQuery(const QString& q) {
    edit_->setText(q);
    runSearch();
}

void MainWindow::bringToFront() {
    if (isMinimized()) showNormal();
    show();
    raise();
    activateWindow();
    edit_->setFocus();
    edit_->selectAll();
}

void MainWindow::runSearch(bool refresh) {
    if (!c_->isConnected()) return;
    SearchOpts o;
    o.filter = filterText();
    o.case_sensitive = case_->isChecked();
    o.regex = regex_->isChecked();
    o.path = path_->isChecked();
    o.sort = sort_;
    o.desc = desc_;
    refresh_ = refresh;
    pendingId_ = c_->search(edit_->text(), o);
}

void MainWindow::onResult(qint64 id, qint64 total, double ms, const QString& error) {
    if (id != pendingId_) return;  // устаревший ответ
    if (!error.isEmpty()) {
        count_->setText(QStringLiteral("<span style='color:#d33'>%1</span>").arg(error.toHtmlEscaped()));
        return;
    }
    if (refresh_ && model_->resultId() >= 0) {
        model_->refreshResult(id, total);  // без сброса: ни мерцания, ни сброса прокрутки
    } else {
        selPaths_.clear();
        curPath_.clear();
        model_->setResult(id, total);
        view_->scrollToTop();
    }
    count_->setText(tr("%1 объектов · %2 мс").arg(QLocale().toString(total)).arg(ms, 0, 'f', 1));
}

void MainWindow::onStatus(const QCborMap& m) {
    QStringList parts;
    bool all_ready = true, live = true;
    for (const QCborValue& v : m.value("volumes").toArray()) {
        const QCborMap vm = v.toMap();
        const QString state = vm.value("state").toString();
        if (state == QLatin1String("removed")) continue;
        QString s = vm.value("path").toString() + QLatin1Char(' ');
        if (state == QLatin1String("ready")) {
            s += human_count(vm.value("entries").toInteger());
        } else {
            s += state == QLatin1String("scanning")      ? tr("индексация…")
                 : state == QLatin1String("catching-up") ? tr("догон изменений…")
                 : state == QLatin1String("error")       ? tr("ошибка")
                                                         : tr("загрузка…");
            all_ready = false;
        }
        live &= vm.value("live").toBool();
        parts << s;
    }
    QString text = parts.join(QStringLiteral(" · "));
    if (!live) text += tr(" · <span style='color:#c80'>без живых обновлений</span>");
    daemon_->setText(text);
    statusTimer_.setInterval(all_ready ? 30000 : 2000);
    if (all_ready && !allReady_) runSearch(true);  // индексация завершилась — обновить результат
    allReady_ = all_ready;
}

void MainWindow::restoreSelection(int first, int last) {
    if (selPaths_.isEmpty() && curPath_.isEmpty()) return;
    restoring_ = true;
    QItemSelectionModel* sm = view_->selectionModel();
    QItemSelection add, remove;
    const int lastCol = ResultModel::ColumnCount - 1;
    for (int r = first; r <= last; ++r) {
        const QString p = model_->fullPath(r);
        if (p.isEmpty()) continue;
        const bool want = selPaths_.contains(p);
        if (want != sm->isRowSelected(r, {}))
            (want ? add : remove).select(model_->index(r, 0), model_->index(r, lastCol));
        if (p == curPath_ && sm->currentIndex().row() != r)
            sm->setCurrentIndex(model_->index(r, 0), QItemSelectionModel::NoUpdate);
    }
    if (!remove.isEmpty()) sm->select(remove, QItemSelectionModel::Deselect);
    if (!add.isEmpty()) sm->select(add, QItemSelectionModel::Select);
    restoring_ = false;
}

QStringList MainWindow::selectedPaths() const {
    QStringList out;
    for (const QModelIndex& i : view_->selectionModel()->selectedRows()) {
        const QString p = model_->fullPath(i.row());
        if (!p.isEmpty()) out << p;
    }
    return out;
}

void MainWindow::openSelected() {
    const QStringList ps = selectedPaths();
    for (const QString& p : ps.mid(0, 20)) QDesktopServices::openUrl(QUrl::fromLocalFile(p));
}

void MainWindow::openFolders() {
    const QStringList ps = selectedPaths();
    if (ps.isEmpty()) return;
    // Файловый менеджер с выделением файла (Dolphin, Nautilus и др. поддерживают FileManager1).
    QStringList uris;
    for (const QString& p : ps.mid(0, 20)) uris << QUrl::fromLocalFile(p).toString();
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                      QStringLiteral("/org/freedesktop/FileManager1"),
                                                      QStringLiteral("org.freedesktop.FileManager1"),
                                                      QStringLiteral("ShowItems"));
    msg << uris << QString();
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ErrorMessage)
        for (const QString& p : ps.mid(0, 20)) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).path()));
}

void MainWindow::copyPaths() { QApplication::clipboard()->setText(selectedPaths().join(QLatin1Char('\n'))); }

void MainWindow::copyNames() {
    QStringList names;
    for (const QString& p : selectedPaths()) names << QFileInfo(p).fileName();
    QApplication::clipboard()->setText(names.join(QLatin1Char('\n')));
}

void MainWindow::copyFiles() {  // как в файловом менеджере: вставляется копия файлов
    auto* md = new QMimeData;
    QList<QUrl> urls;
    for (const QString& p : selectedPaths()) urls << QUrl::fromLocalFile(p);
    md->setUrls(urls);
    md->setText(selectedPaths().join(QLatin1Char('\n')));
    QApplication::clipboard()->setMimeData(md);
}

void MainWindow::showContextMenu(const QPoint& pos) {
    const QModelIndex idx = view_->indexAt(pos);
    if (!idx.isValid()) return;
    if (!view_->selectionModel()->isRowSelected(idx.row(), {})) view_->selectRow(idx.row());
    const bool isdir = idx.siblingAtColumn(0).data(ResultModel::IsDirRole).toBool();
    const QString dir = idx.siblingAtColumn(0).data(ResultModel::DirRole).toString();
    const QString full = model_->fullPath(idx.row());

    QMenu m(this);
    m.addAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("Открыть"), QKeySequence(Qt::Key_Return), this,
                &MainWindow::openSelected);
    m.addAction(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Показать в папке"),
                QKeySequence(Qt::CTRL | Qt::Key_Return), this, &MainWindow::openFolders);
    m.addSeparator();
    m.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Копировать"), QKeySequence::Copy, this,
                &MainWindow::copyFiles);
    m.addAction(QIcon::fromTheme(QStringLiteral("edit-copy-path")), tr("Копировать полный путь"),
                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this, &MainWindow::copyPaths);
    m.addAction(tr("Копировать имя"), this, &MainWindow::copyNames);
    m.addSeparator();
    m.addAction(QIcon::fromTheme(QStringLiteral("system-search")), tr("Искать в «%1»").arg(isdir ? full : dir), this,
                [this, isdir, full, dir] { setFolder(isdir ? full : dir); });
    m.exec(view_->viewport()->mapToGlobal(pos));
}

}  // namespace bx
