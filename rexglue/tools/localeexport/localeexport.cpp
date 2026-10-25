// localeexport: the game's strings (language/strings/MainXX.bin) as a
// localisation mod's text file - "KEY = text" a line, UTF-8, \n for a new
// line - the start of a translation.
//
//   localeexport <MainXX.bin> <out.txt> [file to find key names in]...
//
// The game keeps a string by the hash of its key, not the key: names are found
// by hashing what the given files hold (the interface's movies, the game's
// code) and seeing which hashes the table has. A string whose name was not
// found is written with its hash, "0x1234ABCD = ...", which the mod takes too.
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/locale_hash.h"

namespace {

std::vector<uint8_t> Load(const char* path) {
  std::vector<uint8_t> out;
  if (FILE* f = std::fopen(path, "rb")) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
  }
  return out;
}

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
  return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
}

void PutUtf8(std::string& out, uint32_t c) {
  if (c < 0x80) out += char(c);
  else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
  else { out += char(0xE0 | c >> 12); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: localeexport <MainXX.bin> <out.txt> [file to find key names in]...\n");
    return 2;
  }
  const std::vector<uint8_t> table = Load(argv[1]);
  if (table.size() < 16) {
    std::fprintf(stderr, "%s: not read\n", argv[1]);
    return 1;
  }
  const uint32_t count = U32(table, 8), list = U32(table, 12);
  std::map<int32_t, std::string> texts;  // by hash, in the table's order
  std::vector<int32_t> order;
  for (uint32_t i = 0; i < count && list + i * 4 + 4 <= table.size(); ++i) {
    const uint32_t at = U32(table, list + i * 4);
    if (at + 4 > table.size()) continue;
    const int32_t hash = int32_t(U32(table, at));
    std::string text;
    for (size_t p = at + 4; p + 1 < table.size(); p += 2) {
      const uint32_t c = uint32_t(table[p]) << 8 | table[p + 1];
      if (!c) break;
      if (c == '\n') text += "\\n";
      else if (c == '\\') text += "\\\\";
      else PutUtf8(text, c);
    }
    texts[hash] = text;
    order.push_back(hash);
  }
  // Names: every run of name characters in the given files, with and
  // without a leading $.
  std::map<int32_t, std::string> names;
  for (int a = 3; a < argc; ++a) {
    const std::vector<uint8_t> data = Load(argv[a]);
    std::string word;
    for (size_t i = 0; i <= data.size(); ++i) {
      const char c = i < data.size() ? char(data[i]) : 0;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '$' ||
          c == '.' || c == '-' || c == '%') {
        word += c;
        continue;
      }
      for (std::string w = word; w.size() >= 2;) {
        if (w[0] == '$') w = w.substr(1);
        const int32_t h = xerenge::locale::KeyHash(w);
        if (texts.count(h) && !names.count(h)) names[h] = w;
        break;
      }
      // A key made by a format (EATraxSongTitle%d): each number put in.
      for (const char* spec : {"%d", "%02d", "%03d", "%i", "%u"}) {
        const size_t at = word.find(spec);
        if (at == std::string::npos) continue;
        std::string w = word[0] == '$' ? word.substr(1) : word;
        const size_t pos = w.find(spec);
        for (int n = 0; n < 1000; ++n) {
          char num[8];
          std::snprintf(num, sizeof(num), spec, n);
          const std::string key = w.substr(0, pos) + num + w.substr(pos + std::char_traits<char>::length(spec));
          const int32_t h = xerenge::locale::KeyHash(key);
          if (texts.count(h) && !names.count(h)) names[h] = key;
        }
      }
      word.clear();
    }
  }
  FILE* out = std::fopen(argv[2], "wb");
  if (!out) return 1;
  std::fprintf(out, "# %s: %u strings, %zu with their names\n", argv[1], count, names.size());
  for (int32_t h : order) {
    const auto name = names.find(h);
    if (name != names.end()) std::fprintf(out, "%s = %s\n", name->second.c_str(), texts[h].c_str());
    else std::fprintf(out, "0x%08X = %s\n", uint32_t(h), texts[h].c_str());
  }
  std::fclose(out);
  std::printf("%u strings, %zu named\n", count, names.size());
  return 0;
}
