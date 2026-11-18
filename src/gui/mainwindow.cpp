#include "mainwindow.h"

#include "renamedialog.h"
#include "resultmodel.h"
#include "thumbdelegate.h"
#include "thumbnails.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
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
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QMessageBox>
#include <QMimeData>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableView>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

using namespace Qt::StringLiterals;

namespace bx {
namespace {

struct Preset {
    const char* title;  // для tr()
    QStringView filter;
};
const Preset kPresets[] = {
    {"Все", u""},
    {"Файлы", u"file:"},
    {"Папки", u"folder:"},
    {"Документы", u"ext:doc;docx;odt;ods;odp;pdf;txt;rtf;xls;xlsx;ppt;pptx;md;csv;djvu;epub;fb2"},
    {"Изображения", u"ext:png;jpg;jpeg;gif;bmp;svg;webp;tif;tiff;ico;heic;avif;raw;cr2;nef;xcf;kra;psd"},
    {"Аудио", u"ext:mp3;flac;ogg;opus;wav;m4a;aac;wma;ape"},
    {"Видео", u"ext:mp4;mkv;avi;mov;webm;wmv;flv;m4v;mpg;mpeg;ts"},
    {"Архивы", u"ext:zip;rar;7z;tar;gz;bz2;xz;zst;tgz;iso;cab;rpm;deb"},
    {"Исходники", u"ext:c;h;cpp;hpp;cc;cxx;qml;py;rs;go;js;ts;java;kt;cs;sh;cmake;json;xml;yaml;yml;toml"},
};

constexpr int kTileSizes[] = {64, 96, 128, 192, 256};
constexpr int kModes = 1 + int(std::size(kTileSizes));
const char* kModeTitles[] = {"Таблица", "Эскизы: мелкие", "Эскизы: средние", "Эскизы: крупные",
                             "Эскизы: очень крупные", "Эскизы: огромные"};
const QString kHeaderKey = u"header3"_s;  // состояние заголовка (новый набор столбцов)

QString human_count(qint64 n) {
    if (n >= 1000000) return QLocale().toString(n / 1e6, 'f', 2) + QObject::tr(" млн");
    if (n >= 10000) return QLocale().toString(n / 1000) + QObject::tr(" тыс.");
    return QLocale().toString(n);
}

QString size_text(qint64 n) { return QLocale().formattedDataSize(n, 1, QLocale::DataSizeTraditionalFormat); }

QAction* toggle_action(const QString& icon_text, const QString& text, QObject* parent) {
    auto* a = new QAction(text, parent);
    a->setIconText(icon_text);
    a->setToolTip(text);
    a->setCheckable(true);
    return a;
}

QToolButton* toggle_button(QAction* a, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setDefaultAction(a);
    b->setAutoRaise(true);
    QFont f = b->font();
    f.setBold(true);
    b->setFont(f);
    return b;
}

}  // namespace

MainWindow::MainWindow(Client* client, QWidget* parent) : QMainWindow(parent), c_(client) {
    setWindowTitle(u"btrfind"_s);
    setWindowIcon(QIcon::fromTheme(u"system-search"_s));
    model_ = new ResultModel(c_, this);
    thumbs_ = new ThumbnailProvider(this);

    auto* central = new QWidget(this);
    auto* lay = new QVBoxLayout(central);
    lay->setContentsMargins(6, 6, 6, 0);
    lay->setSpacing(4);

    // --- строка поиска
    auto* top = new QHBoxLayout;
    edit_ = new QLineEdit(central);
    edit_->setPlaceholderText(tr("Поиск файлов и папок…  (*.pdf, ext:docx, size:>10mb, dm:today, regex:…)"));
    edit_->setClearButtonEnabled(true);
    edit_->installEventFilter(this);
    QIcon histIcon = QIcon::fromTheme(u"document-open-recent"_s, QIcon::fromTheme(u"view-history"_s));
    QAction* histBtn = edit_->addAction(histIcon, QLineEdit::TrailingPosition);
    histBtn->setToolTip(tr("История поиска (Ctrl+H)"));
    connect(histBtn, &QAction::triggered, this, &MainWindow::showHistoryPopup);
    folderChip_ = new QToolButton(central);
    folderChip_->setIcon(QIcon::fromTheme(u"folder"_s));
    folderChip_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    folderChip_->setAutoRaise(true);
    folderChip_->setToolTip(tr("Поиск только в этой папке — нажмите, чтобы искать везде"));
    folderChip_->hide();
    connect(folderChip_, &QToolButton::clicked, this, [this] { setFolder({}); });
    aCase_ = toggle_action(u"Aa"_s, tr("С учётом регистра"), this);
    aRegex_ = toggle_action(u".*"_s, tr("Регулярное выражение (весь запрос, PCRE2)"), this);
    aPath_ = toggle_action(u"/"_s, tr("Искать по полному пути"), this);
    filter_ = new QComboBox(central);
    for (const Preset& p : kPresets) filter_->addItem(tr(p.title), p.filter.toString());
    top->addWidget(folderChip_);
    top->addWidget(edit_, 1);
    for (QAction* a : {aCase_, aRegex_, aPath_}) top->addWidget(toggle_button(a, central));
    top->addWidget(filter_);
    lay->addLayout(top);

    // --- таблица
    stack_ = new QStackedWidget(central);
    view_ = new QTableView(stack_);
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
    QHeaderView* hh = view_->horizontalHeader();
    hh->setStretchLastSection(false);
    hh->setSectionResizeMode(QHeaderView::Interactive);
    hh->setHighlightSections(false);
    hh->setSectionsMovable(true);
    hh->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(hh, &QHeaderView::customContextMenuRequested, this, [this, hh](const QPoint& pos) {
        QMenu m(this);
        fillColumnsMenu(&m);
        m.exec(hh->mapToGlobal(pos));
    });
    const int widths[ResultModel::ColumnCount] = {320, 480, 90, 130, 130, 80, 180, 100, 100, 100, 130, 90, 600};
    for (int c = 0; c < ResultModel::ColumnCount; ++c) view_->setColumnWidth(c, widths[c]);
    view_->setSortingEnabled(true);
    hh->setSortIndicator(ResultModel::Name, Qt::AscendingOrder);
    view_->installEventFilter(this);
    view_->viewport()->installEventFilter(this);
    stack_->addWidget(view_);

    // --- плитка эскизов: та же модель и то же выделение, порядок как в таблице
    grid_ = new QListView(stack_);
    grid_->setModel(model_);
    grid_->setSelectionModel(view_->selectionModel());
    grid_->setModelColumn(ResultModel::Name);
    grid_->setViewMode(QListView::ListMode);  // ListMode + перенос: дешёвая раскладка и на миллион элементов
    grid_->setFlow(QListView::LeftToRight);
    grid_->setWrapping(true);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setUniformItemSizes(true);
    grid_->setMovement(QListView::Static);
    grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
    grid_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    grid_->setSelectionRectVisible(true);
    grid_->setDragEnabled(true);
    grid_->setDragDropMode(QAbstractItemView::DragOnly);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tdel_ = new ThumbDelegate(thumbs_, grid_);
    grid_->setItemDelegate(tdel_);
    grid_->installEventFilter(this);
    grid_->viewport()->installEventFilter(this);
    stack_->addWidget(grid_);
    connect(thumbs_, &ThumbnailProvider::updated, grid_->viewport(), qOverload<>(&QWidget::update));
    lay->addWidget(stack_, 1);
    setCentralWidget(central);

    count_ = new QLabel(this);
    daemon_ = new QLabel(this);
    statusBar()->addWidget(count_, 1);
    statusBar()->addPermanentWidget(daemon_);

    buildMenus();

    // --- поиск при вводе, история
    debounce_.setSingleShot(true);
    debounce_.setInterval(30);
    connect(&debounce_, &QTimer::timeout, this, [this] { runSearch(); });
    connect(edit_, &QLineEdit::textChanged, &debounce_, qOverload<>(&QTimer::start));
    historyTimer_.setSingleShot(true);
    historyTimer_.setInterval(3000);  // запрос «устоялся» — в историю
    connect(&historyTimer_, &QTimer::timeout, this, [this] {
        if (total_ > 0) commitHistory();
    });
    connect(edit_, &QLineEdit::textChanged, &historyTimer_, qOverload<>(&QTimer::start));
    for (QAction* a : {aCase_, aRegex_, aPath_}) connect(a, &QAction::toggled, this, [this] { runSearch(); });
    connect(filter_, &QComboBox::currentIndexChanged, this, [this] { runSearch(); });

    // --- сортировка (демон); для столбцов без сортировки вернуть прежний индикатор
    connect(model_, &ResultModel::sortRequested, this, [this, hh](const QString& key, bool desc) {
        sort_ = key;
        desc_ = desc;
        sortCol_ = hh->sortIndicatorSection();
        sortDescCol_ = desc;
        runSearch();
    });
    connect(model_, &ResultModel::sortUnavailable, this, [this, hh](int column) {
        const QSignalBlocker b(hh);
        hh->setSortIndicator(sortCol_, sortDescCol_ ? Qt::DescendingOrder : Qt::AscendingOrder);
        statusBar()->showMessage(
            tr("По столбцу «%1» сортировка недоступна: он читается с диска только для видимых строк")
                .arg(ResultModel::columnTitle(column)),
            5000);
    });

    for (QAbstractItemView* v : {static_cast<QAbstractItemView*>(view_), static_cast<QAbstractItemView*>(grid_)}) {
        connect(v, &QAbstractItemView::doubleClicked, this, [this] { openSelected(); });
        connect(v, &QWidget::customContextMenuRequested, this, [this, v](const QPoint& pos) { showContextMenu(v, pos); });
    }

    // --- связь с демоном
    connect(c_, &Client::result, this, &MainWindow::onResult);
    connect(c_, &Client::status, this, &MainWindow::onStatus);
    connect(c_, &Client::sum, this, [this](qint64 id, qint64 tag, qint64 bytes, qint64 files, qint64 dirs) {
        if (id != model_->resultId() || tag != sumTag_) return;
        selBytes_ = bytes;
        selFiles_ = files;
        selDirs_ = dirs;
        updateCountLabel();
    });
    // Индекс изменился — тихо обновить результат, не чаще раза в секунду.
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(1000);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { runSearch(true); });
    connect(c_, &Client::changed, this, [this] {
        if (!refreshTimer_.isActive()) refreshTimer_.start();
    });
    // Выделение следует за файлами при обновлениях результата; размер выделенного — от демона.
    sumTimer_.setSingleShot(true);
    sumTimer_.setInterval(120);
    connect(&sumTimer_, &QTimer::timeout, this, &MainWindow::requestSelectionSum);
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        sumTimer_.start();
        if (restoring_) return;
        if (view_->selectionModel()->selectedRows().size() > 5000) {  // Ctrl+A на огромном результате
            selPaths_.clear();
            trackSel_ = false;  // не пересчитывать выделение по путям — иначе снимется всё выделенное
            return;
        }
        trackSel_ = true;
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

