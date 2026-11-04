// SPDX-License-Identifier: GPL-3.0-or-later
// Included in the SDL_GPU backend's private namespace.
long long visualNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void releaseVisualGeometry(RecompD3dPresenter *p, VisualGpuFrame &frame) {
    for (auto *b : {frame.vertices, frame.indices}) if (b) SDL_ReleaseGPUBuffer(p->device, b);
    for (auto *b : {frame.vertex_transfer, frame.index_transfer}) if (b) SDL_ReleaseGPUTransferBuffer(p->device, b);
    frame = {};
}

std::shared_ptr<VisualAsset> createVisualAsset(RecompD3dPresenter *p,
    const RecompD3dPresenterDrawCommand &d) {
    const uint32_t vertex_bytes = d.vertex_stride*d.vertex_count, index_bytes = d.index_count*2;
    const uint64_t bytes = uint64_t(vertex_bytes)+index_bytes;
    auto &budget = p->visual.asset_budget;
    if (*budget + bytes > 64ull*1024*1024) return {};
    auto asset = std::make_shared<VisualAsset>();
    asset->device = p->device;
    const SDL_GPUBufferCreateInfo vb{SDL_GPU_BUFFERUSAGE_VERTEX, vertex_bytes, 0};
    const SDL_GPUBufferCreateInfo ib{SDL_GPU_BUFFERUSAGE_INDEX, index_bytes, 0};
    asset->vertices = SDL_CreateGPUBuffer(p->device, &vb);
    asset->indices = SDL_CreateGPUBuffer(p->device, &ib);
    if (!asset->vertices || !asset->indices) return {};
    const uint32_t index_start = (vertex_bytes+15u)&~15u;
    const SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, index_start+index_bytes, 0};
    auto *transfer = SDL_CreateGPUTransferBuffer(p->device, &ti);
    if (!transfer) return {};
    void *mapped = SDL_MapGPUTransferBuffer(p->device, transfer, false);
    if (!mapped) { SDL_ReleaseGPUTransferBuffer(p->device, transfer); return {}; }
    std::memcpy(mapped, d.vertex_bytes, vertex_bytes);
    std::memcpy(static_cast<uint8_t *>(mapped)+index_start, d.index_bytes, index_bytes);
    SDL_UnmapGPUTransferBuffer(p->device, transfer);
    auto *commands = commandBuffer(p);
    auto *copy = commands ? SDL_BeginGPUCopyPass(commands) : nullptr;
    if (!copy) { SDL_ReleaseGPUTransferBuffer(p->device, transfer); return {}; }
    const SDL_GPUTransferBufferLocation vs{transfer, 0}, is{transfer, index_start};
    const SDL_GPUBufferRegion vd{asset->vertices, 0, vertex_bytes}, id{asset->indices, 0, index_bytes};
    SDL_UploadToGPUBuffer(copy, &vs, &vd, false);
    SDL_UploadToGPUBuffer(copy, &is, &id, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(p->device, transfer);
    asset->budget = budget; asset->bytes = bytes; *budget += bytes;
    p->visual.upload_bytes += bytes;
    return asset;
}

std::shared_ptr<VisualAsset> uploadVisualStorage(RecompD3dPresenter *p, const void *data, size_t size) {
    auto &budget = p->visual.asset_budget;
    if (!size || size > 16u*1024*1024 || *budget+size > 64ull*1024*1024) return {};
    auto asset = std::make_shared<VisualAsset>(); asset->device = p->device;
    const SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, static_cast<uint32_t>(size), 0};
    const SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, static_cast<uint32_t>(size), 0};
    asset->vertices = SDL_CreateGPUBuffer(p->device, &bi);
    auto *transfer = asset->vertices ? SDL_CreateGPUTransferBuffer(p->device, &ti) : nullptr;
    if (!transfer) return {};
    void *mapped = SDL_MapGPUTransferBuffer(p->device, transfer, false);
    if (!mapped) { SDL_ReleaseGPUTransferBuffer(p->device, transfer); return {}; }
    std::memcpy(mapped, data, size); SDL_UnmapGPUTransferBuffer(p->device, transfer);
    auto *commands = commandBuffer(p);
    auto *copy = commands ? SDL_BeginGPUCopyPass(commands) : nullptr;
    if (!copy) { SDL_ReleaseGPUTransferBuffer(p->device, transfer); return {}; }
    const SDL_GPUTransferBufferLocation source{transfer, 0};
    const SDL_GPUBufferRegion destination{asset->vertices, 0, static_cast<uint32_t>(size)};
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy); SDL_ReleaseGPUTransferBuffer(p->device, transfer);
    asset->budget = budget; asset->bytes = size; *budget += size;
    p->visual.upload_bytes += size;
    return asset;
}

