// SPDX-License-Identifier: GPL-3.0-or-later
// Presentation-only interpolation. Never writes into either source snapshot.
#pragma once
#include "d3d_presenter.h"
#include "d3d_draw_model.h"
#include "d3d_scene_snapshot.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <unordered_map>

namespace sc2_visual {
// Verify the skinning output against the actual draw, including gathered
// streams. A matching address/range does not establish vertex ownership.
inline bool matchesSkinOutput(const RecompD3dPresenterDrawCommand &draw,
                              const std::vector<float> &output, uint32_t first) {
    RecompD3dVertexLayout layout{};
    if (!draw.vertex_bytes || !draw.vertex_count ||
        uint64_t(first)+draw.vertex_count > output.size()/8 ||
        !recomp_d3d_fvf_layout(draw.fvf, &layout) || layout.pretransformed ||
        layout.stride > draw.vertex_stride || layout.position_offset == RECOMP_D3D_FVF_ABSENT)
        return false;
    for (uint32_t v = 0; v < draw.vertex_count; ++v) {
        const auto *bytes = static_cast<const unsigned char *>(draw.vertex_bytes) + size_t(v)*draw.vertex_stride;
        const float *expected = output.data() + size_t(first+v)*8;
        if (std::memcmp(bytes+layout.position_offset, expected, 12) ||
            (layout.normal_offset != RECOMP_D3D_FVF_ABSENT && std::memcmp(bytes+layout.normal_offset, expected+4, 12)))
            return false;
    }
    return true;
}

// SC2 sorts translucent triangle lists each frame. Order selects compositing,
// not vertex identity: retain the current order while matching the same set of
// directed triangles. Winding and duplicate triangle counts must still agree.
inline bool sortedTriangleList(const RecompD3dPresenterDrawCommand &d) {
    return d.primitive_type==5u && d.index_bytes && d.index_count && d.index_count%3u==0;
}
inline uint64_t triangleAt(const void *indices, size_t triangle) {
    uint16_t v[3];
    std::memcpy(v,static_cast<const unsigned char *>(indices)+triangle*6,sizeof v);
    return uint64_t(v[0]) | (uint64_t(v[1])<<16) | (uint64_t(v[2])<<32);
}
inline bool sameTopology(const RecompD3dPresenterDrawCommand &a,
                         const RecompD3dPresenterDrawCommand &b) {
    if (a.primitive_type!=b.primitive_type || a.index_count!=b.index_count) return false;
    if (!a.index_count) return true;
    if (!a.index_bytes || !b.index_bytes) return false;
    if (!std::memcmp(a.index_bytes,b.index_bytes,size_t(a.index_count)*2)) return true;
    if (!sortedTriangleList(a) || !sortedTriangleList(b)) return false;
    std::vector<uint64_t> left(a.index_count/3),right(a.index_count/3);
    for (size_t i=0;i<left.size();++i) {
        left[i]=triangleAt(a.index_bytes,i);right[i]=triangleAt(b.index_bytes,i);
    }
    std::sort(left.begin(),left.end());std::sort(right.begin(),right.end());
    return left==right;
}

// A geometry/material signature, independent of draw order and animated positions.
// Ambiguous duplicate signatures are left uninterpolated by the caller.
inline uint64_t key(const RecompD3dPresenterDrawCommand &d) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void *data, size_t bytes) {
        const auto *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
    };
    for (auto n : {d.fvf, d.vertex_count, d.vertex_stride, d.index_count,
                  d.primitive_type, d.texture.data, d.target.color.data}) mix(&n, sizeof n);
    // A validated native pose is optional acceleration data, not mesh identity.
    // Post-skinning edits can remove it for one frame; match the actual captured
    // geometry across that handoff using the same topology/UV safety checks.
    if (d.pixel_program.kind != RECOMP_D3D_PIXEL_NONE)
        for (const auto &t : d.stage_textures) mix(&t.desc.data, sizeof t.desc.data);
    if (sortedTriangleList(d)) {
        uint64_t sum=0,squares=0;
        for (size_t i=0;i<d.index_count/3;++i) {
            uint64_t h=triangleAt(d.index_bytes,i)+0x9e3779b97f4a7c15ull;
            h=(h^(h>>30))*0xbf58476d1ce4e5b9ull;
            h=(h^(h>>27))*0x94d049bb133111ebull;h^=h>>31;
            sum+=h;squares+=h*h;
        }
        mix(&sum,sizeof sum);mix(&squares,sizeof squares);
    } else if (d.index_bytes) mix(d.index_bytes, size_t(d.index_count) * 2);
    RecompD3dVertexLayout layout{};
    if (d.vertex_bytes && recomp_d3d_fvf_layout(d.fvf, &layout) &&
        layout.stride <= d.vertex_stride && layout.texcoord_count) {
        for (unsigned i = 0; i < std::min(d.vertex_count, 16u); ++i) {
            const size_t v = size_t(i) * (d.vertex_count - 1) / std::max(1u, std::min(d.vertex_count, 16u) - 1);
            mix(static_cast<const unsigned char *>(d.vertex_bytes) + v * d.vertex_stride + layout.texcoord_offset,
                layout.stride - layout.texcoord_offset);
        }
    }
    return h;
}

