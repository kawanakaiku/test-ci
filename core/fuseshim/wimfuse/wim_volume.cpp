// SPDX-License-Identifier: GPL-2.0-or-later
#include "wim_volume.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "wimfuse_client.h"

namespace usbmem {
namespace wimfuse {

namespace {
// JSON の文字列 (s[i] が開きの '"' の次) を読んで out に入れ、閉じの '"' の次の位置を返す (npos = 不正)。\" \\ \/ \n \r \t \b \f \uXXXX (BMP の UTF-8 化)
size_t read_json_string(const std::string& s, size_t i, std::string* out) {
  out->clear();
  while (i < s.size()) {
    char c = s[i++];
    if (c == '"') return i;
    if (c != '\\') { out->push_back(c); continue; }
    if (i >= s.size()) break;
    char e = s[i++];
    switch (e) {
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'u': {
        if (i + 4 > s.size()) return std::string::npos;
        unsigned cp = static_cast<unsigned>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
        i += 4;
        if (cp < 0x80) out->push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out->push_back(static_cast<char>(0xC0 | (cp >> 6))); out->push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { out->push_back(static_cast<char>(0xE0 | (cp >> 12))); out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out->push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        break;
      }
      default: out->push_back(e);  // " \ /
    }
  }
  return std::string::npos;
}
}  // namespace

std::vector<WimImageName> parse_image_names(const std::string& json) {
  std::vector<WimImageName> r;
  size_t p = json.find("\"images\":[");
  if (p == std::string::npos) return r;
  for (;;) {
    size_t a = json.find("{\"index\":", p);
    if (a == std::string::npos) break;
    WimImageName n;
    n.index = std::atoi(json.c_str() + a + 9);
    size_t b = json.find("\"name\":\"", a);
    if (b == std::string::npos) break;
    size_t after = read_json_string(json, b + 8, &n.name);
    if (after == std::string::npos) break;
    r.push_back(std::move(n));
    p = after;
  }
  return r;
}

std::string image_folder_name(const WimImageName& i) {
  std::string n;
  for (unsigned char c : i.name) n.push_back((c == '/' || c < 0x20 || c == 0x7f) ? '_' : static_cast<char>(c));
  while (!n.empty() && n.back() == ' ') n.pop_back();
  size_t b = 0;
  while (b < n.size() && n[b] == ' ') b++;
  n.erase(0, b);
  if (n.size() > 120) {
    size_t cut = 120;
    while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) cut--;  // UTF-8 の継続バイトの途中で切らない
    n.resize(cut);
  }
  if (n == "." || n == "..") n.clear();
  return n.empty() ? std::to_string(i.index) : std::to_string(i.index) + " - " + n;
}

std::unique_ptr<WimVolume> WimVolume::open(BlockDevicePtr src, SpawnFn spawn, int* err, std::string* msg) {
  int fd = spawn();
  if (fd < 0) { if (err) *err = -fd; return nullptr; }
  std::string json;
  int rc = client_info(fd, src, &json);
  if (rc != 0) {
    if (err) *err = rc < 0 ? -rc : EIO;
    if (msg) *msg = json;
    return nullptr;
  }
  std::unique_ptr<WimVolume> v(new WimVolume);
  v->src_ = std::move(src);
  v->spawn_ = std::move(spawn);
  v->info_json_ = json;
  v->imgs_ = parse_image_names(json);
  if (v->imgs_.empty()) { if (err) *err = EINVAL; if (msg) *msg = "WIM にイメージが無い (または一覧を読めない)"; return nullptr; }
  for (auto& i : v->imgs_) v->folder_.push_back(image_folder_name(i));
  v->kids_.resize(v->imgs_.size());
  return v;
}

int WimVolume::spawned() const {
  std::lock_guard<std::mutex> l(mu_);
  int n = 0;
  for (auto& k : kids_) if (k.vol) n++;
  return n;
}

bool WimVolume::slot_of(uint64_t ino, size_t* slot) const {
  uint64_t i = ino >> kShift;  // イメージ番号 (1 始まり) の添字 + 1 ではなく、そのまま WIM の index ではなく kids_ の添字 + 1
  if (i == 0 || i > kids_.size()) return false;
  *slot = static_cast<size_t>(i - 1);
  return true;
}

FsVolume* WimVolume::kid(size_t slot, int* err) {
  std::lock_guard<std::mutex> l(mu_);
  Kid& k = kids_[slot];
  if (k.vol) return k.vol.get();
  if (k.tried) { if (err) *err = k.err; return nullptr; }
  k.tried = true;
  int fd = spawn_();
  if (fd < 0) { k.err = -fd; if (err) *err = k.err; return nullptr; }
  int e = 0;
  std::string msg;
  k.vol = client_mount(fd, src_, imgs_[slot].index, &e, &msg);
  if (!k.vol) { k.err = e ? e : EIO; if (err) *err = k.err; return nullptr; }
  return k.vol.get();
}

uint64_t WimVolume::to_comp(size_t slot, FsVolume* k, uint64_t c) const {
  uint64_t base = static_cast<uint64_t>(slot + 1) << kShift;
  return c == k->root() ? base : (base | (c & kMask));
}
uint64_t WimVolume::to_child(FsVolume* k, uint64_t comp) const {
  uint64_t low = comp & kMask;
  return low ? low : k->root();
}

int WimVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  if (dir == kRoot) {
    for (size_t i = 0; i < folder_.size(); i++)
      if (folder_[i] == name) { *ino = static_cast<uint64_t>(i + 1) << kShift; return 0; }
    return -ENOENT;
  }
  size_t s;
  if (!slot_of(dir, &s)) return -ENOENT;
  int err = 0;
  FsVolume* k = kid(s, &err);
  if (!k) return -err;
  uint64_t c = 0;
  int rc = k->lookup(to_child(k, dir), name, &c);
  if (rc) return rc;
  *ino = to_comp(s, k, c);
  return 0;
}

int WimVolume::stat(uint64_t ino, FsStat* st) {
  if (ino == kRoot || (ino & kMask) == 0) {
    // ルートとイメージのフォルダ: 子を起こさずに答える (一覧の表示だけで全イメージのプロセスを起こさない)
    *st = FsStat();
    st->ino = ino;
    st->mode = S_IFDIR | 0555;
    st->nlink = 2;
    size_t s;
    if (ino != kRoot && !slot_of(ino, &s)) return -ENOENT;
    return 0;
  }
  size_t s;
  if (!slot_of(ino, &s)) return -ENOENT;
  int err = 0;
  FsVolume* k = kid(s, &err);
  if (!k) return -err;
  int rc = k->stat(to_child(k, ino), st);
  if (rc) return rc;
  st->ino = ino;
  return 0;
}

int WimVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  out->clear();
  if (dir == kRoot) {
    out->push_back(FsDirEntry{".", kRoot, 4});
    out->push_back(FsDirEntry{"..", kRoot, 4});
    for (size_t i = 0; i < folder_.size(); i++) out->push_back(FsDirEntry{folder_[i], static_cast<uint64_t>(i + 1) << kShift, 4});
    return 0;
  }
  size_t s;
  if (!slot_of(dir, &s)) return -ENOENT;
  int err = 0;
  FsVolume* k = kid(s, &err);
  if (!k) return -err;
  int rc = k->readdir(to_child(k, dir), out);
  if (rc) return rc;
  for (auto& e : *out) {
    if (e.name == ".") e.ino = dir;
    else if (e.name == "..") e.ino = ((dir & kMask) == 0) ? kRoot : to_comp(s, k, k->root());
    else e.ino = to_comp(s, k, e.ino);
  }
  return 0;
}

int64_t WimVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  size_t s;
  if (!slot_of(ino, &s) || (ino & kMask) == 0) return -EISDIR;
  int err = 0;
  FsVolume* k = kid(s, &err);
  if (!k) return -err;
  return k->read(to_child(k, ino), off, buf, len);
}

int WimVolume::readlink(uint64_t ino, std::string* out) {
  size_t s;
  if (!slot_of(ino, &s) || (ino & kMask) == 0) return -EINVAL;
  int err = 0;
  FsVolume* k = kid(s, &err);
  if (!k) return -err;
  return k->readlink(to_child(k, ino), out);
}

}  // namespace wimfuse
}  // namespace usbmem
