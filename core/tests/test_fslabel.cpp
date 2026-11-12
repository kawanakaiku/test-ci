// SPDX-License-Identifier: Apache-2.0
// FS ラベル / UUID / シリアルの軽量リーダ (fs_label.cpp) のテスト。実際の mkfs が作ったイメージとの照合は
// tools/check_partedit_tools.py (blkid -p と比較) で行う。ここでは手組みの最小イメージで読み方を固定し、壊れた入力を浴びせる。
#include <random>

#include "partedit_test_util.h"
#include "usbmem/fs_label.h"

using namespace pt;

namespace {

FsLabelInfo label_of(const Bytes& img, FsType fs, Status* st = nullptr) {
  MemoryDevice d(img, true);
  FsLabelInfo li;
  Status s = read_fs_label(d, 0, img.size(), fs, &li);
  if (st) *st = s;
  return li;
}

void put_str(Bytes& b, size_t off, const std::string& s) { std::memcpy(&b[off], s.data(), s.size()); }
void put_u16str(Bytes& b, size_t off, const std::u16string& s) {
  for (size_t i = 0; i < s.size(); i++) tb::le16(b, off + 2 * i, s[i]);
}

Bytes fat16_img(bool with_root_label) {
  Bytes b = blank(4096);
  put_str(b, 3, "MSDOS5.0");
  tb::le16(b, 11, 512); b[13] = 4; tb::le16(b, 14, 1); b[16] = 2; tb::le16(b, 17, 512); tb::le16(b, 19, 4096);
  b[21] = 0xF8; tb::le16(b, 22, 32);
  b[36] = 0x80; b[38] = 0x29; tb::le32(b, 39, 0x1234ABCD);
  put_str(b, 43, "BOOTLABEL  ");
  put_str(b, 54, "FAT16   ");
  tb::sign55aa(b);
  if (with_root_label) {
    size_t root = (1 + 2 * 32) * 512;
    put_str(b, root, "ROOTLABEL  ");
    b[root + 11] = 0x08;
  }
  return b;
}

Bytes fat32_img(bool with_root_label) {
  Bytes b = blank(8192);
  put_str(b, 3, "MSDOS5.0");
  tb::le16(b, 11, 512); b[13] = 1; tb::le16(b, 14, 32); b[16] = 2; tb::le32(b, 32, 8192); tb::le32(b, 36, 16);
  tb::le32(b, 44, 2);
  b[66] = 0x29; tb::le32(b, 67, 0xCAFEF00D);
  put_str(b, 71, "NO NAME    ");
  put_str(b, 82, "FAT32   ");
  tb::sign55aa(b);
  // FAT: クラスタ 2 は終端
  tb::le32(b, 32 * 512 + 8, 0x0FFFFFFF);
  size_t data = (32 + 2 * 16) * 512;
  if (with_root_label) {
    put_str(b, data + 32, "Data Disk  ");  // 通常のエントリの後ろにある
    b[data + 32 + 11] = 0x08;
  }
  put_str(b, data, "FILE    TXT");
  b[data + 11] = 0x20;
  return b;
}

Bytes exfat_img() {
  Bytes b = blank(4096);
  put_str(b, 3, "EXFAT   ");
  tb::le32(b, 80, 24);       // FAT offset
  tb::le32(b, 84, 8);        // FAT length
  tb::le32(b, 88, 64);       // cluster heap offset
  tb::le32(b, 92, 100);      // cluster count
  tb::le32(b, 96, 2);        // root cluster
  tb::le32(b, 100, 0x5A5AA5A5);
  b[108] = 9; b[109] = 0;    // 512B セクタ, 1 セクタ/クラスタ
  tb::sign55aa(b);
  tb::le32(b, 24 * 512 + 8, 0xFFFFFFFF);
  size_t root = 64 * 512;
  b[root] = 0x83; b[root + 1] = 4;
  put_u16str(b, root + 2, u"Ｄａｔａ");  // 全角 4 文字
  b[root + 32] = 0xA0;
  for (int i = 0; i < 16; i++) b[root + 32 + 6 + i] = static_cast<uint8_t>(0x10 + i);
  return b;
}

Bytes ext_img() {
  Bytes b = blank(256);
  tb::le16(b, 1024 + 56, 0xEF53);
  for (int i = 0; i < 16; i++) b[1024 + 104 + i] = static_cast<uint8_t>(0xA0 + i);
  put_str(b, 1024 + 120, "rootfs");
  return b;
}

void tag(Bytes& b, size_t off, uint16_t id, uint32_t loc) {
  tb::le16(b, off, id);
  tb::le32(b, off + 12, loc);
  uint8_t sum = 0;
  for (int i = 0; i < 16; i++) if (i != 4) sum = static_cast<uint8_t>(sum + b[off + i]);
  b[off + 4] = sum;
}

Bytes iso_img() {
  Bytes b = blank(512);
  size_t pvd = 0x8000;
  b[pvd] = 1; put_str(b, pvd + 1, "CD001"); b[pvd + 6] = 1;
  std::string vid = "MY_DISC";
  vid.resize(32, ' ');
  put_str(b, pvd + 40, vid);
  put_str(b, pvd + 813, "2024010203040599");
  size_t term = 0x8000 + 2048;
  b[term] = 255; put_str(b, term + 1, "CD001");
  return b;
}

Bytes udf_img() {
  Bytes b = blank(4096);  // 2 MiB
  put_str(b, 0x8000 + 1, "BEA01");
  put_str(b, 0x8000 + 2048 + 1, "NSR02");
  put_str(b, 0x8000 + 4096 + 1, "TEA01");
  size_t avdp = 256 * 2048;
  tag(b, avdp, 2, 256);
  tb::le32(b, avdp + 16, 4096);  // メインの VDS 長
  tb::le32(b, avdp + 20, 257);
  size_t pvd = 257 * 2048;
  tag(b, pvd, 1, 257);
  b[pvd + 24] = 8; put_str(b, pvd + 25, "PVDNAME"); b[pvd + 24 + 31] = 8;
  size_t lvd = 258 * 2048;
  tag(b, lvd, 6, 258);
  b[lvd + 84] = 16;  // UTF-16BE
  std::u16string name = u"Ｌｏｇｉｃ";
  for (size_t i = 0; i < name.size(); i++) { b[lvd + 85 + 2 * i] = static_cast<uint8_t>(name[i] >> 8); b[lvd + 86 + 2 * i] = static_cast<uint8_t>(name[i]); }
  b[lvd + 84 + 127] = static_cast<uint8_t>(1 + 2 * name.size());
  return b;
}

Bytes ntfs_img() {
  Bytes b = blank(128);  // 64 KiB
  put_str(b, 3, "NTFS    ");
  tb::le16(b, 11, 512); b[13] = 8; tb::le64(b, 0x28, 127); tb::le64(b, 0x30, 4);
  b[0x40] = 0xF6;  // 2^10 = 1024 バイトの MFT レコード
  tb::le64(b, 0x48, 0x0123456789ABCDEFull);
  tb::sign55aa(b);
  size_t rec = 4 * 4096 + 3 * 1024;
  put_str(b, rec, "FILE");
  tb::le16(b, rec + 4, 0x30); tb::le16(b, rec + 6, 3);
  tb::le16(b, rec + 0x14, 0x38); tb::le16(b, rec + 0x16, 1);
  std::u16string label = u"Vol😀";
  uint32_t vlen = static_cast<uint32_t>(label.size() * 2);
  size_t at = rec + 0x38;
  uint32_t alen = (0x18 + vlen + 7) & ~7u;
  tb::le32(b, at, 0x60); tb::le32(b, at + 4, alen);
  tb::le32(b, at + 0x10, vlen); tb::le16(b, at + 0x14, 0x18);
  for (size_t i = 0; i < label.size(); i++) tb::le16(b, at + 0x18 + 2 * i, label[i]);
  tb::le32(b, at + alen, 0xFFFFFFFFu);
  tb::le32(b, rec + 0x18, static_cast<uint32_t>(0x38 + alen + 8));
  tb::le32(b, rec + 0x1C, 1024);
  tb::le16(b, rec + 0x30, 0x0001);  // 更新シーケンス番号
  tb::le16(b, rec + 510, 0x0001); tb::le16(b, rec + 1022, 0x0001);
  return b;
}

}  // namespace

