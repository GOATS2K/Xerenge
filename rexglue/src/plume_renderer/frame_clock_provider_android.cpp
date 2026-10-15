// Android only: looks up the game's rex_frame_clock_provider in libmain_eu.so
// or libmain_us.so by name (see frame_clock_provider.h) - the plugin's plain
// extern reference was not resolved against it there.
#ifdef __ANDROID__
#include "frame_clock_provider.h"

#include <atomic>
#include <initializer_list>

#include <dlfcn.h>

RexFrameClockProviderFn RexFrameClockProvider() {
  // Kept once found; looked for again until then, in case the renderer asks
  // before the game library is in.
  static std::atomic<RexFrameClockProviderFn*> slot{nullptr};
  RexFrameClockProviderFn* found = slot.load(std::memory_order_acquire);
  if (!found) {
    // The game is libmain_eu.so or libmain_us.so, by the disc it was made from.
    for (const char* library : {"libmain_eu.so", "libmain_us.so"}) {
      if (void* host = dlopen(library, RTLD_NOW | RTLD_NOLOAD)) {
        found = static_cast<RexFrameClockProviderFn*>(dlsym(host, "rex_frame_clock_provider"));
        if (found) {
          break;
        }
      }
    }
    if (!found) {
      found = static_cast<RexFrameClockProviderFn*>(dlsym(RTLD_DEFAULT, "rex_frame_clock_provider"));
    }
    if (!found) {
      return nullptr;
    }
    slot.store(found, std::memory_order_release);
  }
  return *found;
}
#endif