    // --- настройки
    QSettings s;
    restoreGeometry(s.value(u"geometry"_s).toByteArray());
    if (!hh->restoreState(s.value(kHeaderKey).toByteArray()))
        for (int c = ResultModel::Created; c < ResultModel::ColumnCount; ++c) hh->setSectionHidden(c, true);
    aCase_->setChecked(s.value(u"case"_s).toBool());
    aRegex_->setChecked(s.value(u"regex"_s).toBool());
    aPath_->setChecked(s.value(u"path"_s).toBool());
    filter_->setCurrentIndex(s.value(u"filter"_s).toInt());
    if (!s.contains(u"geometry"_s)) resize(1100, 700);
    setViewMode(s.value(u"view/mode"_s, 0).toInt());
    sortCol_ = hh->sortIndicatorSection();
    sortDescCol_ = hh->sortIndicatorOrder() == Qt::DescendingOrder;
    if (!ResultModel::sortable(sortCol_)) sortCol_ = ResultModel::Name;
    model_->sort(sortCol_, hh->sortIndicatorOrder());
}

void MainWindow::buildMenus() {
    auto add = [this](QMenu* m, const QString& text, const QString& icon, const QKeySequence& ks, auto fn,
                      QWidget* scope = nullptr) {
        auto* a = new QAction(QIcon::fromTheme(icon), text, this);
        if (!ks.isEmpty()) a->setShortcut(ks);
        if (scope) {  // сочетание только в области результатов (в строке поиска — своё: Ctrl+C, Ctrl+Z…)
            a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            scope->addAction(a);
        } else {
            a->setShortcutContext(Qt::WindowShortcut);
            addAction(a);
        }
        connect(a, &QAction::triggered, this, fn);
        m->addAction(a);
        return a;
    };

    QMenu* file = menuBar()->addMenu(tr("&Файл"));
    add(file, tr("Открыть"), u"document-open"_s, {}, [this] { openSelected(); });
    add(file, tr("Показать в папке"), u"folder-open"_s, {}, [this] { openFolders(); });
    file->addSeparator();
    add(file, tr("Выход"), u"application-exit"_s, QKeySequence::Quit, [this] { close(); });

    QMenu* edit = menuBar()->addMenu(tr("&Правка"));
    add(edit, tr("Копировать"), u"edit-copy"_s, QKeySequence::Copy, [this] { copyFiles(); }, stack_);
    add(edit, tr("Копировать полный путь"), u"edit-copy-path"_s, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
        [this] { copyPaths(); }, stack_);
    add(edit, tr("Копировать имя"), {}, {}, [this] { copyNames(); });
    edit->addSeparator();
    add(edit, tr("Переименовать…"), u"edit-rename"_s, QKeySequence(Qt::Key_F2), [this] { renameSelected(); });
    add(edit, tr("Удалить в корзину"), u"edit-delete"_s, QKeySequence(Qt::Key_Delete), [this] { removeSelected(false); },
        stack_);
    add(edit, tr("Удалить безвозвратно…"), u"edit-delete-shred"_s, QKeySequence(Qt::SHIFT | Qt::Key_Delete),
        [this] { removeSelected(true); }, stack_);
    add(edit, tr("Отменить"), u"edit-undo"_s, QKeySequence::Undo, [this] { undoLast(); }, stack_);
    edit->addSeparator();
    add(edit, tr("Выделить всё"), u"edit-select-all"_s, QKeySequence::SelectAll,
        [this] { currentView()->selectAll(); }, stack_);

    QMenu* view = menuBar()->addMenu(tr("&Вид"));
    viewGroup_ = new QActionGroup(this);
    for (int m = 0; m < kModes; ++m) {
        QAction* a = add(view, tr(kModeTitles[m]), m == 0 ? u"view-list-details"_s : u"view-preview"_s,
                         QKeySequence(Qt::CTRL | (Qt::Key_1 + m)), [this, m] { setViewMode(m); });
        a->setCheckable(true);
        viewGroup_->addAction(a);
        viewActs_ << a;
    }
    view->addSeparator();
    QMenu* cols = view->addMenu(tr("Столбцы"));
    connect(cols, &QMenu::aboutToShow, this, [this, cols] { fillColumnsMenu(cols); });
    QMenu* sort = view->addMenu(tr("Сортировка"));
    connect(sort, &QMenu::aboutToShow, this, [this, sort] { fillSortMenu(sort); });
    view->addSeparator();
    add(view, tr("Обновить"), u"view-refresh"_s, QKeySequence::Refresh, [this] { runSearch(); });
    QAction* hint = view->addAction(tr("Ctrl+колесо мыши — таблица ↔ эскизы"));
    hint->setEnabled(false);

    QMenu* search = menuBar()->addMenu(tr("&Поиск"));
    add(search, tr("Найти"), u"edit-find"_s, QKeySequence::Find, [this] {
        edit_->setFocus();
        edit_->selectAll();
    });
    add(search, tr("История поиска…"), u"document-open-recent"_s, QKeySequence(Qt::CTRL | Qt::Key_H),
        [this] { showHistoryPopup(); });
    QMenu* hist = search->addMenu(tr("Недавние запросы"));
    connect(hist, &QMenu::aboutToShow, this, [this, hist] { fillHistoryMenu(hist); });
    search->addSeparator();
    search->addAction(aCase_);
    search->addAction(aRegex_);
    search->addAction(aPath_);
    search->addSeparator();
    add(search, tr("Очистить историю"), u"edit-clear-history"_s, {}, [this] {
        history_.clear();
        statusBar()->showMessage(tr("История поиска очищена"), 3000);
    });
    aNoHistory_ = add(search, tr("Не сохранять историю"), {}, {}, [this] { history_.setEnabled(!aNoHistory_->isChecked()); });
    aNoHistory_->setCheckable(true);
    aNoHistory_->setChecked(!history_.enabled());
}

