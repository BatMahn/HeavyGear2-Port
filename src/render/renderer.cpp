#include "renderer.h"

#include <algorithm>
#include <cstring>

#include "../../shaders/gen/world_frag.h"
#include "../../shaders/gen/world_vert.h"
#include "hudfont.h"

namespace hg2 {

namespace {
struct VSUniforms {
    float viewProj[16];
    float model[16];
    float lightDir[4];
    float lightColor[4];
    float ambient[4];
    float eye[4];
};
struct FSUniforms {
    float fogColor[4];
    float fogRange[4];
};
}  // namespace

const char* Renderer::driverName() const { return dev_ ? SDL_GetGPUDeviceDriver(dev_) : "none"; }

SDL_GPUShader* Renderer::makeShader(bool vertex) {
    SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(dev_);
    SDL_GPUShaderCreateInfo ci{};
    ci.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    ci.num_uniform_buffers = 1;
    ci.num_samplers = vertex ? 0 : 1;
    if (fmts & SDL_GPU_SHADERFORMAT_SPIRV) {
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        ci.code = vertex ? world_vert_spv : world_frag_spv;
        ci.code_size = vertex ? sizeof(world_vert_spv) : sizeof(world_frag_spv);
        ci.entrypoint = "main";
#if defined(WORLD_VERT_HAS_DXIL) && defined(WORLD_FRAG_HAS_DXIL)
    } else if (fmts & SDL_GPU_SHADERFORMAT_DXIL) {
        ci.format = SDL_GPU_SHADERFORMAT_DXIL;
        ci.code = vertex ? world_vert_dxil : world_frag_dxil;
        ci.code_size = vertex ? sizeof(world_vert_dxil) : sizeof(world_frag_dxil);
        ci.entrypoint = "main";
#endif
    } else if (fmts & SDL_GPU_SHADERFORMAT_MSL) {
        ci.format = SDL_GPU_SHADERFORMAT_MSL;
        ci.code = reinterpret_cast<const Uint8*>(vertex ? world_vert_msl : world_frag_msl);
        ci.code_size = strlen(vertex ? world_vert_msl : world_frag_msl);
        ci.entrypoint = "main0";
    } else {
        SDL_SetError("no supported shader format (need SPIR-V or MSL)");
        return nullptr;
    }
    return SDL_CreateGPUShader(dev_, &ci);
}

SDL_GPUGraphicsPipeline* Renderer::makePipeline(bool blend, bool background, bool additive) {
    SDL_GPUShader* vs = makeShader(true);
    SDL_GPUShader* fs = makeShader(false);
    if (!vs || !fs) return nullptr;
    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot = 0;
    vbd.pitch = sizeof(Vertex);
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[4] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, pos)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, normal)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, uv)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(Vertex, color)},
    };
    SDL_GPUColorTargetDescription ct{};
    ct.format = colorFmt_;
    if (blend) {
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = additive ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs;
    pi.fragment_shader = fs;
    pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attrs;
    pi.vertex_input_state.num_vertex_attributes = 4;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    pi.depth_stencil_state.enable_depth_test = !background;
    pi.depth_stencil_state.enable_depth_write = !blend && !background;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    pi.target_info.has_depth_stencil_target = true;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
    SDL_ReleaseGPUShader(dev_, vs);
    SDL_ReleaseGPUShader(dev_, fs);
    return p;
}

