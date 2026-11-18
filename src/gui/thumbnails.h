#pragma once
// Эскизы файлов для плитки: кэш в памяти, очередь только для видимых элементов.
// С KDE Frameworks — KIO::PreviewJob (те же модули и общий кэш ~/.cache/thumbnails, что у Dolphin:
// видео, PDF, книги, шрифты…); без них — изображения через QImageReader в пуле потоков.

#include <QCache>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QTimer>

namespace bx {

class ThumbnailProvider : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailProvider(QObject* parent = nullptr);

    // Эскиз из кэша или пустой (тогда он поставлен в очередь). size — сторона в логических пикселях.
    QPixmap get(const QString& path, qint64 mtime, int size, qreal dpr);
    void cancelPending();  // новый результат или другой размер — старая очередь не нужна

signals:
    void updated();  // появились новые эскизы (с задержкой, пачкой)

private:
    struct Req {
        QString path;
        qint64 mtime;
        int size;
        qreal dpr;
    };
    static QString key(const QString& path, qint64 mtime, int size);
    void pump();
    void done(const QString& key, const QImage& img);

    QCache<QString, QPixmap> cache_;
    QSet<QString> failed_, inflight_, queued_;
    QList<Req> queue_;
    int running_ = 0;
    QTimer pumpTimer_, notifyTimer_;
    QStringList plugins_;
};

}  // namespace bx
