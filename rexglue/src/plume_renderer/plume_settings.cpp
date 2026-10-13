/**
 * @file        plume_renderer/plume_settings.cpp
 * @brief       The settings that change while the game runs.
 */
#include "plume_renderer/plume_settings.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace rex::plume_renderer::settings {

namespace {

float EnvFloat(const char* name, float fallback, float low, float high) {
  const char* text = std::getenv(name);
  if (!text || !*text) {
    return fallback;
  }
  return std::clamp(std::strtof(text, nullptr), low, high);
}

struct State {
  std::atomic<float> colour_grade{EnvFloat("XERENGE_COLOUR_GRADE", 1.0f, 0.0f, 1.0f)};
  std::atomic<float> brightness{EnvFloat("XERENGE_BRIGHTNESS", 1.0f, 0.7f, 1.6f)};
  std::atomic<float> shadow_lift{EnvFloat("XERENGE_SHADOW_LIFT", 0.0f, 0.0f, 0.3f)};
  std::atomic<uint32_t> colour_table_version{1};
};

State& Get() {
  static State state;
  return state;
}

}  // namespace

float ColourGrade() {
  return Get().colour_grade.load(std::memory_order_relaxed);
}
float Brightness() {
  return Get().brightness.load(std::memory_order_relaxed);
}
float ShadowLift() {
  return Get().shadow_lift.load(std::memory_order_relaxed);
}

void SetColourGrade(float strength) {
  Get().colour_grade = std::clamp(strength, 0.0f, 1.0f);
  ++Get().colour_table_version;
}

void SetBrightness(float gamma) {
  Get().brightness = std::clamp(gamma, 0.7f, 1.6f);
  ++Get().colour_table_version;
}

void SetShadowLift(float lift) {
  Get().shadow_lift = std::clamp(lift, 0.0f, 0.3f);
  ++Get().colour_table_version;
}

uint32_t ColourTableVersion() {
  return Get().colour_table_version.load(std::memory_order_relaxed);
}

}  // namespace rex::plume_renderer::settings
