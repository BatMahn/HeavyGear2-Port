#include "ui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../audio/audio.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb_truetype.h"

namespace hg2 {

namespace {
uint32_t rgbaOf(int r, int g, int b, int a = 255) {
    return uint32_t(std::clamp(r, 0, 255)) << 24 | uint32_t(std::clamp(g, 0, 255)) << 16 | uint32_t(std::clamp(b, 0, 255)) << 8 |
           uint32_t(std::clamp(a, 0, 255));
}
bool isColorNode(const std::string& n) {
    std::string l = lower(n);
    return l.rfind("color", 0) == 0 || l.rfind("activecolor", 0) == 0 || l.rfind("disabledcolor", 0) == 0 || l == "rangery" ||
           l == "rangerx";
}
}  // namespace

bool Ui::init(Renderer& r, const FileSystem& fs, Audio* audio) {
    r_ = &r, fs_ = &fs, audio_ = audio;
    if (!art_.open(fs)) SDL_Log("shell: shell art (rim565.zfs / zfs.zip) not found");
    else SDL_Log("shell: art from %s", art_.source().c_str());
    if (!ldl_.load(fs, "HG2SHELL.LDL")) SDL_Log("shell: HG2SHELL.LDL not found");
    return ok();
}

void Ui::begin(int w, int h) {
    W_ = w, H_ = h;
    s_ = std::min(float(w) / 640.f, float(h) / 480.f);
    ox_ = std::floor((float(w) - 640.f * s_) * 0.5f);
    oy_ = std::floor((float(h) - 480.f * s_) * 0.5f);
    verts_.clear();
    batches_.clear();
    clicked_.clear();
    hot_.clear();
}

void Ui::end() {
    if (r_) r_->setOverlay(verts_, batches_);
    if (released_) pressedOn_.clear();
    pressed_ = released_ = false;
    keys_.clear();
    typed_.clear();
    wheel_ = 0;
    lastHot_ = hot_;
    caretBlink_++;
}

void Ui::forgetTextures(int firstId) {
    for (auto it = textures_.begin(); it != textures_.end();)
        it = it->second.id >= firstId ? textures_.erase(it) : std::next(it);
    for (auto it = fonts_.begin(); it != fonts_.end();)
        it = it->second->tex >= firstId ? fonts_.erase(it) : std::next(it);
}

bool Ui::clicked(const std::string& path) const {
    if (clicked_.empty()) return false;
    std::string p = lower(path);
    return clicked_ == p || (clicked_.size() > p.size() && clicked_.compare(clicked_.size() - p.size(), p.size(), p) == 0 &&
                             clicked_[clicked_.size() - p.size() - 1] == '.');
}

bool Ui::keyPressed(int k) const { return std::find(keys_.begin(), keys_.end(), k) != keys_.end(); }

void Ui::playSound(const std::string& wav, float vol) {
    if (audio_ && !wav.empty()) audio_->play(wav, vol * shellVolume);
}

// ---------------------------------------------------------------- drawing primitives
void Ui::quad(int tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
    if (verts_.size() + 6 > 65536) return;
    if (batches_.empty() || batches_.back().texture != tex) {
        DrawBatch b;
        b.texture = tex;
        b.first = uint32_t(verts_.size());
        b.blend = true;
        batches_.push_back(b);
    }
    auto v = [&](float x, float y, float u, float w) {
        Vertex o{};
        o.pos[0] = x, o.pos[1] = y;
        o.uv[0] = u, o.uv[1] = w;
        o.color[0] = uint8_t(c >> 24), o.color[1] = uint8_t(c >> 16), o.color[2] = uint8_t(c >> 8), o.color[3] = uint8_t(c);
        verts_.push_back(o);
    };
    v(x0, y0, u0, v0), v(x1, y0, u1, v0), v(x1, y1, u1, v1);
    v(x0, y0, u0, v0), v(x1, y1, u1, v1), v(x0, y1, u0, v1);
    batches_.back().count += 6;
}

int Ui::texture(const std::string& name, int* w, int* h) {
    std::string k = lower(basename(name));
    auto it = textures_.find(k);
    if (it == textures_.end()) {
        Tex t;
        Image img;
        bool ok = art_.image(k, img);
        if (!ok) {
            Bytes b;
            ok = fs_->read(k, b) && decodeImage(b, k, img);
        }
        if (ok && r_) t.id = r_->createTexture(img, false), t.w = img.w, t.h = img.h;
        it = textures_.emplace(k, t).first;
    }
    if (w) *w = it->second.w;
    if (h) *h = it->second.h;
    return it->second.id;
}

bool Ui::imageSize(const std::string& name, int& w, int& h) { return texture(name, &w, &h) >= 0; }

bool Ui::image(const std::string& name, float x, float y, float w, float h, uint32_t tint, const std::string& mode) {
    int tw, th;
    int t = texture(name, &tw, &th);
    if (t < 0 || tw <= 0) return false;
    auto X = [&](float v) { return ox_ + v * s_; };
    auto Y = [&](float v) { return oy_ + v * s_; };
    std::string m = lower(mode);
    if (m == "tile" && w > 0 && h > 0) {
        for (float ty = 0; ty < h; ty += float(th))
            for (float tx = 0; tx < w; tx += float(tw)) {
                float cw = std::min(float(tw), w - tx), ch = std::min(float(th), h - ty);
                quad(t, X(x + tx), Y(y + ty), X(x + tx + cw), Y(y + ty + ch), 0, 0, cw / float(tw), ch / float(th), tint);
            }
        return true;
    }
    if (m == "stretch" && w > 0 && h > 0) {
        quad(t, X(x), Y(y), X(x + w), Y(y + h), 0, 0, 1, 1, tint);
        return true;
    }
    // natural size, clipped to the box when one is given
    float cw = float(tw), ch = float(th);
    if (w > 0) cw = std::min(cw, w);
    if (h > 0) ch = std::min(ch, h);
    quad(t, X(x), Y(y), X(x + cw), Y(y + ch), 0, 0, cw / float(tw), ch / float(th), tint);
    return true;
}

void Ui::rect(float x, float y, float w, float h, uint32_t c) {
    quad(r_ ? r_->whiteTexture() : -1, ox_ + x * s_, oy_ + y * s_, ox_ + (x + w) * s_, oy_ + (y + h) * s_, 0, 0, 1, 1, c);
}

void Ui::frame(float x, float y, float w, float h, uint32_t c) {
    float t = 1.f;
    rect(x, y, w, t, c), rect(x, y + h - t, w, t, c), rect(x, y, t, h, c), rect(x + w - t, y, t, h, c);
}

void Ui::cursor() {
    if (mx_ < 0 || my_ < 0) return;
    if (!image("Scursor1.TGA", mx_ - 1, my_ - 1)) {
        rect(mx_, my_, 2, 10, 0xffffffffu), rect(mx_, my_, 8, 2, 0xffffffffu);
    }
}

// ---------------------------------------------------------------- fonts
Ui::Font* Ui::font(const std::string& file, int size) {
    int q = std::max(1, int(std::lround(s_ * 4)));
    std::string key = lower(file) + "/" + std::to_string(size) + "/" + std::to_string(q);
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second.get();
    auto& data = fontFiles_[lower(file)];
    if (data.empty()) {
        Bytes b;
        if (!fs_->read(file, b) && !fs_->read("eum_____.ttf", b)) return nullptr;
        data = b;
    }
    auto f = std::make_unique<Font>();
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0))) return nullptr;
    // point size at 96 dpi, in window pixels
    f->px = float(size) * 96.f / 72.f * float(q) / 4.f;
    float sc = stbtt_ScaleForMappingEmToPixels(&info, f->px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    f->ascent = float(asc) * sc;
    f->lineGap = float(asc - desc + gap) * sc;
    stbtt_packedchar pc[224];
    std::vector<uint8_t> bmp;
    int dim = 256;
    for (;; dim *= 2) {
        bmp.assign(size_t(dim) * dim, 0);
        stbtt_pack_context ctx;
        if (!stbtt_PackBegin(&ctx, bmp.data(), dim, dim, 0, 1, nullptr)) return nullptr;
        stbtt_PackSetOversampling(&ctx, 1, 1);
        int ok = stbtt_PackFontRange(&ctx, data.data(), 0, STBTT_POINT_SIZE(f->px), 32, 224, pc);
        stbtt_PackEnd(&ctx);
        if (ok || dim >= 4096) break;
    }
    Image img;
    img.w = img.h = dim;
    img.rgba.assign(size_t(dim) * dim * 4, 255);
    for (size_t i = 0; i < bmp.size(); i++) img.rgba[i * 4 + 3] = bmp[i];
    img.hasAlpha = true;
    f->tex = r_ ? r_->createTexture(img, false) : -1;
    for (int i = 0; i < 224; i++) {  // packed box in atlas pixels (uv = box / dim)
        auto& g = f->g[i];
        g.x0 = pc[i].x0, g.y0 = pc[i].y0, g.x1 = pc[i].x1, g.y1 = pc[i].y1;
        g.xoff = pc[i].xoff, g.yoff = pc[i].yoff, g.adv = pc[i].xadvance;
    }
    f->lineGap = std::ceil(f->lineGap);
    f->ascent = std::round(f->ascent);
    Font* raw = f.get();
    fonts_[key] = std::move(f);
    atlasDim_[raw] = dim;
    return raw;
}

