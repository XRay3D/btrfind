#pragma once
// История поиска: последние запросы (свежие сверху, без повторов), хранится в QSettings.

#include <QStringList>

namespace bx {

class SearchHistory {
public:
    static constexpr int kMax = 200;

    SearchHistory();
    void add(const QString& query);
    void remove(const QString& query);
    void clear();
    const QStringList& items() const { return items_; }
    bool enabled() const { return enabled_; }
    void setEnabled(bool on);  // выключение заодно стирает историю

private:
    void save() const;
    QStringList items_;
    bool enabled_ = true;
};

}  // namespace bx