void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s;
    s.setValue(u"geometry"_s, saveGeometry());
    s.setValue(kHeaderKey, view_->horizontalHeader()->saveState());
    s.setValue(u"case"_s, aCase_->isChecked());
    s.setValue(u"regex"_s, aRegex_->isChecked());
    s.setValue(u"path"_s, aPath_->isChecked());
    s.setValue(u"filter"_s, filter_->currentIndex());
    s.setValue(u"view/mode"_s, viewMode_);
    commitHistory();
    e->accept();
}

QAbstractItemView* MainWindow::currentView() const {
    return viewMode_ > 0 ? static_cast<QAbstractItemView*>(grid_) : static_cast<QAbstractItemView*>(view_);
}

void MainWindow::setViewMode(int mode) {
    mode = std::clamp(mode, 0, kModes - 1);
    if (mode == viewMode_) return;
    const bool hadFocus = currentView()->hasFocus();
    const QModelIndex cur = view_->selectionModel()->currentIndex();
    viewMode_ = mode;
    if (mode == 0) {
        stack_->setCurrentWidget(view_);
    } else {
        const int s = kTileSizes[mode - 1];
        tdel_->setTileSize(s);
        thumbs_->cancelPending();
        grid_->setIconSize(QSize(s, s));
        const QSize cell = tdel_->cellSize(grid_->fontMetrics());
        grid_->setGridSize(cell);
        grid_->verticalScrollBar()->setSingleStep(cell.height() / 3);
        stack_->setCurrentWidget(grid_);
        grid_->doItemsLayout();
    }
    viewActs_[mode]->setChecked(true);
    QAbstractItemView* v = currentView();
    if (cur.isValid()) v->scrollTo(cur.siblingAtColumn(0), QAbstractItemView::PositionAtCenter);
    if (hadFocus) v->setFocus();
}

