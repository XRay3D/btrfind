#include "icons.h"

#include <QFileIconProvider>
#include <QHash>
#include <QMimeDatabase>

namespace bx {

QIcon icon_for(const QString& name, bool dir) {
    static QHash<QString, QIcon> cache;
    static QMimeDatabase db;
    if (dir) {
        static const QIcon folder = QIcon::fromTheme(QStringLiteral("folder"),
                                                     QFileIconProvider().icon(QFileIconProvider::Folder));
        return folder;
    }
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    const QString key = dot > 0 ? name.mid(dot + 1).toLower() : QStringLiteral("#") + name.toLower();
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;
    const QMimeType mt = db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    QIcon icon = QIcon::fromTheme(mt.iconName());
    if (icon.isNull()) icon = QIcon::fromTheme(mt.genericIconName());
    if (icon.isNull()) icon = QFileIconProvider().icon(QFileIconProvider::File);
    if (cache.size() > 4000) cache.clear();
    cache.insert(key, icon);
    return icon;
}

}  // namespace bx
