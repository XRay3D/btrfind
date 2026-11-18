#pragma once
// Живые изменения через fanotify (аналог чтения USN-журнала в реальном времени).

#include <string>

namespace bx {

struct WatchOptions {
    bool filesystem = true;  // FAN_MARK_FILESYSTEM на всю ФС (root); false — метка на один каталог (без root)
    bool modify = false;     // также FAN_CLOSE_WRITE | FAN_ATTRIB (размер/дата)
    bool quiet = false;      // не печатать каждое событие, только итоговую статистику
};

// Блокирующий цикл до Ctrl+C. Возвращает код выхода.
int fan_watch(const std::string& path, const WatchOptions& opt);

}  // namespace bx
