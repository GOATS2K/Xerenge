// Screens wider than 16:9 (21:9 and 18:9 phones): the scene widens to fill
// them, the interface keeps the console's 16:9 box. Narrower ones (a 16:10
// monitor): the scene grows taller instead, the interface keeps its 16:9 strip.
//
// The cameras are RenderWare's: CGtViewport::Update hands the field of view to
// sub_82353BA8, which makes the view window - x from the viewport's scale
// (+112) and tan(fov), y = x / the viewport's aspect (+116) - and sets it with
// RwCameraSetViewWindow (sub_8234ABC0). In a race that is viewport 82923220,
// aspect 16/9 and scale 1.2, the field of view moving with speed. Scaling both
// fields by the screen's width over 16:9 for the call widens x by that much
// and leaves y alone: the same vertical view, scale and shapes, more to either
// side. The culling frustum is taken from the view-projection matrix
// (CalculateViewFrustrumFromViewProjMatrix), so it widens with it and nothing
// at the edges goes missing.
//
// The width comes from the renderer (XERENGE_SCREEN_ASPECT, set when its
// scene target is made at the screen's shape); without it nothing changes.
//
// The interface is drawn by the title's 2D layer - CGt2dLayer::RenderObjects
// for the HUD and its 2D objects, CB4FlashManager::Render for the menus and
// CB4CustomRevengeMeter::Render (both hooked in game_timing.cpp) - and the
// Direct3D draws made inside those are marked (DrawingInterface) for the
// renderer to keep in the 16:9 box.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include "guest_memory.h"
#include "widescreen.h"

extern "C" void __imp__sub_82353BA8(PPCContext& __restrict, uint8_t*);

namespace {

constexpr float kConsoleAspect = 16.0f / 9.0f;

thread_local int g_interface_depth = 0;
thread_local int g_interface_left_depth = 0;
thread_local int g_interface_fill_depth = 0;
// CGt2dLayer::RenderObjects draws one object per call (it works out the
// object's space and tail-calls the object's own _Render): numbered here so
// the renderer can move each object to its edge as a whole.
thread_local int g_object_depth = 0;
thread_local uint32_t g_object_serial = 0;

float GuestFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = xerenge::LoadGuestU32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void StoreGuestFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  xerenge::StoreGuestU32(base, address, bits);
}

// The screen the scene is drawn for against 16:9: above 1 wider (21:9, 18:9),
// below it narrower (16:10, 4:3); 1 when it is 16:9, or the renderer has not said.
float WideningFactor() {
  const char* text = std::getenv("XERENGE_SCREEN_ASPECT");
  if (!text) {
    return 1.0f;
  }
  // Any shape a window can be dragged to: a fixed range (it was 1.2 to 4)
  // put the 16:9 scene back, stretched, on a window past either end. Only a
  // value no window has is refused.
  const float aspect = std::strtof(text, nullptr);
  const bool wider = aspect > kConsoleAspect * 1.01f && aspect < 64.0f;
  const bool narrower = aspect < kConsoleAspect * 0.99f && aspect > 1.0f / 64.0f;
  return wider || narrower ? aspect / kConsoleAspect : 1.0f;
}

}  // namespace