bool Renderer::init(SDL_Window* window, bool offscreen, int w, int h, std::string* err) {
    win_ = window;
    offscreenMode_ = offscreen;
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL;
#if defined(WORLD_VERT_HAS_DXIL) && defined(WORLD_FRAG_HAS_DXIL)
    formats |= SDL_GPU_SHADERFORMAT_DXIL;  // Direct3D 12 on Windows
#endif
    dev_ = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!dev_) {
        if (err) *err = std::string("SDL_CreateGPUDevice: ") + SDL_GetError();
        return false;
    }
    if (!offscreen) {
        if (!SDL_ClaimWindowForGPUDevice(dev_, win_)) {
            if (err) *err = std::string("SDL_ClaimWindowForGPUDevice: ") + SDL_GetError();
            return false;
        }
        colorFmt_ = SDL_GetGPUSwapchainTextureFormat(dev_, win_);
    } else {
        colorFmt_ = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = colorFmt_;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = Uint32(w);
        ti.height = Uint32(h);
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        offscreen_ = SDL_CreateGPUTexture(dev_, &ti);
        w_ = w;
        h_ = h;
    }
    opaque_ = makePipeline(false);
    blended_ = makePipeline(true);
    background_ = makePipeline(false, true);
    overlayPipe_ = makePipeline(true, true);
    additive_ = makePipeline(true, false, true);
    if (!opaque_ || !blended_ || !background_) {
        if (err) *err = std::string("pipeline: ") + SDL_GetError();
        return false;
    }
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.max_lod = 16;
    sampler_ = SDL_CreateGPUSampler(dev_, &si);
    Image wimg;
    wimg.w = wimg.h = 4;
    wimg.rgba.assign(64, 255);
    white_ = createTexture(wimg, false);
    {  // HUD font atlas; the unused last cell is filled solid for rectangles
        Image f;
        f.w = kFontW, f.h = kFontH;
        f.rgba.assign(size_t(f.w) * f.h * 4, 255);
        for (int i = 0; i < f.w * f.h; i++) f.rgba[size_t(i) * 4 + 3] = kFontAlpha[i];
        int cx = (95 % kFontCols) * kFontCellW, cy = (95 / kFontCols) * kFontCellH;
        for (int y = cy; y < cy + kFontCellH; y++)
            for (int x = cx; x < cx + kFontCellW; x++) f.rgba[(size_t(y) * f.w + x) * 4 + 3] = 255;
        font_ = createTexture(f, false);
    }
    {
        SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_VERTEX, 65536 * sizeof(Vertex), 0};
        overlayVB_ = SDL_CreateGPUBuffer(dev_, &bi);
        SDL_GPUTransferBufferCreateInfo tbi{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 65536 * sizeof(Vertex), 0};
        overlayTB_ = SDL_CreateGPUTransferBuffer(dev_, &tbi);
        dynamicVB_ = SDL_CreateGPUBuffer(dev_, &bi);
        dynamicTB_ = SDL_CreateGPUTransferBuffer(dev_, &tbi);
    }
    return true;
}

void Renderer::shutdown() {
    if (!dev_) return;
    SDL_WaitForGPUIdle(dev_);
    for (auto* t : textures_) SDL_ReleaseGPUTexture(dev_, t);
    for (auto* m : meshes_) {
        SDL_ReleaseGPUBuffer(dev_, m->vb);
        delete m;
    }
    textures_.clear();
    meshes_.clear();
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_);
    if (offscreen_) SDL_ReleaseGPUTexture(dev_, offscreen_);
    if (sampler_) SDL_ReleaseGPUSampler(dev_, sampler_);
    if (opaque_) SDL_ReleaseGPUGraphicsPipeline(dev_, opaque_);
    if (blended_) SDL_ReleaseGPUGraphicsPipeline(dev_, blended_);
    if (background_) SDL_ReleaseGPUGraphicsPipeline(dev_, background_);
    if (overlayPipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, overlayPipe_);
    if (overlayVB_) SDL_ReleaseGPUBuffer(dev_, overlayVB_);
    if (overlayTB_) SDL_ReleaseGPUTransferBuffer(dev_, overlayTB_);
    if (additive_) SDL_ReleaseGPUGraphicsPipeline(dev_, additive_);
    if (dynamicVB_) SDL_ReleaseGPUBuffer(dev_, dynamicVB_);
    if (dynamicTB_) SDL_ReleaseGPUTransferBuffer(dev_, dynamicTB_);
    if (!offscreenMode_) SDL_ReleaseWindowFromGPUDevice(dev_, win_);
    SDL_DestroyGPUDevice(dev_);
    dev_ = nullptr;
}

void Renderer::releaseTo(const ResourceMark& m) {
    if (!dev_) return;
    SDL_WaitForGPUIdle(dev_);
    while (textures_.size() > m.textures) {
        if (textures_.back() && int(textures_.size()) - 1 != white_ && int(textures_.size()) - 1 != font_)
            SDL_ReleaseGPUTexture(dev_, textures_.back());
        textures_.pop_back();
    }
    while (meshes_.size() > m.meshes) {
        if (meshes_.back()) {
            SDL_ReleaseGPUBuffer(dev_, meshes_.back()->vb);
            delete meshes_.back();
        }
        meshes_.pop_back();
    }
}