float Ui::lineHeight(const TextStyle& st) {
    Font* f = font(st.font, st.size);
    return f ? f->lineGap / s_ : float(st.size) * 1.4f;
}

float Ui::textWidth(const std::string& s, const TextStyle& st) {
    Font* f = font(st.font, st.size);
    if (!f) return 0;
    float w = 0;
    for (unsigned char c : s) {
        if (c == '\t') c = ' ';
        if (c >= 32) w += f->g[c - 32].adv;
    }
    return w / s_;
}

float Ui::textBox(const std::string& text, const UiRect& r, const TextStyle& st, uint32_t color, int firstLine, int* totalLines) {
    Font* f = font(st.font, st.size);
    if (!f) return 0;
    int dim = atlasDim_[f];
    float maxW = (r.w - 2 * float(st.insetX)) * s_;
    // break into lines
    std::vector<std::string> lines;
    std::string cur;
    float curW = 0;
    auto flush = [&]() {
        lines.push_back(cur);
        cur.clear(), curW = 0;
    };
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = text[i];
        if (c == '\r') {
            i++;
            continue;
        }
        if (c == '\n') {
            flush();
            i++;
            continue;
        }
        // next word (with its leading spaces)
        size_t j = i;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) j++;
        while (j < text.size() && text[j] != ' ' && text[j] != '\t' && text[j] != '\n' && text[j] != '\r') j++;
        std::string word = text.substr(i, j - i);
        for (auto& ch : word)
            if (ch == '\t') ch = ' ';
        float ww = 0;
        for (unsigned char ch : word) ww += ch >= 32 ? f->g[ch - 32].adv : 0;
        if (st.wrap && curW + ww > maxW && !cur.empty()) {
            flush();
            size_t k = 0;
            while (k < word.size() && word[k] == ' ') k++;
            word = word.substr(k);
            ww = 0;
            for (unsigned char ch : word) ww += ch >= 32 ? f->g[ch - 32].adv : 0;
        }
        cur += word, curW += ww;
        i = j;
    }
    if (!cur.empty() || lines.empty()) flush();
    if (totalLines) *totalLines = int(lines.size());
    float lh = f->lineGap;
    float top = oy_ + (r.y + float(st.insetY)) * s_;
    float boxH = (r.h - 2 * float(st.insetY)) * s_;
    int shown = std::max(0, int(lines.size()) - firstLine);
    float blockH = float(shown) * lh;
    if (st.vcenter && blockH <= boxH + 0.5f) top += std::floor((boxH - blockH) * 0.5f);
    float y = std::floor(top);
    float bottom = oy_ + (r.y + r.h) * s_ + 0.5f;
    int drawn = 0;
    for (size_t l = size_t(std::max(0, firstLine)); l < lines.size(); l++) {
        if (y + lh > bottom + lh * 0.25f && drawn > 0) break;
        const std::string& ln = lines[l];
        float lw = 0;
        for (unsigned char ch : ln) lw += ch >= 32 ? f->g[ch - 32].adv : 0;
        float x = ox_ + (r.x + float(st.insetX)) * s_;
        if (st.just == "center") x += (maxW - lw) * 0.5f;
        else if (st.just == "right") x += maxW - lw;
        x = std::floor(x);
        float base = y + f->ascent;
        for (unsigned char ch : ln) {
            if (ch < 32) continue;
            const auto& g = f->g[ch - 32];
            float gw = g.x1 - g.x0, gh = g.y1 - g.y0;
            if (gw > 0 && gh > 0) {
                float qx = std::floor(x + g.xoff), qy = std::floor(base + g.yoff);
                quad(f->tex, qx, qy, qx + gw, qy + gh, g.x0 / dim, g.y0 / dim, g.x1 / dim, g.y1 / dim, color);
            }
            x += g.adv;
        }
        y += lh;
        drawn++;
    }
    return float(drawn) * lh / s_;
}

