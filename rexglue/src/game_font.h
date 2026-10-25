#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xerenge::fonts {

// A font of the game's (language/fonts/*.bin, "v1.4") with Cyrillic added:
// А-я, Ё and ё, each made of the font's own glyphs (А from A, Я from R
// mirrored, Б from E over B, ...) and put where its atlas is free. False,
// with `out` untouched, for a font it cannot read (or one already done).
bool AddCyrillic(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string* why = nullptr);

// The font's atlas as RGBA (for looking at), and a line of text drawn with
// it, both `width` x `height`.
bool AtlasImage(const std::vector<uint8_t>& font, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
bool DrawText(const std::vector<uint8_t>& font, const std::u16string& text, std::vector<uint8_t>& rgba,
              uint32_t& width, uint32_t& height);

}  // namespace xerenge::fonts

#include <functional>

namespace xerenge::fonts {

// The font as a BMFont (text .fnt and one .png page named `name`.png), with
// Cyrillic made of its own letters first when it has none: the start of a
// font of one's own that looks as the game's.
bool ExportBmfont(const std::vector<uint8_t>& font, const std::string& name, std::string& fnt,
                  std::vector<uint8_t>& png, std::string* why = nullptr);

// A page of a BMFont, by its file name: RGBA.
using PageLoader = std::function<bool(const std::string& file, std::vector<uint8_t>& rgba, uint32_t& width,
                                      uint32_t& height)>;

// The game's font `original` with its glyphs those of a BMFont, or of a TTF
// (sized so its capitals are as tall as the original's), for every code the
// original has, Cyrillic and `extra`.
bool FromBmfont(const std::vector<uint8_t>& original, const std::string& fnt, const PageLoader& load,
                std::vector<uint8_t>& out, std::string* why = nullptr);
bool FromTtf(const std::vector<uint8_t>& original, const std::vector<uint8_t>& ttf, const std::u16string& extra,
             std::vector<uint8_t>& out, std::string* why = nullptr);

}  // namespace xerenge::fonts
