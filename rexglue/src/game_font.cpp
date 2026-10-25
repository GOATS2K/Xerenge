// Cyrillic for the game's fonts (language/fonts/*.bin), made of their own
// glyphs. The fonts hold Latin-1 and a few quotes; Burnout's interface can
// draw any code its font has, so А-я and Ёё are added to the font itself.
//
// The file ("v1.4", big-endian):
//   +0x04  where the texture is (0x2000);
//   +0x20  128 glyph lists by code & 0x7F, each the offset of its first
//          record; a list runs up to the next one's (in file order), and an
//          empty one points at the "no glyph" record, the last of them;
//   records, 0x28 bytes: u, v, width, height (of the atlas, 0..1), x and y
//          offsets and the advance (in the atlas' units too), the code
//          (u16), zeros;
//   the texture: a name, a header (its size at +0x20, the section's size at
//          +0x2C, the pixels' offset at +0x30, a fetch constant at +0x44),
//          and at +0x1000 the pixels, 8888 (ARGB), tiled as the Xenos tiles.
// The glyphs, old and new, are packed anew into an atlas of the original's
// size (the game sizes them by their share of it), the records into their
// codes' lists, the texture after the records.
#include "game_font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

namespace xerenge::fonts {
namespace {

constexpr uint32_t kTable = 0x20;
constexpr uint32_t kBuckets = 128;
constexpr uint32_t kRecord = 0x28;

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
  return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
}
void Set32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
  d[at] = uint8_t(v >> 24);
  d[at + 1] = uint8_t(v >> 16);
  d[at + 2] = uint8_t(v >> 8);
  d[at + 3] = uint8_t(v);
}
float F32(const std::vector<uint8_t>& d, size_t at) {
  const uint32_t bits = U32(d, at);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
uint32_t Bits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

// --- the Xenos' tiling of a 32-bit texture -------------------------------------

uint32_t TiledRow(uint32_t y, uint32_t width) {
  const uint32_t macro = ((y >> 5) * (width >> 5)) << 9;
  const uint32_t micro = ((y & 6) << 2) << 2;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << 5) + ((y & 1) << 4);
}
uint32_t TiledTexel(uint32_t x, uint32_t y, uint32_t row) {
  const uint32_t macro = (x >> 5) << 9;
  const uint32_t micro = (x & 7) << 2;
  const uint32_t off = row + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return (((off & ~0x1FFu) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> 2;
}

// --- the font ----------------------------------------------------------------------

struct Record {
  uint16_t code = 0;
  float f[7] = {};  // u, v, width, height, x offset, y offset, advance
};

struct Font {
  std::vector<Record> records;  // the "no glyph" one apart
  Record missing;
  std::vector<uint8_t> head;     // up to the texture (header and table, unused here)
  std::vector<uint8_t> texture;  // its name and header, 0x1000 bytes
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // untiled
  bool cyrillic = false;  // has some already
};

bool Read(const std::vector<uint8_t>& d, Font& font, std::string* why) {
  const auto fail = [&](const char* reason) {
    if (why) *why = reason;
    return false;
  };
  if (d.size() < 0x3000 || std::memcmp(d.data(), "v1.4", 4) != 0) {
    return fail("not a v1.4 font");
  }
  const uint32_t texture = U32(d, 4);
  if (texture < kTable + kBuckets * 4 || texture + 0x1000 > d.size()) {
    return fail("no texture where the header says");
  }
  font.width = uint32_t(d[texture + 0x20]) << 8 | d[texture + 0x21];
  font.height = uint32_t(d[texture + 0x22]) << 8 | d[texture + 0x23];
  const uint32_t pixels = texture + U32(d, texture + 0x30);
  if (!font.width || !font.height || (font.width & 31) || (font.height & 31) ||
      pixels + size_t(font.width) * font.height * 4 > d.size()) {
    return fail("an atlas of an unknown size");
  }
  // The lists: each from its first record to the next list's.
  std::vector<uint32_t> heads;
  uint32_t missing = 0;
  for (uint32_t b = 0; b < kBuckets; ++b) {
    const uint32_t at = U32(d, kTable + b * 4);
    heads.push_back(at);
    missing = std::max(missing, at);
  }
  if (missing + kRecord > texture) {
    return fail("records past the texture");
  }
  std::vector<uint32_t> starts(heads.begin(), heads.end());
  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
  const auto read_record = [&](uint32_t at) {
    Record r;
    for (int i = 0; i < 7; ++i) r.f[i] = F32(d, at + i * 4);
    r.code = uint16_t(U32(d, at + 28) >> 16);
    return r;
  };
  for (size_t i = 0; i + 1 < starts.size(); ++i) {
    for (uint32_t at = starts[i]; at < starts[i + 1]; at += kRecord) {
      font.records.push_back(read_record(at));
    }
  }
  font.missing = read_record(missing);
  font.head.assign(d.begin(), d.begin() + kTable);
  font.texture.assign(d.begin() + texture, d.begin() + texture + 0x1000);
  font.rgba.resize(size_t(font.width) * font.height * 4);
  for (uint32_t y = 0; y < font.height; ++y) {
    const uint32_t row = TiledRow(y, font.width);
    for (uint32_t x = 0; x < font.width; ++x) {
      const uint8_t* p = &d[pixels + size_t(TiledTexel(x, y, row)) * 4];
      uint8_t* q = &font.rgba[(size_t(y) * font.width + x) * 4];
      q[0] = p[1];
      q[1] = p[2];
      q[2] = p[3];
      q[3] = p[0];
    }
  }
  for (const Record& r : font.records) {
    font.cyrillic = font.cyrillic || (r.code >= 0x400 && r.code < 0x460);
  }
  return true;
}

void Write(const Font& font, std::vector<uint8_t>& out) {
  // Records by list, the "no glyph" one last; the texture on a page after.
  std::vector<const Record*> lists[kBuckets];
  for (const Record& r : font.records) {
    lists[r.code & 0x7F].push_back(&r);
  }
  for (auto& list : lists) {
    std::stable_sort(list.begin(), list.end(), [](const Record* a, const Record* b) { return a->code < b->code; });
  }
  uint32_t at = kTable + kBuckets * 4;
  std::vector<uint8_t> records;
  uint32_t heads[kBuckets];
  const auto put = [&](const Record& r) {
    const size_t o = records.size();
    records.resize(o + kRecord);
    for (int i = 0; i < 7; ++i) Set32(records, o + i * 4, Bits(r.f[i]));
    Set32(records, o + 28, uint32_t(r.code) << 16);
  };
  for (uint32_t b = 0; b < kBuckets; ++b) {
    heads[b] = 0;
    if (!lists[b].empty()) {
      heads[b] = at + uint32_t(records.size());
      for (const Record* r : lists[b]) put(*r);
    }
  }
  const uint32_t missing = at + uint32_t(records.size());
  put(font.missing);
  const uint32_t texture = std::max<uint32_t>(0x2000, (missing + kRecord + 0xFFF) & ~0xFFFu);
  const size_t pixels_size = size_t(font.width) * font.height * 4;
  out.assign(texture + 0x1000 + pixels_size, 0);
  std::memcpy(out.data(), font.head.data(), std::min<size_t>(font.head.size(), kTable));
  Set32(out, 4, texture);
  for (uint32_t b = 0; b < kBuckets; ++b) {
    Set32(out, kTable + b * 4, heads[b] ? heads[b] : missing);
  }
  std::memcpy(&out[at], records.data(), records.size());
  // The texture: its header as it was but for the atlas' height.
  std::memcpy(&out[texture], font.texture.data(), 0x1000);
  out[texture + 0x20] = uint8_t(font.width >> 8);
  out[texture + 0x21] = uint8_t(font.width);
  out[texture + 0x22] = uint8_t(font.height >> 8);
  out[texture + 0x23] = uint8_t(font.height);
  Set32(out, texture + 0x2C, uint32_t(pixels_size + 0x1000));
  Set32(out, texture + 0x30, 0x1000);
  const uint32_t size_word = U32(out, texture + 0x4C);
  Set32(out, texture + 0x4C, (size_word & ~0x3FFFFFFu) | ((font.height - 1) << 13) | (font.width - 1));
  // The fetch constant's pitch, in 32 texels.
  const uint32_t pitch_word = U32(out, texture + 0x44);
  Set32(out, texture + 0x44, (pitch_word & ~(0x1FFu << 22)) | ((font.width / 32) << 22));
  uint8_t* pixels = &out[texture + 0x1000];
  for (uint32_t y = 0; y < font.height; ++y) {
    const uint32_t row = TiledRow(y, font.width);
    for (uint32_t x = 0; x < font.width; ++x) {
      const uint8_t* q = &font.rgba[(size_t(y) * font.width + x) * 4];
      uint8_t* p = pixels + size_t(TiledTexel(x, y, row)) * 4;
      p[0] = q[3];
      p[1] = q[0];
      p[2] = q[1];
      p[3] = q[2];
    }
  }
}

// --- glyphs as pictures ----------------------------------------------------------

struct Glyph {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // RGBA
  float xoff = 0, yoff = 0, adv = 0;  // in atlas pixels
  bool ok = false;

  uint8_t* At(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
  const uint8_t* At(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
  uint8_t Alpha(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : At(x, y)[3]; }
};

Glyph Blank(int w, int h) {
  Glyph g;
  g.w = std::max(w, 1);
  g.h = std::max(h, 1);
  g.px.assign(size_t(g.w) * g.h * 4, 0);
  g.ok = true;
  return g;
}

class Maker {
 public:
  explicit Maker(Font& font) : font_(font) {
    for (const Record& r : font.records) by_code_[r.code] = &r;
  }

  bool Has(char16_t code) const { return by_code_.count(code) != 0; }

  Glyph Get(char16_t code) const {
    const auto found = by_code_.find(code);
    Glyph g;
    if (found == by_code_.end()) return g;
    const Record& r = *found->second;
    const float W = float(font_.width), H = float(font_.height);
    const int x0 = int(std::lround(r.f[0] * W)), y0 = int(std::lround(r.f[1] * H));
    g = Blank(int(std::lround(r.f[2] * W)), int(std::lround(r.f[3] * H)));
    for (int y = 0; y < g.h; ++y) {
      for (int x = 0; x < g.w; ++x) {
        const int sx = x0 + x, sy = y0 + y;
        if (sx >= 0 && sy >= 0 && sx < int(font_.width) && sy < int(font_.height)) {
          std::memcpy(g.At(x, y), &font_.rgba[(size_t(sy) * font_.width + sx) * 4], 4);
        }
      }
    }
    g.xoff = r.f[4] * W;
    g.yoff = r.f[5] * H;
    g.adv = r.f[6] * W;
    return g;
  }

  Font& font_;
  std::map<char16_t, const Record*> by_code_;
};

Glyph FlipH(const Glyph& g) {
  Glyph o = g;
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w; ++x) std::memcpy(o.At(x, y), g.At(g.w - 1 - x, y), 4);
  return o;
}

Glyph FlipV(const Glyph& g) {
  Glyph o = g;
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w; ++x) std::memcpy(o.At(x, y), g.At(x, g.h - 1 - y), 4);
  return o;
}

Glyph Rot180(const Glyph& g) { return FlipV(FlipH(g)); }

// Resampled (bilinear) to w x h; the metrics follow the width.
Glyph Resize(const Glyph& g, int w, int h) {
  Glyph o = Blank(w, h);
  o.xoff = g.xoff * float(w) / float(g.w);
  o.yoff = g.yoff;
  o.adv = g.adv - g.w + w;
  for (int y = 0; y < o.h; ++y) {
    for (int x = 0; x < o.w; ++x) {
      const float sx = (x + 0.5f) * g.w / o.w - 0.5f, sy = (y + 0.5f) * g.h / o.h - 0.5f;
      const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
      const float fx = sx - x0, fy = sy - y0;
      for (int c = 0; c < 4; ++c) {
        float v = 0;
        for (int k = 0; k < 4; ++k) {
          const int xx = std::clamp(x0 + (k & 1), 0, g.w - 1), yy = std::clamp(y0 + (k >> 1), 0, g.h - 1);
          const float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
          v += g.At(xx, yy)[c] * wgt;
        }
        o.At(x, y)[c] = uint8_t(std::lround(std::clamp(v, 0.f, 255.f)));
      }
    }
  }
  return o;
}

Glyph Scale(const Glyph& g, float sx, float sy) {
  return Resize(g, std::max(1, int(std::lround(g.w * sx))), std::max(1, int(std::lround(g.h * sy))));
}

// `b` drawn over `a` with its top-left at (dx, dy) of a's; the canvas grows to
// hold both (a's metrics kept, moved with it).
Glyph Over(const Glyph& a, const Glyph& b, int dx, int dy) {
  const int left = std::min(0, dx), top = std::min(0, dy);
  const int right = std::max(a.w, dx + b.w), bottom = std::max(a.h, dy + b.h);
  Glyph o = Blank(right - left, bottom - top);
  o.xoff = a.xoff + left;
  o.yoff = a.yoff + top;
  o.adv = a.adv + (o.w - a.w);
  const auto draw = [&](const Glyph& g, int ox, int oy) {
    for (int y = 0; y < g.h; ++y)
      for (int x = 0; x < g.w; ++x) {
        uint8_t* p = o.At(ox + x, oy + y);
        const uint8_t* q = g.At(x, y);
        for (int c = 0; c < 4; ++c) p[c] = std::max(p[c], q[c]);
      }
  };
  draw(a, -left, -top);
  draw(b, dx - left, dy - top);
  return o;
}

Glyph Crop(const Glyph& g, int x0, int y0, int w, int h) {
  x0 = std::clamp(x0, 0, g.w - 1);
  y0 = std::clamp(y0, 0, g.h - 1);
  w = std::clamp(w, 1, g.w - x0);
  h = std::clamp(h, 1, g.h - y0);
  Glyph o = Blank(w, h);
  o.xoff = g.xoff;
  o.yoff = g.yoff + y0;
  o.adv = g.adv;
  for (int y = 0; y < h; ++y) std::memcpy(o.At(0, y), g.At(x0, y0 + y), size_t(w) * 4);
  return o;
}

void Erase(Glyph& g, int x0, int y0, int w, int h) {
  for (int y = std::max(0, y0); y < std::min(g.h, y0 + h); ++y)
    for (int x = std::max(0, x0); x < std::min(g.w, x0 + w); ++x) std::memset(g.At(x, y), 0, 4);
}

// The thickness of a horizontal bar at the top (from the top row down) and
// of the left stem (from the left column across), where most is ink.
int TopBar(const Glyph& g) {
  int n = 0;
  for (int y = 0; y < g.h; ++y) {
    int ink = 0;
    for (int x = 0; x < g.w; ++x) ink += g.Alpha(x, y) > 128;
    if (ink * 10 >= g.w * 6) ++n;
    else if (n) break;
  }
  return std::max(n, 1);
}

// The width of a glyph's ink: from its first column with any to its last.
int InkWidth(const Glyph& g) {
  int first = g.w, last = -1;
  for (int x = 0; x < g.w; ++x)
    for (int y = 0; y < g.h; ++y)
      if (g.Alpha(x, y) > 64) {
        first = std::min(first, x);
        last = std::max(last, x);
        break;
      }
  return last >= first ? last - first + 1 : 1;
}

int LeftStem(const Glyph& g) {
  int n = 0;
  for (int x = 0; x < g.w; ++x) {
    int ink = 0;
    for (int y = 0; y < g.h; ++y) ink += g.Alpha(x, y) > 128;
    if (ink * 10 >= g.h * 7) ++n;
    else if (n) break;
  }
  return std::max(n, 1);
}

// A letter with a mark above (Ё's dots, Й's breve): the empty rows between
// the mark and the letter cut down to `gap` - the fonts leave room there for
// capitals' marks over small letters too.
Glyph CloserMark(const Glyph& g, int gap) {
  std::vector<bool> ink(g.h, false);
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w && !ink[y]; ++x) ink[y] = g.Alpha(x, y) > 64;
  int y = 0;
  while (y < g.h && !ink[y]) ++y;  // above the mark
  while (y < g.h && ink[y]) ++y;   // the mark
  const int first_blank = y;
  while (y < g.h && !ink[y]) ++y;  // the gap
  const int blank = y - first_blank;
  if (y >= g.h || blank <= gap) return g;
  const int cut = blank - gap;
  // The mark moved down by `cut`: the rows above the gap shift, the letter stays.
  Glyph o = Blank(g.w, g.h - cut);
  o.xoff = g.xoff;
  o.yoff = g.yoff + cut;
  o.adv = g.adv;
  for (int r = 0; r < o.h; ++r) {
    const int from = r < first_blank ? r : r + cut;
    std::memcpy(o.At(0, r), g.At(0, from), size_t(g.w) * 4);
  }
  return o;
}

// --- the letters --------------------------------------------------------------------

struct Letters {
  const Maker& m;
  int cap = 0, stem = 0, bar = 0;
  Glyph H, E, I;

  explicit Letters(const Maker& maker) : m(maker) {
    H = m.Get(u'H');
    E = m.Get(u'E');
    I = m.Get(u'I');
    cap = H.h;
    // The stem's width: I's, or H's left column where the wear leaves I none.
    stem = std::max(LeftStem(H), InkWidth(I));
    bar = TopBar(E);
  }

  Glyph ToCap(Glyph g) const {
    if (!g.ok || g.h == cap) return g;
    const float top = g.yoff;
    g = Resize(g, std::max(1, int(std::lround(g.w * float(cap) / g.h))), cap);
    g.yoff = top;
    return g;
  }

  // E's top bar, w wide.
  Glyph Bar(int w) const { return Resize(Crop(E, 0, 0, E.w, bar), w, bar); }
  // A stroke of a stem, h tall.
  // (Its middle half: the ends of I may be rounded, worn or short of the box.)
  Glyph Stem(int h) const { return Resize(Crop(I, (I.w - stem) / 2, I.h / 4, stem, I.h / 2), stem, h); }

  Glyph Pe() const {  // П: H without its crossbar, with a bar on top
    Glyph g = H;
    Erase(g, stem, bar, g.w - 2 * stem, g.h - bar);
    return Over(g, Bar(g.w), 0, 0);
  }
  Glyph Soft() const {  // Ь: P upside down - the stem, the bowl at the foot
    Glyph p = FlipV(m.Get(u'P'));
    p.yoff = H.yoff;
    return p;
  }

  // A made letter's advance: its width and H's side bearings.
  Glyph Spaced(Glyph g) const {
    if (!g.ok) return g;
    const float bearing = (H.adv - H.w) * 0.5f;
    g.xoff = H.xoff;
    g.adv = g.w + 2 * bearing;
    return g;
  }
  Glyph Tail(const Glyph& g) const {  // Ц and Щ: a stem below the right corner
    const int t = std::max(2, cap / 6);
    return Over(g, Stem(t), g.w - stem, g.h);
  }
  Glyph Sha() const {  // Ш: П turned over, wider, a stem in the middle
    Glyph u = FlipV(Scale(Pe(), 1.45f, 1.0f));
    u.yoff = H.yoff;
    return Over(u, Stem(u.h), (u.w - stem) / 2, 0);
  }

  Glyph Upper(char16_t c) const {
    const Glyph g = Made(c);
    switch (c) {
      case u'А': case u'В': case u'Е': case u'Ё': case u'К': case u'М': case u'Н':
      case u'О': case u'Р': case u'С': case u'Т': case u'Х': case u'У':
        return g;  // the Latin letter as it is
      default:
        return Spaced(g);
    }
  }

  Glyph Made(char16_t c) const {
    switch (c) {
      case u'А': return m.Get(u'A');
      case u'В': return m.Get(u'B');
      case u'Е': return m.Get(u'E');
      case u'Ё': return CloserMark(m.Get(u'Ë'), std::max(1, stem / 2));
      case u'К': return m.Get(u'K');
      case u'М': return m.Get(u'M');
      case u'Н': return m.Get(u'H');
      case u'О': return m.Get(u'O');
      case u'Р': return m.Get(u'P');
      case u'С': return m.Get(u'C');
      case u'Т': return m.Get(u'T');
      case u'Х': return m.Get(u'X');
      case u'У': return m.Get(u'Y');
      case u'Я': return FlipH(m.Get(u'R'));
      case u'И': return FlipH(m.Get(u'N'));
      case u'Г': return FlipV(m.Get(u'L'));
      case u'Л': return FlipV(m.Get(u'V'));
      case u'З': return ToCap(m.Get(u'3'));
      case u'Ч': return ToCap(Rot180(m.Get(u'h')));
      case u'Б': {  // В's lower half under Г: B without its upper bowl, a bar across the top
        Glyph b = m.Get(u'B');
        // The middle bar: the first row near the middle that is mostly ink.
        int middle = b.h / 2;
        for (int y = b.h * 3 / 10; y < b.h * 7 / 10; ++y) {
          int ink = 0;
          for (int x = 0; x < b.w; ++x) ink += b.Alpha(x, y) > 128;
          if (ink * 10 >= b.w * 6) {
            middle = y;
            break;
          }
        }
        // Г (L turned over: the font's own bar) over B from its middle bar down.
        Glyph g = FlipV(m.Get(u'L'));
        g.yoff = b.yoff;
        return Over(g, Crop(b, 0, middle, b.w, b.h - middle), 0, middle);
      }
      case u'Д': {  // П on a bar a stem wider each side, its ends going down
        Glyph p = Scale(Pe(), 0.9f, 1.0f);
        Glyph g = Over(Blank(p.w + 2 * stem, p.h), p, stem, 0);
        g.xoff = H.xoff;
        g.yoff = H.yoff;
        g.adv = H.adv + 2 * stem;
        Erase(g, stem, g.h - bar, p.w, bar);
        g = Over(g, Bar(g.w), 0, g.h - bar);
        const int leg = std::max(2, cap / 6);
        g = Over(g, Stem(leg), 0, g.h);
        g = Over(g, Stem(leg), g.w - stem, g.h - leg);
        return g;
      }
      case u'Ж': {  // a wide X with a stem through it
        Glyph x = Scale(m.Get(u'X'), 1.4f, 1.0f);
        return Over(x, Stem(x.h), (x.w - stem) / 2, 0);
      }
      case u'Й': {  // И under Ñ's mark
        Glyph i = FlipH(m.Get(u'N'));
        const Glyph n = m.Get(u'Ñ');
        if (!n.ok || n.h <= cap) return i;
        Glyph mark = Crop(n, 0, 0, n.w, n.h - cap);
        return CloserMark(Over(i, mark, (i.w - mark.w) / 2, -(n.h - cap)), std::max(1, stem / 2));
      }
      case u'П': return Pe();
      case u'Ф': {  // O with a stem through it, a little past it above and below
        Glyph o = m.Get(u'O');
        const int past = std::max(2, cap / 10);
        return Over(o, Stem(o.h + 2 * past), (o.w - stem) / 2, -past);
      }
      case u'Ц': {
        Glyph u = FlipV(Pe());
        u.yoff = H.yoff;
        return Tail(u);
      }
      case u'Ш': return Sha();
      case u'Щ': return Tail(Sha());
      case u'Ь': return Soft();
      case u'Ъ': {  // Ь with a bar out to the left at the top
        const Glyph s = Soft();
        const int arm = stem * 2;
        Glyph g = Over(s, Bar(arm + stem), -arm, 0);
        return g;
      }
      case u'Ы': {
        const Glyph s = Soft();
        Glyph g = Over(s, Stem(s.h), s.w + std::max(1, stem / 2), 0);
        g.adv = s.adv + stem + std::max(1, stem / 2);
        return g;
      }
      case u'Э': {  // C turned round, a bar in the middle
        Glyph c = FlipH(m.Get(u'C'));
        const int w = c.w * 5 / 9;
        return Over(c, Resize(Crop(E, 0, 0, E.w, bar), w, bar), c.w - w - stem / 2, (c.h - bar) / 2);
      }
      case u'Ю': {  // I, a bar, a narrower O
        Glyph o = Scale(m.Get(u'O'), 0.85f, 1.0f);
        const int join = std::max(2, stem * 2 / 3);
        Glyph g = Over(I, Resize(Crop(E, 0, 0, E.w, bar), join + stem, bar), I.w - stem / 2, (I.h - bar) / 2);
        g = Over(g, o, I.w + join, 0);
        g.adv = I.adv + join + o.adv;
        return g;
      }
      default: return Glyph();
    }
  }

  Glyph Lower(char16_t c, const Glyph& x) const {
    switch (c) {
      case u'а': return m.Get(u'a');
      case u'е': return m.Get(u'e');
      case u'ё': return CloserMark(m.Get(u'ë'), std::max(1, stem / 2));
      case u'о': return m.Get(u'o');
      case u'р': return m.Get(u'p');
      case u'с': return m.Get(u'c');
      case u'у': return m.Get(u'y');
      case u'х': return m.Get(u'x');
      case u'ф': {  // a wider o on a stem from the ascenders to the descenders
        const Glyph o = Scale(m.Get(u'o'), 1.3f, 1.0f), b = m.Get(u'b'), p = m.Get(u'p');
        if (!o.ok || !b.ok || !p.ok) break;
        const int rise = int(std::lround(o.yoff - b.yoff));
        const int fall = int(std::lround((p.yoff + p.h) - (o.yoff + o.h)));
        return Spaced(Over(o, Stem(rise + o.h + fall), (o.w - stem) / 2, -rise));
      }
      default: break;
    }
    // The rest: the capital, as tall as x.
    const char16_t upper = c == u'ё' ? u'Ё' : char16_t(c - 0x20);
    Glyph g = Upper(upper);
    if (!g.ok || !x.ok) return g;
    const float s = float(x.h) / float(cap);
    const float under = g.yoff + g.h - (H.yoff + cap);  // below the baseline (Д, Ц, Щ)
    const float over = H.yoff - g.yoff;                  // above the capitals (Й)
    g = Scale(g, s, s);
    g.yoff = x.yoff - over * s;
    (void)under;
    return g;
  }
};

// --- where they go in the atlas -------------------------------------------------

struct Packer {
  uint32_t w, h;
  std::vector<uint8_t> used;
  Packer(uint32_t width, uint32_t height) : w(width), h(height), used(size_t(width) * height, 0) {}

  void Mark(int x0, int y0, int gw, int gh, int pad) {
    for (int y = std::max(0, y0 - pad); y < std::min(int(h), y0 + gh + pad); ++y)
      for (int x = std::max(0, x0 - pad); x < std::min(int(w), x0 + gw + pad); ++x) used[size_t(y) * w + x] = 1;
  }
  bool Free(int x0, int y0, int gw, int gh) const {
    if (x0 < 0 || y0 < 0 || x0 + gw > int(w) || y0 + gh > int(h)) return false;
    for (int y = y0; y < y0 + gh; ++y)
      for (int x = x0; x < x0 + gw; ++x)
        if (used[size_t(y) * w + x]) return false;
    return true;
  }
  bool Place(int gw, int gh, int& ox, int& oy) {
    for (int y = 0; y + gh <= int(h); ++y)
      for (int x = 0; x + gw <= int(w); ++x)
        if (Free(x, y, gw, gh)) {
          ox = x;
          oy = y;
          Mark(x, y, gw, gh, 1);
          return true;
        }
    return false;
  }
};

// The ink of a glyph's picture: transparent edges taken off (the metrics kept).
Glyph Trim(const Glyph& g) {
  int x0 = g.w, y0 = g.h, x1 = -1, y1 = -1;
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w; ++x)
      if (g.At(x, y)[3]) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
  if (x1 < 0) return g;
  Glyph o = Crop(g, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
  o.xoff = g.xoff + x0;
  return o;
}

struct NewGlyph {
  uint32_t code;
  int x, y, w, h;
  float xoff, yoff, adv;
};

bool Rebuild(Font& font, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, std::vector<NewGlyph> glyphs,
             std::vector<uint8_t>& out, std::string* why);

}  // namespace

