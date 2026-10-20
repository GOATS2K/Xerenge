// The menus, for adding Xerenge's own settings to them. The interface is EA's
// Apt (Flash-like movies, ../Burnout4/Menu/...); each screen's adapter
// (CB4...AptAdapter::Initialize, e.g. sub_821E2760 for the driver details
// settings) makes its native functions - AptNativeFunction objects, built by
// sub_821AD188 with the function at +32 - and hands each to its extension
// object by name through sub_8242BDB0 (object, name, value). Logged once per
// name: the name and the function it calls, the retail address to hook.
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "guest_memory.h"

extern "C" void __imp__sub_8242BDB0(PPCContext& __restrict, uint8_t*);
void XerengeMenuRegisterNatives(PPCContext& ctx, uint8_t* base, uint32_t object);

namespace {

// AptNativeFunction's vtable, as its constructor leaves it.
constexpr uint32_t kNativeFunctionVtable = 0x8202E19C;

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t raw = 0;
  std::memcpy(&raw, xerenge::GuestPointer(base, address), sizeof(raw));
  return __builtin_bswap32(raw);
}

std::string GuestString(uint8_t* base, uint32_t address) {
  std::string text;
  if (address < 0x10000) {
    return text;
  }
  for (const char* c = reinterpret_cast<const char*>(xerenge::GuestPointer(base, address));
       *c && text.size() < 64; ++c) {
    text += *c;
  }
  return text;
}

}  // namespace

REX_HOOK_RAW(sub_8242BDB0) {
  const uint32_t name = ctx.r4.u32;
  const uint32_t value = ctx.r5.u32;
  const uint32_t caller = ctx.lr;
  const uint32_t object = ctx.r3.u32;
  bool settings_done = false;
  if (value >= 0x10000 && LoadU32(base, value) == kNativeFunctionVtable) {
    static std::set<std::string> seen;
    const std::string text = GuestString(base, name);
    if (seen.insert(text).second) {
      REXLOG_INFO("menu: native {} = {:08X} (from {:08X})", text, LoadU32(base, value + 32), caller);
    }
    // Ours (xerenge_menu.cpp) beside the volume natives, which the Settings
    // screen's adapter and the pause menu's both hand over.
    settings_done = text == "IncreaseMusicVolume";
  }
  __imp__sub_8242BDB0(ctx, base);
  if (settings_done) {
    XerengeMenuRegisterNatives(ctx, base, object);
  }
}