void MainWindow::fillColumnsMenu(QMenu* m) {
    m->clear();
    QHeaderView* hh = view_->horizontalHeader();
    for (int c = 0; c < ResultModel::ColumnCount; ++c) {
        QString title = ResultModel::columnTitle(c);
        if (!ResultModel::sortable(c)) title += tr(" (без сортировки)");
        QAction* a = m->addAction(title);
        a->setCheckable(true);
        a->setChecked(!hh->isSectionHidden(c));
        a->setEnabled(c != ResultModel::Name);  // имя показывается всегда
        connect(a, &QAction::toggled, this, [this, hh, c](bool on) {
            hh->setSectionHidden(c, !on);
            if (on && hh->sectionSize(c) < 40) hh->resizeSection(c, 120);
            if (on) model_->dataChanged(model_->index(0, c), model_->index(std::max(0, model_->rowCount() - 1), c));
        });
    }
    m->addSeparator();
    m->addAction(tr("По умолчанию"), this, [hh] {
        for (int c = 0; c < ResultModel::ColumnCount; ++c) hh->setSectionHidden(c, c >= ResultModel::Created);
    });
}

void MainWindow::fillSortMenu(QMenu* m) {
    m->clear();
    QHeaderView* hh = view_->horizontalHeader();
    auto* g = new QActionGroup(m);
    for (int c = 0; c < ResultModel::ColumnCount; ++c) {
        if (!ResultModel::sortable(c) || c == ResultModel::FullPath || c == ResultModel::Extension) continue;
        QAction* a = m->addAction(ResultModel::columnTitle(c));
        a->setCheckable(true);
        a->setChecked(sortCol_ == c || (c == ResultModel::Path && sortCol_ == ResultModel::FullPath) ||
                      (c == ResultModel::Type && sortCol_ == ResultModel::Extension));
        g->addAction(a);
        connect(a, &QAction::triggered, this, [this, hh, c] { hh->setSortIndicator(c, hh->sortIndicatorOrder()); });
    }
    m->addSeparator();
    QAction* d = m->addAction(tr("По убыванию"));
    d->setCheckable(true);
    d->setChecked(sortDescCol_);
    connect(d, &QAction::toggled, this,
            [hh](bool on) { hh->setSortIndicator(hh->sortIndicatorSection(), on ? Qt::DescendingOrder : Qt::AscendingOrder); });
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    // Ctrl+колесо: таблица -> эскизы всё крупнее и обратно
    if (e->type() == QEvent::Wheel && (o == view_->viewport() || o == grid_->viewport())) {
        auto* we = static_cast<QWheelEvent*>(e);
        if (we->modifiers() & Qt::ControlModifier) {
            wheelAcc_ += we->angleDelta().y();
            while (wheelAcc_ >= 120) {
                wheelAcc_ -= 120;
                setViewMode(viewMode_ + 1);
            }
            while (wheelAcc_ <= -120) {
                wheelAcc_ += 120;
                setViewMode(viewMode_ - 1);
            }
            return true;
        }
        return QMainWindow::eventFilter(o, e);
    }
    if (e->type() != QEvent::KeyPress) return QMainWindow::eventFilter(o, e);
    auto* k = static_cast<QKeyEvent*>(e);
    if (o == edit_) {
        if (k->key() == Qt::Key_Escape && !edit_->text().isEmpty()) {
            edit_->clear();
            return true;
        }
        if (k->key() == Qt::Key_Down && (k->modifiers() & Qt::AltModifier)) {
            showHistoryPopup();
            return true;
        }
        if ((k->key() == Qt::Key_Down || k->key() == Qt::Key_PageDown || k->key() == Qt::Key_Return ||
             k->key() == Qt::Key_Enter)) {
            if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) commitHistory();
            if (model_->rowCount() > 0) {
                QAbstractItemView* v = currentView();
                v->setFocus();
                v->selectionModel()->setCurrentIndex(model_->index(0, 0), QItemSelectionModel::ClearAndSelect |
                                                                              QItemSelectionModel::Rows);
            }
            return true;
        }
    } else if (o == view_ || o == grid_) {
        const bool ctrl = k->modifiers() & Qt::ControlModifier, shift = k->modifiers() & Qt::ShiftModifier;
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            ctrl ? openFolders() : openSelected();
            return true;
        }
        if (k->key() == Qt::Key_C && ctrl) {
            shift ? copyPaths() : copyFiles();
            return true;
        }
        if (o == view_ && k->key() == Qt::Key_Up && view_->currentIndex().row() <= 0) {
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
    if (!folder_.isEmpty() && folder_ != u"/"_s) {
        QString d = folder_;
        d.replace(u'"', QString());
        f += u" path:\"%1/\""_s.arg(d);
    }
    return f.trimmed();
}