int Renderer::createTexture(const Image& img, bool mipmaps) {
    if (img.w <= 0 || img.h <= 0) return white_;
    Uint32 levels = 1;
    if (mipmaps) {
        int m = std::max(img.w, img.h);
        while (m > 1) m >>= 1, levels++;
    }
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    ti.width = Uint32(img.w);
    ti.height = Uint32(img.h);
    ti.layer_count_or_depth = 1;
    ti.num_levels = levels;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev_, &ti);
    if (!tex) return white_;
    Uint32 bytes = Uint32(img.rgba.size());
    SDL_GPUTransferBufferCreateInfo tbi{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &tbi);
    void* map = SDL_MapGPUTransferBuffer(dev_, tb, false);
    memcpy(map, img.rgba.data(), bytes);
    SDL_UnmapGPUTransferBuffer(dev_, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{tb, 0, Uint32(img.w), Uint32(img.h)};
    SDL_GPUTextureRegion dst{};
    dst.texture = tex;
    dst.w = Uint32(img.w);
    dst.h = Uint32(img.h);
    dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    if (levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, tex);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(dev_, tb);
    textures_.push_back(tex);
    return int(textures_.size()) - 1;
}

void Renderer::updateTexture(int id, const Image& img) {
    if (id < 0 || id >= int(textures_.size()) || id == white_ || img.rgba.empty()) return;
    Uint32 bytes = Uint32(img.rgba.size());
    SDL_GPUTransferBufferCreateInfo tbi{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &tbi);
    if (!tb) return;
    void* map = SDL_MapGPUTransferBuffer(dev_, tb, false);
    memcpy(map, img.rgba.data(), bytes);
    SDL_UnmapGPUTransferBuffer(dev_, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{tb, 0, Uint32(img.w), Uint32(img.h)};
    SDL_GPUTextureRegion dst{};
    dst.texture = textures_[size_t(id)];
    dst.w = Uint32(img.w);
    dst.h = Uint32(img.h);
    dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(dev_, tb);
}

GpuMesh* Renderer::createMesh(const std::vector<Vertex>& verts, std::vector<DrawBatch> batches) {
    if (verts.empty()) return nullptr;
    auto* m = new GpuMesh;
    Uint32 bytes = Uint32(verts.size() * sizeof(Vertex));
    SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_VERTEX, bytes, 0};
    m->vb = SDL_CreateGPUBuffer(dev_, &bi);
    SDL_GPUTransferBufferCreateInfo tbi{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &tbi);
    void* map = SDL_MapGPUTransferBuffer(dev_, tb, false);
    memcpy(map, verts.data(), bytes);
    SDL_UnmapGPUTransferBuffer(dev_, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{tb, 0};
    SDL_GPUBufferRegion dst{m->vb, 0, bytes};
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(dev_, tb);
    m->batches = std::move(batches);
    m->bmin = m->bmax = Vec3(verts[0].pos[0], verts[0].pos[1], verts[0].pos[2]);
    for (auto& v : verts) {
        m->bmin = Vec3(std::min(m->bmin.x, v.pos[0]), std::min(m->bmin.y, v.pos[1]), std::min(m->bmin.z, v.pos[2]));
        m->bmax = Vec3(std::max(m->bmax.x, v.pos[0]), std::max(m->bmax.y, v.pos[1]), std::max(m->bmax.z, v.pos[2]));
    }
    meshes_.push_back(m);
    return m;
}

void Renderer::ensureDepth(int w, int h) {
    if (depth_ && depthW_ == w && depthH_ == h) return;
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_);
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    ti.width = Uint32(w);
    ti.height = Uint32(h);
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    depth_ = SDL_CreateGPUTexture(dev_, &ti);
    depthW_ = w;
    depthH_ = h;
}

