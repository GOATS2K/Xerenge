/**
 * @file        plume_renderer/plume_etc2_cache.h
 * @brief       The title's DXT textures as ETC2, kept from one transcode to the next.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace rex::plume_renderer {

// A GPU without BC textures (Mali) has every DXT texture re-encoded as ETC2 on
// the processor (TranscodeBcToEtc2) - up to 200 ms in one frame when a junction
// or a crash brings new ones. The same blocks always make the same ETC2, so it
// is kept: in memory for this run, and on disk under the cache root for the
// next ones. Looked up by the blocks' contents and the shape they are read in.
class Etc2Cache {
 public:
  static Etc2Cache& Instance();

  // Where the files go: <root>/etc2. Without one, memory only.
  void SetRoot(const std::filesystem::path& root);

  // The key for these blocks read so (format, size, rows, and the alpha asked
  // for: -1 to choose, or the base level's).
  static uint64_t Key(uint32_t format, const std::vector<uint8_t>& blocks, uint32_t row_bytes,
                      uint32_t width, uint32_t height, int alpha_in);

  bool Find(uint64_t key, int* alpha, std::vector<uint8_t>* out, uint32_t* row_texels);
  void Store(uint64_t key, int alpha, const std::vector<uint8_t>& data, uint32_t row_texels);

  // Counts so far, for the log.
  void NoteMiss(double milliseconds);

 private:
  Etc2Cache() = default;
};

}  // namespace rex::plume_renderer