namespace xerenge {

bool DrawingInterface() {
  return g_interface_depth > 0 || g_interface_left_depth > 0 || g_interface_fill_depth > 0;
}

bool DrawingInterfaceLeft() {
  return g_interface_left_depth > 0;
}

bool DrawingInterfaceFill() {
  return g_interface_fill_depth > 0;
}

uint32_t InterfaceObject() {
  return g_object_depth > 0 ? g_object_serial : 0;
}

void EnterInterface() {
  ++g_interface_depth;
}

void LeaveInterface() {
  --g_interface_depth;
}

void EnterInterfaceObject() {
  static std::atomic<bool> told{false};
  if (std::getenv("XERENGE_HUD_LOG") && !told.exchange(true)) {
    REXLOG_INFO("widescreen: first HUD object drawn");
  }
  ++g_interface_depth;
  if (g_object_depth++ == 0) {
    g_object_serial = g_object_serial == ~0u ? 1u : g_object_serial + 1;
  }
}

void LeaveInterfaceObject() {
  --g_object_depth;
  --g_interface_depth;
}

// The recompiled code keeps the console's stack frames: a function's prologue
// stores its return address at -8 from the stack pointer it was entered with
// and links its frame to its caller's (stwu r1), so [frame] is the caller's
// frame and [caller's frame - 8] what the function returns to. 0x82353340 is
// where CGt2dLayer::RenderObjects's loop returns to from an element's render.
void TraceInterfaceDraw(const char* what, uint32_t lr, uint32_t sp, const uint8_t* base) {
  static const bool on = std::getenv("XERENGE_HUD_TRACE") != nullptr;
  if (!on || !DrawingInterface()) {
    return;
  }
  // The next 150 interface draws, every 5 seconds.
  static std::atomic<uint64_t> window_ms{0};
  static std::atomic<uint32_t> left{0};
  const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
  uint64_t was = window_ms.load(std::memory_order_relaxed);
  if (now - was >= 5000 && window_ms.compare_exchange_strong(was, now)) {
    left.store(150, std::memory_order_relaxed);
    REXLOG_INFO("hud trace: ---- window ----");
  }
  if (left.load(std::memory_order_relaxed) == 0 ||
      left.fetch_sub(1, std::memory_order_relaxed) == 0) {
    return;
  }
  const auto is_code = [](uint32_t address) {
    return address >= 0x82000000u && address < 0x83000000u;
  };
  std::string chain = fmt::format("{:08X}", lr);
  bool in_element = false;
  // RenderObjects's own frame, once the walk reaches it: the space it works
  // out for the element is in its locals at +96 and copied to +128.
  std::string space;
  uint32_t frame = sp;
  for (int depth = 0; depth < 16; ++depth) {
    const uint32_t caller = LoadGuestU32(base, frame);
    // A frame chain runs up the stack, a frame at a time.
    if (caller <= frame || caller - frame > 0x10000u) {
      break;
    }
    const uint32_t ret = LoadGuestU32(base, caller - 8);
    if (!is_code(ret)) {
      break;
    }
    chain += fmt::format(" < {:08X}", ret);
    if (ret == 0x82353340u && !in_element) {
      in_element = true;
      // The element's render saves RenderObjects's r31 - the list node of the
      // element being drawn - at -16 from the stack pointer it was entered
      // with, which is RenderObjects's own.
      space = fmt::format(" node {:08X}", LoadGuestU32(base, caller - 12));
    }
    frame = caller;
  }
  REXLOG_INFO("hud trace: {} object {} {}:{} : {}", what, InterfaceObject(),
              in_element ? "in element" : "outside", space, chain);
}

}  // namespace xerenge

// The view window from the field of view: r3 the viewport, f1 the angle.
REX_HOOK_RAW(sub_82353BA8) {
  const float factor = WideningFactor();
  const uint32_t viewport = ctx.r3.u32;
  const float scale = GuestFloat(base, viewport + 112);
  const float aspect = GuestFloat(base, viewport + 116);
  // The screen's cameras run from 4:3 (the menus') to 16:9 (the race's);
  // anything else - a reflection's, a shadow's - is left as it is.
  const bool widen = factor != 1.0f && aspect > 1.3f && aspect < 1.8f;
  if (widen) {
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
      REXLOG_INFO("widescreen: cameras {} by {:.3f} for a {:.3f} screen",
                  factor > 1.0f ? "widened" : "made taller", factor, factor * kConsoleAspect);
    }
    // Wider: x grows with the scale, y (x over the aspect) stays. Narrower
    // (16:10): x stays and y grows - the same width of view, more above and
    // below, rather than the picture squeezed upright.
    if (factor > 1.0f) {
      StoreGuestFloat(base, viewport + 112, scale * factor);
    }
    StoreGuestFloat(base, viewport + 116, aspect * factor);
  }
  __imp__sub_82353BA8(ctx, base);
  if (widen) {
    StoreGuestFloat(base, viewport + 112, scale);
    StoreGuestFloat(base, viewport + 116, aspect);
  }
}

#define INTERFACE_HOOK(name)                                       \
  extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*); \
  REX_HOOK_RAW(name) {                                             \
    ++g_interface_depth;                                           \
    __imp__##name(ctx, base);                                      \
    --g_interface_depth;                                           \
  }

// CGt2dLayer::RenderObjects - a layer of the HUD: it walks the layer's list
// of elements and calls each one's render through the pointer the element
// holds, returning to 0x82353340. The layer is numbered here; each element
// gets its own number below (ELEMENT_HOOK).
extern "C" void __imp__sub_82353218(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82353218) {
  xerenge::EnterInterfaceObject();
  __imp__sub_82353218(ctx, base);
  xerenge::LeaveInterfaceObject();
}

// Gt2dRenderer batches the vertices the elements give it in one buffer
// (Gt2dRenderer::CVertexBuffer, the global at 0x82D18190) and hands them to
// Direct3D (CVertexBuffer::Dispatch, sub_82355FD0) only when the texture,
// blend mode or colour next changes - which is usually in the next element's
// render. Each draw then came out under the element after the one it belongs
// to: an element's shadow pass went to one edge and its text to another. So
// the batch is flushed around each element, while that element's number is
// the current one. Dispatch returns at once when the batch is empty.
extern "C" void sub_82355FD0(PPCContext& __restrict, uint8_t*);

