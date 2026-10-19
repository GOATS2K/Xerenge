// Xerenge's settings in the game's own Settings screen: the rows, as
// settings_concept.txt lays them out, and xerenge.conf - the file the
// installer writes and the launcher reads at every start - behind them.
#include "xerenge_settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DECLARE(std::string, lobby_server);

void XerengeSetMenuAntiAliasing(int32_t mode);
void XerengeSetMenuSharpening(float strength);

namespace xerenge::settings {
namespace {

// --- xerenge.conf ----------------------------------------------------------

std::filesystem::path ConfPath() {
  const std::string root = REXCVAR_GET(game_data_root);
  if (root.empty()) {
    return {};
  }
  return std::filesystem::path(root).parent_path() / "xerenge.conf";
}

std::string Trim(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t\r");
  if (begin == std::string::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

std::map<std::string, std::string> ReadConf() {
  std::map<std::string, std::string> values;
  std::ifstream in(ConfPath());
  std::string line;
  while (std::getline(in, line)) {
    const std::string t = Trim(line);
    const size_t equals = t.find('=');
    if (t.empty() || t[0] == '#' || equals == std::string::npos) {
      continue;
    }
    values[Trim(t.substr(0, equals))] = Trim(t.substr(equals + 1));
  }
  return values;
}

// Rewrites the key's line, or adds one; the rest of the file (comments, the
// keys the menu does not show) as it was.
void WriteConf(const std::string& key, const std::string& value) {
  const std::filesystem::path path = ConfPath();
  if (path.empty()) {
    return;
  }
  std::ifstream in(path);
  std::string text, line;
  bool written = false;
  while (std::getline(in, line)) {
    const std::string t = Trim(line);
    const size_t equals = t.find('=');
    if (!t.empty() && t[0] != '#' && equals != std::string::npos && Trim(t.substr(0, equals)) == key) {
      line = key + " = " + value;
      written = true;
    }
    text += line + "\n";
  }
  in.close();
  if (!written) {
    text += key + " = " + value + "\n";
  }
  std::ofstream(path, std::ios::trunc) << text;
  REXLOG_INFO("menu: {} = {} (in {})", key, value, path.string());
}

std::string ConfValue(const std::string& key, const std::string& fallback) {
  const auto values = ReadConf();
  const auto found = values.find(key);
  return found == values.end() ? fallback : found->second;
}

bool ConfFlag(const std::string& key, bool fallback) {
  const std::string value = ConfValue(key, fallback ? "true" : "false");
  return value == "true";
}

// --- the rows ----------------------------------------------------------------

struct Option {
  const char* label;
  const char* value;
};

struct Setting {
  int id;
  Page page;
  const char* label;
  // A setting of one key: its options' values in the file, and the default's index.
  const char* key = nullptr;
  std::vector<Option> options;
  int fallback = 0;
  bool restart = true;
  std::function<void(int)> live;  // applied without a restart
  // Or one of several keys: its own reading and writing.
  std::function<int()> get;
  std::function<void(int)> set;
};

std::vector<Option> OffOn() { return {{"OFF", "false"}, {"ON", "true"}}; }

const std::vector<Setting>& Settings() {
  static const std::vector<Setting> settings = [] {
    std::vector<Setting> s;
    s.push_back({1, kMain, "LANGUAGE", "language",
                 {{"ASK AT START", ""}, {"ENGLISH", "0"}, {"ENGLISH (US)", "1"}, {"ESPANOL", "5"},
                  {"NEDERLANDS", "10"}, {"SVENSKA", "11"}, {"SUOMI", "12"}},
                 1});

    s.push_back({10, kGraphics, "KEEP 16:9", "aspect_16_9", OffOn(), 1});
    s.push_back({11, kGraphics, "RENDER RESOLUTION", "render_resolution",
                 {{"THE WINDOW'S", ""}, {"1600X900", "1600x900"}, {"1280X720", "1280x720"},
                  {"960X540", "960x540"}, {"854X480", "854x480"}, {"640X360", "640x360"}},
                 0});
    s.push_back({12, kGraphics, "ANISOTROPIC FILTERING", "anisotropy", {{"OFF", "off"}, {"4X", "4"}, {"8X", "8"}}, 2});
    Setting aa{13, kGraphics, "ANTI-ALIASING", "aa", {{"OFF", "off"}, {"FXAA", "fxaa"}, {"SMAA", "smaa"}}, 0};
    aa.restart = false;
    aa.live = [](int option) { XerengeSetMenuAntiAliasing(option); };
    s.push_back(aa);
    Setting sharpen{14, kGraphics, "SHARPENING", "sharpen",
                    {{"OFF", "0"}, {"LIGHT", "0.3"}, {"MEDIUM", "0.6"}, {"STRONG", "1"}}, 0};
    sharpen.restart = false;
    sharpen.live = [](int option) {
      static const float kStrength[] = {0.0f, 0.3f, 0.6f, 1.0f};
      XerengeSetMenuSharpening(kStrength[option]);
    };
    s.push_back(sharpen);
    s.push_back({15, kGraphics, "BLOOM", "bloom", OffOn(), 0});
    s.push_back({16, kGraphics, "BLUR", "motion_blur", OffOn(), 0});

    // The frame rates: 30 is fps_30, 120 frame_generation (for the race and
    // Crash mode alike), Crash mode's 60 crash_interpolation.
    Setting game_fps{20, kHacks, "GAME FPS"};
    game_fps.options = {{"30", ""}, {"60", ""}, {"120", ""}};
    game_fps.get = [] {
      return ConfFlag("fps_30", false) ? 0 : ConfFlag("frame_generation", false) ? 2 : 1;
    };
    game_fps.set = [](int option) {
      WriteConf("fps_30", option == 0 ? "true" : "false");
      WriteConf("frame_generation", option == 2 ? "true" : "false");
    };
    s.push_back(game_fps);
    Setting crash_fps{21, kHacks, "CRASH FPS"};
    crash_fps.options = {{"30", ""}, {"60", ""}, {"120", ""}};
    crash_fps.get = [] {
      return !ConfFlag("crash_interpolation", true) ? 0 : ConfFlag("frame_generation", false) ? 2 : 1;
    };
    crash_fps.set = [](int option) {
      WriteConf("crash_interpolation", option == 0 ? "false" : "true");
      if (option == 2) {
        WriteConf("frame_generation", "true");
      }
    };
    s.push_back(crash_fps);
    s.push_back({22, kHacks, "COLOUR GRADING", "colour_grade", {{"AS ON THE CONSOLE", "1"}, {"HALF", "0.5"}, {"OFF", "0"}}, 0});
    s.push_back({23, kHacks, "ASYNCHRONOUS PRESENTATION", "async_present", OffOn(), 0});
    s.push_back({24, kHacks, "EARLY SUBMIT", "early_submit", OffOn(), 1});
    s.push_back({25, kHacks, "PACKED VERTICES", "packed_vertices", OffOn(), 1});
    s.push_back({26, kHacks, "REFLECTIONS IN TURNS", "reflection_split", OffOn(), 0});

    s.push_back({30, kDebug, "DEBUG LOGS", "debug", OffOn(), 0});
    s.push_back({31, kDebug, "POWER MODE", "power_mode", {{"STOCK", "stock"}, {"PERFORMANCE", "performance"}}, 0});
    s.push_back({32, kDebug, "FRAME COUNTER", "fps_counter", OffOn(), 0});
    return s;
  }();
  return settings;
}

const Setting* Find(int id) {
  for (const Setting& s : Settings()) {
    if (s.id == id) {
      return &s;
    }
  }
  return nullptr;
}

Page g_page = kMain;
bool g_restart = false;

}  // namespace

Page CurrentPage() { return g_page; }
void SetPage(Page page) { g_page = page; }

std::string PageTitle(Page page) {
  switch (page) {
    case kGraphics: return "GRAPHICS";
    case kHacks: return "HACKS";
    case kDebug: return "DEBUG";
    default: return {};
  }
}

std::vector<Row> Rows(Page page) {
  std::vector<Row> rows;
  for (const Setting& s : Settings()) {
    if (s.page != page) {
      continue;
    }
    Row row{s.label, {}, s.id};
    for (const Option& o : s.options) {
      row.options.push_back(o.label);
    }
    rows.push_back(std::move(row));
  }
  if (page == kDebug) {
    // Shown, not chosen here: the installer's Network tab sets them.
    const auto conf = ReadConf();
    const auto value = [&](const char* key) {
      const auto found = conf.find(key);
      return found == conf.end() ? std::string() : found->second;
    };
    std::string gamertag = value("gamertag");
    std::string server = value("lobby_server");
    if (server.empty()) {
      server = REXCVAR_GET(lobby_server);
    }
    rows.push_back({"GAMERTAG: " + (gamertag.empty() ? std::string("PLAYER") : gamertag), {}, 0});
    rows.push_back({"SERVER: " + (server.empty() ? std::string("NONE") : server), {}, 0});
  }
  return rows;
}

int Get(int id) {
  const Setting* s = Find(id);
  if (!s) {
    return 0;
  }
  if (s->get) {
    return s->get();
  }
  const std::string value = ConfValue(s->key, s->options[s->fallback].value);
  for (size_t i = 0; i < s->options.size(); ++i) {
    if (value == s->options[i].value) {
      return int(i);
    }
  }
  return s->fallback;
}

void Set(int id, int option) {
  const Setting* s = Find(id);
  if (!s || option < 0 || size_t(option) >= s->options.size()) {
    return;
  }
  if (Get(id) == option) {
    return;
  }
  if (s->set) {
    s->set(option);
  } else {
    WriteConf(s->key, s->options[option].value);
  }
  if (s->live) {
    s->live(option);
  }
  g_restart = g_restart || s->restart;
}

bool TakeRestartNeeded() {
  const bool restart = g_restart;
  g_restart = false;
  return restart;
}

}  // namespace xerenge::settings
