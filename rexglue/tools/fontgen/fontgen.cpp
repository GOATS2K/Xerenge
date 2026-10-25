// fontgen: the game's fonts (language/fonts/*.bin) as BMFonts, and back - what
// the game does at its start with a localisation mod's fonts (src/game_font.cpp).
//
//   fontgen export <font.bin> <name> <dir>     <dir>/<name>.fnt and .png, with
//                                              Cyrillic made of its own letters
//   fontgen fnt <font.bin> <file.fnt> <out.bin>  the font made of a BMFont
//   fontgen ttf <font.bin> <file.ttf> <out.bin>  the font made of a TTF
// The last two also write <out.bin>.png: a line of text drawn with it.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"

#include "../../src/game_font.h"

namespace {

bool Load(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t buf[65536];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

bool Save(const std::string& path, const void* data, size_t size) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fwrite(data, 1, size, f);
  std::fclose(f);
  return true;
}

std::string Dir(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

void Preview(const std::vector<uint8_t>& font, const std::string& path) {
  const std::u16string text =
      u"АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ абвгдеёжзийклмнопрстуфхцчшщъыьэюя "
      u"Съешь же ещё этих мягких французских булок, да выпей чаю. Lights And Sounds";
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  if (!xerenge::fonts::DrawText(font, text, rgba, w, h)) return;
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
  std::fwrite(rgba.data(), 1, rgba.size(), f);
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: fontgen export <font.bin> <name> <dir>\n"
                 "       fontgen fnt <font.bin> <file.fnt> <out.bin>\n"
                 "       fontgen ttf <font.bin> <file.ttf> <out.bin>\n");
    return 2;
  }
  const std::string mode = argv[1];
  std::vector<uint8_t> font;
  if (!Load(argv[2], font)) {
    std::fprintf(stderr, "%s: not read\n", argv[2]);
    return 1;
  }
  std::string why;
  if (mode == "export") {
    std::string fnt;
    std::vector<uint8_t> png;
    if (!xerenge::fonts::ExportBmfont(font, argv[3], fnt, png, &why)) {
      std::fprintf(stderr, "%s: %s\n", argv[2], why.c_str());
      return 1;
    }
    const std::string base = std::string(argv[4]) + "/" + argv[3];
    Save(base + ".fnt", fnt.data(), fnt.size());
    Save(base + ".png", png.data(), png.size());
    std::printf("%s.fnt, %s.png\n", base.c_str(), base.c_str());
    return 0;
  }
  std::vector<uint8_t> out;
  bool ok = false;
  if (mode == "fnt") {
    std::vector<uint8_t> text;
    if (!Load(argv[3], text)) {
      std::fprintf(stderr, "%s: not read\n", argv[3]);
      return 1;
    }
    const std::string dir = Dir(argv[3]);
    ok = xerenge::fonts::FromBmfont(
        font, std::string(text.begin(), text.end()),
        [&](const std::string& file, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
          int x, y, n;
          stbi_uc* px = stbi_load((dir + file).c_str(), &x, &y, &n, 4);
          if (!px) return false;
          rgba.assign(px, px + size_t(x) * y * 4);
          stbi_image_free(px);
          w = uint32_t(x);
          h = uint32_t(y);
          return true;
        },
        out, &why);
  } else if (mode == "ttf") {
    std::vector<uint8_t> ttf;
    if (!Load(argv[3], ttf)) {
      std::fprintf(stderr, "%s: not read\n", argv[3]);
      return 1;
    }
    ok = xerenge::fonts::FromTtf(font, ttf, u"", out, &why);
  }
  if (!ok) {
    std::fprintf(stderr, "%s: %s\n", argv[3], why.c_str());
    return 1;
  }
  Save(argv[4], out.data(), out.size());
  Preview(out, std::string(argv[4]) + ".pam");
  std::printf("%s: %zu bytes\n", argv[4], out.size());
  return 0;
}
