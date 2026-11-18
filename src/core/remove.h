#pragma once
// Удаление найденного: в корзину (freedesktop Trash, с восстановлением) или безвозвратно.
// Защита: не удаляются точки монтирования и папки, внутри которых что-то смонтировано;
// символическая ссылка удаляется сама, без захода в каталог, на который она указывает.

#include "mounts.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace bx {

struct TrashedItem {
    QString original;  // откуда
    QString in_trash;  // где лежит в корзине (…/Trash/files/имя)
};

struct RemoveResult {
    std::vector<TrashedItem> trashed;
    QStringList removed;  // удалено безвозвратно / восстановлено
    QStringList errors;
};

// Убрать из списка пути, лежащие внутри других выбранных папок (их удалит удаление папки).
QStringList top_level_paths(const QStringList& paths);
// Почему путь удалять нельзя ("" — можно).
QString removal_problem(const QString& path, const std::vector<MountInfo>& mounts);

RemoveResult move_to_trash(const QStringList& paths, const std::vector<MountInfo>& mounts);
RemoveResult delete_permanently(const QStringList& paths, const std::vector<MountInfo>& mounts);
// Отмена удаления в корзину: вернуть файлы на прежние места (без перезаписи) и убрать .trashinfo.
RemoveResult restore_from_trash(const std::vector<TrashedItem>& items);

}  // namespace bx
