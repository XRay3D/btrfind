#pragma once
// Групповое переименование (как «Multi-file Rename» в Everything): шаблоны старого/нового имени,
// проверки, безопасное выполнение (без перезаписи, с обменом имён) и отмена.
//
// Старый формат: текст с %1…%9 — переменные части (имя должно совпасть целиком), %% — знак процента.
// В режиме regex старый формат — регулярное выражение PCRE на всё имя.
// Новый формат: %1…%9 (а в режиме regex также \1 и $1) — подстановка частей,
//   %n — порядковый номер, %0Nn — номер с ведущими нулями до ширины N (%03n -> 001), %% — знак процента.

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <vector>

namespace bx {

struct RenameOptions {
    bool regex = false;
    bool case_sensitive = false;
};

class RenamePattern {
public:
    bool compile(const QString& old_fmt, const QString& new_fmt, const RenameOptions& o, QString& err);
    // false — имя не подходит под старый формат. counter — порядковый номер (с 1).
    bool apply(const QString& name, int counter, QString& out) const;

private:
    QRegularExpression re_;
    QString new_fmt_;
    bool regex_ = false;
};

// Старый формат по умолчанию: общий префикс + %1 + общий суффикс (IMG_0001.jpg, IMG_0002.jpg -> IMG_%1.jpg).
QString suggest_old_format(const QStringList& names);

enum class RenameStatus { Rename, Unchanged, NoMatch, Invalid, Duplicate, Exists };

struct PlanItem {
    QString dir, old_name, new_name;
    RenameStatus status = RenameStatus::Unchanged;
    QString message;  // для ошибок
    bool is_error() const {
        return status == RenameStatus::Invalid || status == RenameStatus::Duplicate || status == RenameStatus::Exists;
    }
};

QString status_text(const PlanItem& it);
QString name_problem(const QString& name);  // "" — имя допустимо

// paths — полные пути в порядке таблицы (для %n).
std::vector<PlanItem> plan_rename(const QStringList& paths, const RenamePattern& p);
// Один файл с готовым новым именем.
std::vector<PlanItem> plan_single(const QString& path, const QString& new_name);
// Сколько будет переименовано; ошибок — в errors.
int plan_summary(const std::vector<PlanItem>& plan, int* errors = nullptr);

struct RenameDone {
    QString dir, old_name, new_name;
    QString old_path() const;
    QString new_path() const;
};

struct RenameResult {
    std::vector<RenameDone> done;
    QStringList errors;
};

RenameResult execute_rename(const std::vector<PlanItem>& plan);
// Откат партии: только для файлов, которые всё ещё носят новое имя.
RenameResult undo_rename(const std::vector<RenameDone>& done);

}  // namespace bx
