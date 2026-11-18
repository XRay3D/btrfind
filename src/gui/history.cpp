#include "history.h"

#include <QSettings>

using namespace Qt::StringLiterals;

namespace bx {

SearchHistory::SearchHistory() {
    QSettings s;
    enabled_ = s.value(u"history/enabled"_s, true).toBool();
    items_ = s.value(u"history/queries"_s).toStringList();
}

void SearchHistory::save() const {
    QSettings s;
    s.setValue(u"history/enabled"_s, enabled_);
    s.setValue(u"history/queries"_s, items_);
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