inline bool compatible(const RecompD3dPresenterDrawCommand &a,
                       const RecompD3dPresenterDrawCommand &b) {
    if (a.fvf != b.fvf || a.vertex_stride != b.vertex_stride ||
        a.vertex_count != b.vertex_count || a.index_count != b.index_count ||
        a.primitive_type != b.primitive_type || a.program_count || b.program_count ||
        a.has_transform != b.has_transform || !a.has_transform ||
        a.blend_weight_count != b.blend_weight_count ||
        a.target.offscreen != b.target.offscreen || a.target.color.data != b.target.color.data ||
        a.texture.data != b.texture.data || a.texture.format_byte != b.texture.format_byte ||
        a.directional.enabled != b.directional.enabled ||
        a.directional.count != b.directional.count ||
        a.has_reflection != b.has_reflection || a.texgen_reflection != b.texgen_reflection ||
        a.texture_transform_uv != b.texture_transform_uv || a.texgen_projected != b.texgen_projected ||
        std::memcmp(&a.pixel_program, &b.pixel_program, offsetof(RecompD3dPixelProgram, constants)) != 0 ||
        !a.vertex_bytes || !b.vertex_bytes || a.vertex_stride < 12 ||
        a.vertex_count > 1000000) return false;
    for (unsigned i = 0; a.pixel_program.kind != RECOMP_D3D_PIXEL_NONE && i < RECOMP_D3D_PIXEL_STAGES; ++i)
        if (a.stage_textures[i].desc.data != b.stage_textures[i].desc.data) return false;
    if (!sameTopology(a,b)) return false;
    RecompD3dVertexLayout layout{};
    return recomp_d3d_fvf_layout(a.fvf, &layout) && !layout.pretransformed && layout.stride <= a.vertex_stride;
}

inline void blend(float *out, const float *a, const float *b, size_t count, float t) {
    for (size_t i = 0; i < count; ++i)
        out[i] = std::isfinite(a[i]) && std::isfinite(b[i]) ? a[i] + (b[i] - a[i]) * t : b[i];
}

// Native skeletal shaders replace position/normal from their immutable mesh
// and pose. Only the remaining per-vertex attributes need a resident stream.
inline bool sameAttributes(const RecompD3dPresenterDrawCommand &a,
                           const RecompD3dPresenterDrawCommand &b, bool skeletal) {
    const size_t bytes = size_t(a.vertex_count)*a.vertex_stride;
    if (!skeletal) return std::memcmp(a.vertex_bytes,b.vertex_bytes,bytes) == 0;
    RecompD3dVertexLayout layout{};
    if (!recomp_d3d_fvf_layout(a.fvf,&layout) || layout.pretransformed) return false;
    const auto *p = static_cast<const uint8_t *>(a.vertex_bytes);
    const auto *q = static_cast<const uint8_t *>(b.vertex_bytes);
    for (uint32_t v=0; v<a.vertex_count; ++v) {
        const size_t base = size_t(v)*a.vertex_stride;
        uint32_t begin = 0;
        for (uint32_t offset : {layout.position_offset, layout.normal_offset}) {
            if (offset == RECOMP_D3D_FVF_ABSENT) continue;
            if (offset < begin || offset+12 > a.vertex_stride) return false;
            if (std::memcmp(p+base+begin,q+base+begin,offset-begin)) return false;
            begin = offset+12;
        }
        if (std::memcmp(p+base+begin,q+base+begin,a.vertex_stride-begin)) return false;
    }
    return true;
}

// Validate once when the two immutable simulation snapshots become a pair.
inline bool prepare(const RecompD3dPresenterDrawCommand &previous,
                    const RecompD3dPresenterDrawCommand &current) {
    if (!compatible(previous, current)) return false;
    RecompD3dVertexLayout layout{};
    recomp_d3d_fvf_layout(current.fvf, &layout);
    const auto *a = static_cast<const unsigned char *>(previous.vertex_bytes);
    const auto *b = static_cast<const unsigned char *>(current.vertex_bytes);
    // Texture coordinates identify vertices within a matching draw; don't blend
    // newly allocated objects or a changed mesh merely because its size agrees.
    for (unsigned v = 0; v < current.vertex_count; ++v) {
        const size_t start = size_t(v) * current.vertex_stride;
        if (layout.texcoord_count && std::memcmp(a + start + layout.texcoord_offset,
            b + start + layout.texcoord_offset, layout.stride - layout.texcoord_offset)) return false;
        float p[3], q[3];
        std::memcpy(p, a + start + layout.position_offset, sizeof p);
        std::memcpy(q, b + start + layout.position_offset, sizeof q);
        for (unsigned k = 0; k < 3; ++k)
            if (!std::isfinite(p[k]) || !std::isfinite(q[k]) || std::abs(q[k] - p[k]) > 100.0f)
                return false; // discontinuity: display current geometry immediately
    }
    return true;
}