void MainWindow::setFolder(const QString& dir) {
    QString d = dir;
    while (d.size() > 1 && d.endsWith(u'/')) d.chop(1);
    folder_ = d;
    folderChip_->setVisible(!d.isEmpty() && d != u"/"_s);
    folderChip_->setText(d + u"  ✕"_s);
    runSearch();
}

void MainWindow::setQuery(const QString& q) {
    edit_->setText(q);
    runSearch();
}

void MainWindow::setVisibleColumns(const QList<int>& columns) {
    QHeaderView* hh = view_->horizontalHeader();
    for (int c = 0; c < ResultModel::ColumnCount; ++c) hh->setSectionHidden(c, c != ResultModel::Name && !columns.contains(c));
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
    o.case_sensitive = aCase_->isChecked();
    o.regex = aRegex_->isChecked();
    o.path = aPath_->isChecked();
    o.sort = sort_;
    o.desc = desc_;
    refresh_ = refresh;
    pendingId_ = c_->search(edit_->text(), o);
}

void MainWindow::onResult(qint64 id, qint64 total, double ms, const QString& error, const QCborMap& m) {
    if (id != pendingId_) return;  // устаревший ответ
    error_ = error;
    if (!error.isEmpty()) {
        updateCountLabel();
        return;
    }
    if (refresh_ && model_->resultId() >= 0) {
        model_->refreshResult(id, total);  // без сброса: ни мерцания, ни сброса прокрутки
    } else {
        selPaths_.clear();
        curPath_.clear();
        thumbs_->cancelPending();
        model_->setResult(id, total);
        view_->scrollToTop();
        grid_->scrollToTop();
    }
    total_ = total;
    ms_ = ms;
    totalBytes_ = m.value(u"bytes"_s).toInteger(-1);  // -1 — старый демон без итогов
    totalFiles_ = m.value(u"files"_s).toInteger();
    totalDirs_ = m.value(u"dirs"_s).toInteger();
    requestSelectionSum();
}

void MainWindow::requestSelectionSum() {
    // Диапазоны выделенных строк -> демон считает размер (строки могут быть не загружены).
    const QItemSelection sel = view_->selectionModel()->selection();
    QList<QPair<int, int>> ranges;
    for (const QItemSelectionRange& r : sel) ranges.append({r.top(), r.bottom() + 1});
    std::sort(ranges.begin(), ranges.end());
    QCborArray merged;
    qint64 count = 0;
    for (qsizetype i = 0; i < ranges.size();) {
        int a = ranges[i].first, b = ranges[i].second;
        for (++i; i < ranges.size() && ranges[i].first <= b; ++i) b = std::max(b, ranges[i].second);
        merged.append(QCborArray{a, b});
        count += b - a;
    }
    selCount_ = count;
    selBytes_ = -1;
    if (count > 0 && count == total_) {  // выделено всё
        selBytes_ = totalBytes_;
        selFiles_ = totalFiles_;
        selDirs_ = totalDirs_;
    } else if (count > 0) {
        c_->requestSum(model_->resultId(), merged, ++sumTag_);
    }
    updateCountLabel();
}

