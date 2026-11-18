#include "thumbnails.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QThreadPool>
#include <QUrl>

#ifdef BTRFIND_HAVE_KIO
#include <KFileItem>
#include <KIO/PreviewJob>
#endif

namespace bx {
namespace {
constexpr int kBatch = 32;
constexpr int kMaxQueue = 400;
constexpr int kMaxJobs = 2;
}  // namespace

ThumbnailProvider::ThumbnailProvider(QObject* parent) : QObject(parent) {
    cache_.setMaxCost(256 * 1024 * 1024);  // байты
    pumpTimer_.setSingleShot(true);
    pumpTimer_.setInterval(40);
    connect(&pumpTimer_, &QTimer::timeout, this, &ThumbnailProvider::pump);
    notifyTimer_.setSingleShot(true);
    notifyTimer_.setInterval(60);
    connect(&notifyTimer_, &QTimer::timeout, this, &ThumbnailProvider::updated);
#ifdef BTRFIND_HAVE_KIO
    plugins_ = KIO::PreviewJob::defaultPlugins();  // как настроено в Dolphin
#endif
}

QString ThumbnailProvider::key(const QString& path, qint64 mtime, int size) {
    return QString::number(size) + QLatin1Char('|') + QString::number(mtime) + QLatin1Char('|') + path;
}

QPixmap ThumbnailProvider::get(const QString& path, qint64 mtime, int size, qreal dpr) {
    const QString k = key(path, mtime, size);
    if (QPixmap* pm = cache_.object(k)) return *pm;
    if (failed_.contains(k) || inflight_.contains(k) || queued_.contains(k)) return {};
    queue_.append({path, mtime, size, dpr});
    queued_.insert(k);
    while (queue_.size() > kMaxQueue) {  // давно уехавшие из видимой области
        const Req old = queue_.takeFirst();
        queued_.remove(key(old.path, old.mtime, old.size));
    }
    if (!pumpTimer_.isActive()) pumpTimer_.start();
    return {};
}

void ThumbnailProvider::cancelPending() {
    queue_.clear();
    queued_.clear();
}

void ThumbnailProvider::done(const QString& k, const QImage& img) {
    inflight_.remove(k);
    if (img.isNull()) {
        failed_.insert(k);  // нет эскиза — показываем иконку типа, повторно не просим
    } else {
        auto* pm = new QPixmap(QPixmap::fromImage(img));
        cache_.insert(k, pm, int(qint64(pm->width()) * pm->height() * 4));
    }
    if (!notifyTimer_.isActive()) notifyTimer_.start();
}

void ThumbnailProvider::pump() {
    while (running_ < kMaxJobs && !queue_.isEmpty()) {
        // Сначала самые свежие запросы — это то, что сейчас на экране.
        QList<Req> batch;
        const int size = queue_.last().size;
        for (qsizetype i = queue_.size() - 1; i >= 0 && batch.size() < kBatch; --i)
            if (queue_[i].size == size) batch.append(queue_.takeAt(i));
        for (const Req& r : batch) {
            const QString k = key(r.path, r.mtime, r.size);
            queued_.remove(k);
            inflight_.insert(k);
        }
        ++running_;
#ifdef BTRFIND_HAVE_KIO
        static QMimeDatabase db;
        KFileItemList items;
        QHash<QString, QString> keys;  // локальный путь -> ключ кэша
        for (const Req& r : batch) {
            // MIME по имени — без чтения файла в потоке GUI
            KFileItem it(QUrl::fromLocalFile(r.path), db.mimeTypeForFile(r.path, QMimeDatabase::MatchExtension).name(),
                         KFileItem::Unknown);
            items.append(it);
            keys.insert(r.path, key(r.path, r.mtime, r.size));
        }
        auto* job = KIO::filePreview(items, QSize(size, size), &plugins_);
        job->setScaleType(KIO::PreviewJob::ScaledAndCached);  // общий кэш эскизов с Dolphin
        job->setDevicePixelRatio(batch.first().dpr);
        connect(job, &KIO::PreviewJob::gotPreview, this, [this, keys](const KFileItem& it, const QPixmap& pm) {
            done(keys.value(it.url().toLocalFile()), pm.toImage());
        });
        connect(job, &KIO::PreviewJob::failed, this,
                [this, keys](const KFileItem& it) { done(keys.value(it.url().toLocalFile()), QImage()); });
        connect(job, &KJob::result, this, [this, keys] {
            for (const QString& k : keys)  // не ответившие — считать без эскиза
                if (inflight_.contains(k)) done(k, QImage());
            --running_;
            pump();
        });
#else
        static const QList<QByteArray> formats = QImageReader::supportedImageFormats();
        QList<QPair<QString, QString>> work;  // путь, ключ
        for (const Req& r : batch) work.append({r.path, key(r.path, r.mtime, r.size)});
        const qreal dpr = batch.first().dpr;
        QThreadPool::globalInstance()->start([this, work, size, dpr] {
            for (const auto& [path, k] : work) {
                QImage img;
                if (formats.contains(QFileInfo(path).suffix().toLower().toLatin1())) {
                    QImageReader r(path);
                    r.setAutoTransform(true);
                    QSize s = r.size();
                    const int px = int(size * dpr);
                    if (s.isValid() && (s.width() > px || s.height() > px)) {
                        s.scale(px, px, Qt::KeepAspectRatio);
                        r.setScaledSize(s);
                    }
                    img = r.read();
                    img.setDevicePixelRatio(dpr);
                }
                QMetaObject::invokeMethod(this, [this, k, img] { done(k, img); }, Qt::QueuedConnection);
            }
            QMetaObject::invokeMethod(this, [this] {
                --running_;
                pump();
            }, Qt::QueuedConnection);
        });
#endif
    }
}

}  // namespace bx
