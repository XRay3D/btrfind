#include "cli.h"

#include "client.h"

#include <QCborArray>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QTextStream>

#include <cstring>

using namespace Qt::StringLiterals;

namespace bx {

bool is_cli_invocation(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--cli") || !strcmp(argv[i], "--status") || !strcmp(argv[i], "-h") ||
            !strcmp(argv[i], "--help") || !strcmp(argv[i], "-v") || !strcmp(argv[i], "--version"))
            return true;
    return false;
}

int run_cli(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(u"btrfind"_s);
    QCoreApplication::setApplicationVersion(QString::fromLatin1(BTRFIND_VERSION));
    QTextStream out(stdout), err(stderr);

    QCommandLineParser p;
    p.setApplicationDescription(
        u"Мгновенный поиск файлов на btrfs. Без --cli запускается графический интерфейс.\n\n"
        u"Синтаксис запроса: слова через пробел (И), \"фраза\", !слово (НЕ), a | b (ИЛИ), *.pdf, путь/часть,\n"
        u"regex:выражение, file: folder: ext:pdf;docx size:>10mb dm:today dm:>2026-01-01 path:слово"_s);
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption cli(u"cli"_s, u"Консольный поиск."_s);
    QCommandLineOption status(u"status"_s, u"Состояние демона и томов."_s);
    QCommandLineOption socket(u"socket"_s, u"Сокет демона."_s, u"path"_s,
                              proto::kDefaultSocket);
    QCommandLineOption limit({u"n"_s, u"limit"_s}, u"Сколько строк вывести (0 — все)."_s,
                             u"N"_s, u"50"_s);
    QCommandLineOption sort(u"sort"_s, u"name | path | size | mtime"_s, u"key"_s,
                            u"name"_s);
    QCommandLineOption desc(u"desc"_s, u"Сортировка по убыванию."_s);
    QCommandLineOption cs(u"case"_s, u"С учётом регистра."_s);
    QCommandLineOption rx(u"regex"_s, u"Весь запрос — регулярное выражение (PCRE2)."_s);
    QCommandLineOption path(u"path"_s, u"Сопоставлять с полным путём."_s);
    QCommandLineOption details(u"l"_s, u"Подробно: размер и дата."_s);
    p.addOptions({cli, status, socket, limit, sort, desc, cs, rx, path, details});
    p.addPositionalArgument(u"запрос"_s, u"Строка поиска."_s);
    p.process(app);

    Client c(p.value(socket));
    if (!c.waitConnected(3000)) {
        err << u"Нет связи с демоном ("_s << p.value(socket) << u"): "_s << u"запустите sudo systemctl start btrfindd\n"_s;
        return 2;
    }

    if (p.isSet(status)) {
        c.requestStatus();
        const QCborMap m = c.waitFor(u"status"_s, -1, 5000);
        if (m.isEmpty()) {
            err << u"Демон не ответил\n"_s;
            return 2;
        }
        out << u"btrfindd "_s << m.value(u"version"_s).toString() << u", событий fanotify: "_s << m.value(u"events"_s).toInteger()
            << u"\n"_s;
        for (const QCborValue& v : m.value(u"volumes"_s).toArray()) {
            const QCborMap vm = v.toMap();
            out << u"  %1  subvol %2  %3  записей %4  %5\n"_s
                       .arg(vm.value(u"path"_s).toString(), -24)
                       .arg(vm.value(u"subvol"_s).toInteger())
                       .arg(vm.value(u"state"_s).toString(), -12)
                       .arg(vm.value(u"entries"_s).toInteger())
                       .arg(vm.value(u"live"_s).toBool() ? u"живые изменения"_s : u"без живых изменений"_s);
        }
        return 0;
    }

    SearchOpts o;
    o.case_sensitive = p.isSet(cs);
    o.regex = p.isSet(rx);
    o.path = p.isSet(path);
    o.sort = p.value(sort);
    o.desc = p.isSet(desc);
    const qint64 id = c.search(p.positionalArguments().join(u' '), o);
    const QCborMap r = c.waitFor(u"result"_s, id, 60000);
    if (r.isEmpty()) {
        err << u"Демон не ответил\n"_s;
        return 2;
    }
    if (!r.value(u"error"_s).toString().isEmpty()) {
        err << u"Ошибка запроса: "_s << r.value(u"error"_s).toString() << u"\n"_s;
        return 1;
    }
    const qint64 total = r.value(u"total"_s).toInteger();
    qint64 lim = p.value(limit).toLongLong();
    if (lim <= 0 || lim > total) lim = total;
    for (qint64 from = 0; from < lim; from += 5000) {
        c.requestRows(id, from, std::min<qint64>(5000, lim - from));
        const QCborMap rows = c.waitFor(u"rows"_s, id, 30000);
        for (const QCborValue& rv : rows.value(u"rows"_s).toArray()) {
            const QCborArray a = rv.toArray();
            const QString full = a.at(1).toString() == u"/"_s ? u"/"_s + a.at(0).toString()
                                                           : a.at(1).toString() + u"/"_s + a.at(0).toString();
            if (p.isSet(details))
                out << u"%1  %2  "_s
                           .arg(a.at(4).toBool() ? u"<dir>"_s : QString::number(a.at(2).toInteger()), 14)
                           .arg(QDateTime::fromSecsSinceEpoch(a.at(3).toInteger()).toString(u"yyyy-MM-dd HH:mm"_s));
            out << full << (a.at(4).toBool() ? u"/"_s : u""_s) << u"\n"_s;
        }
    }
    err << total << u" объектов, "_s << QString::number(r.value(u"ms"_s).toDouble(), 'f', 1) << u" мс\n"_s;
    return 0;
}

}  // namespace bx