bool Renderer::beginFrame(const FrameParams& fp) {
    fp_ = fp;
    cmd_ = SDL_AcquireGPUCommandBuffer(dev_);
    SDL_GPUTexture* target = offscreen_;
    if (!offscreenMode_) {
        Uint32 w = 0, h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_, win_, &target, &w, &h) || !target) {
            SDL_SubmitGPUCommandBuffer(cmd_);
            cmd_ = nullptr;
            return false;
        }
        w_ = int(w);
        h_ = int(h);
    }
    ensureDepth(w_, h_);
    overlayCount_ = uint32_t(std::min<size_t>(overlay_.size(), 65536));
    if (overlayCount_) {
        void* map = SDL_MapGPUTransferBuffer(dev_, overlayTB_, true);
        memcpy(map, overlay_.data(), overlayCount_ * sizeof(Vertex));
        SDL_UnmapGPUTransferBuffer(dev_, overlayTB_);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd_);
        SDL_GPUTransferBufferLocation src{overlayTB_, 0};
        SDL_GPUBufferRegion dst{overlayVB_, 0, Uint32(overlayCount_ * sizeof(Vertex))};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        SDL_EndGPUCopyPass(cp);
    }
    dynamicCount_ = uint32_t(std::min<size_t>(dynamic_.size(), 65536));
    if (dynamicCount_) {
        void* map = SDL_MapGPUTransferBuffer(dev_, dynamicTB_, true);
        memcpy(map, dynamic_.data(), dynamicCount_ * sizeof(Vertex));
        SDL_UnmapGPUTransferBuffer(dev_, dynamicTB_);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd_);
        SDL_GPUTransferBufferLocation src{dynamicTB_, 0};
        SDL_GPUBufferRegion dst{dynamicVB_, 0, Uint32(dynamicCount_ * sizeof(Vertex))};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        SDL_EndGPUCopyPass(cp);
    }
    SDL_GPUColorTargetInfo ct{};
    ct.texture = target;
    ct.clear_color = {fp.clearColor.x, fp.clearColor.y, fp.clearColor.z, 1};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = depth_;
    dt.clear_depth = 1;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    pass_ = SDL_BeginGPURenderPass(cmd_, &ct, 1, &dt);
    FSUniforms fu{{fp.fogColor.x, fp.fogColor.y, fp.fogColor.z, fp.fogIntensity},
                  {fp.fogStart, fp.fogEnd, 0.5f, 1.f}};
    SDL_PushGPUFragmentUniformData(cmd_, 0, &fu, sizeof(fu));
    return true;
}

void Renderer::draw(const GpuMesh* mesh, const Mat4& model, bool lit, float alpha, bool fog, bool background, int textureOverride) {
    if (!mesh || !pass_) return;
    VSUniforms u{};
    Mat4 vp = fp_.view * fp_.proj;
    memcpy(u.viewProj, vp.data(), 64);
    memcpy(u.model, model.data(), 64);
    Vec3 ld = normalize(fp_.lightDir);
    u.lightDir[0] = ld.x, u.lightDir[1] = ld.y, u.lightDir[2] = ld.z;
    u.lightColor[0] = fp_.lightColor.x, u.lightColor[1] = fp_.lightColor.y, u.lightColor[2] = fp_.lightColor.z;
    u.ambient[0] = fp_.ambient.x, u.ambient[1] = fp_.ambient.y, u.ambient[2] = fp_.ambient.z;
    u.ambient[3] = lit ? 1.f : 0.f;
    u.eye[0] = fp_.eye.x, u.eye[1] = fp_.eye.y, u.eye[2] = fp_.eye.z;
    SDL_PushGPUVertexUniformData(cmd_, 0, &u, sizeof(u));
    SDL_GPUBufferBinding vb{mesh->vb, 0};
    for (int pass = 0; pass < 2; pass++) {
        SDL_BindGPUGraphicsPipeline(pass_, background ? background_ : pass ? blended_ : opaque_);
        SDL_BindGPUVertexBuffers(pass_, 0, &vb, 1);
        FSUniforms fu{{fp_.fogColor.x, fp_.fogColor.y, fp_.fogColor.z, fog ? fp_.fogIntensity : 0.f},
                      {fp_.fogStart, fp_.fogEnd, 0.f, alpha}};
        for (auto& b : mesh->batches) {
            bool isBlend = !background && (b.blend || alpha < 1.f || textureOverride >= 0);
            if (isBlend != (pass == 1) || !b.count) continue;
            fu.fogRange[2] = b.alphaTest ? 0.5f : 0.f;
            SDL_PushGPUFragmentUniformData(cmd_, 0, &fu, sizeof(fu));
            int t = textureOverride >= 0 ? textureOverride : (b.texture >= 0 && b.texture < int(textures_.size())) ? b.texture : white_;
            if (t >= int(textures_.size())) t = white_;
            SDL_GPUTextureSamplerBinding tsb{textures_[size_t(t)], sampler_};
            SDL_BindGPUFragmentSamplers(pass_, 0, &tsb, 1);
            SDL_DrawGPUPrimitives(pass_, b.count, 1, b.first, 0);
        }
    }
}

