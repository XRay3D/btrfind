#pragma once
// Язык запросов (подмножество синтаксиса Everything):
//   слова через пробел — И; "фраза"; !слово — НЕ; a | b — ИЛИ;
//   * и ? — маска на всё имя; '/' в слове — поиск по полному пути;
//   regex:выражение; file: folder: ext:pdf;docx size:>10mb size:1kb..5mb size:large
//   dm:today|yesterday|thisweek|thismonth|thisyear|2026-01-01|>2026-01-01|2026-01-01..2026-02-01
//   path:слово — по полному пути.

#include "matcher.h"

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bx {

struct QueryOptions {
    bool case_sensitive = false;
    bool regex = false;       // весь запрос — одно регулярное выражение
    bool match_path = false;  // все слова сопоставляются с полным путём
};

struct Term {
    enum Kind : uint8_t { Substr, Glob, Regex, Ext, Size, Date, IsFile, IsDir };
    Kind kind = Substr;
    bool negate = false;
    bool path = false;
    bool case_sensitive = false;
    std::string text;   // образец (свёрнутый, если без учёта регистра)
    std::string ftext;  // всегда свёрнутый (для префильтра по пулу lnames)
    std::vector<std::string> exts;
    uint64_t min = 0, max = UINT64_MAX;
    int64_t tmin = INT64_MIN, tmax = INT64_MAX;
    std::shared_ptr<bx::Regex> re;
};

struct Query {
    std::vector<std::vector<Term>> groups;  // И по группам, ИЛИ внутри группы
    std::string error;
    bool no_pool_scan = false;  // запретить префильтр memmem по пулу (тесты/отладка)
    bool ok() const { return error.empty(); }
    bool match_all() const { return groups.empty(); }
};

Query parse_query(std::string_view text, const QueryOptions& opts, time_t now = time(nullptr));

// Вспомогательное (для тестов): разбор размеров и дат.
bool parse_size(std::string_view s, uint64_t& out);
bool parse_size_filter(std::string_view v, uint64_t& min, uint64_t& max);
bool parse_date_filter(std::string_view v, time_t now, int64_t& tmin, int64_t& tmax);

}  // namespace bx