bool AddCyrillic(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string* why) {
  Font font;
  if (!Read(in, font, why)) return false;
  if (font.cyrillic) {
    if (why) *why = "Cyrillic already there";
    return false;
  }
  Maker maker(font);
  if (!maker.Has(u'H') || !maker.Has(u'E') || !maker.Has(u'I')) {
    if (why) *why = "no H, E or I to make the letters of";
    return false;
  }
  Letters letters(maker);
  const Glyph x = maker.Get(u'x');
  const bool lower = maker.Has(u'a') && x.ok;

  std::vector<std::pair<char16_t, Glyph>> made;
  for (char16_t c = u'А'; c <= u'Я'; ++c) made.push_back({c, letters.Upper(c)});
  made.push_back({u'Ё', letters.Upper(u'Ё')});
  for (char16_t c = u'а'; c <= u'я'; ++c) made.push_back({c, lower ? letters.Lower(c, x) : letters.Upper(char16_t(c - 0x20))});
  made.push_back({u'ё', lower ? letters.Lower(u'ё', x) : letters.Upper(u'Ё')});

  // The font's glyphs where they are, the new ones in a strip under the
  // atlas, all packed anew into an atlas of the font's own size (Rebuild).
  int strip_w = 0, strip_h = 0;
  for (const auto& m : made) {
    if (!m.second.ok) continue;
    strip_w += m.second.w + 1;
    strip_h = std::max(strip_h, m.second.h);
  }
  const uint32_t src_w = std::max<uint32_t>(font.width, uint32_t(strip_w));
  const uint32_t src_h = font.height + uint32_t(strip_h);
  std::vector<uint8_t> source(size_t(src_w) * src_h * 4, 0);
  for (uint32_t y = 0; y < font.height; ++y)
    std::memcpy(&source[size_t(y) * src_w * 4], &font.rgba[size_t(y) * font.width * 4], size_t(font.width) * 4);
  const float W = float(font.width), H = float(font.height);
  std::vector<NewGlyph> glyphs;
  for (const Record& r : font.records) {
    NewGlyph g{r.code, 0, 0, 0, 0, r.f[4] * W, r.f[5] * H, r.f[6] * W};
    if (r.f[0] >= 0) {
      g.x = int(std::lround(r.f[0] * W));
      g.y = int(std::lround(r.f[1] * H));
      g.w = int(std::lround(r.f[2] * W));
      g.h = int(std::lround(r.f[3] * H));
    }
    glyphs.push_back(g);
  }
  int pen = 0;
  for (const auto& m : made) {
    const Glyph& g = m.second;
    if (!g.ok) continue;
    for (int y = 0; y < g.h; ++y)
      std::memcpy(&source[(size_t(font.height + y) * src_w + pen) * 4], g.At(0, y), size_t(g.w) * 4);
    glyphs.push_back({m.first, pen, int(font.height), g.w, g.h, g.xoff, g.yoff, g.adv});
    pen += g.w + 1;
  }
  return Rebuild(font, source, src_w, src_h, glyphs, out, why);
}

