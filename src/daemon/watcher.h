#pragma once
// Живые изменения: fanotify с FAN_MARK_FILESYSTEM на каждую ФС btrfs (метка ставится через
// отсоединённое монтирование верхнего подтома). Каждое событие -> Reconciler::reconcile_name.

#include "btrfs_tree.h"
#include "volume.h"

#include <QObject>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QSocketNotifier;

namespace bx {

class Watcher : public QObject {
    Q_OBJECT
public:
    // Поиск тома по (uuid ФС, id подтома); вызывается в главном потоке.
    using Resolver = std::function<Volume*(const FsUuid&, uint64_t subvol)>;

    explicit Watcher(Resolver r, QObject* parent = nullptr);
    ~Watcher() override;

    bool watch_fs(const FsUuid& uuid, const std::string& device, std::string& err);
    bool watching(const FsUuid& uuid) const;
    void drain();  // немедленно обработать все события в очередях

    struct Stats {
        uint64_t events = 0, applied = 0, unknown = 0, overflows = 0;
    } stats;

signals:
    void changed();
    void overflow(QByteArray uuid);

private:
    struct Group {
        FsUuid uuid{};
        int fan = -1;
        int top = -1;
        QSocketNotifier* sn = nullptr;
    };
    void read_events(Group& g);

    Resolver resolve_;
    std::vector<std::unique_ptr<Group>> groups_;
};

}  // namespace bx
