/**
 * @file        widescreen.h
 * @brief       Screens wider than 16:9: what the title is drawing (widescreen.cpp).
 */
#pragma once

#include <cstdint>

namespace xerenge {

// Whether the title's thread is inside its 2D layer (the interface, the menus)
// right now - the draws it makes there keep the console's 16:9 box when the
// scene is widened to the screen.
bool DrawingInterface();
// ... the kind kept against the screen's left edge (the music player's panel).
bool DrawingInterfaceLeft();
// ... the kind drawn across the whole screen: overlays over all of the
// console's 16:9 screen (the crash border, the front end's full-screen flash).
bool DrawingInterfaceFill();
// The 2D object being drawn (CGt2dLayer::RenderObjects draws one at a time),
// numbered; 0 outside one.
uint32_t InterfaceObject();

// Around the title's calls that draw interface (game_timing.cpp hooks the
// menus' and the revenge meter's renders).
void EnterInterface();
void LeaveInterface();
// ... and around one that draws a single HUD object outside the 2D layer (the
// revenge meter): numbered like the layer's own, so it goes to its edge too.
void EnterInterfaceObject();
void LeaveInterfaceObject();

// XERENGE_HUD_TRACE: the guest call chain of an interface draw, from the
// Direct3D call's return address (lr) and the caller's stack pointer (r1).
void TraceInterfaceDraw(const char* what, uint32_t lr, uint32_t sp, const uint8_t* base);

}  // namespace xerenge
