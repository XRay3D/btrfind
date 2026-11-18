// btrfindd — демон индексации btrfs для мгновенного поиска файлов по имени.
#include "daemon.h"
#include "protocol.h"

#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSocketNotifier>
#include <QtDebug>

int main(int argc, char** argv) {
    // Сигналы завершения — через signalfd в цикл событий (маска наследуется всеми потоками).
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);

    qSetMessagePattern(QStringLiteral("%{if-warning}W: %{endif}%{if-critical}E: %{endif}%{message}"));
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("btrfindd"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BTRFIND_VERSION));

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("Демон индексации btrfs для мгновенного поиска файлов"));
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption socket({QStringLiteral("s"), QStringLiteral("socket")}, QStringLiteral("Путь unix-сокета."),
                              QStringLiteral("path"), QString::fromLatin1(bx::proto::kDefaultSocket));
    QCommandLineOption state({QStringLiteral("d"), QStringLiteral("state-dir")},
                             QStringLiteral("Каталог для сохранённых индексов."), QStringLiteral("dir"),
                             QStringLiteral("/var/lib/btrfind"));
    QCommandLineOption nosave(QStringLiteral("no-save"), QStringLiteral("Не сохранять и не загружать индексы."));
    QCommandLineOption interval(QStringLiteral("save-interval"), QStringLiteral("Интервал сохранения, минут."),
                                QStringLiteral("min"), QStringLiteral("10"));
    p.addOptions({socket, state, nosave, interval});
    p.process(app);

    bx::Daemon::Options o;
    o.socket = p.value(socket);
    o.state_dir = p.value(state).toStdString();
    o.save = !p.isSet(nosave);
    o.save_interval_min = std::max(1, p.value(interval).toInt());

    bx::Daemon d(o);
    if (!d.start()) return 1;

    const int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    QSocketNotifier sn(sfd, QSocketNotifier::Read);
    QObject::connect(&sn, &QSocketNotifier::activated, &app, [&] {
        signalfd_siginfo si{};
        while (read(sfd, &si, sizeof si) == sizeof si) {}
        qInfo() << "завершение: сохранение индексов…";
        d.shutdown();
        app.quit();
    });
    return app.exec();
}
