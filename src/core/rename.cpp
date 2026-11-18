#include "rename.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QFile>
#include <QHash>
#include <QSet>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>

using namespace Qt::StringLiterals;

namespace bx {
namespace {

QString join(const QString& dir, const QString& name) {
    return dir == u"/"_s ? u'/' + name : dir + u'/' + name;
}

bool exists(const QString& path) {
    struct stat st {};
    return lstat(QFile::encodeName(path).constData(), &st) == 0;
}

// Номера групп, на которые ссылается новый формат.
QList<int> referenced_groups(const QString& f, bool regex) {
    QList<int> out;
    for (qsizetype i = 0; i + 1 < f.size(); ++i) {
        const QChar c = f[i], n = f[i + 1];
        if (c == u'%') {
            if (n == u'%') {
                ++i;
                continue;
            }
            if (n >= u'1' && n <= u'9') out << n.digitValue();
        } else if (regex && (c == u'\\' || c == u'$') && n.isDigit()) {
            out << n.digitValue();
            ++i;
        }
    }
    return out;
}

// Переименование без перезаписи существующего файла.
int do_rename(const QString& from, const QString& to) {
    const QByteArray f = QFile::encodeName(from), t = QFile::encodeName(to);
    if (renameat2(AT_FDCWD, f.constData(), AT_FDCWD, t.constData(), RENAME_NOREPLACE) == 0) return 0;
    if (errno == EINVAL || errno == ENOSYS) {  // ФС без RENAME_NOREPLACE
        struct stat st {};
        if (lstat(t.constData(), &st) == 0) {
            errno = EEXIST;
            return -1;
        }
        return ::rename(f.constData(), t.constData());
    }
    return -1;
}

QString err_text(const QString& from, const QString& to_name) {
    return u"%1 → %2: %3"_s.arg(from, to_name, QString::fromLocal8Bit(strerror(errno)));
}

RenameResult run(const std::vector<PlanItem>& plan, bool deepest_first) {
    struct Op {
        QString dir, old_name, fin, tmp;
        bool failed = false;
    };
    // Уровни по глубине папки: сначала глубже (содержимое каталога до самого каталога), для отката — наоборот.
    std::map<int, std::vector<Op>> levels;
    for (const PlanItem& it : plan)
        if (it.status == RenameStatus::Rename)
            levels[it.dir == u"/"_s ? 0 : int(it.dir.count(u'/'))].push_back(
                {it.dir, it.old_name, it.new_name, {}});

    RenameResult res;
    auto process = [&](std::vector<Op>& ops) {
        QSet<QString> olds;
        for (const Op& op : ops) olds.insert(join(op.dir, op.old_name));
        // 1) цели, занятые файлами этой же партии (обмен a<->b, цепочки): сначала во временное имя
        int k = 0;
        for (Op& op : ops) {
            if (!olds.contains(join(op.dir, op.fin))) continue;
            for (;;) {
                op.tmp = u".btrfind-tmp-%1-%2"_s.arg(getpid()).arg(k++);
                if (do_rename(join(op.dir, op.old_name), join(op.dir, op.tmp)) == 0) break;
                if (errno != EEXIST) {
                    res.errors << err_text(join(op.dir, op.old_name), op.fin);
                    op.failed = true;
                    op.tmp.clear();
                    break;
                }
            }
        }
        // 2) прямые переименования
        for (Op& op : ops) {
            if (op.failed || !op.tmp.isEmpty()) continue;
            if (do_rename(join(op.dir, op.old_name), join(op.dir, op.fin)) == 0)
                res.done.push_back({op.dir, op.old_name, op.fin});
            else
                res.errors << err_text(join(op.dir, op.old_name), op.fin);
        }
        // 3) из временных имён в окончательные
        for (Op& op : ops) {
            if (op.failed || op.tmp.isEmpty()) continue;
            if (do_rename(join(op.dir, op.tmp), join(op.dir, op.fin)) == 0) {
                res.done.push_back({op.dir, op.old_name, op.fin});
                continue;
            }
            res.errors << err_text(join(op.dir, op.old_name), op.fin);
            if (do_rename(join(op.dir, op.tmp), join(op.dir, op.old_name)) != 0)
                res.errors << u"файл остался под временным именем %1"_s.arg(join(op.dir, op.tmp));
        }
    };
    if (deepest_first)
        for (auto it = levels.rbegin(); it != levels.rend(); ++it) process(it->second);
    else
        for (auto& [depth, ops] : levels) process(ops);
    return res;
}

}  // namespace

// ---------------------------------------------------------------- шаблоны

bool RenamePattern::compile(const QString& old_fmt, const QString& new_fmt, const RenameOptions& o, QString& err) {
    regex_ = o.regex;
    new_fmt_ = new_fmt;
    QRegularExpression::PatternOptions po =
        QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::DotMatchesEverythingOption;
    if (!o.case_sensitive) po |= QRegularExpression::CaseInsensitiveOption;

    bool seen[10] = {};
    QString pat;
    if (regex_) {
        pat = old_fmt;
    } else {
        QString lit;
        auto flush = [&] {
            pat += QRegularExpression::escape(lit);
            lit.clear();
        };
        for (qsizetype i = 0; i < old_fmt.size(); ++i) {
            const QChar c = old_fmt[i];
            if (c == u'%' && i + 1 < old_fmt.size()) {
                const QChar n = old_fmt[i + 1];
                if (n == u'%') {
                    lit += u'%';
                    ++i;
                    continue;
                }
                if (n >= u'1' && n <= u'9') {
                    flush();
                    const int d = n.digitValue();
                    pat += seen[d] ? u"\\k<g%1>"_s.arg(d) : u"(?<g%1>.*?)"_s.arg(d);
                    seen[d] = true;
                    ++i;
                    continue;
                }
            }
            lit += c;
        }
        flush();
    }
    re_ = QRegularExpression(QRegularExpression::anchoredPattern(pat), po);
    if (!re_.isValid()) {
        err = u"старый формат: %1"_s.arg(re_.errorString());
        return false;
    }
    for (int g : referenced_groups(new_fmt, regex_)) {
        if (regex_ ? g > re_.captureCount() : (g < 1 || !seen[g])) {
            err = regex_ ? u"в выражении нет группы %1"_s.arg(g)
                         : u"в старом формате нет %%1"_s.arg(g);
            return false;
        }
    }
    re_.optimize();
    return true;
}

bool RenamePattern::apply(const QString& name, int counter, QString& out) const {
    const QRegularExpressionMatch m = re_.match(name);
    if (!m.hasMatch()) return false;
    auto group = [&](int d) { return regex_ ? m.captured(d) : m.captured(u"g%1"_s.arg(d)); };
    const QString& f = new_fmt_;
    out.clear();
    for (qsizetype i = 0; i < f.size();) {
        const QChar c = f[i];
        if (c == u'%' && i + 1 < f.size()) {
            const QChar n = f[i + 1];
            if (n == u'%') {
                out += u'%';
                i += 2;
                continue;
            }
            if (n == u'n') {
                out += QString::number(counter);
                i += 2;
                continue;
            }
            if (n == u'0') {  // %03n
                qsizetype j = i + 1;
                while (j < f.size() && f[j].isDigit()) ++j;
                if (j < f.size() && f[j] == u'n') {
                    const int width = f.mid(i + 1, j - i - 1).toInt();
                    out += u"%1"_s.arg(counter, width, 10, u'0');
                    i = j + 1;
                    continue;
                }
            }
            if (n >= u'1' && n <= u'9') {
                out += group(n.digitValue());
                i += 2;
                continue;
            }
        }
        if (regex_ && (c == u'\\' || c == u'$') && i + 1 < f.size() && f[i + 1].isDigit()) {
            out += m.captured(f[i + 1].digitValue());
            i += 2;
            continue;
        }
        out += c;
        ++i;
    }
    return true;
}

QString suggest_old_format(const QStringList& names) {
    auto esc = [](QString s) { return s.replace(u'%', u"%%"_s); };
    if (names.isEmpty()) return u"%1"_s;
    qsizetype minlen = names[0].size();
    for (const QString& n : names) minlen = std::min(minlen, n.size());
    qsizetype pre = 0;
    while (pre < minlen) {
        const QChar c = names[0][pre];
        bool all = true;
        for (const QString& n : names)
            if (n[pre] != c) all = false;
        if (!all) break;
        ++pre;
    }
    qsizetype suf = 0;
    while (suf < minlen - pre) {
        const QChar c = names[0][names[0].size() - 1 - suf];
        bool all = true;
        for (const QString& n : names)
            if (n[n.size() - 1 - suf] != c) all = false;
        if (!all) break;
        ++suf;
    }
    if (pre == minlen && names.size() > 1) {  // все имена одинаковые
        bool same = true;
        for (const QString& n : names) same &= n == names[0];
        if (same) return esc(names[0]);
    }
    // Числа целиком в переменную часть: IMG_0001/IMG_0002 -> IMG_%1, а не IMG_000%1
    auto digit_at = [&](qsizetype i) { return i >= 0 && i < names[0].size() && names[0][i].isDigit(); };
    while (pre > 0 && digit_at(pre - 1)) --pre;
    while (suf > 0 && digit_at(names[0].size() - suf)) --suf;
    return esc(names[0].left(pre)) + u"%1"_s + esc(names[0].right(suf));
}

// ---------------------------------------------------------------- план

QString name_problem(const QString& n) {
    if (n.isEmpty()) return u"пустое имя"_s;
    if (n.contains(u'/')) return u"«/» в имени"_s;
    if (n.contains(QChar(0))) return u"недопустимый символ"_s;
    if (n == u"."_s || n == u".."_s) return u"недопустимое имя"_s;
    if (QFile::encodeName(n).size() > 255) return u"имя длиннее 255 байт"_s;
    return {};
}

QString status_text(const PlanItem& it) {
    switch (it.status) {
        case RenameStatus::Rename: return u"будет переименован"_s;
        case RenameStatus::Unchanged: return u"без изменений"_s;
        case RenameStatus::NoMatch: return u"не подходит под шаблон"_s;
        case RenameStatus::Invalid: return it.message;
        case RenameStatus::Duplicate: return u"повторяется новое имя"_s;
        case RenameStatus::Exists: return u"файл с таким именем уже есть"_s;
    }
    return {};
}

namespace {
void split(const QString& path, QString& dir, QString& name) {
    const qsizetype s = path.lastIndexOf(u'/');
    dir = s <= 0 ? u"/"_s : path.left(s);
    name = path.mid(s + 1);
}

void check_collisions(std::vector<PlanItem>& plan) {
    QHash<QString, int> final_count;
    QSet<QString> renamed_olds;
    for (const PlanItem& it : plan) {
        const bool ren = it.status == RenameStatus::Rename;
        final_count[join(it.dir, ren ? it.new_name : it.old_name)]++;
        if (ren) renamed_olds.insert(join(it.dir, it.old_name));
    }
    for (PlanItem& it : plan) {
        if (it.status != RenameStatus::Rename) continue;
        const QString target = join(it.dir, it.new_name);
        if (final_count.value(target) > 1)
            it.status = RenameStatus::Duplicate;
        else if (!renamed_olds.contains(target) && exists(target))
            it.status = RenameStatus::Exists;
    }
}
}  // namespace

std::vector<PlanItem> plan_rename(const QStringList& paths, const RenamePattern& p) {
    std::vector<PlanItem> plan;
    plan.reserve(size_t(paths.size()));
    int counter = 0;
    for (const QString& path : paths) {
        PlanItem it;
        split(path, it.dir, it.old_name);
        ++counter;
        if (!p.apply(it.old_name, counter, it.new_name)) {
            it.status = RenameStatus::NoMatch;
            it.new_name = it.old_name;
        } else if (it.new_name == it.old_name) {
            it.status = RenameStatus::Unchanged;
        } else if (const QString pr = name_problem(it.new_name); !pr.isEmpty()) {
            it.status = RenameStatus::Invalid;
            it.message = pr;
        } else {
            it.status = RenameStatus::Rename;
        }
        plan.push_back(std::move(it));
    }
    check_collisions(plan);
    return plan;
}

std::vector<PlanItem> plan_single(const QString& path, const QString& new_name) {
    PlanItem it;
    split(path, it.dir, it.old_name);
    it.new_name = new_name;
    if (new_name == it.old_name) {
        it.status = RenameStatus::Unchanged;
    } else if (const QString pr = name_problem(new_name); !pr.isEmpty()) {
        it.status = RenameStatus::Invalid;
        it.message = pr;
    } else {
        it.status = RenameStatus::Rename;
    }
    std::vector<PlanItem> plan{it};
    check_collisions(plan);
    return plan;
}

int plan_summary(const std::vector<PlanItem>& plan, int* errors) {
    int n = 0, e = 0;
    for (const PlanItem& it : plan) {
        n += it.status == RenameStatus::Rename;
        e += it.is_error();
    }
    if (errors) *errors = e;
    return n;
}

// ---------------------------------------------------------------- выполнение

QString RenameDone::old_path() const { return join(dir, old_name); }
QString RenameDone::new_path() const { return join(dir, new_name); }

RenameResult execute_rename(const std::vector<PlanItem>& plan) { return run(plan, true); }

RenameResult undo_rename(const std::vector<RenameDone>& done) {
    std::vector<PlanItem> plan;
    for (const RenameDone& d : done) {
        PlanItem it;
        it.dir = d.dir;
        it.old_name = d.new_name;
        it.new_name = d.old_name;
        it.status = RenameStatus::Rename;
        plan.push_back(it);
    }
    // Сначала мельче: каталог вернуть до его содержимого (пути содержимого записаны со старым именем каталога).
    return run(plan, false);
}

}  // namespace bx
