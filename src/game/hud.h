// Heads-up display drawn through the renderer overlay (pixel coordinates).
#pragma once
#include <string>
#include <vector>

#include "../render/renderer.h"

namespace hg2 {

class Hud {
public:
    void begin(int w, int h) { v_.clear(), w_ = w, h_ = h; }
    void rect(float x, float y, float w, float h, uint32_t rgba);
    void text(float x, float y, const std::string& s, uint32_t rgba, float scale = 1.f);
    float textWidth(const std::string& s, float scale = 1.f) const;
    void frame(float x, float y, float w, float h, uint32_t rgba, float t = 1.f);
    const std::vector<Vertex>& vertices() const { return v_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba);
    std::vector<Vertex> v_;
    int w_ = 0, h_ = 0;
};

}  // namespace hg2
