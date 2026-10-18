/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d_card_model.h"
#include "d3d_draw_model.h"

#include <string.h>

/* Guest textures key by address; this one lies above all guest RAM aliases. */
enum { CARD_TEXTURE_KEY = 0xfff00000u };

static const uint16_t card_indices[4] = {0u, 1u, 2u, 3u};

void recomp_d3d_card_draw(const uint32_t *pixels, uint32_t width, uint32_t height,
    float target_width, float target_height, float display_aspect, uint8_t level,
    RecompD3dCardVertex vertices[4], RecompD3dPresenterDrawCommand *draw)
{
    const float image_aspect = (float)width / (float)height;
    float w = target_width, h = target_height;
    if (image_aspect >= display_aspect) h *= display_aspect / image_aspect;
    else w *= image_aspect / display_aspect;
    const float x = (target_width - w) * 0.5f, y = (target_height - h) * 0.5f;
    const uint32_t color = 0xff000000u | level * 0x010101u;
    const RecompD3dCardVertex quad[4] = {
        {x, y, 0.0f, 1.0f, color, 0.0f, 0.0f},
        {x + w, y, 0.0f, 1.0f, color, (float)width, 0.0f},
        {x, y + h, 0.0f, 1.0f, color, 0.0f, (float)height},
        {x + w, y + h, 0.0f, 1.0f, color, (float)width, (float)height},
    };
    memcpy(vertices, quad, sizeof quad);

    memset(draw, 0, sizeof *draw);
    draw->fvf = 0x144u;
    draw->primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw->index_count = draw->vertex_count = 4u;
    draw->triangle_count = 2u;
    draw->vertex_stride = sizeof quad[0];
    draw->vertex_bytes = vertices;
    draw->index_bytes = card_indices;
    draw->blend.color_write_mask = 15u;
    draw->has_texture = true;
    draw->texture.format_byte = 0x12u;
    draw->texture.bits_per_pixel = 32u;
    draw->texture.linear = true;
    draw->texture.width = width;
    draw->texture.height = height;
    draw->texture.pitch = width * 4u;
    draw->texture.data = CARD_TEXTURE_KEY;
    draw->texture.mip_levels = 1u;
    draw->texture_bytes = pixels;
    draw->texture_byte_count = width * height * 4u;
    draw->address_u = draw->address_v = 3u; /* clamp */
    /* Stage 0: texture RGB times diffuse, alpha one. */
    draw->directional.stage_mode = true;
    draw->directional.stage[0] = 1.0f;
    draw->directional.stage[1] = 1.0f;
}
