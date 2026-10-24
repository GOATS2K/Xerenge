/**
 * @file        plume_renderer/plume_prompts.h
 * @brief       The game's button icons replaced by another controller's (XERENGE_PROMPTS).
 */
#pragma once

#include <cstdint>
#include <vector>

namespace rex::plume_renderer {

// The game's button icons are a few textures: the menus' 128x256 atlas
// (Buttons.tif of textures/B4ButtonTextures: A, B, X, Y, LB, RB, LT, RT, Start,
// the stick, the d-pad) and the race's own (A, B, the left stick, the pad).
// With XERENGE_PROMPTS=<set> (playstation, xbox) and a texture of a content hash
// the set has a picture for (assets/input_prompts/replace/<set>/<hash>.png), that
// picture as width * height RGBA rows; false otherwise - stock, or any other
// texture, or a picture of another size.
bool PromptAtlasFor(uint64_t content_hash, uint32_t width, uint32_t height, std::vector<uint8_t>* rgba);

}  // namespace rex::plume_renderer
