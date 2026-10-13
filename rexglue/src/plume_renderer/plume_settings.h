/**
 * @file        plume_renderer/plume_settings.h
 * @brief       The settings that change while the game runs.
 */
#pragma once

#include <cstdint>

namespace rex::plume_renderer::settings {

// Each of these starts from an environment variable, so a run where nothing
// changes them behaves as the variable says. Whatever shows the settings sets
// them from its own thread; the renderer reads them every frame.
//
// A setting that must be noticed by something with its own cache (the colour
// tables) has a version that goes up when it changes.

// XERENGE_COLOUR_GRADE: how much of the title's colour grading is kept, 0 to 1.
float ColourGrade();
void SetColourGrade(float strength);
// XERENGE_BRIGHTNESS: a gamma on the 3D picture, 0.7 to 1.6; 1 leaves it as drawn.
float Brightness();
void SetBrightness(float gamma);
// XERENGE_SHADOW_LIFT: how far the darkest colours are raised, 0 to 0.3.
float ShadowLift();
void SetShadowLift(float lift);
// Goes up whenever one of the three above changes: the colour tables are
// blended again.
uint32_t ColourTableVersion();

}  // namespace rex::plume_renderer::settings
