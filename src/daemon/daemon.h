#pragma once
// Оркестратор: обнаружение томов, очередь задач (загрузка/скан/догон), точки монтирования,
// сохранение, живые изменения и сервер запросов.

#include "mounts.h"
#include "volume.h"

#include <QCborMap>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <deque>
#include <memory>
#include <shared_mutex>
#include <string>

class QSocketNotifier;

namespace bx {

class Server;
class Watcher;

class Daemon : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString socket;
        std::string state_dir;
        bool save = true;
        int save_interval_min = 10;
    };

    explicit Daemon(Options o, QObject* parent = nullptr);
    ~Daemon() override;

    bool start();
    void shutdown();  // сохранить индексы перед выходом

    // Тома: индекс в deque = Volume::id. Добавление — под эксклюзивной блокировкой.
    std::shared_mutex volumes_mu;
    std::deque<std::unique_ptr<Volume>> volumes;

    QCborMap status();

private:
    enum class Task { Init, CatchUp };

    void discover();
    Volume* find_volume(const FsUuid& uuid, uint64_t subvol);
    Volume* add_volume(const std::string& path, bool from_mount);
    void schedule(Volume* v, Task t);
    void task_done(Volume* v, bool ok, const std::string& log);
    void after_ready(Volume* v);
    void apply_mountpoints(Volume* v);
    void add_nested(Volume* v);
    void save_all(bool final);
    void mounts_changed();
    void log(Volume* v, const std::string& msg);

    Options o_;
    Watcher* watcher_ = nullptr;
    Server* server_ = nullptr;
    QThreadPool maint_;  // один поток: задачи томов выполняются по очереди
    QTimer save_timer_;
    QTimer mounts_timer_;
    QSocketNotifier* mounts_sn_ = nullptr;
    int mounts_fd_ = -1;
    std::vector<MountInfo> mounts_;
    std::vector<bool> from_mount_;
    std::vector<char> busy_;  // задача тома в очереди/выполняется
    bool live_ok_ = true;     // fanotify доступен
};

}  // namespace bx