TEST(fslabel_fat16_boot_label_and_root_entry_wins) {
  FsLabelInfo a = label_of(fat16_img(false), FsType::Fat16);
  CHECK(a.label == "BOOTLABEL");
  CHECK(a.serial == "1234-ABCD" && a.uuid == "1234-ABCD");
  FsLabelInfo b = label_of(fat16_img(true), FsType::Fat16);
  CHECK(b.label == "ROOTLABEL");  // ルートディレクトリのラベルが優先 (blkid と同じ)
}

TEST(fslabel_fat32_root_chain_and_no_name) {
  FsLabelInfo a = label_of(fat32_img(false), FsType::Fat32);
  CHECK(a.label.empty());  // "NO NAME" はラベル無し
  CHECK(a.serial == "CAFE-F00D");
  FsLabelInfo b = label_of(fat32_img(true), FsType::Fat32);
  CHECK(b.label == "Data Disk");
}

TEST(fslabel_exfat_label_serial_guid) {
  FsLabelInfo a = label_of(exfat_img(), FsType::Exfat);
  CHECK(a.label == "Ｄａｔａ");
  CHECK(a.serial == "5A5A-A5A5");
  CHECK(a.uuid == "5A5A-A5A5");
  CHECK(a.guid == "13121110-1514-1716-1819-1a1b1c1d1e1f");  // Microsoft 形式 (先頭 3 フィールドはリトルエンディアン)
}

