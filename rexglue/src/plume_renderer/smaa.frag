#version 450
#extension GL_GOOGLE_include_directive : require

// SMAA 1x: enhanced subpixel morphological anti-aliasing, by Jorge Jimenez,
// Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez. The
// method and the code are theirs: this file is only the three entry points
// around the reference implementation, thirdparty/smaa/SMAA.hlsl, which is
// included as it is and carries its (MIT) licence notice.
//
// Three passes, each this file compiled with one of SMAA_PASS_EDGES,
// SMAA_PASS_WEIGHTS and SMAA_PASS_BLEND defined:
//
//   edges    the frame                     -> where it has edges (red: to the
//                                             left of the pixel, green: above)
//   weights  the edges, the two tables     -> how far each edge pixel is to be
//                                             blended into its neighbours
//   blend    the frame, the weights        -> the anti-aliased frame
//
// The preset is the reference's HIGH: an edge is a contrast of 0.1, the walk
// along it goes 16 steps of two pixels either way (8 along a diagonal), and
// corners are kept a quarter rounded.
//
// Edges are found from luma rather than from each colour channel: the same
// measure FXAA uses, so the two settings differ in the method and not in what
// they call an edge, and it marks fewer pixels, which is what the weights pass
// costs by. What it gives up is the border between two colours of one
// brightness, which stays as it was. The frame is as the title wrote it,
// gamma-encoded in an 8-bit target with no sRGB view, which is what the
// reference asks of luma edge detection. The last pass mixes neighbours in
// that same encoding, as FXAA does, where the reference would have it done on
// linear values.
//
// Texture coordinates run from the top left, as Direct3D's do and as the
// reference's tables are laid out, so nothing is flipped. Every fetch is
// through the one bilinear sampler: the fetches the reference calls "point"
// are at texel centres, where the two are the same.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// Three sets of one layout, a texture and the sampler (plume_postfx.cpp):
// what the pass reads, in the order of the table above.
layout(set = 0, binding = 0) uniform texture2D g_first;
layout(set = 0, binding = 1) uniform sampler g_sampler;
#if !defined(SMAA_PASS_EDGES)
layout(set = 1, binding = 0) uniform texture2D g_second;
#endif
#if defined(SMAA_PASS_WEIGHTS)
layout(set = 2, binding = 0) uniform texture2D g_third;
#endif

layout(push_constant) uniform Params {
    vec2 texel;       // 1 / target size
    float sharpness;  // unused here; shared with cas.frag
} u;

// The reference's porting layer, for GLSL with textures and samplers apart: a
// texture is handed on as it is and meets the sampler where it is read.
#define SMAA_CUSTOM_SL
#define SMAA_PRESET_HIGH
#define SMAA_RT_METRICS vec4(u.texel, 1.0 / u.texel)
#define SMAATexture2D(tex) texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) textureLod(sampler2D(tex, g_sampler), coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(sampler2D(tex, g_sampler), coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) \
    textureLodOffset(sampler2D(tex, g_sampler), coord, 0.0, offset)
#define SMAASample(tex, coord) textureLod(sampler2D(tex, g_sampler), coord, 0.0)
#define SMAASamplePoint(tex, coord) textureLod(sampler2D(tex, g_sampler), coord, 0.0)
#define SMAASampleOffset(tex, coord, offset) \
    textureLodOffset(sampler2D(tex, g_sampler), coord, 0.0, offset)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) (a * b + c)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4

#include "SMAA.hlsl"

// The reference works its sample positions out in vertex shaders; the passes
// here share postfx.vert, so the same functions are called per pixel.
void main() {
#if defined(SMAA_PASS_EDGES)
    vec4 offset[3];
    SMAAEdgeDetectionVS(v_uv, offset);
    // A pixel with no edge is discarded: the target is cleared first.
    o_color = vec4(SMAALumaEdgeDetectionPS(v_uv, offset, g_first), 0.0, 0.0);
#elif defined(SMAA_PASS_WEIGHTS)
    vec2 pixcoord;
    vec4 offset[3];
    SMAABlendingWeightCalculationVS(v_uv, pixcoord, offset);
    // g_second is the area table, g_third the search table. The last argument
    // is for the modes with more than one sample a pixel; none here.
    o_color = SMAABlendingWeightCalculationPS(v_uv, pixcoord, offset, g_first, g_second, g_third,
                                              vec4(0.0));
#elif defined(SMAA_PASS_BLEND)
    vec4 offset;
    SMAANeighborhoodBlendingVS(v_uv, offset);
    o_color = SMAANeighborhoodBlendingPS(v_uv, offset, g_first, g_second);
#else
#error define one of SMAA_PASS_EDGES, SMAA_PASS_WEIGHTS, SMAA_PASS_BLEND
#endif
}