// Convert a simulation snapshot into retained GPU geometry once. The next
// display frames reference it directly; no intermediate vertex copies/uploads.
bool prepareVisualGeometry(RecompD3dPresenter *p) {
    auto &v = p->visual;
    std::swap(v.gpu_previous, v.gpu_current);
    std::swap(v.gpu_current, v.gpu_spare);
    auto &f = v.gpu_current;
    f.vertex_data.clear(); f.index_data.clear();
    f.geometry.assign(v.current.count(), {});
    std::unordered_map<const Sc2SkinPose *, std::shared_ptr<VisualAsset>> pose_assets;
    // Keep only immutable model resources referenced by live snapshots.
    for (auto it = v.skin_assets.begin(); it != v.skin_assets.end();) {
        if (it->second.source.use_count() == 1) it = v.skin_assets.erase(it); else ++it;
    }
    for (size_t i = 0; i < v.current.count(); ++i) {
        const auto &c = v.current.command(i);
        if (c.type != RECOMP_D3D_PRESENTER_COMMAND_DRAW) continue;
        const auto &d = c.data.draw;
        const uint64_t bytes = uint64_t(d.vertex_stride) * d.vertex_count;
        if (!d.vertex_bytes || !d.index_bytes || !bytes || !d.index_count ||
            recomp_d3d_draw_geometry_line(d) || bytes > 64ull*1024*1024 ||
            uint64_t(d.index_count)*2 > 64ull*1024*1024 ||
            (d.primitive_type == 7 && (d.triangle_count > 4096 || d.index_count < d.triangle_count + 2))) continue;
        if (f.vertex_data.size() + f.index_data.size() + bytes + uint64_t(d.index_count)*6 > 128ull*1024*1024)
            return false;
        auto &g = f.geometry[i];
        if (d.scene_pose) {
            const auto &pose = *static_cast<const Sc2SkinPose *>(d.scene_pose);
            auto &mesh = v.skin_assets[pose.mesh.get()];
            if (!mesh.gpu) {
                mesh.source = pose.mesh;
                const size_t prefix = (pose.mesh->starts.size()*8+15)&~size_t(15);
                std::vector<uint8_t> data(prefix+pose.mesh->influences.size()*32);
                for (size_t vertex = 0; vertex < pose.mesh->starts.size(); ++vertex) {
                    const uint32_t range[]{static_cast<uint32_t>(prefix)+pose.mesh->starts[vertex]*32,
                        pose.mesh->lengths[vertex]};
                    std::memcpy(data.data()+vertex*8, range, sizeof range);
                }
                std::memcpy(data.data()+prefix, pose.mesh->influences.data(), pose.mesh->influences.size()*32);
                mesh.gpu = uploadVisualStorage(p, data.data(), data.size());
            }
            auto &palette = pose_assets[&pose];
            if (!palette) palette = uploadVisualStorage(p, pose.matrices.data(), pose.matrices.size()*4);
            if (mesh.gpu && palette) {
                g.skin_mesh = mesh.gpu; g.skin_pose = palette; g.skin_first = d.scene_first_vertex;
            }
        }
        // Promote byte-identical geometry to immutable assets. Identity alone
        // never authorizes reuse: validate complete vertex/index contents, even
        // when the game reuses a memory address or a draw signature collides.
        const auto key = v.current_keys[i];
        const auto prior = v.previous_draws.find(key);
        const size_t prior_index = prior != v.previous_draws.end() && prior->second != SIZE_MAX &&
            v.current_counts[key] == 1 ? prior->second : v.matches[i];
        if (d.primitive_type != 7 && prior_index != SIZE_MAX && prior_index < v.gpu_previous.geometry.size()) {
            const auto &old = v.previous.command(prior_index).data.draw;
            if (bool(g.skin_mesh) == bool(v.gpu_previous.geometry[prior_index].skin_mesh) &&
                old.vertex_bytes && old.index_bytes && d.fvf == old.fvf &&
                d.vertex_stride == old.vertex_stride && d.vertex_count == old.vertex_count &&
                d.index_count == old.index_count && d.primitive_type == old.primitive_type &&
                sc2_visual::sameAttributes(d, old, g.skin_mesh &&
                    g.skin_mesh == v.gpu_previous.geometry[prior_index].skin_mesh &&
                    g.skin_first == v.gpu_previous.geometry[prior_index].skin_first) &&
                std::memcmp(d.index_bytes, old.index_bytes, d.index_count*2) == 0) {
                g.asset = v.gpu_previous.geometry[prior_index].asset;
                if (!g.asset) g.asset = createVisualAsset(p, d);
                if (g.asset) { g.valid = true; g.index_count = d.index_count; continue; }
            }
        }
        g.vertex = appendAligned(f.vertex_data, d.vertex_bytes, static_cast<uint32_t>(bytes));
        if (d.primitive_type == 7) {
            std::vector<uint16_t> fan(d.triangle_count * 3);
            const auto *src = static_cast<const uint16_t *>(d.index_bytes);
            for (unsigned t = 0; t < d.triangle_count; ++t) {
                fan[t*3] = src[0]; fan[t*3+1] = src[t+1]; fan[t*3+2] = src[t+2];
            }
            g.index = appendAligned(f.index_data, fan.data(), fan.size()*2);
            g.index_count = static_cast<uint32_t>(fan.size());
        } else {
            g.index = appendAligned(f.index_data, d.index_bytes, d.index_count*2);
            g.index_count = d.index_count;
        }
        g.valid = true;
    }
    if (f.vertex_data.empty() || f.index_data.empty()) return true;
    uint32_t unused = 0;
    if (!ensureRingCapacity(p, f.vertices, f.vertex_transfer, f.vertex_capacity, unused,
            static_cast<uint32_t>(f.vertex_data.size()), SDL_GPU_BUFFERUSAGE_VERTEX) ||
        !ensureRingCapacity(p, f.indices, f.index_transfer, f.index_capacity, unused,
            static_cast<uint32_t>(f.index_data.size()), SDL_GPU_BUFFERUSAGE_INDEX)) return false;
    auto *commands = commandBuffer(p);
    if (!commands) return false;
    auto *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) return false;
    const auto upload = [&](SDL_GPUBuffer *buffer, SDL_GPUTransferBuffer *transfer, const std::vector<uint8_t> &data) {
        // Both resources cycle: queued GPU work may still reference the old slot.
        void *mapped = SDL_MapGPUTransferBuffer(p->device, transfer, true);
        if (!mapped) return false;
        std::memcpy(mapped, data.data(), data.size());
        SDL_UnmapGPUTransferBuffer(p->device, transfer);
        const SDL_GPUTransferBufferLocation source{transfer, 0};
        const SDL_GPUBufferRegion dest{buffer, 0, static_cast<uint32_t>(data.size())};
        SDL_UploadToGPUBuffer(copy, &source, &dest, true);
        v.upload_bytes += data.size();
        return true;
    };
    const bool ok = upload(f.vertices, f.vertex_transfer, f.vertex_data) &&
        upload(f.indices, f.index_transfer, f.index_data);
    SDL_EndGPUCopyPass(copy);
    return ok;
}

