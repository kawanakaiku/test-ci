// SPDX-License-Identifier: Apache-2.0
// tools/gen_fixtures.py(別実装)が作ったイメージを我々のパーサで読み、期待値と照合する。
#include <fstream>
#include <sstream>

#include "test_util.h"

extern std::string g_fixture_dir;
using namespace usbmem;

static void check_fixture(const char* name) {
  std::string base = g_fixture_dir + "/" + name;
  BlockDevicePtr dev;
  if (!RawImageDevice::open_path(base + ".img", true, 512, &dev).ok()) {
    std::fprintf(stderr, "  cannot open %s.img\n", base.c_str());
    g_failures++;
    return;
  }
  PartitionTable t;
  CHECK(read_partition_table(*dev, &t).ok());
  std::ifstream exp(base + ".expected");
  std::string line, kind;
  std::getline(exp, kind);
  CHECK(kind == (t.kind == TableKind::Gpt ? "gpt" : t.kind == TableKind::Mbr ? "mbr" : "none"));
  size_t n = 0;
  while (std::getline(exp, line)) {
    std::istringstream is(line);
    unsigned idx; unsigned long long start, cnt; std::string fs;
    is >> idx >> start >> cnt >> fs;
    bool found = false;
    for (auto& p : t.partitions)
      if (p.index == idx && !p.extended_container) {
        found = true;
        CHECK_EQ(p.start_lba, start);
        CHECK_EQ(p.sector_count, cnt);
        CHECK(fs == fs_type_name(p.fs));
      }
    CHECK(found);
    n++;
  }
  size_t have = 0;
  for (auto& p : t.partitions) have += !p.extended_container;
  CHECK_EQ(have, n);
  CHECK(!t.has_errors());
}

TEST(fixture_mbr_primary) { check_fixture("mbr_primary"); }
TEST(fixture_mbr_extended) { check_fixture("mbr_extended"); }
TEST(fixture_gpt_basic) { check_fixture("gpt_basic"); }
TEST(fixture_gpt_4k) { check_fixture("gpt_4k"); }
TEST(fixture_superfloppy_fat32) { check_fixture("superfloppy_fat32"); }
TEST(fixture_superfloppy_ntfs) { check_fixture("superfloppy_ntfs"); }
