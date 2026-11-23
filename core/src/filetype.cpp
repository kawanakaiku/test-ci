// SPDX-License-Identifier: Apache-2.0
#include "filetype.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace usbmem {
namespace {
bool eq(const uint8_t* p, size_t n, size_t off, const char* s, size_t l) { return n >= off + l && std::memcmp(p + off, s, l) == 0; }
template <size_t N>
bool eqs(const uint8_t* p, size_t n, size_t off, const char (&s)[N]) { return eq(p, n, off, s, N - 1); }

bool mp3_sync(const uint8_t* p, size_t n) {
  if (n < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
  int ver = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;
  if (ver == 1 || layer == 0) return false;
  int br = p[2] >> 4, sr = (p[2] >> 2) & 3;
  return br != 0 && br != 15 && sr != 3;
}

struct TypeDef {
  const char* name;
  const char* exts;  // ',' 区切り (前後にも ',')
  bool (*match)(const uint8_t*, size_t);
};
const TypeDef kTypes[] = {
    {"jpeg", ",jpg,jpeg,jpe,jfif,", [](const uint8_t* p, size_t n) { return n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF; }},
    {"png", ",png,", [](const uint8_t* p, size_t n) { return eq(p, n, 0, "\x89PNG\r\n\x1a\n", 8); }},
    {"gif", ",gif,", [](const uint8_t* p, size_t n) { return eqs(p, n, 0, "GIF87a") || eqs(p, n, 0, "GIF89a"); }},
    {"bmp", ",bmp,dib,", [](const uint8_t* p, size_t n) { return eqs(p, n, 0, "BM"); }},
    {"pdf", ",pdf,", [](const uint8_t* p, size_t n) { return eqs(p, n, 0, "%PDF-"); }},
    {"zip", ",zip,docx,xlsx,pptx,jar,war,apk,aar,odt,ods,odp,epub,xpi,kmz,", [](const uint8_t* p, size_t n) { return eq(p, n, 0, "PK\x03\x04", 4) || eq(p, n, 0, "PK\x05\x06", 4); }},
    {"mp4", ",mp4,m4a,m4v,mov,3gp,3g2,heic,heif,qt,", [](const uint8_t* p, size_t n) { return eqs(p, n, 4, "ftyp") || eqs(p, n, 4, "moov") || eqs(p, n, 4, "mdat") || eqs(p, n, 4, "wide") || eqs(p, n, 4, "free"); }},
    {"mp3", ",mp3,", [](const uint8_t* p, size_t n) { return eqs(p, n, 0, "ID3") || mp3_sync(p, n); }},
    {"riff", ",wav,avi,webp,", [](const uint8_t* p, size_t n) { return eqs(p, n, 0, "RIFF"); }},
    {"7z", ",7z,", [](const uint8_t* p, size_t n) { return eq(p, n, 0, "7z\xBC\xAF\x27\x1C", 6); }},
    {"gzip", ",gz,tgz,", [](const uint8_t* p, size_t n) { return n >= 3 && p[0] == 0x1F && p[1] == 0x8B && p[2] == 0x08; }},
    {"sqlite", ",sqlite,sqlite3,db,", [](const uint8_t* p, size_t n) { return eq(p, n, 0, "SQLite format 3", 16); }},
    {"pe", ",exe,dll,sys,ocx,scr,", [](const uint8_t* p, size_t n) { return eqs(p, n, 0, "MZ"); }},
    {"rar", ",rar,", [](const uint8_t* p, size_t n) { return eqs(p, n, 0, "Rar!\x1A\x07"); }},
    {"ole2", ",doc,xls,ppt,msi,msg,", [](const uint8_t* p, size_t n) { return eq(p, n, 0, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8); }},
    {"elf", ",so,elf,", [](const uint8_t* p, size_t n) { return eq(p, n, 0, "\x7f" "ELF", 4); }},
    {"tiff", ",tif,tiff,", [](const uint8_t* p, size_t n) { return eq(p, n, 0, "II*\0", 4) || eq(p, n, 0, "MM\0*", 4); }},
    {"ogg", ",ogg,oga,ogv,opus,", [](const uint8_t* p, size_t n) { return eqs(p, n, 0, "OggS"); }},
    {"flac", ",flac,", [](const uint8_t* p, size_t n) { return eqs(p, n, 0, "fLaC"); }},
    {"matroska", ",mkv,webm,mka,", [](const uint8_t* p, size_t n) { return eq(p, n, 0, "\x1A\x45\xDF\xA3", 4); }},
};
const char kTextExts[] = ",txt,csv,tsv,xml,html,htm,json,log,md,ini,cfg,conf,c,h,cpp,py,js,css,svg,sh,bat,yaml,yml,srt,tex,";

std::string ext_of(const std::string& name) {
  size_t d = name.rfind('.');
  if (d == std::string::npos || d + 1 >= name.size() || name.size() - d > 8) return "";
  std::string e = name.substr(d + 1);
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}
bool in_list(const char* list, const std::string& e) { return std::string(list).find("," + e + ",") != std::string::npos; }
}  // namespace

std::string sniff_type(const uint8_t* p, size_t n) {
  for (const TypeDef& t : kTypes)
    if (t.match(p, n)) {
      if (!std::strcmp(t.name, "riff")) {
        if (eqs(p, n, 8, "WAVE")) return "wav";
        if (eqs(p, n, 8, "AVI ")) return "avi";
        if (eqs(p, n, 8, "WEBP")) return "webp";
        return "riff";
      }
      return t.name;
    }
  return "";
}

const char* sig_check_name(SigCheck c) { return c == SigCheck::Match ? "match" : c == SigCheck::Mismatch ? "mismatch" : "unknown"; }

SigCheck check_head_for_name(const std::string& name, const uint8_t* head, size_t n, uint64_t file_size) {
  if (n == 0) return SigCheck::Unknown;
  std::string e = ext_of(name);
  if (!e.empty()) {
    for (const TypeDef& t : kTypes)
      if (in_list(t.exts, e)) return t.match(head, n) ? SigCheck::Match : SigCheck::Mismatch;
    if (in_list(kTextExts, e)) {
      size_t m = std::min<size_t>(n, 512), ok = 0;
      for (size_t i = 0; i < m; i++) {
        uint8_t c = head[i];
        if (c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127) || c >= 0x80) ok++;  // UTF-8 / 各国語の 8 ビット文字は許す
      }
      bool bom_utf16 = n >= 2 && ((head[0] == 0xFF && head[1] == 0xFE) || (head[0] == 0xFE && head[1] == 0xFF));
      return (bom_utf16 || ok * 100 >= m * 95) ? SigCheck::Match : SigCheck::Mismatch;
    }
  }
  // 拡張子から判断できない: 中身が全部同じ値 (消去された・未書き込みの領域) のときだけ不整合とする
  if (file_size >= 512 && n >= 512) {
    bool zero = true, ff = true;
    for (size_t i = 0; i < 512; i++) { zero = zero && head[i] == 0; ff = ff && head[i] == 0xFF; }
    if (zero || ff) return SigCheck::Mismatch;
  }
  return SigCheck::Unknown;
}

}  // namespace usbmem