void swapVisualTargets(RecompD3dPresenter *p) {
    auto &t = p->visual.targets;
    std::swap(p->main_color, t.color);
    std::swap(p->main_resolve, t.resolve);
    std::swap(p->main_depth, t.depth);
    std::swap(p->main_resolved, t.resolved);
    std::swap(p->back_buffer_copy, t.back);
    std::swap(p->previous_frame, t.previous);
    std::swap(p->copy_mips, t.copy_mips);
    std::swap(p->previous_frame_mips, t.previous_mips);
    std::swap(p->target_bytes, t.bytes);
    p->render_targets.swap(t.colors);
    p->depth_targets.swap(t.depths);
}

void releaseVisualTargets(RecompD3dPresenter *p) {
    if (p->visual.trace) { std::fclose(p->visual.trace); p->visual.trace = nullptr; }
    releaseVisualGeometry(p, p->visual.gpu_previous);
    releaseVisualGeometry(p, p->visual.gpu_current);
    releaseVisualGeometry(p, p->visual.gpu_spare);
    p->visual.skin_assets.clear();
    auto &t = p->visual.targets;
    for (auto &e : t.colors) {
        if (e.resolve && e.resolve != e.render) SDL_ReleaseGPUTexture(p->device, e.resolve);
        if (e.render) SDL_ReleaseGPUTexture(p->device, e.render);
    }
    for (auto &e : t.depths) if (e.texture) SDL_ReleaseGPUTexture(p->device, e.texture);
    if (t.resolve && t.resolve != t.color) SDL_ReleaseGPUTexture(p->device, t.resolve);
    for (auto *texture : {t.color, t.depth, t.back, t.previous})
        if (texture) SDL_ReleaseGPUTexture(p->device, texture);
    t = {};
}