void MainWindow::updateCountLabel() {
    if (!error_.isEmpty()) {
        count_->setText(u"<span style='color:#d33'>%1</span>"_s.arg(error_.toHtmlEscaped()));
        return;
    }
    QString t = tr("%1 объектов").arg(QLocale().toString(total_));
    if (totalBytes_ >= 0) {
        t += tr(" (файлов %1, папок %2) · %3")
                 .arg(QLocale().toString(totalFiles_), QLocale().toString(totalDirs_), size_text(totalBytes_));
    }
    t += tr(" · %1 мс").arg(ms_, 0, 'f', 1);
    if (selCount_ > 0) {
        QString s = tr("Выделено: %1").arg(QLocale().toString(selCount_));
        if (selBytes_ >= 0) s += tr(" · %1").arg(size_text(selBytes_));
        if (selBytes_ >= 0 && selDirs_ > 0) s += tr(" (папок %1 — размер без содержимого)").arg(selDirs_);
        t = u"<b>%1</b> — %2"_s.arg(s.toHtmlEscaped(), t.toHtmlEscaped());
    }
    count_->setText(t);
}

void MainWindow::onStatus(const QCborMap& m) {
    QStringList parts;
    bool all_ready = true, live = true;
    for (const QCborValue& v : m.value(u"volumes"_s).toArray()) {
        const QCborMap vm = v.toMap();
        const QString state = vm.value(u"state"_s).toString();
        if (state == u"removed"_s) continue;
        QString s = vm.value(u"path"_s).toString() + u' ';
        if (state == u"ready"_s) {
            s += human_count(vm.value(u"entries"_s).toInteger());
        } else {
            s += state == u"scanning"_s      ? tr("индексация…")
                 : state == u"catching-up"_s ? tr("догон изменений…")
                 : state == u"error"_s       ? tr("ошибка")
                                                         : tr("загрузка…");
            all_ready = false;
        }
        live &= vm.value(u"live"_s).toBool();
        parts << s;
    }
    QString text = parts.join(u" · "_s);
    if (!live) text += tr(" · <span style='color:#c80'>без живых обновлений</span>");
    daemon_->setText(text);
    statusTimer_.setInterval(all_ready ? 30000 : 2000);
    if (all_ready && !allReady_) runSearch(true);  // индексация завершилась — обновить результат
    allReady_ = all_ready;
}

void MainWindow::restoreSelection(int first, int last) {
    QItemSelectionModel* sm = view_->selectionModel();
    // Пусто и ничего не выделено — сверять нечего. Если строки выделены, а путей нет (после удаления
    // строки сдвинулись), выделение с чужих строк надо снять.
    if (!trackSel_ || (selPaths_.isEmpty() && curPath_.isEmpty() && !sm->hasSelection())) return;
    restoring_ = true;
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

// ---------------------------------------------------------------- история

void MainWindow::commitHistory() {
    historyTimer_.stop();
    history_.add(edit_->text());
}

void MainWindow::fillHistoryMenu(QMenu* m) {
    m->clear();
    const QStringList& items = history_.items();
    if (items.isEmpty()) {
        m->addAction(history_.enabled() ? tr("История пуста") : tr("История отключена"))->setEnabled(false);
        return;
    }
    const QFontMetrics fm(m->font());
    for (const QString& q : items.mid(0, 30)) {
        QAction* a = m->addAction(fm.elidedText(q, Qt::ElideMiddle, 500));
        a->setToolTip(q);
        connect(a, &QAction::triggered, this, [this, q] {
            setQuery(q);
            commitHistory();  // наверх списка
            edit_->setFocus();
            edit_->selectAll();
        });
    }
    m->addSeparator();
    m->addAction(QIcon::fromTheme(u"edit-clear-history"_s), tr("Очистить историю"), this,
                 [this] { history_.clear(); });
}

void MainWindow::showHistoryPopup() {
    QMenu m(this);
    fillHistoryMenu(&m);
    m.setMinimumWidth(edit_->width());
    m.exec(edit_->mapToGlobal(QPoint(0, edit_->height())));
}

// ---------------------------------------------------------------- переименование

void MainWindow::renameSelected() {
    const QModelIndexList rows = view_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    const QStringList paths = selectedPaths();
    if (paths.size() != rows.size()) {
        statusBar()->showMessage(tr("Не все выделенные строки загружены — прокрутите к ним или выделите меньше"), 6000);
        return;
    }
    commitHistory();
    RenameResult r;
    if (paths.size() == 1) {
        const bool isdir = rows[0].siblingAtColumn(0).data(ResultModel::IsDirRole).toBool();
        if (!rename_single(this, paths[0], isdir, r)) return;
    } else {
        RenameDialog d(paths, this);
        if (d.exec() != QDialog::Accepted) return;
        r = d.result();
    }
    if (r.done.empty()) return;
    lastRename_ = r.done;
    lastOp_ = LastOp::Rename;
    applyRenamed(r.done);
    statusBar()->showMessage(tr("Переименовано: %1 · Ctrl+Z — отменить").arg(r.done.size()), 10000);
}

void MainWindow::undoRename() {
    if (lastRename_.empty()) {
        statusBar()->showMessage(tr("Нечего отменять"), 3000);
        return;
    }
    const RenameResult r = undo_rename(lastRename_);
    lastRename_.clear();
    lastOp_ = LastOp::None;
    applyRenamed(r.done);
    if (!r.errors.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, tr("Отмена переименования"),
                        tr("Возвращено: %1, не удалось: %2.").arg(r.done.size()).arg(r.errors.size()), QMessageBox::Ok,
                        this);
        box.setDetailedText(r.errors.join(u'\n'));
        box.exec();
    }
    statusBar()->showMessage(tr("Возвращено прежних имён: %1").arg(r.done.size()), 6000);
}

// ---------------------------------------------------------------- удаление

