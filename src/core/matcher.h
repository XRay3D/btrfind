#pragma once
// Сопоставители: glob (* и ?) по UTF-8 и регулярные выражения PCRE2 с JIT.

#include <memory>
#include <string>
#include <string_view>

namespace bx {

// Glob на всю строку: '*' — любая последовательность, '?' — один символ UTF-8.
bool glob_match(std::string_view pattern, std::string_view s);

class Regex {
public:
    // PCRE2_UTF (+ PCRE2_CASELESS без учёта регистра), JIT-компиляция.
    static std::shared_ptr<Regex> compile(std::string_view pattern, bool case_sensitive, std::string& err);
    bool match(std::string_view subject) const;  // потокобезопасно (match_data на поток)
    ~Regex();
    Regex(const Regex&) = delete;
    Regex& operator=(const Regex&) = delete;

private:
    Regex() = default;
    void* code_ = nullptr;  // pcre2_code_8*
};

}  // namespace bx
