#include "matcher.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace bx {

namespace {
inline size_t utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}
}  // namespace

bool glob_match(std::string_view p, std::string_view s) {
    size_t pi = 0, si = 0;
    size_t star_p = std::string_view::npos, star_s = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '*') {
            star_p = pi++;
            star_s = si;
        } else if (pi < p.size() && p[pi] == '?') {
            ++pi;
            si += utf8_len((unsigned char)s[si]);
        } else if (pi < p.size() && p[pi] == s[si]) {
            ++pi;
            ++si;
        } else if (star_p != std::string_view::npos) {
            pi = star_p + 1;
            star_s += utf8_len((unsigned char)s[star_s]);
            si = star_s;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size() && si <= s.size();
}

std::shared_ptr<Regex> Regex::compile(std::string_view pattern, bool case_sensitive, std::string& err) {
    int errcode = 0;
    PCRE2_SIZE erroff = 0;
    uint32_t opts = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF;
    if (!case_sensitive) opts |= PCRE2_CASELESS | PCRE2_UCP;
    pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), opts,
                                     &errcode, &erroff, nullptr);
    if (!code) {
        PCRE2_UCHAR msg[256];
        pcre2_get_error_message(errcode, msg, sizeof msg);
        err = "regex: " + std::string(reinterpret_cast<const char*>(msg)) + " (позиция " +
              std::to_string(erroff) + ")";
        return nullptr;
    }
    pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);  // без JIT тоже работает, только медленнее
    std::shared_ptr<Regex> r(new Regex);
    r->code_ = code;
    return r;
}

bool Regex::match(std::string_view subject) const {
    struct Md {
        pcre2_match_data* md = pcre2_match_data_create(1, nullptr);
        ~Md() { pcre2_match_data_free(md); }
    };
    thread_local Md md;
    const int rc = pcre2_match(static_cast<pcre2_code*>(code_), reinterpret_cast<PCRE2_SPTR>(subject.data()),
                               subject.size(), 0, 0, md.md, nullptr);
    return rc >= 0;
}

Regex::~Regex() { pcre2_code_free(static_cast<pcre2_code*>(code_)); }

}  // namespace bx
