// Mods beside the game's files (mods.h). A localisation mod's strings stand in
// for the game's own: the string table's lookup (sub_8210FCF8, its key
// without the $) asks Text() first. Its fonts stand in for the game's: the
// game loads each font's file at its start (sub_8210E1F0, "Language/Fonts/
// Big.bin" and so on) and is handed the file made of the mod's - built once,
// at the first load, into the mod's .cache/ and again only when a file it is
// made of is newer (game_font.cpp does the making).
#include "mods.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"

#include "game_font.h"
#include "guest_memory.h"
#include "locale_hash.h"
#include "xerenge_settings.h"

REXCVAR_DECLARE(std::string, game_data_root);

extern "C" void __imp__sub_8210E1F0(PPCContext& __restrict, uint8_t*);

namespace xerenge::mods {
namespace {

std::string Trim(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

// "Key = value" lines; # starts a comment.
std::map<std::string, std::string> ReadConfig(const std::filesystem::path& path) {
  std::map<std::string, std::string> values;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, equals));
    if (!key.empty()) values[key] = Trim(line.substr(equals + 1));
  }
  return values;
}

std::string Lower(std::string text) {
  for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::vector<Mod> Find() {
  std::vector<Mod> found;
  const std::string root = REXCVAR_GET(game_data_root);
  if (root.empty()) return found;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(root) / "mods", ec)) {
    const auto cfg = entry.path() / "mod.cfg";
    if (!entry.is_directory(ec) || !std::filesystem::exists(cfg, ec)) continue;
    Mod mod;
    mod.folder = entry.path().filename().string();
    mod.root = entry.path();
    mod.config = ReadConfig(cfg);
    if (Lower(mod.Get("Type")) == "localization") {
      REXLOG_INFO("mods: {} ({}, version {})", mod.folder, mod.Get("Name"), mod.Get("Version"));
      found.push_back(std::move(mod));
    }
  }
  std::sort(found.begin(), found.end(), [](const Mod& a, const Mod& b) { return a.folder < b.folder; });
  return found;
}

// --- strings ---------------------------------------------------------------------------

std::mutex g_mutex;
bool g_loaded = false;
const Mod* g_texts_mod = nullptr;  // whose texts g_texts holds
const Mod* g_live = nullptr;       // the mod whose text is used now
bool g_live_set = false;
const Mod* g_fonts_mod = nullptr;  // whose fonts the game loaded at the start
std::unordered_map<int32_t, std::u16string> g_texts;  // by the key's hash
std::unordered_map<int32_t, uint32_t> g_guest;        // made into guest memory, once each
std::u16string g_chars;                               // every character the texts use

std::u16string Utf16(const std::string& utf8) {
  std::u16string out;
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t point = extra ? c & (0x3F >> extra) : c;
    for (int k = 1; k <= extra && i + k < utf8.size(); ++k) point = point << 6 | (uint8_t(utf8[i + k]) & 0x3F);
    i += size_t(extra) + 1;
    if (point >= 0x10000) {
      point -= 0x10000;
      out += char16_t(0xD800 | (point >> 10));
      out += char16_t(0xDC00 | (point & 0x3FF));
    } else {
      out += char16_t(point);
    }
  }
  return out;
}

// KEY = text, \n a new line, \\ a backslash; 0x1234ABCD as a key is a hash.
void LoadTexts(const Mod& mod) {
  g_loaded = true;
  g_texts_mod = &mod;
  g_texts.clear();
  g_guest.clear();  // the old ones stay in guest memory: a switch of language costs a little
  const std::string file = mod.Get("Localization");
  if (file.empty()) return;
  std::ifstream in(mod.root / file, std::ios::binary);
  std::string line;
  std::set<char16_t> chars;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    std::string key = Trim(line.substr(0, equals));
    if (!key.empty() && key[0] == '$') key.erase(0, 1);
    std::string value = line.substr(equals + 1);
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    std::string text;
    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\\' && i + 1 < value.size()) {
        const char next = value[++i];
        text += next == 'n' ? '\n' : next;
      } else {
        text += value[i];
      }
    }
    const int32_t hash = key.size() == 10 && key.rfind("0x", 0) == 0
                             ? int32_t(std::strtoul(key.c_str() + 2, nullptr, 16))
                             : locale::KeyHash(key);
    g_texts[hash] = Utf16(text);
    for (char16_t c : g_texts[hash]) chars.insert(c);
  }
  g_chars.assign(chars.begin(), chars.end());
  REXLOG_INFO("mods: {} strings from {}", g_texts.size(), (mod.root / file).string());
}