// Repeated stage decorations can have the same topology/UVs but positions
// baked into different places. Exact geometry resolves those collisions without
// relying on draw order or guessing a nearest object as the camera moves.
inline bool staticGeometryKey(const RecompD3dPresenterDrawCommand &d, uint64_t &hash) {
    RecompD3dVertexLayout layout{};
    if (!d.has_transform || d.program_count || !d.vertex_bytes || !d.vertex_count ||
        !recomp_d3d_fvf_layout(d.fvf, &layout) || layout.pretransformed ||
        layout.stride > d.vertex_stride || uint64_t(d.vertex_stride)*d.vertex_count > 64ull*1024*1024)
        return false;
    hash = key(d);
    const auto *bytes = static_cast<const unsigned char *>(d.vertex_bytes);
    for (size_t i = 0; i < size_t(d.vertex_stride)*d.vertex_count; ++i)
        hash = (hash ^ bytes[i])*1099511628211ull;
    return true;
}

inline uint64_t provenanceKey(uint64_t geometry, const RecompD3dPresenterDrawCommand &d) {
    geometry=(geometry ^ d.visual_vertex_source)*1099511628211ull;
    return (geometry ^ d.visual_transform_source)*1099511628211ull;
}

template<class Packet>
void matchSnapshots(const Packet &previous, const Packet &current,
                    std::vector<size_t> &matches,
                    std::unordered_map<uint64_t,size_t> &previous_draws,
                    std::unordered_map<uint64_t,uint32_t> &current_counts) {
    previous_draws.clear(); current_counts.clear();
    const auto insertUnique = [](auto &map, uint64_t hash, size_t index) {
        auto result = map.emplace(hash,index);
        if (!result.second) result.first->second = SIZE_MAX;
    };
    for (size_t i=0; i<previous.count(); ++i) {
        const auto &c=previous.command(i);
        if (c.type==RECOMP_D3D_PRESENTER_COMMAND_DRAW) insertUnique(previous_draws,key(c.data.draw),i);
    }
    for (size_t i=0; i<current.count(); ++i) {
        const auto &c=current.command(i);
        if (c.type==RECOMP_D3D_PRESENTER_COMMAND_DRAW) ++current_counts[key(c.data.draw)];
    }
    const auto ambiguous = [&](uint64_t hash) {
        const auto p=previous_draws.find(hash);
        const auto c=current_counts.find(hash);
        return p!=previous_draws.end() && c!=current_counts.end() &&
            (p->second==SIZE_MAX || c->second>1);
    };
    std::unordered_map<uint64_t,size_t> old_geometry,new_geometry,old_sources,new_sources;
    const auto indexGeometry = [&](const Packet &packet, auto &map, auto &sources) {
        for (size_t i=0; i<packet.count(); ++i) {
            const auto &c=packet.command(i);
            uint64_t hash;
            if (c.type==RECOMP_D3D_PRESENTER_COMMAND_DRAW && ambiguous(key(c.data.draw)) &&
                staticGeometryKey(c.data.draw,hash)) {
                insertUnique(map,hash,i);
                if (c.data.draw.visual_vertex_source) insertUnique(sources,provenanceKey(hash,c.data.draw),i);
            }
        }
    };
    indexGeometry(previous,old_geometry,old_sources); indexGeometry(current,new_geometry,new_sources);
    matches.assign(current.count(),SIZE_MAX);
    for (size_t i=0; i<current.count(); ++i) {
        const auto &c=current.command(i);
        if (c.type!=RECOMP_D3D_PRESENTER_COMMAND_DRAW) continue;
        const auto hash=key(c.data.draw);
        const auto p=previous_draws.find(hash);
        if (p==previous_draws.end()) continue;
        size_t index=p->second;
        if (ambiguous(hash)) {
            uint64_t geometry;
            if (!staticGeometryKey(c.data.draw,geometry)) continue;
            const auto old=old_geometry.find(geometry), now=new_geometry.find(geometry);
            if (old==old_geometry.end() || now==new_geometry.end()) continue;
            index=old->second;
            if (index==SIZE_MAX || now->second!=i) {
                if (!c.data.draw.visual_vertex_source) continue;
                const auto source=provenanceKey(geometry,c.data.draw);
                const auto old_source=old_sources.find(source), new_source=new_sources.find(source);
                if (old_source==old_sources.end() || old_source->second==SIZE_MAX ||
                    new_source==new_sources.end() || new_source->second!=i) continue;
                index=old_source->second;
                const auto &d=previous.command(index).data.draw;
                if (d.visual_vertex_source!=c.data.draw.visual_vertex_source ||
                    d.visual_transform_source!=c.data.draw.visual_transform_source) continue;
            }
            const auto &d=previous.command(index).data.draw;
            if (key(d)!=hash || d.vertex_stride!=c.data.draw.vertex_stride ||
                d.vertex_count!=c.data.draw.vertex_count ||
                std::memcmp(d.vertex_bytes,c.data.draw.vertex_bytes,size_t(d.vertex_stride)*d.vertex_count)) continue;
        }
        if (index!=SIZE_MAX && prepare(previous.command(index).data.draw,c.data.draw)) matches[i]=index;
    }
}

