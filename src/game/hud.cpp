#include "hud.h"

#include "../render/hudfont.h"

namespace hg2 {

void Hud::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
    auto mk = [&](float x, float y, float u, float v) {
        Vertex o{};
        o.pos[0] = x, o.pos[1] = y, o.pos[2] = 0;
        o.normal[1] = 1;
        o.uv[0] = u, o.uv[1] = v;
        o.color[0] = uint8_t(c >> 24), o.color[1] = uint8_t(c >> 16), o.color[2] = uint8_t(c >> 8), o.color[3] = uint8_t(c);
        return o;
    };
    Vertex a = mk(x0, y0, u0, v0), b = mk(x1, y0, u1, v0), cc = mk(x1, y1, u1, v1), d = mk(x0, y1, u0, v1);
    v_.insert(v_.end(), {a, b, cc, a, cc, d});
}

void Hud::rect(float x, float y, float w, float h, uint32_t c) {
    // solid cell (index 95) of the font atlas
    float u = ((95 % kFontCols) * kFontCellW + kFontCellW * 0.5f) / float(kFontW);
    float v = ((95 / kFontCols) * kFontCellH + kFontCellH * 0.5f) / float(kFontH);
    quad(x, y, x + w, y + h, u, v, u, v, c);
}

void Hud::frame(float x, float y, float w, float h, uint32_t c, float t) {
    rect(x, y, w, t, c), rect(x, y + h - t, w, t, c), rect(x, y, t, h, c), rect(x + w - t, y, t, h, c);
}

float Hud::textWidth(const std::string& s, float scale) const { return float(s.size()) * kFontCellW * scale; }

void Hud::text(float x, float y, const std::string& s, uint32_t c, float scale) {
    float cx = x;
    for (char ch : s) {
        int i = (unsigned char)ch;
        if (i == '\n') {
            cx = x, y += kFontCellH * scale;
            continue;
        }
        if (i < 32 || i > 126) i = '?';
        i -= 32;
        if (i) {
            float u0 = float((i % kFontCols) * kFontCellW) / kFontW, v0 = float((i / kFontCols) * kFontCellH) / kFontH;
            float u1 = u0 + float(kFontCellW) / kFontW, v1 = v0 + float(kFontCellH) / kFontH;
            // 1-pixel shadow keeps text readable over bright terrain
            quad(cx + scale, y + scale, cx + kFontCellW * scale + scale, y + kFontCellH * scale + scale, u0, v0, u1, v1,
                 0x000000c0u & (0xffffff00u | (c & 0xff)));
            quad(cx, y, cx + kFontCellW * scale, y + kFontCellH * scale, u0, v0, u1, v1, c);
        }
        cx += kFontCellW * scale;
    }
}

}  // namespace hg2
