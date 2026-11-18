#include "history.h"

#include <QSettings>

namespace bx {

SearchHistory::SearchHistory() {
    QSettings s;
    enabled_ = s.value(QStringLiteral("history/enabled"), true).toBool();
    items_ = s.value(QStringLiteral("history/queries")).toStringList();
}

void SearchHistory::save() const {
    QSettings s;
    s.setValue(QStringLiteral("history/enabled"), enabled_);
    s.setValue(QStringLiteral("history/queries"), items_);
}

void SearchHistory::add(const QString& query) {
    const QString q = query.trimmed();
    if (!enabled_ || q.isEmpty()) return;
    if (!items_.isEmpty() && items_.first() == q) return;
    items_.removeAll(q);
    items_.prepend(q);
    while (items_.size() > kMax) items_.removeLast();
    save();
}

void SearchHistory::remove(const QString& query) {
    items_.removeAll(query);
    save();
}

void SearchHistory::clear() {
    items_.clear();
    save();
}

void SearchHistory::setEnabled(bool on) {
    enabled_ = on;
    if (!on) items_.clear();
    save();
}

}  // namespace bx