TextStyle Ui::style(const LdlNode* n) {
    TextStyle st;
    if (!n) return st;
    st.font = ldl_.getStr(n, "fontname", st.font);
    st.size = ldl_.getInt(n, "fontsize", st.size);
    st.just = lower(ldl_.getStr(n, "just", st.just));
    st.vcenter = ldl_.getBool(n, "vertcenter", false);
    st.wrap = ldl_.getBool(n, "wordwrap", false);
    st.insetX = ldl_.getInt(n, "deflatetextrectx", 0);
    st.insetY = ldl_.getInt(n, "deflatetextrecty", 0);
    uint32_t base = rgbaOf(ldl_.getInt(n, "fontcolorred", 76), ldl_.getInt(n, "fontcolorgreen", 142), ldl_.getInt(n, "fontcolorblue", 248));
    st.color = nodeColor(n, "color0", base);
    st.hover = nodeColor(n, "activecolor0", st.color);
    st.down = nodeColor(n, "color1", st.hover);
    st.disabled = nodeColor(n, "disabledcolor0", 0x0d2b63ffu);
    return st;
}

uint32_t Ui::nodeColor(const LdlNode* n, const char* which, uint32_t def) {
    const LdlNode* c = ldl_.childOf(n, which);
    if (!c && std::string(which) == "activecolor0") c = ldl_.childOf(n, "activecolor 0");
    if (!c) return def;
    return rgbaOf(ldl_.getInt(c, "fontcolorred", int(def >> 24)), ldl_.getInt(c, "fontcolorgreen", int((def >> 16) & 255)),
                  ldl_.getInt(c, "fontcolorblue", int((def >> 8) & 255)));
}

