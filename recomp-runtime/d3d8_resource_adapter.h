/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_D3D8_RESOURCE_ADAPTER_H
#define DOA2U_RECOMP_D3D8_RESOURCE_ADAPTER_H

#include "runtime.h"
#include "d3d_texture_model.h"

RecompFunction recomp_d3d8_resource_lookup_manual(uint32_t address);
bool recomp_d3d8_resource_describe(uint32_t resource, RecompD3dTextureDesc *out);
void recomp_d3d8_resource_reset(void);
void recomp_d3d8_resource_set_render_targets(uint32_t color, uint32_t depth);
uint32_t recomp_d3d8_resource_add_ref(uint32_t resource);

#endif