TEST(fslabel_ext_iso_udf_ntfs) {
  FsLabelInfo e = label_of(ext_img(), FsType::Ext4);
  CHECK(e.label == "rootfs");
  CHECK(e.uuid == "a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf");
  FsLabelInfo i = label_of(iso_img(), FsType::Iso9660);
  CHECK(i.label == "MY_DISC");
  CHECK(i.uuid == "2024-01-02-03-04-05-99");
  FsLabelInfo u = label_of(udf_img(), FsType::Udf);
  CHECK(u.label == "Ｌｏｇｉｃ");
  FsLabelInfo n = label_of(ntfs_img(), FsType::Ntfs);
  CHECK(n.serial == "0123456789ABCDEF");
  CHECK(n.label == "Vol😀");
}

TEST(fslabel_magic_detection_of_erofs_squashfs_luks_btrfs_xfs) {
  Bytes sq = blank(64);
  put_str(sq, 0, "hsqs");
  CHECK(label_of(sq, FsType::Unknown).detected == "squashfs");
  Bytes er = blank(64);
  tb::le32(er, 1024, 0xE0F5E1E2u);
  for (int i = 0; i < 16; i++) er[1024 + 48 + i] = static_cast<uint8_t>(i + 1);
  put_str(er, 1024 + 64, "erofs-vol");
  FsLabelInfo e = label_of(er, FsType::Unknown);
  CHECK(e.detected == "erofs" && e.label == "erofs-vol" && e.uuid == "01020304-0506-0708-090a-0b0c0d0e0f10");
  Bytes lk = blank(64);
  put_str(lk, 0, std::string("LUKS\xba\xbe", 6));
  lk[6] = 0; lk[7] = 2;
  put_str(lk, 24, "enc-label");
  put_str(lk, 168, "11111111-2222-3333-4444-555555555555");
  FsLabelInfo l = label_of(lk, FsType::Luks);
  CHECK(l.label == "enc-label" && l.uuid == "11111111-2222-3333-4444-555555555555");
  Bytes bt = blank(256);
  put_str(bt, 0x10000 + 64, "_BHRfS_M");
  for (int i = 0; i < 16; i++) bt[0x10000 + 32 + i] = static_cast<uint8_t>(0xF0 + i);
  put_str(bt, 0x10000 + 299, "pool");
  FsLabelInfo b = label_of(bt, FsType::Btrfs);
  CHECK(b.label == "pool" && b.uuid == "f0f1f2f3-f4f5-f6f7-f8f9-fafbfcfdfeff");
  Bytes xf = blank(64);
  put_str(xf, 0, "XFSB");
  put_str(xf, 108, "xfsvol");
  CHECK(label_of(xf, FsType::Xfs).label == "xfsvol");
}

