#pragma once
// Отрисовка клетки плитки: эскиз (или иконка типа) по центру и имя в две строки.

#include <QStyledItemDelegate>

namespace bx {

class ThumbnailProvider;

class ThumbDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    ThumbDelegate(ThumbnailProvider* provider, QObject* parent = nullptr);
    void setTileSize(int px) { size_ = px; }
    int tileSize() const { return size_; }
    QSize cellSize(const QFontMetrics& fm) const;

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    ThumbnailProvider* provider_;
    int size_ = 96;
};

}  // namespace bx
