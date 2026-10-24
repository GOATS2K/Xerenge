#include "plume_renderer/plume_prompts.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <rex/logging.h>

#define STBI_ONLY_PNG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

struct PromptAtlas {
  const char* set;
  uint64_t hash;
  const unsigned char* png;
  size_t size;
};
extern const PromptAtlas kPromptAtlases[];

namespace rex::plume_renderer {
namespace {

struct Picture {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

// The chosen set's pictures by the hash of the texture each replaces, decoded once.
const std::map<uint64_t, Picture>& ChosenSet() {
  static const std::map<uint64_t, Picture> pictures = [] {
    std::map<uint64_t, Picture> chosen;
    const char* wanted = std::getenv("XERENGE_PROMPTS");
    if (!wanted || !*wanted || std::strcmp(wanted, "stock") == 0) {
      return chosen;
    }
    for (const PromptAtlas* entry = kPromptAtlases; entry->set; ++entry) {
      if (std::strcmp(entry->set, wanted) != 0) {
        continue;
      }
      int w = 0, h = 0, channels = 0;
      if (stbi_uc* pixels = stbi_load_from_memory(entry->png, int(entry->size), &w, &h, &channels, 4)) {
        Picture& picture = chosen[entry->hash];
        picture.width = uint32_t(w);
        picture.height = uint32_t(h);
        picture.rgba.assign(pixels, pixels + size_t(w) * h * 4);
        stbi_image_free(pixels);
      }
    }
    if (chosen.empty()) {
      REXLOG_WARN("plume: no button prompts named {}; the game's are kept", wanted);
    } else {
      REXLOG_INFO("plume: button prompts: {} ({} pictures)", wanted, chosen.size());
    }
    return chosen;
  }();
  return pictures;
}

}  // namespace

bool PromptAtlasFor(uint64_t content_hash, uint32_t width, uint32_t height, std::vector<uint8_t>* rgba) {
  const auto& pictures = ChosenSet();
  const auto found = pictures.find(content_hash);
  if (found == pictures.end() || found->second.width != width || found->second.height != height) {
    return false;
  }
  *rgba = found->second.rgba;
  return true;
}

}  // namespace rex::plume_renderer
