#pragma once

#include <QStringList>

namespace bx {
// btrfind --cli [опции] запрос   |   btrfind --status
bool is_cli_invocation(int argc, char** argv);
int run_cli(int argc, char** argv);
}  // namespace bx
