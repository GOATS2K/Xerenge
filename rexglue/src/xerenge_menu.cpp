// Xerenge's settings in Driver Details > Settings: moving between its pages,
// and the natives its rows call (settings_movie.cpp builds the pages,
// xerenge_settings.cpp holds the settings).
//
// The menus run on a service ([0x82A522A0]) that the interface polls for the
// next movie to load (+20, sub_821E6BA8 hands it over and clears it), and
// that MenuScreenManager.SelectedOption(n) reaches as sub_821EC580(service, n)
// - the game's menu state machine. Our buttons hand it 0x58001-0x58003 (a
// page); back on the screen hands it -1. Both are taken here when they are
// ours: the Settings movie asked for again, built for the page.
//
// A row's values come from the natives XerengeGet(id) and XerengeSet(id * 16
// + option), registered beside the screen's own (DriverDetailsSettings's,
// sub_8242BDB0 from its adapter) as AptNativeFunction objects (sub_8242B338
// to allocate, sub_821AD188 to construct) around guest addresses the SDK
// routes to the functions below (FunctionDispatcher::AllocateThunk).
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"
#include "xerenge_settings.h"

extern "C" void __imp__sub_821EC580(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821E6BA8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8242BDB0(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8242B338(PPCContext& __restrict, uint8_t*);
extern "C" void sub_821AD188(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8242C678(PPCContext& __restrict, uint8_t*);
extern "C" void sub_821C84A8(PPCContext& __restrict, uint8_t*);
extern "C" void sub_821C7588(PPCContext& __restrict, uint8_t*);

namespace {

namespace settings = xerenge::settings;

constexpr uint32_t kMenuService = 0x82A522A0;   // holds the service's address
constexpr int32_t kOurOptions = 0x58000;         // + page (settings_movie.cpp)
constexpr uint32_t kAptAllocator = 0x82D39804;   // what sub_8242B338 allocates from
constexpr uint32_t kAptStack = 0x82D3A1B0;       // {count, ?, values}: the natives' arguments

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t raw = 0;
  std::memcpy(&raw, xerenge::GuestPointer(base, address), sizeof(raw));
  return __builtin_bswap32(raw);
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(xerenge::GuestPointer(base, address), &value, sizeof(value));
}

std::string GuestString(uint8_t* base, uint32_t address) {
  std::string text;
  if (address < 0x10000) {
    return text;
  }
  for (const char* c = reinterpret_cast<const char*>(xerenge::GuestPointer(base, address));
       *c && text.size() < 128; ++c) {
    text += *c;
  }
  return text;
}

uint32_t GuestCString(uint8_t* base, const char* text);

bool g_on_settings = false;  // the Settings screen is the one on show

// Whether a movie path names the Settings screen, whatever its letters' case.
bool IsSettings(const std::string& path) {
  for (const char* name : {"/driverdetailssettings", "/xerengesettingsa", "/xerengesettingsb"}) {
    const size_t length = std::strlen(name);
    if (path.size() >= length &&
        std::equal(name, name + length, path.end() - length,
                   [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); })) {
      return true;
    }
  }
  return false;
}

// The Settings screen asked for again, to show another page. Apt does not load
// again a movie it is showing - by its name, whatever its letters' case - so
// the screen is asked for under names of ours, two in turn, which the load
// below turns back into the Settings movie's.
void ShowSettings(uint8_t* base) {
  static uint32_t spellings[2] = {};
  static int next = 0;
  if (!spellings[0]) {
    spellings[0] = GuestCString(base, "../Burnout4/Menu/DriverDetails/XerengeSettingsA");
    spellings[1] = GuestCString(base, "../Burnout4/Menu/DriverDetails/XerengeSettingsB");
  }
  StoreU32(base, LoadU32(base, kMenuService) + 20, spellings[next]);
  next ^= 1;
}

// The natives' one argument, as an integer: the top of the interpreter's
// stack, through the interpreter's own conversion.
int32_t Argument(PPCContext& ctx, uint8_t* base) {
  const uint32_t count = LoadU32(base, kAptStack);
  const uint32_t values = LoadU32(base, kAptStack + 8);
  if (!count || !values) {
    return 0;
  }
  ctx.r3.u64 = LoadU32(base, values + (count - 1) * 4);
  sub_8242C678(ctx, base);
  return int32_t(ctx.r3.u32);
}

// XerengeGet(id): the option the row is at. The integer value it returns is
// made by the screen's GetSpeakerConfig (an integer of the right kind, from
// the interpreter's pool), its number put in.
void XerengeGet(PPCContext& ctx, uint8_t* base) {
  const int32_t id = Argument(ctx, base);
  const int option = settings::Get(id);
  sub_821C84A8(ctx, base);
  if (ctx.r3.u32) {
    StoreU32(base, ctx.r3.u32 + 8, uint32_t(option));
  }
}

// XerengeSet(id * 16 + option). Returns what GetTakedownCamera does - the
// rows do not read it.
void XerengeSet(PPCContext& ctx, uint8_t* base) {
  const int32_t code = Argument(ctx, base);
  settings::Set(code / 16, code % 16);
  sub_821C7588(ctx, base);
}

uint32_t GuestCString(uint8_t* base, const char* text) {
  const uint32_t size = uint32_t(std::strlen(text) + 1);
  const uint32_t at = REX_KERNEL_MEMORY()->SystemHeapAlloc(size, 0x10);
  std::memcpy(xerenge::GuestPointer(base, at), text, size);
  return at;
}

void Register(PPCContext& ctx, uint8_t* base, uint32_t object, const char* name, uint32_t function) {
  static auto* names = new std::map<std::string, uint32_t>;
  uint32_t& guest_name = (*names)[name];
  if (!guest_name) {
    guest_name = GuestCString(base, name);
  }
  ctx.r3.u64 = LoadU32(base, kAptAllocator);
  ctx.r4.u64 = 36;
  sub_8242B338(ctx, base);
  const uint32_t native = ctx.r3.u32;
  if (!native) {
    return;
  }
  ctx.r3.u64 = native;
  ctx.r4.u64 = function;
  sub_821AD188(ctx, base);
  ctx.r3.u64 = object;
  ctx.r4.u64 = guest_name;
  ctx.r5.u64 = native;
  __imp__sub_8242BDB0(ctx, base);
}

}  // namespace

// Called by menu_trace.cpp's hook on sub_8242BDB0 after the screen's adapter
// hands it IsInControl, its last: ours beside its own, on the same object.
void XerengeMenuRegisterNatives(PPCContext& ctx, uint8_t* base, uint32_t object) {
  static uint32_t get = 0, set = 0;
  if (!get) {
    auto* dispatcher = rex::Runtime::instance()->function_dispatcher();
    get = dispatcher->AllocateThunk(&XerengeGet, 0);
    set = dispatcher->AllocateThunk(&XerengeSet, 0);
    REXLOG_INFO("menu: XerengeGet at {:08X}, XerengeSet at {:08X}", get, set);
  }
  if (!get || !set) {
    return;
  }
  const PPCContext saved = ctx;
  Register(ctx, base, object, "XerengeGet", get);
  Register(ctx, base, object, "XerengeSet", set);
  ctx = saved;
}

REX_HOOK_RAW(sub_821EC580) {
  const int32_t option = int32_t(ctx.r4.u32);
  if (option > kOurOptions && option <= kOurOptions + 3) {
    settings::SetPage(settings::Page(option - kOurOptions));
    REXLOG_INFO("menu: Xerenge settings page {}", option - kOurOptions);
    ShowSettings(base);
    return;
  }
  if (option == -1 && g_on_settings) {
    if (settings::CurrentPage() != settings::kMain) {
      settings::SetPage(settings::kMain);
      ShowSettings(base);
      return;
    }
    if (settings::TakeRestartNeeded()) {
      REXLOG_INFO("menu: settings changed that take effect after a restart");
    }
  }
  __imp__sub_821EC580(ctx, base);
}

// The movie the interface loads next: the screen on show.
REX_HOOK_RAW(sub_821E6BA8) {
  const uint32_t requested = LoadU32(base, ctx.r3.u32 + 20);
  __imp__sub_821E6BA8(ctx, base);
  if (requested) {
    g_on_settings = IsSettings(GuestString(base, requested));
    REXLOG_INFO("menu: interface takes the movie {}", GuestString(base, requested));
  }
}

// CB4AptManager::AptCBLoadAnimation(name, file): Apt asking for a movie's data.
extern "C" void __imp__sub_821FF408(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821FF408) {
  const std::string name = GuestString(base, ctx.r3.u32);
  REXLOG_INFO("menu: Apt loads {}", name);
  // Our names for the Settings screen (ShowSettings): its data.
  if (name.find("/XerengeSettings") != std::string::npos) {
    ctx.r3.u64 = 0x82036178;  // "../Burnout4/Menu/DriverDetails/DriverDetailsSettings"
  }
  __imp__sub_821FF408(ctx, base);
}