void MainWindow::removeSelected(bool permanent) {
    const QModelIndexList rows = view_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    const QStringList paths = selectedPaths();
    if (paths.size() != rows.size()) {
        statusBar()->showMessage(tr("Не все выделенные строки загружены — прокрутите к ним или выделите меньше"), 6000);
        return;
    }
    commitHistory();
    const QStringList top = top_level_paths(paths);
    int dirs = 0;
    for (const QModelIndex& i : rows) dirs += i.siblingAtColumn(0).data(ResultModel::IsDirRole).toBool();

    QStringList names;
    for (const QString& p : top.mid(0, 10)) names << p;
    if (top.size() > 10) names << tr("… и ещё %1").arg(top.size() - 10);
    QSettings s;
    if (permanent) {
        QMessageBox box(QMessageBox::Warning, tr("Удалить безвозвратно"),
                        tr("Удалить безвозвратно объектов: %1? Это действие нельзя отменить.").arg(top.size()),
                        QMessageBox::Yes | QMessageBox::No, this);
        box.setInformativeText((dirs ? tr("Папки удаляются вместе со всем содержимым.\n\n") : QString()) +
                               names.join(u'\n'));
        box.setDefaultButton(QMessageBox::No);
        box.button(QMessageBox::Yes)->setText(tr("Удалить безвозвратно"));
        if (box.exec() != QMessageBox::Yes) return;
    } else if (s.value(u"delete/confirmTrash"_s, true).toBool()) {
        QMessageBox box(QMessageBox::Question, tr("Удалить в корзину"),
                        tr("Переместить в корзину объектов: %1?").arg(top.size()), QMessageBox::Yes | QMessageBox::No,
                        this);
        box.setInformativeText(names.join(u'\n'));
        box.setDefaultButton(QMessageBox::Yes);
        box.button(QMessageBox::Yes)->setText(tr("В корзину"));
        auto* dontAsk = new QCheckBox(tr("Больше не спрашивать (Ctrl+Z вернёт удалённое)"), &box);
        box.setCheckBox(dontAsk);
        if (box.exec() != QMessageBox::Yes) return;
        if (dontAsk->isChecked()) s.setValue(u"delete/confirmTrash"_s, false);
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::vector<MountInfo> mounts = read_mountinfo();
    const RemoveResult r = permanent ? delete_permanently(top, mounts) : move_to_trash(top, mounts);
    QApplication::restoreOverrideCursor();

    // Выделение снимается полностью: строки ниже удалённых сдвинутся, и по номерам строк
    // выделились бы другие файлы.
    QStringList gone = r.removed;
    for (const TrashedItem& t : r.trashed) gone << t.original;
    if (!gone.isEmpty()) {
        QItemSelectionModel* sm = view_->selectionModel();
        restoring_ = true;
        sm->clearSelection();
        sm->clearCurrentIndex();
        restoring_ = false;
        selPaths_.clear();
        curPath_.clear();
        trackSel_ = true;
        requestSelectionSum();
    }
    if (!permanent && !r.trashed.empty()) {
        lastTrash_ = r.trashed;
        lastOp_ = LastOp::Trash;
    }
    if (!r.errors.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, permanent ? tr("Удаление") : tr("Удаление в корзину"),
                        tr("Удалено: %1, не удалось: %2.").arg(gone.size()).arg(r.errors.size()), QMessageBox::Ok, this);
        box.setDetailedText(r.errors.join(u'\n'));
        box.exec();
    }
    if (!gone.isEmpty())
        statusBar()->showMessage(permanent ? tr("Удалено безвозвратно: %1").arg(gone.size())
                                           : tr("В корзине: %1 · Ctrl+Z — восстановить").arg(gone.size()),
                                 10000);
    if (!refreshTimer_.isActive()) refreshTimer_.start();
}

void MainWindow::restoreTrash() {
    const RemoveResult r = restore_from_trash(lastTrash_);
    lastTrash_.clear();
    lastOp_ = LastOp::None;
    for (const QString& p : r.removed) selPaths_.insert(p);  // вернувшиеся снова выделены
    if (!r.errors.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, tr("Восстановление из корзины"),
                        tr("Восстановлено: %1, не удалось: %2.").arg(r.removed.size()).arg(r.errors.size()),
                        QMessageBox::Ok, this);
        box.setDetailedText(r.errors.join(u'\n'));
        box.exec();
    }
    statusBar()->showMessage(tr("Восстановлено из корзины: %1").arg(r.removed.size()), 6000);
    if (!refreshTimer_.isActive()) refreshTimer_.start();
}

void MainWindow::undoLast() {
    switch (lastOp_) {
        case LastOp::Rename: undoRename(); break;
        case LastOp::Trash: restoreTrash(); break;
        case LastOp::None: statusBar()->showMessage(tr("Нечего отменять"), 3000); break;
    }
}

void MainWindow::applyRenamed(const std::vector<RenameDone>& done) {
    // Выделение и текущая строка следуют за файлами (и за содержимым переименованных папок).
    auto remap = [](QString& p, const QString& from, const QString& to) {
        if (p == from)
            p = to;
        else if (p.startsWith(from + u'/'))
            p = to + p.mid(from.size());
    };
    for (const RenameDone& d : done) {
        const QString from = d.old_path(), to = d.new_path();
        QSet<QString> next;
        for (QString p : std::as_const(selPaths_)) {
            remap(p, from, to);
            next.insert(p);
        }
        selPaths_ = std::move(next);
        remap(curPath_, from, to);
    }
    if (!refreshTimer_.isActive()) refreshTimer_.start();  // демон пришлёт «changed», но обновим и сами
}