inline void interpolateTransforms(const RecompD3dPresenterDrawCommand &previous,
                                  const RecompD3dPresenterDrawCommand &current,
                                  float alpha, RecompD3dPresenterDrawCommand &out) {
    blend(out.transform, previous.transform, current.transform, 16, alpha);
    if (current.pixel_program.kind != RECOMP_D3D_PIXEL_NONE)
        // UV matrices can select discrete atlas cells. Blending their offsets
        // samples between cells (or across a row wrap), inventing animation frames.
        for (unsigned i=0; i<RECOMP_D3D_PIXEL_CONSTANT_UV; ++i)
            blend(out.pixel_program.constants[i], previous.pixel_program.constants[i],
                current.pixel_program.constants[i], 4, alpha);
    for (unsigned i = 0; i < current.blend_weight_count && i < 3; ++i)
        blend(out.blend_transforms[i], previous.blend_transforms[i], current.blend_transforms[i], 16, alpha);
    if (current.directional.enabled) {
        // Keep the specular viewer at the same interpolated camera pose.
        if (previous.directional.sources[3] == current.directional.sources[3])
            blend(out.directional.view, previous.directional.view, current.directional.view, 3, alpha);
        for (unsigned i = 0; i < 4; ++i) {
            blend(out.directional.normal_transforms[i], previous.directional.normal_transforms[i], current.directional.normal_transforms[i], 16, alpha);
            blend(out.directional.world_transforms[i], previous.directional.world_transforms[i], current.directional.world_transforms[i], 16, alpha);
        }
    }
    if (previous.general_fog[0][0] == current.general_fog[0][0] &&
        previous.general_fog[0][1] == current.general_fog[0][1] &&
        previous.general_fog[2][1] == current.general_fog[2][1])
        for (unsigned i : {1u,3u,4u})
            blend(out.general_fog[i], previous.general_fog[i], current.general_fog[i], 4, alpha);
    if (current.has_reflection || current.texgen_reflection || current.texture_transform_uv || current.texgen_projected) {
        blend(out.reflection_world_view, previous.reflection_world_view, current.reflection_world_view, 16, alpha);
        blend(out.reflection_normal, previous.reflection_normal, current.reflection_normal, 16, alpha);
    }
}

inline bool interpolate(const RecompD3dPresenterDrawCommand &previous,
                        const RecompD3dPresenterDrawCommand &current,
                        float alpha, RecompD3dPresenterDrawCommand &out,
                        std::vector<unsigned char> &vertices) {
    out = current;
    if (!std::isfinite(alpha) || !prepare(previous, current)) return false;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    RecompD3dVertexLayout layout{};
    recomp_d3d_fvf_layout(current.fvf, &layout);
    const auto *a = static_cast<const unsigned char *>(previous.vertex_bytes);
    const auto *b = static_cast<const unsigned char *>(current.vertex_bytes);
    vertices.assign(b, b + size_t(current.vertex_count) * current.vertex_stride);
    for (unsigned v = 0; v < current.vertex_count; ++v) {
        const size_t start = size_t(v) * current.vertex_stride;
        for (unsigned offset : {layout.position_offset, layout.normal_offset}) {
            if (offset == RECOMP_D3D_FVF_ABSENT) continue;
            float p[3], q[3], r[3];
            std::memcpy(p, a + start + offset, sizeof p);
            std::memcpy(q, b + start + offset, sizeof q);
            blend(r, p, q, 3, alpha);
            if (offset == layout.normal_offset) {
                float length = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
                if (length > 0.00001f) for (float &x : r) x /= length;
            }
            std::memcpy(vertices.data() + start + offset, r, sizeof r);
        }
    }
    out.vertex_bytes = vertices.data();
    interpolateTransforms(previous, current, alpha, out);
    return true;
}
} // namespace sc2_visual