bool AtlasImage(const std::vector<uint8_t>& data, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  Font font;
  std::string why;
  if (!Read(data, font, &why)) return false;
  rgba = font.rgba;
  width = font.width;
  height = font.height;
  return true;
}

bool DrawText(const std::vector<uint8_t>& data, const std::u16string& text, std::vector<uint8_t>& rgba,
              uint32_t& width, uint32_t& height) {
  Font font;
  std::string why;
  if (!Read(data, font, &why)) return false;
  Maker maker(font);
  Letters letters(maker);
  const int line = letters.cap * 2;
  width = 0;
  for (char16_t c : text) {
    const Glyph g = maker.Get(c);
    width += uint32_t(std::max(0.f, g.ok ? g.adv : letters.cap * 0.3f)) + 1;
  }
  width = std::max<uint32_t>(width + 8, 8);
  height = uint32_t(line);
  rgba.assign(size_t(width) * height * 4, 0);
  float pen = 4;
  for (char16_t c : text) {
    const Glyph g = maker.Get(c);
    if (!g.ok) {
      pen += letters.cap * 0.3f;
      continue;
    }
    const int ox = int(pen + g.xoff), oy = int(letters.cap / 2 + g.yoff);
    for (int y = 0; y < g.h; ++y)
      for (int x = 0; x < g.w; ++x) {
        const int px = ox + x, py = oy + y;
        if (px < 0 || py < 0 || px >= int(width) || py >= int(height)) continue;
        uint8_t* p = &rgba[(size_t(py) * width + px) * 4];
        const uint8_t* q = g.At(x, y);
        for (int k = 0; k < 4; ++k) p[k] = std::max(p[k], q[k]);
      }
    pen += g.adv;
  }
  return true;
}

