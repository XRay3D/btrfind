#include "protocol.h"

#include <QCborValue>
#include <QIODevice>
#include <QtEndian>

namespace bx::proto {

QByteArray encode(const QCborMap& m) {
    const QByteArray body = QCborValue(m).toCbor();
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(body.size()), frame.data());
    frame.append(body);
    return frame;
}

void send(QIODevice* dev, const QCborMap& m) { dev->write(encode(m)); }

bool FrameReader::next(QCborMap& out, bool& error) {
    error = false;
    if (buf_.size() < 4) return false;
    const quint32 len = qFromBigEndian<quint32>(buf_.constData());
    if (len > kMaxFrame) {
        error = true;
        return false;
    }
    if (quint32(buf_.size()) < 4 + len) return false;
    QCborParserError perr;
    const QCborValue v = QCborValue::fromCbor(buf_.mid(4, len), &perr);
    buf_.remove(0, 4 + len);
    if (perr.error != QCborError::NoError || !v.isMap()) {
        error = true;
        return false;
    }
    out = v.toMap();
    return true;
}

}  // namespace bx::proto