// ---------------------------------------------------------------- layout nodes
UiRect Ui::place(const LdlNode* n, const UiRect& p) {
    UiRect r;
    int iw = 0, ih = 0;
    std::string img = ldl_.getStr(n, "image0");
    auto dim = [&](const char* pix, const char* pct, float parent, int natural) {
        if (ldl_.has(n, pix)) return float(ldl_.getInt(n, pix));
        if (ldl_.has(n, pct)) return parent * float(ldl_.getInt(n, pct)) / 100.f;
        if (natural > 0) return float(natural);
        return -1.f;
    };
    if (!img.empty()) texture(img, &iw, &ih);
    r.w = dim("widthpix", "widthpct", p.w, iw);
    r.h = dim("heightpix", "heightpct", p.h, ih);
    float xo = ldl_.has(n, "xpix") ? float(ldl_.getInt(n, "xpix")) : p.w * float(ldl_.getInt(n, "xpct", 0)) / 100.f;
    float yo = ldl_.has(n, "ypix") ? float(ldl_.getInt(n, "ypix")) : p.h * float(ldl_.getInt(n, "ypct", 0)) / 100.f;
    if (r.w < 0) r.w = std::max(0.f, p.w - xo);
    if (r.h < 0) r.h = std::max(0.f, p.h - yo);
    std::string xa = lower(ldl_.getStr(n, "xanchor")), ya = lower(ldl_.getStr(n, "yanchor"));
    if (xa == "center") r.x = p.x + (p.w - r.w) * 0.5f + xo;
    else if (xa == "right") r.x = p.x + p.w - r.w - xo;
    else r.x = p.x + xo;
    if (ya == "center") r.y = p.y + (p.h - r.h) * 0.5f + yo;
    else if (ya == "bottom") r.y = p.y + p.h - r.h - yo;
    else r.y = p.y + yo;
    return r;
}

bool Ui::isButton(const LdlNode* n) const {
    return ldl_.has(n, "activeimage0") || ldl_.has(n, "image1") || ldl_.has(n, "executesound");
}

UiRect Ui::rectOf(const std::string& path) {
    std::string p = lower(path);
    UiRect r{0, 0, 640, 480};
    const LdlNode* n = nullptr;
    size_t a = 0;
    while (a <= p.size()) {
        size_t e = p.find('.', a);
        std::string part = p.substr(a, e == std::string::npos ? std::string::npos : e - a);
        n = n ? ldl_.childOf(n, part) : ldl_.find(part);
        if (!n) return r;
        r = place(n, r);
        if (e == std::string::npos) break;
        a = e + 1;
    }
    return r;
}

