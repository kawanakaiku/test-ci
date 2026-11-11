// SPDX-License-Identifier: Apache-2.0
#include <cstring>

#include "test_util.h"

std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
int g_failures = 0;
std::string g_fixture_dir;

int main(int argc, char** argv) {
  bool fixtures_only = false;
  for (int i = 1; i < argc; i++)
    if (!std::strcmp(argv[i], "--fixtures") && i + 1 < argc) { g_fixture_dir = argv[++i]; fixtures_only = true; }
  int ran = 0, failed = 0;
  for (auto& t : registry()) {
    bool is_fix = std::strncmp(t.name, "fixture_", 8) == 0;
    if (fixtures_only != is_fix) continue;
    int before = g_failures;
    t.fn();
    ran++;
    if (g_failures != before) { failed++; std::fprintf(stderr, "FAIL %s\n", t.name); }
    else std::fprintf(stderr, "ok   %s\n", t.name);
  }
  std::fprintf(stderr, "%d tests, %d failed\n", ran, failed);
  return failed ? 1 : 0;
}