// --- fonts -----------------------------------------------------------------------------

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A path under root as the game names it, its case found on the disk (the
// game's file system ignores it; Language/Fonts is language/fonts there).
std::filesystem::path OnDisk(const std::filesystem::path& root, const std::string& relative) {
  std::filesystem::path at = root;
  std::error_code ec;
  for (const auto& part : std::filesystem::path(relative)) {
    std::filesystem::path next = at / part;
    if (!std::filesystem::exists(next, ec)) {
      for (const auto& entry : std::filesystem::directory_iterator(at, ec)) {
        if (Lower(entry.path().filename().string()) == Lower(part.string())) {
          next = entry.path();
          break;
        }
      }
    }
    at = next;
  }
  return at;
}

std::filesystem::file_time_type Newest(const std::vector<std::filesystem::path>& files) {
  std::filesystem::file_time_type newest{};
  std::error_code ec;
  for (const auto& f : files) newest = std::max(newest, std::filesystem::last_write_time(f, ec));
  return newest;
}

// The mod's font for the game's file `game_file` (Language/Fonts/<name>.bin),
// made if need be: its path relative to the game's files, or empty.
std::string MadeFont(const Mod& mod, const std::string& game_file) {
  const std::string name = std::filesystem::path(game_file).stem().string();
  static const std::map<std::string, std::string> kKeys = {
      {"big", "FontBig"}, {"bigdistressed", "FontBigDistressed"}, {"small", "FontSmall"}, {"digital", "FontDigital"}};
  const auto key = kKeys.find(Lower(name));
  if (key == kKeys.end()) return {};
  const std::string source = mod.Get(key->second);
  if (source.empty()) return {};
  const std::filesystem::path root = REXCVAR_GET(game_data_root);
  const std::filesystem::path original = OnDisk(root, game_file);
  const std::filesystem::path font = mod.root / source;
  const std::filesystem::path made = mod.root / ".cache" / (name + ".bin");
  const std::string ext = Lower(font.extension().string());
  std::vector<std::filesystem::path> inputs = {original, font, mod.root / "mod.cfg"};
  if (!mod.Get("Localization").empty()) inputs.push_back(mod.root / mod.Get("Localization"));
  std::error_code ec;
  if (!std::filesystem::exists(font, ec)) {
    REXLOG_WARN("mods: {}: {} not found", mod.folder, font.string());
    return {};
  }
  const bool fresh = std::filesystem::exists(made, ec) && std::filesystem::last_write_time(made, ec) >= Newest(inputs);
  if (!fresh) {
    std::vector<uint8_t> out;
    std::string why;
    bool ok = false;
    const std::vector<uint8_t> base = ReadFile(original);
    if (ext == ".fnt") {
      const std::vector<uint8_t> text = ReadFile(font);
      const auto dir = font.parent_path();
      ok = fonts::FromBmfont(
          base, std::string(text.begin(), text.end()),
          [&](const std::string& page, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
            const std::vector<uint8_t> png = ReadFile(dir / page);
            int x = 0, y = 0, n = 0;
            stbi_uc* px = stbi_load_from_memory(png.data(), int(png.size()), &x, &y, &n, 4);
            if (!px) return false;
            rgba.assign(px, px + size_t(x) * y * 4);
            stbi_image_free(px);
            w = uint32_t(x);
            h = uint32_t(y);
            return true;
          },
          out, &why);
    } else if (ext == ".ttf" || ext == ".otf") {
      ok = fonts::FromTtf(base, ReadFile(font), g_chars, out, &why);
    } else {
      why = "not a .fnt, .ttf or .otf";
    }
    if (!ok) {
      REXLOG_WARN("mods: {}: {} not made of {}: {}", mod.folder, name, source, why);
      return {};
    }
    std::filesystem::create_directories(made.parent_path(), ec);
    std::ofstream(made, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    REXLOG_INFO("mods: {}: {} made of {} ({} bytes)", mod.folder, name, source, out.size());
  }
  return std::filesystem::relative(made, root, ec).generic_string();
}

// The game's font files (lower case) -> the mod's made of them, relative to
// the game's files.
std::map<std::string, std::string> g_made;

}  // namespace

void PrepareAtStart() {
  const Mod* mod = ActiveLocalization();
  if (!mod) return;
  {
    // The characters its text uses, for a TTF to draw.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_loaded) LoadTexts(*mod);
  }
  for (const char* game_file : {"Language/Fonts/Big.bin", "Language/Fonts/small.bin",
                                "Language/Fonts/BigDistressed.bin", "Language/Fonts/digital.bin"}) {
    const std::string path = MadeFont(*mod, game_file);
    if (!path.empty()) g_made[Lower(game_file)] = path;
  }
  if (!g_made.empty()) g_fonts_mod = mod;
}

const std::vector<Mod>& Localizations() {
  static const std::vector<Mod> mods = Find();
  return mods;
}

const Mod* ActiveLocalization() {
  const auto& mods = Localizations();
  if (mods.empty()) return nullptr;
  const std::string chosen = settings::ConfString("locale_mod", "\x01");
  if (chosen == "\x01") return &mods.front();
  for (const Mod& mod : mods) {
    if (mod.folder == chosen) return &mod;
  }
  return nullptr;
}

// The mod's text for a key, in guest memory (big-endian UTF-16); 0 for none.
uint32_t Text(uint8_t* base, const char* key) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const Mod* mod = g_live_set ? g_live : ActiveLocalization();
  if (!mod) return 0;
  if (!g_loaded || g_texts_mod != mod) LoadTexts(*mod);
  const int32_t hash = locale::KeyHash(key);
  const auto text = g_texts.find(hash);
  if (text == g_texts.end()) return 0;
  uint32_t& guest = g_guest[hash];
  if (!guest) {
    guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(text->second.size() + 1) * 2, 0x10);
    uint8_t* out = xerenge::GuestPointer(base, guest);
    for (size_t i = 0; i <= text->second.size(); ++i) {
      const char16_t c = i < text->second.size() ? text->second[i] : 0;
      out[i * 2] = uint8_t(c >> 8);
      out[i * 2 + 1] = uint8_t(c);
    }
  }
  return guest;
}

}  // namespace xerenge::mods