long long visualIdle(long long deadline) {
    auto *p = active_presenter;
    if (!p || !p->visual.enabled || p->visual.replaying || p->visual.suspended || p->close_requested) return 0;
    auto &v = p->visual;
    const long long now = visualNow();
    if (!v.current.count()) return 0;
    if (now < v.next) return v.next;
    // Never start a speculative redraw at the very end of the guest's wait.
    // A missed render deadline is dropped; simulation is never fast-forwarded.
    if (deadline - now < 2000000 || p->visibility_open) return deadline;
    const long long period = static_cast<long long>(1e9 / v.fps);
    const auto draws_before = v.draws, blended_before = v.blended, misses_before = v.missed_buffers;
    if (!v.next) v.next = now;
    v.next += ((std::max(0ll, now - v.next) / period) + 1) * period;
    const float alpha = recomp_visual::interpolationAlpha(
        now, v.previous.produced_ns, v.current.produced_ns);
    // Both packets are sealed, owned snapshots. No guest pointers are consulted.
    const bool match = v.epoch == recomp_d3d_visual_generation() &&
        v.previous.count() != 0;
    v.latency_alpha = alpha;
    v.latency_history = match;
    flushSegment(p);
    submitCommands(p);
    const BoundTarget saved_bound = p->last_bound;
    const bool saved_log = p->draw_log_active;
    p->draw_log_active = false;
    v.replaying = true;
    swapVisualTargets(p);
    bool failed = false;
    try {
        RecompD3dPresenterClearCommand clear{};
        clear.clear_color = clear.clear_depth = clear.clear_stencil = true;
        clear.z = 1.0f;
        failed = submitClear(p, clear) != RECOMP_D3D_PRESENTER_OK;
        std::vector<unsigned char> vertices;
        for (size_t i = 0; !failed && i < v.current.count(); ++i) {
            auto command = v.current.command(i);
            v.active_geometry = v.prior_geometry = nullptr;
            v.gpu_blend = false;
            if (command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW) {
                ++v.draws;
                const size_t prior = v.matches[i];
                if (v.gpu_enabled && i < v.gpu_current.geometry.size() && v.gpu_current.geometry[i].valid)
                    v.active_geometry = &v.gpu_current.geometry[i];
                if (v.active_geometry && v.active_geometry->skin_mesh && v.active_geometry->skin_pose) {
                    v.gpu_blend = 2;
                    v.prior_geometry = v.active_geometry;
                    v.alpha = 1;
                }
                const VisualGeometry *prior_gpu = prior < v.gpu_previous.geometry.size() ?
                    &v.gpu_previous.geometry[prior] : nullptr;
                // Native assets omit changing position/normal uploads. Their
                // resident vertex streams are not valid morph endpoints when
                // either side changes skinning path; use owned CPU snapshots.
                const bool native_pair = v.active_geometry && prior_gpu &&
                    v.active_geometry->skin_mesh && v.active_geometry->skin_pose &&
                    prior_gpu->skin_pose && prior_gpu->skin_mesh == v.active_geometry->skin_mesh &&
                    prior_gpu->skin_first == v.active_geometry->skin_first;
                const bool morph_pair = v.active_geometry && prior_gpu &&
                    !v.active_geometry->skin_mesh && !prior_gpu->skin_mesh;
                if (match && prior != SIZE_MAX && v.active_geometry &&
                    prior_gpu && prior_gpu->valid && (native_pair || morph_pair)) {
                    v.gpu_blend = true;
                    v.prior_geometry = &v.gpu_previous.geometry[prior];
                    if (native_pair) v.gpu_blend = 2;
                    v.alpha = alpha;
                    sc2_visual::interpolateTransforms(v.previous.command(prior).data.draw,
                        v.current.command(i).data.draw, alpha, command.data.draw);
                    ++v.blended;
                } else if (match && prior != SIZE_MAX &&
                    sc2_visual::interpolate(v.previous.command(prior).data.draw,
                        v.current.command(i).data.draw, alpha, command.data.draw, vertices)) {
                    v.active_geometry = nullptr;
                    v.prior_geometry = nullptr;
                    v.gpu_blend = false;
                    ++v.blended;
                }
            }
            const auto error = d3d_backend_submit(p, &command);
            if (error != RECOMP_D3D_PRESENTER_OK) failed = true;
        }
        if (!failed) failed = submitPresent(p, v.present) != RECOMP_D3D_PRESENTER_OK;
    } catch (...) {
        failed = true;
    }
    flushSegment(p);
    submitCommands(p);
    swapVisualTargets(p);
    p->last_bound = saved_bound;
    p->draw_log_active = saved_log;
    v.replaying = false;
    v.gpu_blend = false;
    v.active_geometry = v.prior_geometry = nullptr;
    v.replay_ms += (visualNow() - now) / 1e6;
    if (v.trace) std::fprintf(v.trace, "render,%lld,%.4f,%llu,%llu,0,%llu\n", now,
        (visualNow()-now)/1e6, (unsigned long long)(v.draws-draws_before),
        (unsigned long long)(v.blended-blended_before), (unsigned long long)(v.missed_buffers-misses_before));
    if (failed && !p->close_requested) {
        std::fputs("SC2 visual interpolation: replay failed; reverting to original rendering\n", stderr);
        v.enabled = false;
    }
    return v.enabled ? v.next : 0;
}

