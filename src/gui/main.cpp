// btrfind — графический интерфейс поиска (и консольный режим --cli/--status).
#include "cli.h"
#include "client.h"
#include "mainwindow.h"

#include <unistd.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QDataStream>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLibraryInfo>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

namespace {

// Одно окно на пользователя: повторный запуск передаёт аргументы уже открытому окну.
QString instance_name() { return QStringLiteral("btrfind-gui-%1").arg(getuid()); }

bool forward_to_running(const QString& folder, const QString& query, bool has_query) {
    QLocalSocket s;
    s.connectToServer(instance_name());
    if (!s.waitForConnected(300)) return false;
    QByteArray data;
    QDataStream ds(&data, QIODevice::WriteOnly);
    ds << folder << query << has_query;
    s.write(data);
    s.waitForBytesWritten(1000);
    s.disconnectFromServer();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (bx::is_cli_invocation(argc, argv)) return bx::run_cli(argc, argv);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("btrfind"));
    QApplication::setOrganizationName(QStringLiteral("btrfind"));
    QApplication::setApplicationVersion(QStringLiteral(BTRFIND_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("btrfind"));
    static QTranslator qt_tr;  // «Отмена», «Да» и т.п. в стандартных кнопках Qt
    if (qt_tr.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                   QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qt_tr);

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("Мгновенный поиск файлов на btrfs"));
    QCommandLineOption in(QStringLiteral("in"), QStringLiteral("Искать только в папке."), QStringLiteral("dir"));
    QCommandLineOption socket(QStringLiteral("socket"), QStringLiteral("Сокет демона."), QStringLiteral("path"),
                              QString::fromLatin1(bx::proto::kDefaultSocket));
    QCommandLineOption newwin(QStringLiteral("new-window"), QStringLiteral("Открыть отдельное окно (не передавать запрос открытому)."));
    p.addOptions({in, socket, newwin});
    p.addPositionalArgument(QStringLiteral("запрос"), QStringLiteral("Начальный запрос."));
    p.process(app);

    QString folder = p.value(in);
    if (!folder.isEmpty()) {
        QFileInfo fi(folder);
        folder = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();  // для файла — его папка
    }
    const QString query = p.positionalArguments().join(QLatin1Char(' '));
    const bool has_query = !p.positionalArguments().isEmpty();

    if (!p.isSet(newwin) && forward_to_running(folder, query, has_query)) return 0;

    bx::Client client(p.value(socket));
    bx::MainWindow w(&client);

    // Окно с --new-window — отдельное: не забирает общий сокет у основного окна.
    QLocalServer instance;
    if (!p.isSet(newwin)) {
        QLocalServer::removeServer(instance_name());  // выше проверено: живого окна нет, сокет устарел
        instance.listen(instance_name());
    }
    QObject::connect(&instance, &QLocalServer::newConnection, &w, [&] {
        while (QLocalSocket* s = instance.nextPendingConnection()) {
            QObject::connect(s, &QLocalSocket::readyRead, &w, [s, &w] {
                QDataStream ds(s->readAll());
                QString f, q;
                bool hq = false;
                ds >> f >> q >> hq;
                w.setFolder(f);
                if (hq) w.setQuery(q);
                w.bringToFront();
            });
            QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        }
    });

    // Для снимков экрана (README): набор видимых столбцов из окружения.
    if (const QString cols = qEnvironmentVariable("BTRFIND_COLUMNS"); !cols.isEmpty()) {
        QList<int> list;
        for (const QString& c : cols.split(QLatin1Char(','))) list << c.toInt();
        w.setVisibleColumns(list);
    }
    if (!folder.isEmpty()) w.setFolder(folder);
    if (has_query) w.setQuery(query);
    w.show();
    client.connectToDaemon();
    // Отладка/проверка: снимок окна и выход (BTRFIND_SCREENSHOT=/путь/к/файлу.png).
    if (const QString size = qEnvironmentVariable("BTRFIND_SCREENSHOT_SIZE"); !size.isEmpty())  // например 1100x680
        w.resize(size.section(QLatin1Char('x'), 0, 0).toInt(), size.section(QLatin1Char('x'), 1, 1).toInt());
    if (const QString shot = qEnvironmentVariable("BTRFIND_SCREENSHOT"); !shot.isEmpty())
        QTimer::singleShot(qEnvironmentVariableIntValue("BTRFIND_SCREENSHOT_DELAY") > 0 ? qEnvironmentVariableIntValue("BTRFIND_SCREENSHOT_DELAY") : 3000, &w, [&w, shot] {
            w.grab().save(shot);
            QApplication::quit();
        });
    return app.exec();
}