namespace {

constexpr uint32_t kVertexBatch = 0x82D18190;
constexpr uint32_t kRenderObjectsLoop = 0x82353340;

void FlushVertexBatch(const PPCContext& ctx, uint8_t* base) {
  PPCContext flush = ctx;
  flush.r3.u64 = kVertexBatch;
  sub_82355FD0(flush, base);
}

}  // namespace

// An element's render, numbered as an element only when RenderObjects's loop
// calls it: the same functions are also called from inside other elements
// (the boost bar draws its label with CB4HUDText2dObject::_RenderSolidFill),
// and there they are part of the element calling them.
#define ELEMENT_HOOK_DEPTH(name, depth)                                  \
  extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*);       \
  REX_HOOK_RAW(name) {                                                   \
    if (uint32_t(ctx.lr) != kRenderObjectsLoop) {                        \
      __imp__##name(ctx, base);                                          \
      return;                                                            \
    }                                                                    \
    FlushVertexBatch(ctx, base);                                         \
    g_object_serial = g_object_serial == ~0u ? 1u : g_object_serial + 1; \
    ++depth;                                                             \
    __imp__##name(ctx, base);                                            \
    FlushVertexBatch(ctx, base);                                         \
    --depth;                                                             \
  }
#define ELEMENT_HOOK(name) ELEMENT_HOOK_DEPTH(name, g_interface_depth)
// ... an element drawn across the whole screen.
#define ELEMENT_FILL_HOOK(name) ELEMENT_HOOK_DEPTH(name, g_interface_fill_depth)

// The element renders a race's HUD was seen to use (names from Beta 5's
// symbols, carried over by opcode match).
ELEMENT_HOOK(sub_8215B940)  // CB4EATrax2dObject::_Render
ELEMENT_HOOK(sub_82186F30)  // C2dObjectBoostBarMessage::_Render
ELEMENT_HOOK(sub_82188FF0)  // CB4HUDMessagePanel2dObject::_Render
ELEMENT_HOOK(sub_82189238)  // CB4HUDMessageStars2dObject::_Render
ELEMENT_HOOK(sub_8218AA90)  // CB4HUDRatingBar::_Render
ELEMENT_HOOK(sub_8218AFC8)  // CB4HUDRotateImage2dObject::_Render
ELEMENT_HOOK(sub_8218B458)  // CB4HUDSmallPanel2dObject::_Render
ELEMENT_HOOK(sub_8218C168)  // CB4HUDSmallPanel2dObject::_RenderCenter
ELEMENT_HOOK(sub_8218C3C8)  // CB4HUDLargePanel2dObject::_Render
ELEMENT_HOOK(sub_8218E0C8)  // CB4HUDText2dObject::_RenderSolidFill
ELEMENT_HOOK(sub_8218E128)  // CB4HUDText2dObject::_RenderVGradientEmboss
ELEMENT_HOOK(sub_8218E248)  // CB4HUDText2dObject::_RenderMessage1In
ELEMENT_HOOK(sub_8218E730)  // CB4HUDText2dObject::_RenderMessage1Out
ELEMENT_HOOK(sub_8218F0A8)  // CB4HUDText2dObject::_RenderVGradientEmbossZoomFlash
ELEMENT_HOOK(sub_8218F978)  // CB4HUDText2dObject::_RenderRatingText
ELEMENT_HOOK(sub_82197A88)  // C2dObjectBoostBar::_Render
ELEMENT_HOOK(sub_8235DB20)  // CGt2dObjectImageRotatable::_Render
// The border a crash or a takedown is framed in: black bars over the top and
// bottom of the whole screen, not of its middle.
ELEMENT_FILL_HOOK(sub_82187BE0)  // CB4HUDBorder2dObject::_Render

// CB4GraphicsManagerBase::RenderFullScreenPass - a pass over the whole screen,
// such as the flash the front end draws when a car is won (from
// CB4FlashManager::Render, whose draws are otherwise kept in the box).
extern "C" void __imp__sub_82142CD8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82142CD8) {
  ++g_interface_fill_depth;
  __imp__sub_82142CD8(ctx, base);
  --g_interface_fill_depth;
}

// CB4EATraxDisplay::Render - the music player's panel, in a 16:9 box against
// the screen's left edge: in the corner of a wide screen, where it sat when it
// was first left out of the box, rather than in the corner of the middle.
extern "C" void __imp__sub_8215C0E8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8215C0E8) {
  ++g_interface_left_depth;
  __imp__sub_8215C0E8(ctx, base);
  --g_interface_left_depth;
}
