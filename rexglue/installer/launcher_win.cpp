// Burnout Revenge.exe - the Windows launcher the installer puts beside bin\ and
// the shortcuts point at, in place of a batch file (GitHub #26): Steam, given a
// shortcut to it, sees a game that lives as long as the game does, and starts
// burnout.exe with no console in between.
//
// It reads xerenge.conf beside itself at every start - the settings the
// installer writes, KEY = VALUE lines, # for comments - turns them into the
// game's environment and arguments exactly as the batch file did, starts
// bin\burnout.exe with them (its own arguments go after), waits for it and
// exits with its exit code.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

std::wstring Widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring wide(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
  return wide;
}

std::wstring Trim(const std::wstring& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::iswspace(text[begin])) {
    ++begin;
  }
  while (end > begin && std::iswspace(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::wstring Lower(std::wstring text) {
  for (auto& c : text) {
    c = wchar_t(std::towlower(c));
  }
  return text;
}

// xerenge.conf: KEY = VALUE, in the order written (the keybind_* lines are
// passed on in it). An empty value is no value.
struct Config {
  std::vector<std::pair<std::wstring, std::wstring>> lines;
  std::map<std::wstring, std::wstring> values;

  std::wstring Get(const std::wstring& key) const {
    const auto found = values.find(key);
    return found == values.end() ? std::wstring() : found->second;
  }
  bool Has(const std::wstring& key) const { return !Get(key).empty(); }
  bool Is(const std::wstring& key, const wchar_t* value) const { return Lower(Get(key)) == value; }
};

Config ReadConfig(const std::wstring& path) {
  Config config;
  std::ifstream file{std::filesystem::path(path)};
  std::string raw;
  while (std::getline(file, raw)) {
    const std::wstring line = Trim(Widen(raw));
    if (line.empty() || line[0] == L'#') {
      continue;
    }
    const size_t equals = line.find(L'=');
    if (equals == std::wstring::npos) {
      continue;
    }
    const std::wstring key = Trim(line.substr(0, equals));
    const std::wstring value = Trim(line.substr(equals + 1));
    if (key.empty()) {
      continue;
    }
    config.lines.emplace_back(key, value);
    config.values[key] = value;
  }
  return config;
}

// One argument as CreateProcess's command line wants it.
std::wstring Quote(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    quoted += c;
  }
  quoted.append(backslashes * 2, L'\\');
  return quoted + L"\"";
}

// What follows the program's own name on this launcher's command line.
std::wstring OwnArguments() {
  const wchar_t* line = GetCommandLineW();
  bool quoted = false;
  while (*line && (quoted || (*line != L' ' && *line != L'\t'))) {
    if (*line == L'"') {
      quoted = !quoted;
    }
    ++line;
  }
  while (*line == L' ' || *line == L'\t') {
    ++line;
  }
  return line;
}

void Env(const wchar_t* name, const std::wstring& value) { SetEnvironmentVariableW(name, value.c_str()); }

void Fail(const std::wstring& text) {
  MessageBoxW(nullptr, text.c_str(), L"Burnout Revenge", MB_OK | MB_ICONERROR);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t self[MAX_PATH * 4];
  const DWORD length = GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  std::wstring dir(self, length);
  dir.resize(dir.find_last_of(L"\\/") + 1);

  const Config conf = ReadConfig(dir + L"xerenge.conf");

  if (!conf.Is(L"bloom", L"true")) Env(L"XERENGE_NO_BLOOM", L"1");
  if (!conf.Is(L"motion_blur", L"true")) Env(L"XERENGE_NO_MOTION_BLUR", L"1");

  const std::wstring renderer = conf.Is(L"renderer", L"xenos") ? L"xenos" : L"plume";
  if (renderer == L"plume") {
    for (const wchar_t* name : {L"XERENGE_D3D_TARGETS", L"XERENGE_D3D_UI", L"XERENGE_SKIP_LOGOS",
                                L"XERENGE_REAL_SHADERS", L"XERENGE_D3D_DRAWS", L"XERENGE_SECONDARY_TICKS"}) {
      Env(name, L"1");
    }
  }

  if (conf.Has(L"language")) Env(L"XERENGE_LANGUAGE", conf.Get(L"language"));
  if (conf.Is(L"async_present", L"true")) Env(L"XERENGE_ASYNC_PRESENT", L"1");
  if (conf.Is(L"early_submit", L"false")) Env(L"XERENGE_ACQUIRE_FIRST", L"1");
  if (conf.Is(L"packed_vertices", L"false")) Env(L"XERENGE_FULL_VERTICES", L"1");
  if (conf.Is(L"culling", L"true")) Env(L"XERENGE_CULL", L"1");
  if (conf.Is(L"reflection_split", L"true")) Env(L"XERENGE_REFLECTION_SPLIT", L"1");
  if (conf.Has(L"render_resolution")) Env(L"XERENGE_RENDER_RESOLUTION", conf.Get(L"render_resolution"));
  if (conf.Is(L"anisotropy", L"off")) {
    Env(L"XERENGE_MAX_ANISOTROPY", L"1");
  } else if (conf.Has(L"anisotropy")) {
    Env(L"XERENGE_MAX_ANISOTROPY", conf.Get(L"anisotropy"));
  }
  if (conf.Has(L"colour_grade")) Env(L"XERENGE_COLOUR_GRADE", conf.Get(L"colour_grade"));
  if (conf.Has(L"aa")) Env(L"XERENGE_AA", conf.Get(L"aa"));
  if (conf.Has(L"sharpen")) Env(L"XERENGE_SHARPEN", conf.Get(L"sharpen"));
  if (conf.Has(L"brightness")) Env(L"XERENGE_BRIGHTNESS", conf.Get(L"brightness"));
  if (conf.Has(L"shadow_lift")) Env(L"XERENGE_SHADOW_LIFT", conf.Get(L"shadow_lift"));
  if (conf.Has(L"power_mode")) Env(L"XERENGE_POWER_MODE", conf.Get(L"power_mode"));
  if (conf.Has(L"spin_lead_ms")) Env(L"XERENGE_SPIN_LEAD_MS", conf.Get(L"spin_lead_ms"));
  if (conf.Is(L"fps_counter", L"true")) Env(L"XERENGE_FPS_SHOW", L"1");
  if (conf.Is(L"fps_30", L"true")) Env(L"XERENGE_FPS_30", L"1");
  if (conf.Is(L"frame_generation", L"true")) Env(L"XERENGE_FRAME_GENERATION", L"120");
  if (conf.Is(L"crash_interpolation", L"false")) Env(L"XERENGE_CRASH_INTERPOLATION", L"0");
  if (!conf.Is(L"aspect_16_9", L"false")) Env(L"XERENGE_ASPECT_16_9", L"1");

  std::vector<std::wstring> args = {dir + L"bin\\burnout.exe",
                                    L"--game_data_root", dir + L"game",
                                    L"--gpu_plugin", renderer,
                                    L"--gpu_backend", renderer,
                                    L"--no-vulkan_async_skip_incomplete_frames"};

  // Debug mode: the debug_* settings decide what is logged.
  std::wstring level = L"info";
  std::vector<std::wstring> extra;
  if (conf.Is(L"debug", L"true")) {
    level = conf.Has(L"debug_log_level") ? conf.Get(L"debug_log_level") : L"debug";
    Env(L"XERENGE_DEBUG", L"1");
    if (conf.Is(L"debug_gpu_trace", L"true")) Env(L"XERENGE_GPU_TRACE", L"1");
    if (conf.Is(L"debug_movie_trace", L"true")) Env(L"XERENGE_MOVIE_TRACE", L"1");
    if (conf.Is(L"debug_pipeline_log", L"true")) Env(L"XERENGE_PIPELINE_LOG", L"1");
    if (!conf.Is(L"debug_vertex_trace", L"false")) Env(L"XERENGE_VERTEX_TRACE", L"1");
    if (conf.Is(L"debug_constant_trace", L"true")) Env(L"XERENGE_CONSTANT_TRACE", L"1");
    if (conf.Has(L"debug_video_gpu")) Env(L"XERENGE_VIDEO_GPU", conf.Get(L"debug_video_gpu"));
    if (conf.Is(L"debug_noisy", L"true")) extra.push_back(L"--log_noisy=true");
  }

  std::wstring logs;
  if (conf.Has(L"log_path")) {
    logs = conf.Get(L"log_path");
  } else {
    wchar_t local[MAX_PATH * 4];
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, DWORD(std::size(local)));
    logs = std::wstring(local, size < std::size(local) ? size : 0) + L"\\xerenge-burnout";
  }
  CreateDirectoryW(logs.c_str(), nullptr);

  args.insert(args.end(), {L"--log_level", level, L"--log_file", logs + L"\\burnout.log",
                           L"--log_max_file_size_mb", L"32", L"--log_max_files", L"3"});

  // Fullscreen unless windowed; the keyboard as a pad with its keybind_* lines.
  args.push_back(conf.Is(L"windowed", L"true") ? L"--no-fullscreen" : L"--fullscreen");
  if (conf.Is(L"keyboard", L"true")) {
    args.push_back(L"--mnk_mode=true");
    for (const auto& [key, value] : conf.lines) {
      if (key.rfind(L"keybind_", 0) == 0 && !value.empty()) {
        args.push_back(L"--" + key + L"=" + value);
      }
    }
  }
  args.insert(args.end(), extra.begin(), extra.end());

  if (conf.Has(L"gamertag")) {
    args.push_back(L"--gamertag=" + conf.Get(L"gamertag"));
  }
  // Online: a server address means that server and nothing local; otherwise this copy
  // runs a lobby of its own when local multiplayer is on; otherwise no online mode.
  if (conf.Has(L"lobby_server") || conf.Is(L"online", L"true")) {
    args.insert(args.end(), {L"--online", L"--online_fake_lobby=false"});
    if (conf.Has(L"lobby_server")) {
      args.push_back(L"--lobby_server=" + conf.Get(L"lobby_server"));
    }
  }

  std::wstring line;
  for (const auto& arg : args) {
    line += (line.empty() ? L"" : L" ") + Quote(arg);
  }
  if (const std::wstring own = OwnArguments(); !own.empty()) {
    line += L" " + own;
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(args[0].c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(),
                      &startup, &process)) {
    Fail(L"Cannot start " + args[0] + L" (error " + std::to_wstring(GetLastError()) +
         L"). Run the installer again to repair the installation.");
    return 1;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return int(code);
}