UiRect Ui::window(const std::string& path) {
    std::string p = lower(path);
    size_t dot = p.rfind('.');
    UiRect parent{0, 0, 640, 480};
    if (dot != std::string::npos) parent = rectOf(p.substr(0, dot));
    return window(p, parent);
}

UiRect Ui::window(const std::string& path, const UiRect& parent) {
    const LdlNode* n = ldl_.find(path);
    if (!n) return parent;
    UiRect r = place(n, parent);
    drawNode(n, lower(path), r, 0);
    return r;
}

void Ui::drawNode(const LdlNode* n, const std::string& path, const UiRect& r, int depth) {
    if (depth > 12) return;
    auto hit = hidden.find(path);
    if (hit != hidden.end() && hit->second) return;
    bool button = isButton(n);
    bool dis = !ldl_.getBool(n, "enabled", true);
    if (auto it = disabled.find(path); it != disabled.end()) dis = it->second;
    bool hover = !dis && r.contains(mx_, my_);
    bool sel = false;
    if (auto it = selected.find(path); it != selected.end()) sel = it->second;
    bool held = button && down_ && pressedOn_ == path;
    if (button && hover) {
        hot_ = path;
        if (lastHot_ != path) playSound(ldl_.getStr(n, "activatesound"), 0.6f);
        if (pressed_) pressedOn_ = path;
        if (released_ && pressedOn_ == path) {
            clicked_ = path;
            playSound(ldl_.getStr(n, "executesound"));
        }
    }
    std::string img;
    if (dis) img = ldl_.getStr(n, "disabledimage0", ldl_.getStr(n, "image0"));
    else if ((held && hover) || sel) img = ldl_.getStr(n, "image1", ldl_.getStr(n, "activeimage0", ldl_.getStr(n, "image0")));
    else if (hover && button) img = ldl_.getStr(n, "activeimage0", ldl_.getStr(n, "image0"));
    else img = ldl_.getStr(n, "image0");
    if (!img.empty()) image(img, r.x, r.y, r.w, r.h, 0xffffffffu, ldl_.getStr(n, "imageblttype"));
    std::string txt;
    if (auto it = text.find(path); it != text.end()) txt = it->second;
    else txt = ldl_.getStr(n, "text");
    if (!txt.empty()) {
        TextStyle st = style(n);
        uint32_t c = dis ? st.disabled : (held && hover) || sel ? st.down : hover && button ? st.hover : st.color;
        textBox(txt, r, st, c);
    }
    for (const LdlNode* c : ldl_.children(n)) {
        if (isColorNode(c->name)) continue;
        drawNode(c, path + "." + lower(c->name), place(c, r), depth + 1);
    }
}