// --- BMFont and TTF ------------------------------------------------------------------

namespace {

void PngWrite(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

uint32_t PowerOfTwo(uint32_t v, uint32_t at_least) {
  uint32_t p = at_least;
  while (p < v) p <<= 1;
  return p;
}

// key=value pairs of a BMFont line, values unquoted.
std::map<std::string, std::string> Fields(const std::string& line) {
  std::map<std::string, std::string> fields;
  size_t i = line.find(' ');
  while (i != std::string::npos && i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    const size_t eq = line.find('=', i);
    if (eq == std::string::npos) break;
    const std::string key = line.substr(i, eq - i);
    size_t end;
    std::string value;
    if (eq + 1 < line.size() && line[eq + 1] == '"') {
      end = line.find('"', eq + 2);
      value = line.substr(eq + 2, (end == std::string::npos ? line.size() : end) - eq - 2);
      end = end == std::string::npos ? line.size() : end + 1;
    } else {
      end = line.find(' ', eq + 1);
      if (end == std::string::npos) end = line.size();
      value = line.substr(eq + 1, end - eq - 1);
    }
    fields[key] = value;
    i = end;
  }
  return fields;
}

// The baseline, from the line's top: H's foot.
float Baseline(const Font& font) {
  for (const Record& r : font.records) {
    if (r.code == u'H') return (r.f[5] + r.f[3]) * font.height;
  }
  return font.height * 0.1f;
}

float CapHeight(const Font& font) {
  for (const Record& r : font.records) {
    if (r.code == u'H') return r.f[3] * font.height;
  }
  return 0;
}

// A font of `original`'s header with these glyphs: the atlas `rgba` (w x h, as
// the Xenos wants it: a power of two from 32), each glyph's box in it and its
// metrics in pixels.
// The glyphs on shelves in a W x H atlas, the tallest first, a texel apart:
// where each goes, or false when they do not all fit.
bool Pack(const std::vector<NewGlyph>& glyphs, uint32_t W, uint32_t H, std::vector<std::pair<int, int>>& spot) {
  std::vector<size_t> order(glyphs.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return glyphs[a].h != glyphs[b].h ? glyphs[a].h > glyphs[b].h : glyphs[a].w > glyphs[b].w;
  });
  spot.assign(glyphs.size(), {-1, -1});
  int x = 1, y = 1, shelf = 0;
  for (size_t i : order) {
    const NewGlyph& g = glyphs[i];
    if (g.w <= 0 || g.h <= 0) continue;
    if (x + g.w + 1 > int(W)) {
      x = 1;
      y += shelf + 1;
      shelf = 0;
    }
    if (y + g.h + 1 > int(H) || g.w + 2 > int(W)) return false;
    spot[i] = {x, y};
    x += g.w + 1;
    shelf = std::max(shelf, g.h);
  }
  return true;
}

// The glyphs (their pictures at x, y in `rgba`, w x h) packed anew into an
// atlas of the font's own size: the game sizes a glyph on the screen by its
// share of the atlas as the original's was (its own scale, not the atlas'
// pixels), so a larger atlas draws its letters smaller - squashed.
bool Rebuild(Font& font, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, std::vector<NewGlyph> glyphs,
             std::vector<uint8_t>& out, std::string* why) {
  const uint32_t W = font.width, H = font.height;
  // What may be left out when they do not all fit, the first first: the
  // accented Latin letters, then the rarer signs - not ASCII, the quotes, ©,
  // ®, ™ or Cyrillic. Each try leaves out one more of these.
  const auto droppable = [](uint32_t c) {
    if (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7) return 2;
    if (c >= 0xA0 && c <= 0xFF && c != 0xA9 && c != 0xAE) return 1;
    if ((c >= 0x100 && c < 0x400) || (c >= 0x460 && c < 0x2000)) return 2;
    return 0;
  };
  for (int tries = 0;; ++tries) {
    std::vector<std::pair<int, int>> spot;
    if (Pack(glyphs, W, H, spot)) break;
    // The next to leave out: an accented letter first, the widest of them.
    int best = -1;
    for (size_t i = 0; i < glyphs.size(); ++i) {
      const int d = droppable(glyphs[i].code);
      if (!d || glyphs[i].w <= 0) continue;
      if (best < 0 || d > droppable(glyphs[best].code) ||
          (d == droppable(glyphs[best].code) && glyphs[i].w * glyphs[i].h > glyphs[best].w * glyphs[best].h))
        best = int(i);
    }
    if (best < 0) {
      if (why) *why = "the glyphs do not fit the font's atlas (" + std::to_string(W) + "x" + std::to_string(H) + ")";
      return false;
    }
    glyphs.erase(glyphs.begin() + best);
  }
  std::vector<std::pair<int, int>> spot;
  Pack(glyphs, W, H, spot);
  font.rgba.assign(size_t(W) * H * 4, 0);
  for (size_t i = 0; i < glyphs.size(); ++i) {
    NewGlyph& g = glyphs[i];
    if (spot[i].first < 0) continue;
    for (int row = 0; row < g.h; ++row) {
      const int sy = g.y + row;
      if (sy < 0 || sy >= int(h) || g.x < 0 || g.x + g.w > int(w)) continue;
      std::memcpy(&font.rgba[(size_t(spot[i].second + row) * W + spot[i].first) * 4], &rgba[(size_t(sy) * w + g.x) * 4],
                  size_t(g.w) * 4);
    }
    g.x = spot[i].first;
    g.y = spot[i].second;
  }
  font.records.clear();
  float space = 0;
  for (const NewGlyph& g : glyphs) {
    if (g.code > 0xFFFF) continue;
    Record r;
    r.code = uint16_t(g.code);
    if (g.w > 0 && g.h > 0) {
      r.f[0] = float(g.x) / W;
      r.f[1] = float(g.y) / H;
      r.f[2] = float(g.w) / W;
      r.f[3] = float(g.h) / H;
    } else {
      r.f[0] = r.f[1] = -1;
    }
    r.f[4] = g.xoff / W;
    r.f[5] = g.yoff / H;
    r.f[6] = g.adv / W;
    if (g.code == ' ') space = g.adv;
    font.records.push_back(r);
  }
  // A code it has no glyph for: as wide as a space, nothing drawn.
  font.missing = Record();
  font.missing.f[0] = font.missing.f[1] = -1;
  font.missing.f[6] = space / W;
  Write(font, out);
  return true;
}

}  // namespace

