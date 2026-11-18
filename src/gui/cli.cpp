#include "cli.h"

#include "client.h"

#include <QCborArray>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QTextStream>

#include <cstring>

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
    QCoreApplication::setApplicationName(QStringLiteral("btrfind"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BTRFIND_VERSION));
    QTextStream out(stdout), err(stderr);

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral(
        "Мгновенный поиск файлов на btrfs. Без --cli запускается графический интерфейс.\n\n"
        "Синтаксис запроса: слова через пробел (И), \"фраза\", !слово (НЕ), a | b (ИЛИ), *.pdf, путь/часть,\n"
        "regex:выражение, file: folder: ext:pdf;docx size:>10mb dm:today dm:>2026-01-01 path:слово"));
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption cli(QStringLiteral("cli"), QStringLiteral("Консольный поиск."));
    QCommandLineOption status(QStringLiteral("status"), QStringLiteral("Состояние демона и томов."));
    QCommandLineOption socket(QStringLiteral("socket"), QStringLiteral("Сокет демона."), QStringLiteral("path"),
                              QString::fromLatin1(proto::kDefaultSocket));
    QCommandLineOption limit({QStringLiteral("n"), QStringLiteral("limit")}, QStringLiteral("Сколько строк вывести (0 — все)."),
                             QStringLiteral("N"), QStringLiteral("50"));
    QCommandLineOption sort(QStringLiteral("sort"), QStringLiteral("name | path | size | mtime"), QStringLiteral("key"),
                            QStringLiteral("name"));
    QCommandLineOption desc(QStringLiteral("desc"), QStringLiteral("Сортировка по убыванию."));
    QCommandLineOption cs(QStringLiteral("case"), QStringLiteral("С учётом регистра."));
    QCommandLineOption rx(QStringLiteral("regex"), QStringLiteral("Весь запрос — регулярное выражение (PCRE2)."));
    QCommandLineOption path(QStringLiteral("path"), QStringLiteral("Сопоставлять с полным путём."));
    QCommandLineOption details(QStringLiteral("l"), QStringLiteral("Подробно: размер и дата."));
    p.addOptions({cli, status, socket, limit, sort, desc, cs, rx, path, details});
    p.addPositionalArgument(QStringLiteral("запрос"), QStringLiteral("Строка поиска."));
    p.process(app);

    Client c(p.value(socket));
    if (!c.waitConnected(3000)) {
        err << "Нет связи с демоном (" << p.value(socket) << "): " << "запустите sudo systemctl start btrfindd\n";
        return 2;
    }

    if (p.isSet(status)) {
        c.requestStatus();
        const QCborMap m = c.waitFor(QStringLiteral("status"), -1, 5000);
        if (m.isEmpty()) {
            err << "Демон не ответил\n";
            return 2;
        }
        out << "btrfindd " << m.value("version").toString() << ", событий fanotify: " << m.value("events").toInteger()
            << "\n";
        for (const QCborValue& v : m.value("volumes").toArray()) {
            const QCborMap vm = v.toMap();
            out << QStringLiteral("  %1  subvol %2  %3  записей %4  %5\n")
                       .arg(vm.value("path").toString(), -24)
                       .arg(vm.value("subvol").toInteger())
                       .arg(vm.value("state").toString(), -12)
                       .arg(vm.value("entries").toInteger())
                       .arg(vm.value("live").toBool() ? QStringLiteral("живые изменения") : QStringLiteral("без живых изменений"));
        }
        return 0;
    }

    SearchOpts o;
    o.case_sensitive = p.isSet(cs);
    o.regex = p.isSet(rx);
    o.path = p.isSet(path);
    o.sort = p.value(sort);
    o.desc = p.isSet(desc);
    const qint64 id = c.search(p.positionalArguments().join(QLatin1Char(' ')), o);
    const QCborMap r = c.waitFor(QStringLiteral("result"), id, 60000);
    if (r.isEmpty()) {
        err << "Демон не ответил\n";
        return 2;
    }
    if (!r.value("error").toString().isEmpty()) {
        err << "Ошибка запроса: " << r.value("error").toString() << "\n";
        return 1;
    }
    const qint64 total = r.value("total").toInteger();
    qint64 lim = p.value(limit).toLongLong();
    if (lim <= 0 || lim > total) lim = total;
    for (qint64 from = 0; from < lim; from += 5000) {
        c.requestRows(id, from, std::min<qint64>(5000, lim - from));
        const QCborMap rows = c.waitFor(QStringLiteral("rows"), id, 30000);
        for (const QCborValue& rv : rows.value("rows").toArray()) {
            const QCborArray a = rv.toArray();
            const QString full = a.at(1).toString() == "/" ? "/" + a.at(0).toString()
                                                           : a.at(1).toString() + "/" + a.at(0).toString();
            if (p.isSet(details))
                out << QStringLiteral("%1  %2  ")
                           .arg(a.at(4).toBool() ? QStringLiteral("<dir>") : QString::number(a.at(2).toInteger()), 14)
                           .arg(QDateTime::fromSecsSinceEpoch(a.at(3).toInteger()).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
            out << full << (a.at(4).toBool() ? "/" : "") << "\n";
        }
    }
    err << total << " объектов, " << QString::number(r.value("ms").toDouble(), 'f', 1) << " мс\n";
    return 0;
}

}  // namespace bx