// ---------------------------------------------------------------- действия с результатами

QStringList MainWindow::selectedPaths() const {
    QStringList out;
    for (const QModelIndex& i : view_->selectionModel()->selectedRows()) {
        const QString p = model_->fullPath(i.row());
        if (!p.isEmpty()) out << p;
    }
    return out;
}

void MainWindow::openSelected() {
    commitHistory();
    const QStringList ps = selectedPaths();
    for (const QString& p : ps.mid(0, 20)) QDesktopServices::openUrl(QUrl::fromLocalFile(p));
}

void MainWindow::openFolders() {
    commitHistory();
    const QStringList ps = selectedPaths();
    if (ps.isEmpty()) return;
    // Файловый менеджер с выделением файла (Dolphin, Nautilus и др. поддерживают FileManager1).
    QStringList uris;
    for (const QString& p : ps.mid(0, 20)) uris << QUrl::fromLocalFile(p).toString();
    QDBusMessage msg = QDBusMessage::createMethodCall(u"org.freedesktop.FileManager1"_s,
                                                      u"/org/freedesktop/FileManager1"_s,
                                                      u"org.freedesktop.FileManager1"_s,
                                                      u"ShowItems"_s);
    msg << uris << QString();
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ErrorMessage)
        for (const QString& p : ps.mid(0, 20)) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).path()));
}

void MainWindow::copyPaths() {
    commitHistory();
    QApplication::clipboard()->setText(selectedPaths().join(u'\n'));
}

void MainWindow::copyNames() {
    commitHistory();
    QStringList names;
    for (const QString& p : selectedPaths()) names << QFileInfo(p).fileName();
    QApplication::clipboard()->setText(names.join(u'\n'));
}

void MainWindow::copyFiles() {  // как в файловом менеджере: вставляется копия файлов
    commitHistory();
    auto* md = new QMimeData;
    QList<QUrl> urls;
    for (const QString& p : selectedPaths()) urls << QUrl::fromLocalFile(p);
    md->setUrls(urls);
    md->setText(selectedPaths().join(u'\n'));
    QApplication::clipboard()->setMimeData(md);
}

void MainWindow::showContextMenu(QAbstractItemView* v, const QPoint& pos) {
    const QModelIndex idx = v->indexAt(pos);
    if (!idx.isValid()) return;
    QItemSelectionModel* sm = view_->selectionModel();
    if (!sm->isRowSelected(idx.row(), {}))
        sm->setCurrentIndex(idx.siblingAtColumn(0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const bool isdir = idx.siblingAtColumn(0).data(ResultModel::IsDirRole).toBool();
    const QString dir = idx.siblingAtColumn(0).data(ResultModel::DirRole).toString();
    const QString full = model_->fullPath(idx.row());

    QMenu m(this);
    m.addAction(QIcon::fromTheme(u"document-open"_s), tr("Открыть"), QKeySequence(Qt::Key_Return), this,
                &MainWindow::openSelected);
    m.addAction(QIcon::fromTheme(u"folder-open"_s), tr("Показать в папке"),
                QKeySequence(Qt::CTRL | Qt::Key_Return), this, &MainWindow::openFolders);
    m.addSeparator();
    m.addAction(QIcon::fromTheme(u"edit-copy"_s), tr("Копировать"), QKeySequence::Copy, this,
                &MainWindow::copyFiles);
    m.addAction(QIcon::fromTheme(u"edit-copy-path"_s), tr("Копировать полный путь"),
                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this, &MainWindow::copyPaths);
    m.addAction(tr("Копировать имя"), this, &MainWindow::copyNames);
    m.addSeparator();
    const int nsel = int(sm->selectedRows().size());
    m.addAction(QIcon::fromTheme(u"edit-rename"_s),
                nsel > 1 ? tr("Переименовать %1 объектов…").arg(nsel) : tr("Переименовать…"),
                QKeySequence(Qt::Key_F2), this, &MainWindow::renameSelected);
    m.addSeparator();
    m.addAction(QIcon::fromTheme(u"edit-delete"_s), tr("Удалить в корзину"), QKeySequence(Qt::Key_Delete), this,
                [this] { removeSelected(false); });
    m.addAction(QIcon::fromTheme(u"edit-delete-shred"_s), tr("Удалить безвозвратно…"),
                QKeySequence(Qt::SHIFT | Qt::Key_Delete), this, [this] { removeSelected(true); });
    if (lastOp_ == LastOp::Rename)
        m.addAction(QIcon::fromTheme(u"edit-undo"_s), tr("Отменить переименование"), QKeySequence::Undo, this,
                    &MainWindow::undoLast);
    else if (lastOp_ == LastOp::Trash)
        m.addAction(QIcon::fromTheme(u"edit-undo"_s), tr("Восстановить из корзины (%1)").arg(lastTrash_.size()),
                    QKeySequence::Undo, this, &MainWindow::undoLast);
    m.addSeparator();
    m.addAction(QIcon::fromTheme(u"system-search"_s), tr("Искать в «%1»").arg(isdir ? full : dir), this,
                [this, isdir, full, dir] { setFolder(isdir ? full : dir); });
    m.exec(v->viewport()->mapToGlobal(pos));
}

}  // namespace bx
