// SPDX-License-Identifier: GPL-3.0-or-later
/* Included after lookupPipeline: recipes contain state, never guest or GPU
   pointers. Current shader generation is used again on every launch. */
bool prepareGraphics(RecompD3dPresenter *p) {
    const char *folder=std::getenv("RECOMP_GRAPHICS_CATALOG");
    if (!folder || !*folder) return true;
    const std::string profile="-v2-"+std::to_string(p->msaa_count)+"-"+std::to_string(p->depth_format)+"-"+std::to_string(static_cast<int>(p->shader_target))+".bin";
    const std::string shaders_path=std::string(folder)+"/shaders"+profile;
    const std::string pipelines_path=std::string(folder)+"/pipelines"+profile;
    const auto shaders=recomp_warmup::read<WarmShader>(shaders_path);
    const auto offline=recomp_warmup::read<WarmShader>(std::string(folder)+"/offline-shaders-v1.bin");
    const auto pipelines=recomp_warmup::read<WarmPipeline>(pipelines_path,32768);
    if (recomp_warmup::rewrite(shaders_path,shaders)) p->warm_shader_file=std::fopen(shaders_path.c_str(),"ab");
    if (recomp_warmup::rewrite(pipelines_path,pipelines)) p->warm_pipeline_file=std::fopen(pipelines_path.c_str(),"ab");
    if (!p->warm_shader_file || !p->warm_pipeline_file)
        std::fprintf(stderr,"Graphics preparation: catalog is not writable; new entries may not persist\n");
    // Keep learning the catalog even when startup preparation is disabled.
    if (const char *prepare=std::getenv("RECOMP_GRAPHICS_PREPARE"); prepare && std::strcmp(prepare,"0")==0)
        return true;
    const std::string original_title = SDL_GetWindowTitle(p->window);
    p->warming=true;
    const auto start=std::chrono::steady_clock::now();
    const size_t total=offline.size()+shaders.size()+pipelines.size();
    auto last_progress=start-std::chrono::seconds(1);
    std::string phase="Preparing graphics";
    const bool strict=std::getenv("RECOMP_GRAPHICS_INVENTORY_STRICT")!=nullptr;
    size_t done=0,failed=strict && offline.empty()?1:0;
    const auto progress=[&]() {
        const auto now=std::chrono::steady_clock::now();
        if (done!=total && now-last_progress<std::chrono::milliseconds(33)) return !p->close_requested;
        last_progress=now;
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) p->close_requested=true;
        const std::string title="Preparing graphics - "+phase+" "+std::to_string(done)+" / "+std::to_string(total);
        SDL_SetWindowTitle(p->window,title.c_str());
        // Keep the startup window responsive without executing guest frames.
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(p->device);
        if (cmd) {
            SDL_GPUTexture *swap=nullptr;
            if (SDL_AcquireGPUSwapchainTexture(cmd,p->window,&swap,nullptr,nullptr) && swap) {
                SDL_GPUColorTargetInfo color{}; color.texture=swap;
                color.clear_color={0.025f,0.04f,0.08f,1};
                color.load_op=SDL_GPU_LOADOP_CLEAR; color.store_op=SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass *pass=SDL_BeginGPURenderPass(cmd,&color,1,nullptr);
                if (pass) SDL_EndGPURenderPass(pass);
            }
            SDL_SubmitGPUCommandBuffer(cmd);
        }
        return !p->close_requested;
    };
    SDL_SetWindowTitle(p->window,"Preparing base shaders");
    precompileDrawShaders(p);
    std::unordered_map<uint64_t,ShaderPair> prepared;
    size_t offline_pipelines=0;
    for (const auto &entry:p->shaders) {
        prepared.emplace(entry.second.recipe,entry.second);
        if (!offline.empty() && entry.second.vertex && entry.second.fragment) {
            WarmShader r{};r.fvf=entry.first.fvf;
            const size_t before=p->pipelines.size();
            if (!prepareOfflinePipelines(p,r,entry.second,progress)) {
                if (p->close_requested) { p->warming=false; return false; }
                ++failed;
            }
            offline_pipelines+=p->pipelines.size()-before;
        }
    }
    std::vector<WarmShader> all=offline;
    all.insert(all.end(),shaders.begin(),shaders.end());
    // Prefill source-keyed binaries concurrently, then construct GPU resources
    // using the ordinary renderer path below. No guest work runs on workers.
    unsigned workers=std::min(16u,std::max(1u,std::thread::hardware_concurrency()));
    if (const char *setting=std::getenv("RECOMP_SHADER_WORKERS")) {
        char *end=nullptr; const long value=std::strtol(setting,&end,10);
        if (end!=setting && *end=='\0' && value>=1 && value<=16) workers=static_cast<unsigned>(value);
    }
    const char *cache=std::getenv("RECOMP_SHADER_CACHE");
    if (workers>1 && cache && *cache && !all.empty()) {
        std::atomic<size_t> next{0},completed{0};
        std::atomic<bool> cancel{false};
        phase="Compiling shaders";
        std::vector<std::thread> pool;
        pool.reserve(workers);
        const auto job=[&]() {
            while (!cancel.load()) {
                const size_t i=next.fetch_add(1);
                if (i>=all.size()) break;
                const auto &r=all[i];
                if (r.count<=136 && r.reserved==0 && r.coverage<=1 && r.scaled_bias<=1 && r.sample_bias<=1 && r.count_pass<=1) {
                    try {
                        RecompD3dDrawShaderKey key{r.fvf,r.count,r.program,r.coverage!=0,r.scaled_bias!=0,r.sample_bias!=0,r.pixel.kind?&r.pixel:nullptr};
                        bool ok=false;
                        lookupShaders(p,key,r.count_pass!=0,&ok);
                        // Normal preparation retries failures and reports them.
                    } catch (...) { /* Renderer-thread retry remains available. */ }
                }
                ++completed;
            }
        };
        try { for (unsigned i=0;i<workers;++i) pool.emplace_back(job); }
        catch (const std::system_error &) { /* Use the workers already created. */ }
        if (!pool.empty()) {
            while (completed.load()<all.size()) {
                done=completed.load();
                if (!progress()) { cancel=true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        for (auto &thread:pool) thread.join();
        std::fprintf(stderr,"Graphics preparation: shader workers=%zu recipes=%zu\n",pool.size(),completed.load());
        done=0;
        phase="Preparing pipelines";
        if (cancel) { p->warming=false; return false; }
    }
    for (const auto &r:all) {
        if (!progress()) { p->warming=false; return false; }
        ++done;
        if (r.count>136 || r.reserved!=0 || r.coverage>1 || r.scaled_bias>1 || r.sample_bias>1 || r.count_pass>1) { ++failed; continue; }
        RecompD3dDrawShaderKey key{r.fvf,r.count,r.program,r.coverage!=0,r.scaled_bias!=0,r.sample_bias!=0,
            r.pixel.kind?&r.pixel:nullptr};
        const auto pair=lookupShaders(p,key,r.count_pass!=0);
        if (!pair.vertex || !pair.fragment) ++failed;
        else {
            prepared.emplace(recomp_warmup::checksum(&r,sizeof r),pair);
            if (done<=offline.size()) {
                const size_t before=p->pipelines.size();
                if (!prepareOfflinePipelines(p,r,pair,progress)) {
                    if (p->close_requested) { p->warming=false; return false; }
                    ++failed;
                }
                offline_pipelines+=p->pipelines.size()-before;
            }
        }
    }
    if (const char *audit=std::getenv("RECOMP_GRAPHICS_INVENTORY_AUDIT")) {
        std::unordered_map<SDL_GPUShader*,uint64_t> identities;
        for (const auto &s:p->shaders) identities.emplace(s.second.vertex,s.second.recipe);
        std::vector<WarmPipeline> records;
        for (const auto &entry:p->pipelines) {
            WarmPipeline r{};r.shader=identities[entry.first.vertex];r.key=entry.first;
            r.key.vertex=r.key.fragment=nullptr;records.push_back(r);
        }
        if (!recomp_warmup::rewrite(audit,records)) ++failed;
    }
    for (const auto &r:pipelines) {
        if (!progress()) { p->warming=false; return false; }
        ++done;
        const auto found=prepared.find(r.shader);
        if (found==prepared.end()) { ++failed; continue; }
        const auto &k=r.key;
        RecompD3dPresenterDrawCommand draw{};
        draw.depth_bias=k.depth_bias;
        draw.depth.depth_test_enable=k.depth_test; draw.depth.depth_write_enable=k.depth_write;
        draw.depth.depth_func=static_cast<decltype(draw.depth.depth_func)>(k.depth_func);
        draw.depth.stencil_enable=k.stencil_enable; draw.depth.stencil_read_mask=k.stencil_read;
        draw.depth.stencil_write_mask=k.stencil_write;
        draw.depth.stencil_func=static_cast<decltype(draw.depth.stencil_func)>(k.stencil_func);
        draw.depth.stencil_fail=static_cast<decltype(draw.depth.stencil_fail)>(k.stencil_fail);
        draw.depth.stencil_zfail=static_cast<decltype(draw.depth.stencil_zfail)>(k.stencil_zfail);
        draw.depth.stencil_pass=static_cast<decltype(draw.depth.stencil_pass)>(k.stencil_pass);
        draw.blend.blend_enable=k.blend_enable; draw.blend.color_write_mask=k.write_mask;
        draw.blend.src_factor=static_cast<decltype(draw.blend.src_factor)>(k.src_factor);
        draw.blend.dst_factor=static_cast<decltype(draw.blend.dst_factor)>(k.dst_factor);
        draw.blend.op=static_cast<decltype(draw.blend.op)>(k.blend_op);
        PipelineState state{&draw,found->second,k.fvf,k.stride,static_cast<SDL_GPUPrimitiveType>(k.primitive),
            static_cast<RecompD3dCullMode>(k.cull),static_cast<SDL_GPUTextureFormat>(k.color_format),
            static_cast<SDL_GPUTextureFormat>(k.depth_format),static_cast<SDL_GPUSampleCount>(k.samples),
            k.slope_bias,k.coverage_only!=0,k.count_pass!=0};
        if (!lookupPipeline(p,state)) ++failed;
    }
    SDL_WaitForGPUIdle(p->device);
    progress();
    p->warming=false;
    SDL_SetWindowTitle(p->window, original_title.c_str());
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    std::fprintf(stderr,"Graphics preparation: finished shaders=%zu pipelines=%zu offline_shaders=%zu offline_pipelines=%zu unavailable=%zu ms=%lld; gameplay begins\n",
        shaders.size(),pipelines.size(),offline.size(),offline_pipelines,failed,static_cast<long long>(ms));
    return !p->close_requested && (!strict || failed==0);
}
