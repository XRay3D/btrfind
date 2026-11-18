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

using namespace Qt::StringLiterals;

int main(int argc, char** argv) {
    // Сигналы завершения — через signalfd в цикл событий (маска наследуется всеми потоками).
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);

    qSetMessagePattern(u"%{if-warning}W: %{endif}%{if-critical}E: %{endif}%{message}"_s);
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(u"btrfindd"_s);
    QCoreApplication::setApplicationVersion(QString::fromLatin1(BTRFIND_VERSION));

    QCommandLineParser p;
    p.setApplicationDescription(u"Демон индексации btrfs для мгновенного поиска файлов"_s);
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption socket({u"s"_s, u"socket"_s}, u"Путь unix-сокета."_s,
                              u"path"_s, bx::proto::kDefaultSocket);
    QCommandLineOption state({u"d"_s, u"state-dir"_s},
                             u"Каталог для сохранённых индексов."_s, u"dir"_s,
                             u"/var/lib/btrfind"_s);
    QCommandLineOption nosave(u"no-save"_s, u"Не сохранять и не загружать индексы."_s);
    QCommandLineOption interval(u"save-interval"_s, u"Интервал сохранения, минут."_s,
                                u"min"_s, u"10"_s);
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
