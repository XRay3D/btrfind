#pragma once
// Протокол btrfindd <-> клиент: кадры [u32 big-endian длина][CBOR-map].
//
// Клиент -> демон:
//   {op:"search", id, q, filter, case, regex, path, sort:"name|path|size|mtime|btime|ext|owner|group|mode", desc}
//      filter — дополнительные условия обычного синтаксиса (И), например "ext:pdf;doc" или "folder:"
//   {op:"rows", id, from, count}
//   {op:"sum", id, tag, ranges:[[from, to), ...]}  — размер выделенного
//   {op:"status"}
// Демон -> клиент:
//   {op:"result", id, total, ms, bytes, files, dirs, error?}
//   {op:"sum", id, tag, bytes, files, dirs}
//   {op:"rows", id, from, rows:[[name, dir, size, mtime, isdir, btime, mode, uid, gid], ...]}
//   {op:"status", volumes:[{path, subvol, uuid, entries, state, generation}], version}
//   {op:"changed"}  — индекс изменился, имеет смысл повторить запрос

#include <QByteArray>
#include <QCborMap>
#include <QString>

class QIODevice;

namespace bx::proto {

inline constexpr const char* kDefaultSocket = "/run/btrfind/btrfind.sock";
inline constexpr quint32 kMaxFrame = 64u << 20;

QByteArray encode(const QCborMap& m);
void send(QIODevice* dev, const QCborMap& m);

// Накопитель входящих байтов: извлекает целые кадры.
class FrameReader {
public:
    void feed(const QByteArray& data) { buf_.append(data); }
    // true — кадр извлечён в out; false — данных пока мало. error=true при битом потоке.
    bool next(QCborMap& out, bool& error);

private:
    QByteArray buf_;
};

}  // namespace bx::proto