// ---------------------------------------------------------------- widgets
int Ui::listbox(const std::string& lbPath, const std::vector<std::string>& items, int sel, int& scroll, const std::string& itemTemplate) {
    const LdlNode* lb = ldl_.find(lbPath);
    if (!lb) return -1;
    UiRect r = rectOf(lbPath);
    const LdlNode* it = ldl_.find(itemTemplate);
    float ih = float(ldl_.getInt(lb, "itemheight", 15));
    float ix = float(ldl_.getInt(lb, "itemoffsetx", 4)), iy = float(ldl_.getInt(lb, "itemoffsety", 8));
    float hy = ldl_.childOf(lb, "header") ? 20.f : 0.f;
    float iw = it && ldl_.has(it, "widthpix") ? float(ldl_.getInt(it, "widthpix")) : r.w - ix - 14;
    iw = std::min(iw, r.w - ix - 14);
    UiRect area{r.x + ix, r.y + iy + hy, iw, r.h - iy - hy - 6};
    int visible = std::max(1, int(area.h / ih));
    int maxScroll = std::max(0, int(items.size()) - visible);
    if (area.contains(mx_, my_) && wheel_ != 0) scroll -= int(wheel_ > 0 ? 1 : -1);
    scroll = std::clamp(scroll, 0, maxScroll);
    TextStyle st = style(it);
    int result = -1;
    std::string hl = it ? ldl_.getStr(it, "activeimage0") : std::string();
    for (int i = 0; i < visible && scroll + i < int(items.size()); i++) {
        int idx = scroll + i;
        UiRect ir{area.x, area.y + float(i) * ih, area.w, ih};
        bool hov = ir.contains(mx_, my_);
        if (idx == sel || hov) {
            if (!hl.empty()) image(hl, ir.x, ir.y, ir.w, ir.h, idx == sel ? 0xffffffffu : 0xffffff90u, "stretch");
            else rect(ir.x, ir.y, ir.w, ir.h, idx == sel ? 0x2050a0a0u : 0x20509060u);
        }
        textBox(items[size_t(idx)], ir, st, idx == sel ? st.down : hov ? st.hover : st.color);
        if (hov && released_ && pressedOn_ == "#lb" + lbPath) {
            result = idx;
            playSound(ldl_.getStr(lb, "executesound", "general.wav"));
        }
        if (hov && pressed_) pressedOn_ = "#lb" + lbPath;
    }
    if (maxScroll > 0) {  // scroll bar: arrows and a thumb along the right edge
        float sx = r.x + r.w - 14, top = area.y, bot = area.y + area.h;
        image("SSBR001X.TGA", sx, top + 12, 12, std::max(0.f, bot - top - 24), 0xffffffffu, "tile");
        UiRect up{sx, top, 12, 12}, dn{sx, bot - 12, 12, 12};
        image(up.contains(mx_, my_) ? "SSAR001M.TGA" : "SSAR001U.TGA", up.x, up.y);
        image(dn.contains(mx_, my_) ? "SSAR002M.TGA" : "SSAR002U.TGA", dn.x, dn.y);
        float t = float(scroll) / float(maxScroll);
        image("SSBT001U.TGA", sx, top + 12 + t * std::max(0.f, bot - top - 35), 12, 11);
        if (pressed_ && up.contains(mx_, my_)) scroll = std::max(0, scroll - 1), playSound("scrollbar.wav");
        if (pressed_ && dn.contains(mx_, my_)) scroll = std::min(maxScroll, scroll + 1), playSound("scrollbar.wav");
    }
    return result;
}

void Ui::scrollText(const std::string& s, const UiRect& r, const TextStyle& st, int& scroll) {
    int total = 0;
    UiRect tr{r.x, r.y, r.w - 14, r.h};
    float lh = lineHeight(st);
    int visible = std::max(1, int(r.h / std::max(1.f, lh)));
    // measure first (draws nothing when every line is skipped)
    textBox(s, tr, st, st.color, 1 << 20, &total);
    int maxScroll = std::max(0, total - visible);
    if (r.contains(mx_, my_) && wheel_ != 0) scroll -= int(wheel_ > 0 ? 3 : -3);
    scroll = std::clamp(scroll, 0, maxScroll);
    textBox(s, tr, st, st.color, scroll);
    if (maxScroll > 0) {
        float sx = r.x + r.w - 12;
        image("SSBR001X.TGA", sx, r.y + 12, 12, std::max(0.f, r.h - 24), 0xffffffffu, "tile");
        UiRect up{sx, r.y, 12, 12}, dn{sx, r.y + r.h - 12, 12, 12};
        image(up.contains(mx_, my_) ? "SSAR001M.TGA" : "SSAR001U.TGA", up.x, up.y);
        image(dn.contains(mx_, my_) ? "SSAR002M.TGA" : "SSAR002U.TGA", dn.x, dn.y);
        float t = float(scroll) / float(maxScroll);
        image("SSBT001U.TGA", sx, r.y + 12 + t * std::max(0.f, r.h - 35), 12, 11);
        if (pressed_ && up.contains(mx_, my_)) scroll = std::max(0, scroll - 3), playSound("scrollbar.wav");
        if (pressed_ && dn.contains(mx_, my_)) scroll = std::min(maxScroll, scroll + 3), playSound("scrollbar.wav");
    }
}

bool Ui::textField(std::string& value, const UiRect& r, const TextStyle& st, size_t maxLen, bool focused) {
    bool enter = false;
    if (focused) {
        for (unsigned char c : typed_)
            if (c >= 32 && c < 127 && value.size() < maxLen) value += char(c);
        if (keyPressed(SDLK_BACKSPACE) && !value.empty()) value.pop_back();
        enter = keyPressed(SDLK_RETURN) || keyPressed(SDLK_KP_ENTER);
    }
    std::string shown = value;
    if (focused && (caretBlink_ / 20) % 2 == 0) shown += "_";
    textBox(shown, r, st, st.color);
    return enter;
}

}  // namespace hg2
