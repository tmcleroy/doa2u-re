#ifndef DOA2U_RECOMP_D3D_CARD_MODEL_H
#define DOA2U_RECOMP_D3D_CARD_MODEL_H

#include "d3d_presenter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FVF 0x144: XYZRHW, ARGB diffuse, one UV set in texels (linear texture). */
typedef struct RecompD3dCardVertex {
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} RecompD3dCardVertex;

/* Fills a full-target draw of a linear BGRA image, letterboxed so it keeps
   its aspect on a display of display_aspect (width / height). level scales
   RGB from black (0) to the image (255). The draw points at vertices and the
   pixels, which must outlive its submission. */
void recomp_d3d_card_draw(const uint32_t *pixels, uint32_t width, uint32_t height,
    float target_width, float target_height, float display_aspect, uint8_t level,
    RecompD3dCardVertex vertices[4], RecompD3dPresenterDrawCommand *draw);

#ifdef __cplusplus
}
#endif

#endif
