#ifndef DOA2U_RECOMP_D3D_TEXTURE_REPLACE_H
#define DOA2U_RECOMP_D3D_TEXTURE_REPLACE_H

/* Texture packs: a texture the game uploads is named by a hash of its pixels, and a file of that name in the
   replacement folder is drawn in its place (any size: swizzled textures are sampled 0-1, so a larger image needs
   no change to the game's coordinates). The dump folder receives each texture the game uploads, once, under the
   same name, for making packs.

   The name: XXH64 (seed 0) of the texture's first mip level as the game stores it (DXT blocks; P8 swizzled
   indices; other formats their swizzled texels), and for P8 then XXH64 of the 1024-byte palette seeded with that,
   written as 16 lower-case hex digits: <name>.dds. Linear, volume and render-target textures are left alone.

   A replacement is a DDS: DXT1, DXT3 or DXT5 (FourCC), or 32-bit BGRA (uncompressed, alpha in the top byte), with
   its own mip levels (or one). */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "d3d_texture_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RecompD3dReplacementFormat {
    RECOMP_D3D_REPLACEMENT_BGRA8 = 0,
    RECOMP_D3D_REPLACEMENT_DXT1 = 1,
    RECOMP_D3D_REPLACEMENT_DXT3 = 2,
    RECOMP_D3D_REPLACEMENT_DXT5 = 3,
} RecompD3dReplacementFormat;

typedef struct RecompD3dReplacement {
    RecompD3dReplacementFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t levels;
    /* Every level, largest first, each as the format packs it (DXT: 4x4 blocks; BGRA8: rows of width * 4) */
    const uint8_t *data;
    size_t size;
} RecompD3dReplacement;

/* The folders (NULL or "" for none), any thread; the presenter looks at them from its next upload. The
   replacement folder's names are read once, here. RECOMP_TEXTURES and RECOMP_TEXTURE_DUMP name them too, and win. */
void recomp_d3d_textures_configure(const char *replace_folder, const char *dump_folder);

/* A number that changes with each configure: a presenter that sees it change drops the textures it has made, so
   the game's textures are made again with (or without) the folder's from the next frame. */
uint32_t recomp_d3d_textures_generation(void);

/* Whether there is a replacement folder with at least one texture, or a dump folder: when neither, a presenter
   need not hash textures at all. */
bool recomp_d3d_textures_active(void);

/* The name of a texture (above), or 0 when it is one the packs leave alone. `bytes` are the game's texture bytes
   (`count` of them, from its first level), `palette` its 1024-byte palette for P8. */
uint64_t recomp_d3d_texture_key(const RecompD3dTextureDesc *desc, const void *bytes, uint32_t count,
    const void *palette);

/* The replacement for a name, read from its file: true and *out filled (its data stays valid until the next call
   from the same thread), or false. */
bool recomp_d3d_texture_replacement(uint64_t key, RecompD3dReplacement *out);

/* Writes the texture to the dump folder under its name, if there is a dump folder and it is not there yet. */
void recomp_d3d_texture_dump(uint64_t key, const RecompD3dTextureDesc *desc, const void *bytes, uint32_t count,
    const void *palette);

/* (tests) The DDS reader on bytes in memory. */
bool recomp_d3d_texture_parse_dds(const uint8_t *file, size_t size, RecompD3dReplacement *out);

#ifdef __cplusplus
}
#endif

#endif