bool ExportBmfont(const std::vector<uint8_t>& data, const std::string& name, std::string& fnt,
                  std::vector<uint8_t>& png, std::string* why) {
  std::vector<uint8_t> with_cyrillic;
  std::string ignored;
  const std::vector<uint8_t>& source = AddCyrillic(data, with_cyrillic, &ignored) ? with_cyrillic : data;
  Font font;
  if (!Read(source, font, why)) return false;
  const float W = float(font.width), H = float(font.height);
  char line[256];
  fnt.clear();
  std::snprintf(line, sizeof(line), "info face=\"%s\" size=%d bold=0 italic=0 charset=\"\" unicode=1 stretchH=100 smooth=1 aa=1 padding=0,0,0,0 spacing=1,1\n",
                name.c_str(), int(std::lround(CapHeight(font))));
  fnt += line;
  std::snprintf(line, sizeof(line), "common lineHeight=%d base=%d scaleW=%u scaleH=%u pages=1 packed=0\n",
                int(std::lround(CapHeight(font) * 1.25f)), int(std::lround(Baseline(font))), font.width, font.height);
  fnt += line;
  std::snprintf(line, sizeof(line), "page id=0 file=\"%s.png\"\n", name.c_str());
  fnt += line;
  std::snprintf(line, sizeof(line), "chars count=%zu\n", font.records.size());
  fnt += line;
  for (const Record& r : font.records) {
    const bool drawn = r.f[0] >= 0;
    std::snprintf(line, sizeof(line),
                  "char id=%u x=%d y=%d width=%d height=%d xoffset=%d yoffset=%d xadvance=%d page=0 chnl=15\n",
                  r.code, drawn ? int(std::lround(r.f[0] * W)) : 0, drawn ? int(std::lround(r.f[1] * H)) : 0,
                  drawn ? int(std::lround(r.f[2] * W)) : 0, drawn ? int(std::lround(r.f[3] * H)) : 0,
                  int(std::lround(r.f[4] * W)), int(std::lround(r.f[5] * H)), int(std::lround(r.f[6] * W)));
    fnt += line;
  }
  png.clear();
  stbi_write_png_to_func(PngWrite, &png, int(font.width), int(font.height), 4, font.rgba.data(),
                         int(font.width * 4));
  return !png.empty();
}