TEST(fslabel_text_is_sanitized) {
  Bytes e = ext_img();
  std::memset(&e[1024 + 120], 0, 16);
  e[1024 + 120] = 'a'; e[1024 + 121] = 0xFF; e[1024 + 122] = 0x07; e[1024 + 123] = 'b';  // 不正な UTF-8 と制御文字
  FsLabelInfo li = label_of(e, FsType::Ext4);
  CHECK(li.label == "a? b");  // 0xFF は '?'、0x07 は空白
  for (unsigned char c : li.label) CHECK(c >= 0x20 && c != 0x7F);
  Bytes f = fat16_img(false);
  put_str(f, 43, "caf\xc3\xa9      ");  // 正しい UTF-8 はそのまま
  CHECK(label_of(f, FsType::Fat16).label == "caf\xc3\xa9");
  put_str(f, 43, "caf\x82      ");       // OEM コードページの 8 ビット文字は '?'
  CHECK(label_of(f, FsType::Fat16).label == "caf?");
}

TEST(fslabel_io_error_is_reported_and_garbage_is_harmless) {
  Bytes e = ext_img();
  auto inner = std::make_shared<MemoryDevice>(e, true);
  tb::FaultDevice f(inner);
  f.fail_after_reads = 0;
  FsLabelInfo li;
  Status st = read_fs_label(f, 0, e.size(), FsType::Ext4, &li);
  CHECK(!st.ok());
  CHECK(st.code == Errc::Io);
  // 範囲が小さすぎる / 0 バイト
  CHECK(label_of(Bytes(100, 0), FsType::Ext4).label.empty());
  MemoryDevice d(e, true);
  CHECK(read_fs_label(d, 0, 0, FsType::Ext4, &li).ok());
  CHECK(read_fs_label(d, e.size() + 10, 4096, FsType::Ext4, &li).ok());
  CHECK(!read_fs_label(d, 0, 100, FsType::Ntfs, nullptr).ok());
}

TEST(fslabel_fuzz_corrupt_images_never_crash) {
  std::mt19937_64 rng(12345);
  struct Case { Bytes img; FsType fs; };
  std::vector<Case> cases = {{fat16_img(true), FsType::Fat16}, {fat32_img(true), FsType::Fat32}, {exfat_img(), FsType::Exfat},
                             {ext_img(), FsType::Ext4},        {iso_img(), FsType::Iso9660},     {udf_img(), FsType::Udf},
                             {ntfs_img(), FsType::Ntfs},       {ext_img(), FsType::Unknown},     {ext_img(), FsType::Luks},
                             {ext_img(), FsType::Btrfs},       {ext_img(), FsType::Xfs}};
  long runs = 0;
  for (auto& c : cases) {
    for (int it = 0; it < 1500; it++) {
      Bytes img = c.img;
      int n = 1 + static_cast<int>(rng() % 6);
      for (int k = 0; k < n; k++) {
        size_t pos = rng() % std::min<size_t>(img.size(), 70000);
        img[pos] = static_cast<uint8_t>(rng());
      }
      if (rng() % 7 == 0) img.resize(rng() % (img.size() + 1));
      // 先頭 BPB の数値を極端な値にする
      if (rng() % 3 == 0 && img.size() > 120) {
        size_t pos = 11 + rng() % 100;
        img[pos] = (rng() & 1) ? 0xFF : 0x00;
      }
      MemoryDevice d(img, true);
      FsLabelInfo li;
      uint64_t off = (rng() % 5 == 0) ? (rng() % 512) : 0;
      uint64_t len = (rng() % 4 == 0) ? rng() % (img.size() + 100) : img.size();
      Status st = read_fs_label(d, off, len, c.fs, &li);
      (void)st;
      for (unsigned char ch : li.label) CHECK(ch >= 0x20 && ch != 0x7F);
      CHECK(li.label.size() < 300);
      runs++;
    }
  }
  CHECK(runs > 10000);
}
