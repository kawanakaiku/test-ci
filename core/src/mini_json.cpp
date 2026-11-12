// SPDX-License-Identifier: Apache-2.0
#include "mini_json.h"

#include <cstdlib>

namespace usbmem {
namespace tf {

namespace {
constexpr int kMaxDepth = 24;
constexpr size_t kMaxNodes = 20000;
constexpr size_t kMaxString = 1u << 20;

struct Parser {
  const char* p;
  const char* end;
  size_t nodes = 0;
  std::string err;

  bool fail(const char* m) { if (err.empty()) err = m; return false; }
  void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }

  static void put_utf8(std::string* s, uint32_t cp) {
    if (cp < 0x80) *s += char(cp);
    else if (cp < 0x800) { *s += char(0xc0 | (cp >> 6)); *s += char(0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) { *s += char(0xe0 | (cp >> 12)); *s += char(0x80 | ((cp >> 6) & 0x3f)); *s += char(0x80 | (cp & 0x3f)); }
    else { *s += char(0xf0 | (cp >> 18)); *s += char(0x80 | ((cp >> 12) & 0x3f)); *s += char(0x80 | ((cp >> 6) & 0x3f)); *s += char(0x80 | (cp & 0x3f)); }
  }
  bool hex4(uint32_t* v) {
    if (end - p < 4) return false;
    uint32_t r = 0;
    for (int k = 0; k < 4; k++) {
      char c = *p++;
      r <<= 4;
      if (c >= '0' && c <= '9') r |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') r |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') r |= uint32_t(c - 'A' + 10);
      else return false;
    }
    *v = r;
    return true;
  }
  bool string(std::string* out) {
    if (p >= end || *p != '"') return fail("string expected");
    p++;
    out->clear();
    while (p < end) {
      unsigned char c = static_cast<unsigned char>(*p++);
      if (c == '"') return true;
      if (c < 0x20) return fail("control char in string");
      if (out->size() >= kMaxString) return fail("string too long");
      if (c != '\\') { *out += char(c); continue; }
      if (p >= end) return fail("bad escape");
      char e = *p++;
      switch (e) {
        case '"': *out += '"'; break;
        case '\\': *out += '\\'; break;
        case '/': *out += '/'; break;
        case 'b': *out += '\b'; break;
        case 'f': *out += '\f'; break;
        case 'n': *out += '\n'; break;
        case 'r': *out += '\r'; break;
        case 't': *out += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(&cp)) return fail("bad \\u");
          if (cp >= 0xd800 && cp < 0xdc00) {
            uint32_t lo;
            if (end - p < 2 || p[0] != '\\' || p[1] != 'u') return fail("lone surrogate");
            p += 2;
            if (!hex4(&lo) || lo < 0xdc00 || lo >= 0xe000) return fail("bad surrogate");
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          } else if (cp >= 0xdc00 && cp < 0xe000) return fail("lone surrogate");
          put_utf8(out, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }
  bool value(JsonValue* v, int depth) {
    if (depth > kMaxDepth) return fail("too deep");
    if (++nodes > kMaxNodes) return fail("too many nodes");
    ws();
    if (p >= end) return fail("unexpected end");
    char c = *p;
    if (c == '{') {
      p++;
      v->type = JsonValue::Object;
      ws();
      if (p < end && *p == '}') { p++; return true; }
      for (;;) {
        ws();
        std::string key;
        if (!string(&key)) return false;
        ws();
        if (p >= end || *p != ':') return fail("':' expected");
        p++;
        v->obj.emplace_back(std::move(key), JsonValue());
        if (!value(&v->obj.back().second, depth + 1)) return false;
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == '}') { p++; return true; }
        return fail("',' or '}' expected");
      }
    }
    if (c == '[') {
      p++;
      v->type = JsonValue::Array;
      ws();
      if (p < end && *p == ']') { p++; return true; }
      for (;;) {
        v->arr.emplace_back();
        if (!value(&v->arr.back(), depth + 1)) return false;
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == ']') { p++; return true; }
        return fail("',' or ']' expected");
      }
    }
    if (c == '"') { v->type = JsonValue::String; return string(&v->s); }
    if (end - p >= 4 && std::string(p, 4) == "true") { p += 4; v->type = JsonValue::Bool; v->b = true; return true; }
    if (end - p >= 5 && std::string(p, 5) == "false") { p += 5; v->type = JsonValue::Bool; v->b = false; return true; }
    if (end - p >= 4 && std::string(p, 4) == "null") { p += 4; v->type = JsonValue::Null; return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      const char* q = p;
      bool neg = false, real = false;
      if (*q == '-') { neg = true; q++; }
      if (q >= end || *q < '0' || *q > '9') return fail("bad number");
      uint64_t acc = 0;
      bool overflow = false;
      while (q < end && *q >= '0' && *q <= '9') {
        unsigned d = unsigned(*q - '0');
        if (acc > (UINT64_MAX - d) / 10) overflow = true; else acc = acc * 10 + d;
        q++;
      }
      if (q < end && (*q == '.' || *q == 'e' || *q == 'E')) {
        real = true;
        while (q < end && ((*q >= '0' && *q <= '9') || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-')) q++;
      }
      p = q;
      if (real || overflow || acc > uint64_t(INT64_MAX)) { v->type = JsonValue::Real; return true; }
      v->type = JsonValue::Int;
      v->i = neg ? -int64_t(acc) : int64_t(acc);
      return true;
    }
    return fail("unexpected character");
  }
};
}  // namespace

bool JsonValue::as_u64(uint64_t* out) const {
  if (type == Int) {
    if (i < 0) return false;
    *out = uint64_t(i);
    return true;
  }
  if (type == String) {
    if (s.empty() || s.size() > 20) return false;
    uint64_t acc = 0;
    for (char c : s) {
      if (c < '0' || c > '9') return false;
      unsigned d = unsigned(c - '0');
      if (acc > (UINT64_MAX - d) / 10) return false;
      acc = acc * 10 + d;
    }
    *out = acc;
    return true;
  }
  return false;
}

bool json_parse(const char* text, size_t len, JsonValue* out, std::string* err) {
  Parser ps;
  ps.p = text;
  ps.end = text + len;
  *out = JsonValue();
  bool ok = ps.value(out, 0);
  if (ok) {
    ps.ws();
    if (ps.p != ps.end) { ok = false; ps.fail("trailing data"); }
  }
  if (!ok && err) *err = ps.err;
  return ok;
}

bool base64_decode(const std::string& in, std::vector<uint8_t>* out, size_t max_out) {
  out->clear();
  uint32_t acc = 0;
  int bits = 0;
  size_t pad = 0;
  for (char c : in) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    if (c == '=') { pad++; continue; }
    if (pad) return false;  // パディングの後に文字
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else return false;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (out->size() >= max_out) return false;
      out->push_back(uint8_t((acc >> bits) & 0xff));
    }
  }
  return pad <= 2;
}

}  // namespace tf
}  // namespace usbmem