bool FromBmfont(const std::vector<uint8_t>& original, const std::string& fnt, const PageLoader& load,
                std::vector<uint8_t>& out, std::string* why) {
  Font font;
  if (!Read(original, font, why)) return false;
  std::map<int, std::string> page_files;
  std::vector<std::map<std::string, std::string>> chars;
  size_t start = 0;
  while (start < fnt.size()) {
    size_t end = fnt.find('\n', start);
    if (end == std::string::npos) end = fnt.size();
    std::string line = fnt.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    start = end + 1;
    if (line.rfind("page ", 0) == 0) {
      auto f = Fields(line);
      page_files[std::atoi(f["id"].c_str())] = f["file"];
    } else if (line.rfind("char ", 0) == 0) {
      chars.push_back(Fields(line));
    }
  }
  if (page_files.empty() || chars.empty()) {
    if (why) *why = "a .fnt with no pages or no chars (only the text format is read)";
    return false;
  }
  // The pages one under another.
  std::map<int, uint32_t> page_y;
  std::vector<std::vector<uint8_t>> pages;
  std::vector<std::pair<uint32_t, uint32_t>> sizes;
  uint32_t w = 0, h = 0;
  for (const auto& [id, file] : page_files) {
    std::vector<uint8_t> rgba;
    uint32_t pw = 0, ph = 0;
    if (!load(file, rgba, pw, ph)) {
      if (why) *why = "page " + file + " not read";
      return false;
    }
    page_y[id] = h;
    w = std::max(w, pw);
    h += ph;
    pages.push_back(std::move(rgba));
    sizes.push_back({pw, ph});
  }
  std::vector<uint8_t> atlas(size_t(w) * h * 4, 0);
  uint32_t y0 = 0;
  for (size_t p = 0; p < pages.size(); ++p) {
    for (uint32_t y = 0; y < sizes[p].second; ++y)
      std::memcpy(&atlas[(size_t(y0 + y) * w) * 4], &pages[p][size_t(y) * sizes[p].first * 4], size_t(sizes[p].first) * 4);
    y0 += sizes[p].second;
  }
  std::vector<NewGlyph> glyphs;
  for (auto& c : chars) {
    NewGlyph g;
    g.code = uint32_t(std::strtoul(c["id"].c_str(), nullptr, 10));
    g.x = std::atoi(c["x"].c_str());
    g.y = std::atoi(c["y"].c_str()) + int(page_y[std::atoi(c["page"].c_str())]);
    g.w = std::atoi(c["width"].c_str());
    g.h = std::atoi(c["height"].c_str());
    g.xoff = float(std::atoi(c["xoffset"].c_str()));
    g.yoff = float(std::atoi(c["yoffset"].c_str()));
    g.adv = float(std::atoi(c["xadvance"].c_str()));
    glyphs.push_back(g);
  }
  return Rebuild(font, atlas, w, h, glyphs, out, why);
}

