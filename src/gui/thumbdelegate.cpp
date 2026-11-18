#include "thumbdelegate.h"

#include "icons.h"
#include "resultmodel.h"
#include "thumbnails.h"

#include <QApplication>
#include <QPainter>
#include <QTextLayout>

namespace bx {

ThumbDelegate::ThumbDelegate(ThumbnailProvider* provider, QObject* parent)
    : QStyledItemDelegate(parent), provider_(provider) {}

QSize ThumbDelegate::cellSize(const QFontMetrics& fm) const {
    return {size_ + 24, size_ + 2 * fm.height() + 16};
}

QSize ThumbDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
    return cellSize(option.fontMetrics);
}

void ThumbDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget* w = opt.widget;
    QStyle* style = w ? w->style() : QApplication::style();
    opt.text.clear();
    opt.icon = QIcon();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, p, w);  // фон выделения/наведения

    const QRect r = option.rect.adjusted(6, 6, -6, -4);
    const QRect img(r.x() + (r.width() - size_) / 2, r.y(), size_, size_);
    const QString name = index.data(ResultModel::NameRole).toString();
    if (name.isEmpty()) {  // строка ещё не загружена
        p->setPen(opt.palette.color(QPalette::Disabled, QPalette::Text));
        p->drawText(img, Qt::AlignCenter, QStringLiteral("…"));
        return;
    }
    const bool dir = index.data(ResultModel::IsDirRole).toBool();
    QPixmap pm;
    if (!dir)
        pm = provider_->get(index.data(ResultModel::FullPathRole).toString(), index.data(ResultModel::MtimeRole).toLongLong(),
                            size_, w ? w->devicePixelRatioF() : 1.0);
    if (!pm.isNull()) {
        QSizeF s = pm.deviceIndependentSize();
        if (s.width() > img.width() || s.height() > img.height()) s.scale(img.size(), Qt::KeepAspectRatio);
        const QRectF target(img.x() + (img.width() - s.width()) / 2, img.y() + (img.height() - s.height()) / 2,
                            s.width(), s.height());
        p->drawPixmap(target, pm, QRectF(pm.rect()));
    } else {
        const int is = std::max(32, size_ * 3 / 4);
        icon_for(name, dir).paint(p, QRect(img.x() + (img.width() - is) / 2, img.y() + (img.height() - is) / 2, is, is));
    }

    // Имя: до двух строк, вторая — с многоточием.
    const QFontMetrics& fm = opt.fontMetrics;
    const QRect tr(r.x(), img.bottom() + 4, r.width(), 2 * fm.height());
    p->setPen(opt.palette.color(QPalette::Normal,
                                (opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));
    QTextLayout layout(name, opt.font);
    QTextOption to(Qt::AlignHCenter);
    to.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(to);
    layout.beginLayout();
    QStringList lines;
    for (int i = 0; i < 2; ++i) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(tr.width());
        lines << (i == 1 ? fm.elidedText(name.mid(line.textStart()), Qt::ElideRight, tr.width())
                         : name.mid(line.textStart(), line.textLength()));
    }
    layout.endLayout();
    for (int i = 0; i < lines.size(); ++i)
        p->drawText(QRect(tr.x(), tr.y() + i * fm.height(), tr.width(), fm.height()), Qt::AlignHCenter | Qt::AlignVCenter,
                    lines[i]);
}

}  // namespace bx
