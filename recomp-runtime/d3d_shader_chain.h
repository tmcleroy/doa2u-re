#ifndef DOA2U_RECOMP_D3D_SHADER_CHAIN_H
#define DOA2U_RECOMP_D3D_SHADER_CHAIN_H

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>

/* RetroArch's slang shader presets (https://github.com/libretro/slang-shaders: a .slangp naming passes of GLSL
   with their scales, formats, filters and look-up textures) drawn on SDL_GPU, for the presenter's picture. SDL_GPU
   keeps its Vulkan and Metal objects to itself, so librashader cannot draw on it; this does what librashader's
   runtimes do with the presenter's own tools: shaderc compiles each pass's GLSL, its bindings are moved to
   SDL_GPU's sets in the SPIR-V (the push constant block becomes a second uniform buffer), SPIRV-Cross makes MSL
   for Metal, and each pass's uniforms are filled by their names (MVP, the sizes, FrameCount, the parameters).

   The chain takes the game's picture at the game's own size (`original_width` x `original_height`, the Xbox's
   640x480: scanlines are the game's lines whatever the internal resolution), and draws a picture of the
   viewport's size. */
struct RecompShaderChain;

/* nullptr with the reason. */
RecompShaderChain *recomp_shader_chain_load(SDL_GPUDevice *device, SDL_GPUShaderFormat format,
    const std::string &preset, std::string &error);
void recomp_shader_chain_free(RecompShaderChain *chain);

/* Records the passes into `commands`, from `source`'s region, and returns the drawn picture, `drawn_width` x
   `drawn_height` (the viewport's size unless the preset's last pass has a scale of its own; B8G8R8A8 unless it
   asks for another format), or nullptr. */
SDL_GPUTexture *recomp_shader_chain_draw(RecompShaderChain *chain, SDL_GPUCommandBuffer *commands,
    const SDL_GPUBlitRegion &source, uint32_t original_width, uint32_t original_height, uint32_t viewport_width,
    uint32_t viewport_height, uint32_t frame_count, uint32_t &drawn_width, uint32_t &drawn_height);

#endif
