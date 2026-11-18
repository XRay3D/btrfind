#pragma once

#include <QIcon>
#include <QString>

namespace bx {
// Иконка по MIME-типу имени файла (по расширению, без чтения файла), с кэшем.
QIcon icon_for(const QString& name, bool dir);
}  // namespace bx
