#include "query.h"

#include "fold.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace bx {
namespace {

struct Tok {
    std::string text;
    bool quoted = false;
    bool or_op = false;
};

std::vector<Tok> tokenize(std::string_view s) {
    std::vector<Tok> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        if (i >= s.size()) break;
        if (s[i] == '|' && (i + 1 == s.size() || std::isspace((unsigned char)s[i + 1]))) {
            out.push_back({"", false, true});
            ++i;
            continue;
        }
        Tok t;
        bool inq = false;
        while (i < s.size() && (inq || !std::isspace((unsigned char)s[i]))) {
            if (s[i] == '"') {
                inq = !inq;
                t.quoted = true;
            } else {
                t.text += s[i];
            }
            ++i;
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::string lower_ascii(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower((unsigned char)c));
    return r;
}

time_t day_start(time_t t) {
    tm lt{};
    localtime_r(&t, &lt);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    return mktime(&lt);
}

time_t add_days(time_t t, int d) {
    tm lt{};
    localtime_r(&t, &lt);
    lt.tm_mday += d;
    lt.tm_isdst = -1;
    return mktime(&lt);
}

bool parse_date(std::string_view s, time_t& out) {
    int y = 0, m = 0, d = 0;
    char tail = 0;
    const std::string str(s);
    if (sscanf(str.c_str(), "%d-%d-%d%c", &y, &m, &d, &tail) != 3 &&
        sscanf(str.c_str(), "%d.%d.%d%c", &d, &m, &y, &tail) != 3)
        return false;
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    tm lt{};
    lt.tm_year = y - 1900;
    lt.tm_mon = m - 1;
    lt.tm_mday = d;
    lt.tm_isdst = -1;
    out = mktime(&lt);
    return out != time_t(-1);
}

}  // namespace

bool parse_size(std::string_view s, uint64_t& out) {
    const std::string str = lower_ascii(s);
    char* end = nullptr;
    const double v = strtod(str.c_str(), &end);
    if (end == str.c_str() || v < 0 || !std::isfinite(v)) return false;
    const std::string unit(end);
    double mul = 1;
    if (unit.empty() || unit == "b") mul = 1;
    else if (unit == "k" || unit == "kb") mul = 1024.0;
    else if (unit == "m" || unit == "mb") mul = 1024.0 * 1024;
    else if (unit == "g" || unit == "gb") mul = 1024.0 * 1024 * 1024;
    else if (unit == "t" || unit == "tb") mul = 1024.0 * 1024 * 1024 * 1024;
    else return false;
    out = uint64_t(v * mul);
    return true;
}

bool parse_size_filter(std::string_view v, uint64_t& mn, uint64_t& mx) {
    constexpr uint64_t K = 1024, M = K * K;
    const std::string s = lower_ascii(v);
    mn = 0;
    mx = UINT64_MAX;
    if (s == "empty") { mx = 0; return true; }
    if (s == "tiny") { mx = 10 * K; return true; }
    if (s == "small") { mn = 10 * K + 1; mx = 100 * K; return true; }
    if (s == "medium") { mn = 100 * K + 1; mx = M; return true; }
    if (s == "large") { mn = M + 1; mx = 16 * M; return true; }
    if (s == "huge") { mn = 16 * M + 1; mx = 128 * M; return true; }
    if (s == "gigantic") { mn = 128 * M + 1; return true; }
    if (const size_t r = s.find(".."); r != std::string::npos) {
        if (r > 0 && !parse_size(s.substr(0, r), mn)) return false;
        if (r + 2 < s.size() && !parse_size(s.substr(r + 2), mx)) return false;
        return r > 0 || r + 2 < s.size();
    }
    uint64_t x = 0;
    if (s.rfind(">=", 0) == 0) { if (!parse_size(s.substr(2), x)) return false; mn = x; return true; }
    if (s.rfind("<=", 0) == 0) { if (!parse_size(s.substr(2), x)) return false; mx = x; return true; }
    if (s.rfind('>', 0) == 0) { if (!parse_size(s.substr(1), x)) return false; mn = x + 1; return true; }
    if (s.rfind('<', 0) == 0) { if (!parse_size(s.substr(1), x)) return false; mx = x ? x - 1 : 0; return true; }
    if (s.rfind('=', 0) == 0) { if (!parse_size(s.substr(1), x)) return false; mn = mx = x; return true; }
    if (!parse_size(s, x)) return false;
    mn = mx = x;
    return true;
}

bool parse_date_filter(std::string_view v, time_t now, int64_t& tmin, int64_t& tmax) {
    std::string s = lower_ascii(v);
    tmin = INT64_MIN;
    tmax = INT64_MAX;
    const time_t today = day_start(now);
    if (s == "today") { tmin = today; return true; }
    if (s == "yesterday") { tmin = add_days(today, -1); tmax = today - 1; return true; }
    if (s == "thisweek") {
        tm lt{};
        localtime_r(&today, &lt);
        tmin = add_days(today, -((lt.tm_wday + 6) % 7));  // с понедельника
        return true;
    }
    if (s == "thismonth" || s == "thisyear") {
        tm lt{};
        localtime_r(&today, &lt);
        lt.tm_mday = 1;
        if (s == "thisyear") lt.tm_mon = 0;
        lt.tm_isdst = -1;
        tmin = mktime(&lt);
        return true;
    }
    if (s.rfind("last", 0) == 0 && s.size() > 8 && s.compare(s.size() - 4, 4, "days") == 0) {
        const int n = atoi(s.substr(4, s.size() - 8).c_str());
        if (n <= 0) return false;
        tmin = add_days(today, -(n - 1));
        return true;
    }
    time_t a = 0, b = 0;
    if (const size_t r = s.find(".."); r != std::string::npos) {
        if (r > 0) { if (!parse_date(s.substr(0, r), a)) return false; tmin = a; }
        if (r + 2 < s.size()) { if (!parse_date(s.substr(r + 2), b)) return false; tmax = add_days(b, 1) - 1; }
        return r > 0 || r + 2 < s.size();
    }
    if (s.rfind(">=", 0) == 0) { if (!parse_date(s.substr(2), a)) return false; tmin = a; return true; }
    if (s.rfind("<=", 0) == 0) { if (!parse_date(s.substr(2), a)) return false; tmax = add_days(a, 1) - 1; return true; }
    if (s.rfind('>', 0) == 0) { if (!parse_date(s.substr(1), a)) return false; tmin = add_days(a, 1); return true; }
    if (s.rfind('<', 0) == 0) { if (!parse_date(s.substr(1), a)) return false; tmax = a - 1; return true; }
    if (s.rfind('=', 0) == 0) s.erase(0, 1);
    if (!parse_date(s, a)) return false;
    tmin = a;
    tmax = add_days(a, 1) - 1;
    return true;
}

Query parse_query(std::string_view text, const QueryOptions& opts, time_t now) {
    Query q;
    auto make_text_term = [&](const std::string& raw, bool neg, bool force_path) {
        Term t;
        t.negate = neg;
        t.case_sensitive = opts.case_sensitive;
        t.path = opts.match_path || force_path || raw.find('/') != std::string::npos;
        t.kind = raw.find_first_of("*?") != std::string::npos ? Term::Glob : Term::Substr;
        t.ftext = fold_case(raw);
        t.text = opts.case_sensitive ? raw : t.ftext;
        return t;
    };
    auto make_regex = [&](std::string_view pat, bool neg, bool path) -> Term {
        Term t;
        t.kind = Term::Regex;
        t.negate = neg;
        t.path = path;
        t.case_sensitive = opts.case_sensitive;
        t.re = bx::Regex::compile(pat, opts.case_sensitive, q.error);
        return t;
    };

    if (opts.regex) {
        std::string_view p = text;
        while (!p.empty() && std::isspace((unsigned char)p.front())) p.remove_prefix(1);
        while (!p.empty() && std::isspace((unsigned char)p.back())) p.remove_suffix(1);
        if (!p.empty()) q.groups.push_back({make_regex(p, false, opts.match_path)});
        return q;
    }

    bool pending_or = false;
    auto push = [&](Term t) {
        if (pending_or && !q.groups.empty())
            q.groups.back().push_back(std::move(t));
        else
            q.groups.push_back({std::move(t)});
        pending_or = false;
    };

    for (const Tok& tok : tokenize(text)) {
        if (tok.or_op) {
            pending_or = true;
            continue;
        }
        std::string s = tok.text;
        bool neg = false;
        if (s.size() > 1 && s[0] == '!') {
            neg = true;
            s.erase(0, 1);
        }
        if (s.empty()) continue;

        const size_t colon = s.find(':');
        if (colon != std::string::npos && colon > 0) {
            const std::string pfx = lower_ascii(s.substr(0, colon));
            const std::string val = s.substr(colon + 1);
            Term t;
            t.negate = neg;
            if (pfx == "file" || pfx == "folder") {
                t.kind = pfx == "file" ? Term::IsFile : Term::IsDir;
                push(t);
                if (!val.empty()) push(make_text_term(val, false, false));
                continue;
            }
            if (pfx == "ext") {
                t.kind = Term::Ext;
                size_t b = 0;
                while (b <= val.size()) {
                    size_t e = val.find_first_of(";,", b);
                    if (e == std::string::npos) e = val.size();
                    std::string x = val.substr(b, e - b);
                    if (!x.empty() && x[0] == '.') x.erase(0, 1);
                    if (!x.empty()) t.exts.push_back(fold_case(x));
                    b = e + 1;
                }
                if (t.exts.empty()) {
                    q.error = "ext: — укажите расширения, например ext:pdf;docx";
                    return q;
                }
                push(t);
                continue;
            }
            if (pfx == "size") {
                t.kind = Term::Size;
                if (!parse_size_filter(val, t.min, t.max)) {
                    q.error = "size: — не понял «" + val + "» (примеры: >10mb, 1kb..5mb, large)";
                    return q;
                }
                push(t);
                continue;
            }
            if (pfx == "dm" || pfx == "datemodified") {
                t.kind = Term::Date;
                if (!parse_date_filter(val, now, t.tmin, t.tmax)) {
                    q.error = "dm: — не понял «" + val + "» (примеры: today, 2026-01-01, >2026-01-01)";
                    return q;
                }
                push(t);
                continue;
            }
            if (pfx == "regex") {
                push(make_regex(val, neg, opts.match_path));
                if (!q.ok()) return q;
                continue;
            }
            if (pfx == "path") {
                if (!val.empty()) push(make_text_term(val, neg, true));
                continue;
            }
            // неизвестный префикс — обычный текст (например, "C:" в имени)
        }
        push(make_text_term(s, neg, false));
    }
    return q;
}

}  // namespace bx
