#pragma once
// Свёртка регистра для поиска без учёта регистра: ASCII — быстро на месте,
// остальное (кириллица и т.д.) — через QString::toCaseFolded.

#include <string>
#include <string_view>

namespace bx {

bool is_ascii(std::string_view s);
void fold_case_append(std::string_view s, std::string& out);
std::string fold_case(std::string_view s);

}  // namespace bx
