/**
 * @file        plume_renderer/plume_postfx.h
 * @brief       Anti-aliasing (FXAA, SMAA) and sharpening (CAS) of the finished frame.
 */
#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <plume_render_interface_types.h>

namespace rex::plume_renderer {

enum class AntiAliasing : uint32_t {
  kOff = 0,   // the console's look: no post-process
  kFxaa = 1,  // FXAA, over the frame at the size it is drawn at
  kSmaa = 2,  // SMAA 1x, likewise: three passes for FXAA's one, and less softening
};

// What a mode is called in the settings (XERENGE_AA, the `aa` key, the menu):
// "off", "fxaa", "smaa". Any other name, or none, is off.
const char* AntiAliasingName(AntiAliasing mode);
AntiAliasing AntiAliasingFromName(const char* name);

// The settings, live: read once a frame, so a change made on any thread (the
// in-game menu) shows from the next frame on, with nothing to restart. They
// start from the environment - XERENGE_AA=fxaa|smaa|off (default off) and
// XERENGE_SHARPEN=0..1 (default 0, off) - which the launcher's `aa` and
// `sharpen` keys and run.sh's --aa= and --sharpen= set.
AntiAliasing GetAntiAliasing();
void SetAntiAliasing(AntiAliasing mode);
// 0 (off) to 1 (the strongest sharpening); values outside are clamped.
float GetSharpening();
void SetSharpening(float strength);
// Whether the bloom is off: the shaders then take the bloom term from the pixel
// itself (plume_draw.cpp). XERENGE_NO_BLOOM at the start, then as the game's menus
// set it (read with the frame clock, in Settings::Current).
bool BloomOff();

// The passes themselves, run by the swapchain between the scene target and
// the copy onto the window's image.
class PlumePostFx {
 public:
  // What a frame is made with: the live settings, taken once.
  struct Settings {
    AntiAliasing aa = AntiAliasing::kOff;
    float sharpen = 0.0f;
    bool Any() const { return aa != AntiAliasing::kOff || sharpen > 0.0f; }
    static Settings Current();
  };

  PlumePostFx();
  ~PlumePostFx();
  PlumePostFx(const PlumePostFx&) = delete;
  PlumePostFx& operator=(const PlumePostFx&) = delete;

  void SetDevice(plume::RenderDevice* device) { device_ = device; }

  // Records the passes the settings ask for over `scene` (width x height) into
  // `list`, outside a render pass, and returns the texture holding the result
  // - `scene` itself, untouched, when there is nothing to do or the passes
  // could not be made. Its layout is the caller's to set for what comes next.
  // The targets and pipelines are made on first use.
  plume::RenderTexture* Record(plume::RenderCommandList* list, plume::RenderTexture* scene,
                               uint32_t width, uint32_t height, const Settings& settings);

  // Whether there are targets to give back (ReleaseTargets).
  bool HasTargets() const { return stage_a_.texture != nullptr || scene_.set != nullptr; }
  // Whether there are targets these settings have no use for: any at all with
  // everything off, SMAA's with another anti-aliasing chosen. ReleaseTargets
  // gives all of them back and Record makes again the ones still used.
  bool HasSpareTargets(const Settings& settings) const {
    return settings.Any() ? settings.aa != AntiAliasing::kSmaa && edges_.texture != nullptr
                          : HasTargets();
  }
  // Frees the targets, and the set that reads the scene target. Only with no
  // frame still on the GPU - and before the scene target is remade, which the
  // set points into.
  void ReleaseTargets();
  // Frees everything, the pipelines too.
  void Shutdown();

 private:
  // A texture the passes write, and the set that lets a pass read it.
  struct Stage {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    std::unique_ptr<plume::RenderDescriptorSet> set;
  };

  bool EnsurePipelines();
  // SMAA's own, made when it is first asked for.
  bool EnsureSmaaPipelines();
  std::unique_ptr<plume::RenderPipeline> MakePipeline(const plume::RenderPipelineLayout* layout,
                                                      const plume::RenderShader* pixel_shader,
                                                      plume::RenderFormat format);
  // A stage reading `scene`, or - as_target - a target of its own to write and read.
  bool MakeStage(Stage* stage, plume::RenderTexture* scene, uint32_t width, uint32_t height,
                 bool as_target, plume::RenderFormat format);
  // The view and the set through which a pass reads `readable`.
  bool MakeSet(Stage* stage, const plume::RenderTexture* readable, plume::RenderFormat format);
  // SMAA's two targets and its two tables, whichever are not there yet.
  bool MakeSmaaStages(plume::RenderCommandList* list, plume::RenderTexture* scene, uint32_t width,
                      uint32_t height);
  // One of the tables: a texture filled from the staging buffer at `offset`,
  // the copy recorded into `list`.
  bool MakeTable(plume::RenderCommandList* list, Stage* stage, uint32_t width, uint32_t height,
                 plume::RenderFormat format, uint64_t offset);
  // One pass over the whole of `target`. `more` is what an SMAA pass reads
  // besides `source`, through sets 1 and 2; `clear` empties the target first,
  // for a pass that does not write every pixel.
  void Pass(plume::RenderCommandList* list, plume::RenderTexture* source,
            plume::RenderDescriptorSet* source_set, Stage* target, plume::RenderPipeline* pipeline,
            uint32_t width, uint32_t height, float sharpness,
            std::initializer_list<const Stage*> more = {}, bool clear = false);

  plume::RenderDevice* device_ = nullptr;
  bool pipelines_failed_ = false;
  std::unique_ptr<plume::RenderShader> vertex_shader_;
  std::unique_ptr<plume::RenderShader> fxaa_shader_;
  std::unique_ptr<plume::RenderShader> cas_shader_;
  std::unique_ptr<plume::RenderSampler> sampler_;
  plume::RenderDescriptorSetBuilder set_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderPipeline> fxaa_pipeline_;
  std::unique_ptr<plume::RenderPipeline> cas_pipeline_;
  // SMAA: a pass reads up to three textures, so its layout is three of the
  // one set. The edge pass reads one and uses the layout above.
  bool smaa_failed_ = false;
  std::unique_ptr<plume::RenderShader> smaa_edges_shader_;
  std::unique_ptr<plume::RenderShader> smaa_weights_shader_;
  std::unique_ptr<plume::RenderShader> smaa_blend_shader_;
  std::unique_ptr<plume::RenderPipelineLayout> smaa_layout_;
  std::unique_ptr<plume::RenderPipeline> smaa_edges_pipeline_;
  std::unique_ptr<plume::RenderPipeline> smaa_weights_pipeline_;
  std::unique_ptr<plume::RenderPipeline> smaa_blend_pipeline_;

  // The scene target is only read: its set, here, and the view the set holds.
  Stage scene_;
  // The anti-aliased frame, or CAS's output when it runs alone; and CAS's
  // after anti-aliasing.
  Stage stage_a_;
  Stage stage_b_;
  // SMAA's first two passes: where the frame has edges (two channels), and
  // how far each edge pixel is to be blended into its neighbours. Its third
  // pass writes stage_a_.
  Stage edges_;
  Stage weights_;
  // The reference's two lookup tables, as textures, and the buffer they are
  // filled from, which has to last until the frame that copies it is done.
  Stage area_;
  Stage search_;
  std::unique_ptr<plume::RenderBuffer> tables_staging_;
  // Passes have been recorded since the targets were made (for the log).
  bool announced_ = false;
};

}  // namespace rex::plume_renderer