bool FromTtf(const std::vector<uint8_t>& original, const std::vector<uint8_t>& ttf, const std::u16string& extra,
             std::vector<uint8_t>& out, std::string* why) {
  Font font;
  if (!Read(original, font, why)) return false;
  stbtt_fontinfo info;
  if (ttf.empty() || !stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
    if (why) *why = "not a TrueType or OpenType font";
    return false;
  }
  // The size: capitals as tall as the original's H.
  int x0, y0, x1, y1;
  if (!stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1) || y1 <= 0) {
    if (why) *why = "the font has no H";
    return false;
  }
  const float scale = CapHeight(font) / float(y1);
  const float baseline = Baseline(font);
  // What to draw: the original's codes, Latin-1, Cyrillic, and the extra ones.
  std::vector<uint32_t> codes;
  for (const Record& r : font.records) codes.push_back(r.code);
  for (uint32_t c = 0x20; c < 0x100; ++c) codes.push_back(c);
  for (uint32_t c = 0x400; c < 0x460; ++c) codes.push_back(c);
  for (char16_t c : extra) codes.push_back(c);
  std::sort(codes.begin(), codes.end());
  codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
  struct Drawn {
    NewGlyph g;
    std::vector<uint8_t> alpha;
  };
  std::vector<Drawn> drawn;
  for (uint32_t c : codes) {
    if (c < 0x20 || !stbtt_FindGlyphIndex(&info, int(c))) continue;
    Drawn d;
    int advance, lsb, bx0, by0, bx1, by1;
    stbtt_GetCodepointHMetrics(&info, int(c), &advance, &lsb);
    stbtt_GetCodepointBitmapBox(&info, int(c), scale, scale, &bx0, &by0, &bx1, &by1);
    d.g = {c, 0, 0, bx1 - bx0, by1 - by0, float(bx0), baseline + float(by0), advance * scale};
    if (d.g.w > 0 && d.g.h > 0) {
      d.alpha.resize(size_t(d.g.w) * d.g.h);
      stbtt_MakeCodepointBitmap(&info, d.alpha.data(), d.g.w, d.g.h, d.g.w, scale, scale, int(c));
    }
    drawn.push_back(std::move(d));
  }
  // Packed into an atlas as wide as the original's, as tall as they need.
  std::vector<size_t> order(drawn.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return drawn[a].g.h > drawn[b].g.h; });
  for (uint32_t h = 32;; h *= 2) {
    Packer packer(font.width, h);
    bool fits = true;
    for (size_t i : order) {
      NewGlyph& g = drawn[i].g;
      if (g.w <= 0 || g.h <= 0) continue;
      if (!packer.Place(g.w, g.h, g.x, g.y)) {
        fits = false;
        break;
      }
    }
    if (!fits) {
      if (h >= 8192) {
        if (why) *why = "the glyphs do not fit an atlas";
        return false;
      }
      continue;
    }
    std::vector<uint8_t> atlas(size_t(font.width) * h * 4, 0);
    std::vector<NewGlyph> glyphs;
    for (const Drawn& d : drawn) {
      for (int y = 0; y < d.g.h && !d.alpha.empty(); ++y)
        for (int x = 0; x < d.g.w; ++x) {
          uint8_t* p = &atlas[(size_t(d.g.y + y) * font.width + d.g.x + x) * 4];
          p[0] = p[1] = p[2] = 255;
          p[3] = d.alpha[size_t(y) * d.g.w + x];
        }
      glyphs.push_back(d.g);
    }
    return Rebuild(font, atlas, font.width, h, glyphs, out, why);
  }
}

}  // namespace xerenge::fonts
