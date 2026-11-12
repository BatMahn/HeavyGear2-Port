// SDL_GPU renderer (Vulkan / Metal / Direct3D 12 via SDL3).
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/assets.h"
#include "../core/math.h"

namespace hg2 {

struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
    uint8_t color[4];
};

struct DrawBatch {
    int texture = -1;     // renderer texture id
    uint32_t first = 0;   // first vertex in the mesh buffer
    uint32_t count = 0;
    bool blend = false;
    bool alphaTest = false;
    bool additive = false;  // dynamic batches: additive blending (fire, sparks)
};

// A static mesh uploaded to the GPU (triangle list) with per-texture batches.
struct GpuMesh {
    SDL_GPUBuffer* vb = nullptr;
    std::vector<DrawBatch> batches;
    Vec3 bmin, bmax;
};

struct FrameParams {
    Mat4 view, proj;
    Vec3 eye;
    Vec3 lightDir{0, -1, 0};
    Vec3 lightColor{0.7f, 0.7f, 0.7f};
    Vec3 ambient{0.3f, 0.3f, 0.3f};
    Vec3 fogColor{0.5f, 0.5f, 0.5f};
    float fogStart = 100, fogEnd = 300, fogIntensity = 1;
    Vec3 clearColor{0.2f, 0.2f, 0.25f};
};

class Renderer {
public:
    bool init(SDL_Window* window, bool offscreen, int w, int h, std::string* err);
    void shutdown();

    int createTexture(const Image& img, bool mipmaps = true);
    // Replaces the pixels of a texture made by createTexture(img, false) with an image of the same size (movies).
    void updateTexture(int id, const Image& img);
    int whiteTexture() const { return white_; }
    GpuMesh* createMesh(const std::vector<Vertex>& verts, std::vector<DrawBatch> batches);

    bool beginFrame(const FrameParams& fp);  // false if no swapchain image this frame
    void draw(const GpuMesh* mesh, const Mat4& model, bool lit = true, float alpha = 1.f, bool fog = true,
              bool background = false, int textureOverride = -1);  // background: no depth test/write (sky);
                                                                 // textureOverride: every batch drawn blended with it
    void endFrame();
    // Anti-aliasing: 1 (off), 2, 4 or 8 samples; applied from the next frame, clamped to what the GPU supports.
    void setSamples(int n) { wantSamples_ = n < 2 ? 1 : n; }
    int samples() const { return samples_; }
    // 2D overlay (HUD) in pixel coordinates, drawn last. Call before beginFrame each frame.
    void setOverlay(const std::vector<Vertex>& verts) { overlay_ = verts, overlayBatches_.clear(); }
    // Textured 2D overlay (shell screens): batches index into `verts`; texture -1 = the HUD font.
    void setOverlay(const std::vector<Vertex>& verts, const std::vector<DrawBatch>& batches) {
        overlay_ = verts, overlayBatches_ = batches;
    }
    // GPU resources created after a mark can be released together (a mission's textures and meshes).
    struct ResourceMark { size_t textures = 0, meshes = 0; };
    ResourceMark mark() const { return {textures_.size(), meshes_.size()}; }
    void releaseTo(const ResourceMark& m);
    int textureCount() const { return int(textures_.size()); }
    int fontTexture() const { return font_; }
    // Per-frame world-space geometry (particles), uploaded in beginFrame and drawn by drawDynamic()
    // (blended, depth-tested, no depth writes). Call before beginFrame each frame.
    void setDynamic(const std::vector<Vertex>& verts, const std::vector<DrawBatch>& batches) {
        dynamic_ = verts, dynamicBatches_ = batches;
    }
    void drawDynamic();
    bool readback(std::vector<uint8_t>& rgba, int& w, int& h);  // offscreen only, after endFrame

    SDL_GPUDevice* device() const { return dev_; }
    const char* driverName() const;
    int width() const { return w_; }
    int height() const { return h_; }

private:
    SDL_GPUShader* makeShader(bool vertex);
    SDL_GPUGraphicsPipeline* makePipeline(bool blend, bool background = false, bool additive = false);
    void buildPipelines();
    void releasePipelines();
    void ensureDepth(int w, int h);

    SDL_Window* win_ = nullptr;
    SDL_GPUDevice* dev_ = nullptr;
    SDL_GPUGraphicsPipeline* opaque_ = nullptr;
    SDL_GPUGraphicsPipeline* blended_ = nullptr;
    SDL_GPUGraphicsPipeline* background_ = nullptr;
    SDL_GPUGraphicsPipeline* overlayPipe_ = nullptr;
    SDL_GPUGraphicsPipeline* additive_ = nullptr;
    SDL_GPUBuffer* dynamicVB_ = nullptr;
    SDL_GPUTransferBuffer* dynamicTB_ = nullptr;
    std::vector<Vertex> dynamic_;
    std::vector<DrawBatch> dynamicBatches_;
    uint32_t dynamicCount_ = 0;
    SDL_GPUBuffer* overlayVB_ = nullptr;
    SDL_GPUTransferBuffer* overlayTB_ = nullptr;
    std::vector<Vertex> overlay_;
    std::vector<DrawBatch> overlayBatches_;
    uint32_t overlayCount_ = 0;
    int font_ = -1;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    SDL_GPUTexture* msaaColor_ = nullptr;  // multisampled scene target, resolved into the swapchain
    int samples_ = 1, wantSamples_ = 1, msaaW_ = 0, msaaH_ = 0;
    SDL_GPUTexture* offscreen_ = nullptr;
    SDL_GPUTextureFormat colorFmt_ = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    int depthW_ = 0, depthH_ = 0, w_ = 0, h_ = 0;
    bool offscreenMode_ = false;
    std::vector<SDL_GPUTexture*> textures_;
    std::vector<GpuMesh*> meshes_;
    int white_ = -1;

    SDL_GPUCommandBuffer* cmd_ = nullptr;
    SDL_GPURenderPass* pass_ = nullptr;
    FrameParams fp_;
};

}  // namespace hg2
