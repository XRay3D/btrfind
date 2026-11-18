#include "fold.h"

#include <QString>

namespace bx {

bool is_ascii(std::string_view s) {
    for (unsigned char c : s)
        if (c & 0x80) return false;
    return true;
}

void fold_case_append(std::string_view s, std::string& out) {
    if (is_ascii(s)) {
        const size_t base = out.size();
        out.append(s);
        for (size_t i = base; i < out.size(); ++i)
            if (out[i] >= 'A' && out[i] <= 'Z') out[i] = char(out[i] - 'A' + 'a');
        return;
    }
    const QByteArray f = QString::fromUtf8(s.data(), qsizetype(s.size())).toCaseFolded().toUtf8();
    out.append(f.constData(), size_t(f.size()));
}

std::string fold_case(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    fold_case_append(s, r);
    return r;
}

}  // namespace bx