void finishVisualFrame(RecompD3dPresenter *p, const RecompD3dPresenterPresentCommand &present) {
    auto &v = p->visual;
    const auto started = visualNow();
    const auto bytes_before = v.upload_bytes;
    if (v.bad_capture) {
        std::fputs("SC2 visual interpolation: capture limit exceeded; reverting to original rendering\n", stderr);
        v.enabled = false;
        submitPresent(p, present);
        return;
    }
    v.building.produced_ns = recomp_visual::executing_produced_ns > 0
        ? recomp_visual::executing_produced_ns : started;
    v.building.latency = recomp_latency::executing;
    recomp_latency::record("snapshot", v.building.latency);
    v.building.seal();
    std::swap(v.previous, v.current);
    std::swap(v.previous_keys, v.current_keys);
    std::swap(v.current, v.building);
    v.building.clear(); // retain the oldest slot's allocation for the next tick
    if (v.epoch != recomp_d3d_visual_generation()) v.previous.clear();
    sc2_visual::matchSnapshots(v.previous, v.current, v.matches, v.previous_draws, v.current_counts,
        &v.current_keys, &v.previous_keys);
    if (v.gpu_enabled && !prepareVisualGeometry(p)) {
        v.gpu_enabled = false;
        std::fputs("SC2 visual: retained geometry unavailable; using CPU interpolation\n", stderr);
    }
    // Bounded, opt-in matching audit for diagnosing objects left at simulation rate.
    static unsigned audit_frame = 0;
    static const char *audit_path = std::getenv("RECOMP_VISUAL_MATCH_AUDIT");
    if (audit_path) ++audit_frame;
    if (audit_path && audit_frame >= 1800 && audit_frame <= 2400 && audit_frame % 60 == 0) {
        static const uint32_t audit_texture = [] {
            const char *value=std::getenv("RECOMP_VISUAL_MATCH_TEXTURE");
            return value ? uint32_t(std::strtoul(value,nullptr,0)) : 0u;
        }();
        unsigned draws=0, legacy=0, matched=0, missing=0, ambiguous=0, incompatible=0, constant_changes=0;
        for (size_t i=0; i<v.current.count(); ++i) {
            const auto &c=v.current.command(i);
            if (c.type!=RECOMP_D3D_PRESENTER_COMMAND_DRAW ||
                (audit_texture && c.data.draw.texture.data!=audit_texture)) continue;
            ++draws; matched += v.matches[i]!=SIZE_MAX;
            const auto hash=sc2_visual::key(c.data.draw);
            const auto found=v.previous_draws.find(hash);
            if (found!=v.previous_draws.end() && found->second!=SIZE_MAX) {
                const auto &old=v.previous.command(found->second).data.draw;
                constant_changes += std::memcmp(old.pixel_program.constants,c.data.draw.pixel_program.constants,
                    sizeof old.pixel_program.constants)!=0;
            }
            if (found==v.previous_draws.end()) ++missing;
            else if (found->second==SIZE_MAX || v.current_counts[hash]!=1) ++ambiguous;
            else if (sc2_visual::prepare(v.previous.command(found->second).data.draw,c.data.draw)) ++legacy;
            else ++incompatible;
        }
        if (FILE *file=std::fopen((std::string(audit_path)+".summary.csv").c_str(),audit_frame==1800?"w":"a")) {
            if (audit_frame==1800) std::fputs("frame,draws,signature_matched,matched,missing,ambiguous,incompatible,constant_changes\n",file);
            std::fprintf(file,"%u,%u,%u,%u,%u,%u,%u,%u\n",audit_frame,draws,legacy,matched,missing,ambiguous,incompatible,constant_changes);
            std::fclose(file);
        }
    }
    static const bool audit_series = std::getenv("RECOMP_VISUAL_MATCH_SERIES") != nullptr;
    static const unsigned audit_capture_frame = [] {
        const char *value=std::getenv("RECOMP_VISUAL_MATCH_AT");
        return value ? unsigned(std::strtoul(value,nullptr,10)) : 1800u;
    }();
    if (audit_path && (audit_frame == audit_capture_frame ||
        (audit_series && audit_frame>=600 && audit_frame<=2400 && audit_frame%30==0))) {
        const std::string dump_path=std::string(audit_path)+(audit_series ? "."+std::to_string(audit_frame) : "");
        const auto dumpGeometry = [&](const D3dCapturePacket &packet, const char *suffix) {
            FILE *file=std::fopen((dump_path+suffix).c_str(),"wb");
            if (!file) return;
            FILE *metadata=std::fopen((dump_path+suffix+".metadata").c_str(),"wb");
            for (size_t i=0; i<std::min(packet.count(),size_t(2048)); ++i) {
                const auto &c=packet.command(i);
                if (c.type!=RECOMP_D3D_PRESENTER_COMMAND_DRAW || !c.data.draw.vertex_bytes) continue;
                const auto &d=c.data.draw;
                if (metadata) {
                    const uint32_t index=uint32_t(i);
                    std::fwrite(&index,sizeof index,1,metadata);
                    std::fwrite(&d,sizeof d,1,metadata);
                }
                const uint32_t header[]{uint32_t(i),d.fvf,d.vertex_stride,d.vertex_count,d.index_count,d.texture.data};
                std::fwrite(header,sizeof header,1,file);
                std::fwrite(d.transform,sizeof d.transform,1,file);
                std::fwrite(d.vertex_bytes,size_t(d.vertex_stride)*d.vertex_count,1,file);
                if (d.index_count) std::fwrite(d.index_bytes,size_t(d.index_count)*2,1,file);
            }
            std::fclose(file);
            if (metadata) std::fclose(metadata);
        };
        dumpGeometry(v.previous,".previous.bin"); dumpGeometry(v.current,".current.bin");
        if (FILE *audit = std::fopen((dump_path+".matches.csv").c_str(), "w")) {
            std::fputs("draw,key,duplicates,matched,fvf,program,vertices,texture,x,y,z,wx,wy,wz,primitive,native_pose\n", audit);
            for (size_t i = 0; i < v.current.count(); ++i) {
                const auto &c = v.current.command(i);
                if (c.type != RECOMP_D3D_PRESENTER_COMMAND_DRAW) continue;
                const auto &d = c.data.draw;
                float xyz[3]{};
                if (d.vertex_bytes && d.vertex_stride >= 12) std::memcpy(xyz,d.vertex_bytes,12);
                const auto key = sc2_visual::key(d);
                std::fprintf(audit,"%zu,%llu,%u,%d,%u,%u,%u,%u,%g,%g,%g,%g,%g,%g,%u,%u\n",i,
                    (unsigned long long)key,v.current_counts[key],v.matches[i]!=SIZE_MAX,d.fvf,d.program_count,
                    d.vertex_count,d.texture.data,xyz[0],xyz[1],xyz[2],
                    d.directional.enabled ? d.directional.world_transforms[0][12] : 0,
                    d.directional.enabled ? d.directional.world_transforms[0][13] : 0,
                    d.directional.enabled ? d.directional.world_transforms[0][14] : 0,d.primitive_type,d.scene_pose?1u:0u);
            }
            std::fclose(audit);
        }
    }
    v.epoch = recomp_d3d_visual_generation();
    v.arrived = visualNow();
    v.present = present;
    v.prepare_ms += (visualNow() - started) / 1e6;
    if (v.trace) std::fprintf(v.trace, "snapshot,%lld,%.4f,0,0,%llu,0\n", started,
        (visualNow()-started)/1e6, (unsigned long long)(v.upload_bytes-bytes_before));
    // Render immediately only when the independent display deadline is due.
    visualIdle(v.arrived + 16666667);
    static long long report = 0;
    if (v.arrived - report >= 1000000000) {
        std::fprintf(stderr, "SC2 visual: target=%.3f interpolated_draws=%llu/%llu snapshot=%.2f MiB\n",
            v.fps, (unsigned long long)v.blended, (unsigned long long)v.draws,
            v.current.bytes() / 1048576.0);
        v.draws = v.blended = 0;
        std::fprintf(stderr, "SC2 render: retained_draws=%llu upload_mib=%.2f prepare_ms=%.2f replay_ms=%.2f unavailable_buffers=%llu static_draws=%llu assets_mib=%.2f skeletal_draws=%llu\n",
            (unsigned long long)v.retained_draws, v.upload_bytes/1048576.0, v.prepare_ms,
            v.replay_ms, (unsigned long long)v.missed_buffers, (unsigned long long)v.static_draws,
            *v.asset_budget/1048576.0, (unsigned long long)v.skin_draws);
        v.skin_draws = 0;
        v.static_draws = 0;
        v.retained_draws = v.upload_bytes = v.missed_buffers = 0;
        v.prepare_ms = v.replay_ms = 0;
        std::fflush(stderr);
        if (v.trace) std::fflush(v.trace);
        report = v.arrived;
    }
}