void Renderer::drawDynamic() {
    if (!pass_ || !dynamicCount_) return;
    VSUniforms u{};
    Mat4 vp = fp_.view * fp_.proj, id = Mat4::identity();
    memcpy(u.viewProj, vp.data(), 64);
    memcpy(u.model, id.data(), 64);
    u.eye[0] = fp_.eye.x, u.eye[1] = fp_.eye.y, u.eye[2] = fp_.eye.z;
    SDL_PushGPUVertexUniformData(cmd_, 0, &u, sizeof(u));
    SDL_GPUBufferBinding vb{dynamicVB_, 0};
    FSUniforms fu{{fp_.fogColor.x, fp_.fogColor.y, fp_.fogColor.z, fp_.fogIntensity}, {fp_.fogStart, fp_.fogEnd, 0.f, 1.f}};
    for (auto& b : dynamicBatches_) {
        if (!b.count || b.first + b.count > dynamicCount_) continue;
        SDL_BindGPUGraphicsPipeline(pass_, b.additive ? additive_ : blended_);
        SDL_BindGPUVertexBuffers(pass_, 0, &vb, 1);
        SDL_PushGPUFragmentUniformData(cmd_, 0, &fu, sizeof(fu));
        int t = (b.texture >= 0 && b.texture < int(textures_.size())) ? b.texture : white_;
        SDL_GPUTextureSamplerBinding tsb{textures_[size_t(t)], sampler_};
        SDL_BindGPUFragmentSamplers(pass_, 0, &tsb, 1);
        SDL_DrawGPUPrimitives(pass_, b.count, 1, b.first, 0);
    }
}

void Renderer::endFrame() {
    if (pass_ && overlayCount_) {
        VSUniforms u{};
        Mat4 o = Mat4::identity();
        o.m[0][0] = 2.f / float(w_), o.m[1][1] = -2.f / float(h_), o.m[2][2] = 0;
        o.m[3][0] = -1, o.m[3][1] = 1, o.m[3][2] = 0.5f;
        Mat4 id = Mat4::identity();
        memcpy(u.viewProj, o.data(), 64);
        memcpy(u.model, id.data(), 64);
        SDL_PushGPUVertexUniformData(cmd_, 0, &u, sizeof(u));
        FSUniforms fu{{0, 0, 0, 0}, {0, 1, 0.f, 1.f}};
        SDL_PushGPUFragmentUniformData(cmd_, 0, &fu, sizeof(fu));
        SDL_BindGPUGraphicsPipeline(pass_, overlayPipe_);
        SDL_GPUBufferBinding vb{overlayVB_, 0};
        SDL_BindGPUVertexBuffers(pass_, 0, &vb, 1);
        if (overlayBatches_.empty()) {
            SDL_GPUTextureSamplerBinding tsb{textures_[size_t(font_)], sampler_};
            SDL_BindGPUFragmentSamplers(pass_, 0, &tsb, 1);
            SDL_DrawGPUPrimitives(pass_, overlayCount_, 1, 0, 0);
        }
        for (auto& b : overlayBatches_) {
            if (!b.count || b.first + b.count > overlayCount_) continue;
            int t = b.texture < 0 ? font_ : b.texture;
            if (t >= int(textures_.size()) || !textures_[size_t(t)]) t = white_;
            SDL_GPUTextureSamplerBinding tsb{textures_[size_t(t)], sampler_};
            SDL_BindGPUFragmentSamplers(pass_, 0, &tsb, 1);
            SDL_DrawGPUPrimitives(pass_, b.count, 1, b.first, 0);
        }
    }
    if (pass_) SDL_EndGPURenderPass(pass_);
    pass_ = nullptr;
    if (cmd_) SDL_SubmitGPUCommandBuffer(cmd_);
    cmd_ = nullptr;
}

bool Renderer::readback(std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!offscreen_) return false;
    Uint32 bytes = Uint32(w_ * h_ * 4);
    SDL_GPUTransferBufferCreateInfo tbi{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &tbi);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = offscreen_;
    src.w = Uint32(w_);
    src.h = Uint32(h_);
    src.d = 1;
    SDL_GPUTextureTransferInfo dst{tb, 0, Uint32(w_), Uint32(h_)};
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(dev_, true, &f, 1);
    SDL_ReleaseGPUFence(dev_, f);
    const uint8_t* p = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(dev_, tb, false));
    rgba.assign(p, p + bytes);
    SDL_UnmapGPUTransferBuffer(dev_, tb);
    SDL_ReleaseGPUTransferBuffer(dev_, tb);
    w = w_;
    h = h_;
    return true;
}

}  // namespace hg2