// A font being loaded (sub_8210E1F0(loader, path, done flag, buffer, size,
// ...), from sub_82115D10 at the start, one call a font): the localisation
// mod's, where it has one. Its file is bigger than the game's - an atlas grown
// to hold the new letters - so the font gets a buffer of its own (physical
// memory, as the game's is: the GPU reads the atlas there) in place of the one
// the game set aside, also where the game keeps that one (its font slot, at
// the done flag - 53 + the slot's offset).
REX_HOOK_RAW(sub_8210E1F0) {
  struct GameFont {
    uint32_t path;  // the game's file name, in its image
    uint32_t slot;  // where the font's buffer is kept
  };
  static constexpr GameFont kFonts[] = {
      {0x82011A80, 24}, {0x82011A64, 28}, {0x82011A40, 36}, {0x82011A24, 32}};
  static const xerenge::mods::Mod* mod = xerenge::mods::ActiveLocalization();
  for (const GameFont& font : kFonts) {
    if (!mod || ctx.r4.u32 != font.path) continue;
    // Made at the start (PrepareAtStart), before the game's files were
    // mounted: they are listed then, and a file made later is not found.
    const char* game_file = reinterpret_cast<const char*>(xerenge::GuestPointer(base, font.path));
    const auto mine = xerenge::mods::g_made.find(xerenge::mods::Lower(game_file));
    if (mine == xerenge::mods::g_made.end()) break;
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path(REXCVAR_GET(game_data_root)) / mine->second, ec);
    const std::string& path = mine->second;
    if (ec || !size) break;
    const uint32_t buffer_size = (uint32_t(size) + 0xFFF) & ~0xFFFu;
    const uint32_t buffer =
        REX_KERNEL_MEMORY()->SystemHeapAlloc(buffer_size, 0x1000, rex::memory::kSystemHeapPhysical);
    const uint32_t name = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(path.size() + 1), 0x10);
    if (!buffer || !name) break;
    std::memcpy(xerenge::GuestPointer(base, name), path.c_str(), path.size() + 1);
    const uint32_t slot_base = ctx.r5.u32 - 53;
    const uint32_t raw = __builtin_bswap32(buffer);
    std::memcpy(xerenge::GuestPointer(base, slot_base + font.slot), &raw, 4);
    REXLOG_INFO("mods: the game's {} is {} ({} bytes, at {:08X} in place of {:08X})", game_file, path, size,
                buffer, ctx.r6.u32);
    ctx.r4.u64 = name;
    ctx.r6.u64 = buffer;
    ctx.r7.u64 = buffer_size;
    break;
  }
  __imp__sub_8210E1F0(ctx, base);
}

namespace xerenge::mods {

void SetLive(const Mod* mod) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_live = mod;
  g_live_set = true;
  REXLOG_INFO("mods: text now {}", mod ? mod->folder : std::string("the game's"));
}

bool CanShowLive(const Mod* mod) {
  if (!mod) return true;
  const bool has_fonts = !mod->Get("FontBig").empty() || !mod->Get("FontSmall").empty() ||
                         !mod->Get("FontBigDistressed").empty() || !mod->Get("FontDigital").empty();
  return !has_fonts || g_fonts_mod == mod;
}

}  // namespace xerenge::mods
