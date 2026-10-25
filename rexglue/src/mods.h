#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace xerenge::mods {

// A mod: a folder of mods/ beside the game's files (game_data_root), with its
// mod.cfg - "Key = value" lines, # comments:
//   Version = 0.1
//   Type = Localization
//   Name = Russian
//   Localization = locale_rus.txt     KEY = text lines (localeexport writes them)
//   FontBig = big.fnt                 a BMFont (.fnt and its .png pages) or a
//   FontBigDistressed = big_d.fnt     .ttf/.otf, in place of the game's font of
//   FontSmall = small.fnt             that name; a font not named stays the
//   FontDigital = digital.fnt         game's
struct Mod {
  std::string folder;  // its name in mods/
  std::filesystem::path root;
  std::map<std::string, std::string> config;
  std::string Get(const std::string& key) const {
    const auto found = config.find(key);
    return found == config.end() ? std::string() : found->second;
  }
};

// The localisation mods found, by folder name.
const std::vector<Mod>& Localizations();

// The one in use: locale_mod in xerenge.conf names it (empty: none); without
// that key, the first found.
const Mod* ActiveLocalization();

// The localisation mod's text for a string key (without its $), in guest
// memory as the game reads strings (big-endian UTF-16); 0 when it has none.
uint32_t Text(uint8_t* base, const char* key);

// At the start, before the game's files are mounted: the localisation mod's
// fonts made (or found made) in its .cache/.
void PrepareAtStart();

// The mod whose text is used from now on (none: the game's), chosen in the
// menus; and whether it can be, without a restart - its fonts, if it has any,
// must be the ones the game loaded at the start.
void SetLive(const Mod* mod);
bool CanShowLive(const Mod* mod);

}  // namespace xerenge::mods
