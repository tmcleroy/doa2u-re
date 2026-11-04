// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// Immutable native scene data. Ownership follows captured draw packets, never
// guest RAM. The GPU thread cannot observe later animation/gameplay writes.
struct Sc2SkinInfluence { float position[4], normal[3]; uint32_t bone; };
struct Sc2SkinMesh {
    uint32_t model = 0, source = 0;
    uint32_t classes[4]{}, bones = 0;
    std::vector<Sc2SkinInfluence> influences;
    std::vector<uint32_t> starts, lengths;
};
struct Sc2SkinPose : std::enable_shared_from_this<Sc2SkinPose> {
    std::shared_ptr<const Sc2SkinMesh> mesh;
    std::vector<float> matrices;
};
