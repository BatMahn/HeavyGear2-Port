// Shell UI drawing on top of the LDL layouts: the 640x480 design is scaled to the window (letterboxed), shell
// art comes from the ZFS containers, text is rendered from the game's TrueType fonts (EUM / EUB / s017016d).
// Screens are drawn immediate-mode each frame: window("splash_all_singles") draws a top-level layout node
// and everything under it; nodes that look like buttons (activeimage0 / image1 / executesound) react to
// the mouse, and the path of a button released this frame is reported by clicked().
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../core/zfs.h"
#include "../render/renderer.h"
#include "ldl.h"

namespace hg2 {

class Audio;

struct UiRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct TextStyle {
    std::string font = "eum_____.ttf";
    int size = 8;
    uint32_t color = 0x4c8ef8ff, hover = 0x4c8ef8ff, down = 0x4c8ef8ff, disabled = 0x0d2b63ff;
    std::string just = "left";
    bool vcenter = false, wrap = false;
    int insetX = 0, insetY = 0;
};

class Ui {
public:
    bool init(Renderer& r, const FileSystem& fs, Audio* audio);
    bool ok() const { return art_.ok() && ldl_.files() > 0; }
    Ldl& ldl() { return ldl_; }
    const ShellArt& art() const { return art_; }
    Audio* audio() const { return audio_; }
    float shellVolume = 0.8f;

    // Frame: window size in pixels; input in window pixels.
    void begin(int w, int h);
    void mouseMove(float x, float y) { mx_ = (x - ox_) / s_, my_ = (y - oy_) / s_; }
    void mouseDown() { down_ = true, pressed_ = true; }
    void mouseUp() { down_ = false, released_ = true; }
    void wheel(float dy) { wheel_ += dy; }
    void key(int sdlKey) { keys_.push_back(sdlKey); }
    void textInput(const std::string& t) { typed_ += t; }
    void end();  // hands the overlay to the renderer
    float mouseX() const { return mx_; }
    float mouseY() const { return my_; }

    // Layout drawing. `path` is a top-level node or dotted path; the node is placed in `parent` (the screen
    // when omitted). Returns the node's rectangle.
    UiRect window(const std::string& path);
    UiRect window(const std::string& path, const UiRect& parent);
    UiRect rectOf(const std::string& path);  // rectangle without drawing
    const std::string& clicked() const { return clicked_; }
    bool clicked(const std::string& path) const;  // case-insensitive, suffix match on the button path
    // Per-node overrides for the next draws (keyed by full lowercase path): replace the text, hide, disable,
    // force the selected (pressed) look.
    std::map<std::string, std::string> text;
    std::map<std::string, bool> hidden, disabled, selected;
    void clearOverrides() { text.clear(), hidden.clear(), disabled.clear(), selected.clear(); }

    // Primitives (640x480 design coordinates)
    bool image(const std::string& name, float x, float y, float w = -1, float h = -1, uint32_t tint = 0xffffffff,
               const std::string& mode = std::string());
    bool imageSize(const std::string& name, int& w, int& h);
    void rect(float x, float y, float w, float h, uint32_t rgba);
    // A renderer texture (e.g. a movie frame) stretched over a rectangle.
    void textureQuad(int tex, float x, float y, float w, float h) { quad(tex, ox_ + x * s_, oy_ + y * s_, ox_ + (x + w) * s_, oy_ + (y + h) * s_, 0, 0, 1, 1, 0xffffffff); }
    Renderer* renderer() const { return r_; }
    void frame(float x, float y, float w, float h, uint32_t rgba);
    // Text in a box; returns the height used. firstLine skips wrapped lines (scrolling text boxes).
    float textBox(const std::string& s, const UiRect& r, const TextStyle& st, uint32_t color, int firstLine = 0, int* totalLines = nullptr);
    float textWidth(const std::string& s, const TextStyle& st);
    float lineHeight(const TextStyle& st);
    TextStyle style(const LdlNode* n);

    // Widgets built from layout parts
    // List box: `lbPath` is the layout's list node (frame image, header, item offsets / height); returns
    // the clicked item index or -1. `sel` is drawn highlighted; `scroll` is kept by the caller.
    int listbox(const std::string& lbPath, const std::vector<std::string>& items, int sel, int& scroll,
                const std::string& itemTemplate = "msn_lb_item");
    // Scrolling text area in a rectangle with a scroll bar; `scroll` in lines.
    void scrollText(const std::string& s, const UiRect& r, const TextStyle& st, int& scroll);
    // One-line text entry; returns true when Enter is pressed.
    bool textField(std::string& value, const UiRect& r, const TextStyle& st, size_t maxLen, bool focused);
    bool keyPressed(int sdlKey) const;
    bool anyKeyPressed() const { return !keys_.empty(); }
    bool mousePressed() const { return pressed_; }
    bool mouseReleased() const { return released_; }
    bool mouseHeld() const { return down_; }
    void playSound(const std::string& wav, float vol = 1.f);
    void cursor();  // draw the mouse pointer (Scursor1)

private:
    struct Font {
        int tex = -1;
        float px = 0, ascent = 0, lineGap = 0;
        struct G { float x0, y0, x1, y1, xoff, yoff, adv; };
        G g[224];
    };
    Font* font(const std::string& file, int size);
    int texture(const std::string& name, int* w = nullptr, int* h = nullptr);
    void quad(int tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba);
    void drawNode(const LdlNode* n, const std::string& path, const UiRect& r, int depth);
    UiRect place(const LdlNode* n, const UiRect& parent);
    bool isButton(const LdlNode* n) const;
    uint32_t nodeColor(const LdlNode* n, const char* which, uint32_t def);

    Renderer* r_ = nullptr;
    const FileSystem* fs_ = nullptr;
    Audio* audio_ = nullptr;
    ShellArt art_;
    Ldl ldl_;
    std::map<std::string, std::vector<uint8_t>> fontFiles_;
    std::map<std::string, std::unique_ptr<Font>> fonts_;
    std::map<const Font*, int> atlasDim_;
    struct Tex { int id = -1, w = 0, h = 0; };
    std::map<std::string, Tex> textures_;
    std::vector<Vertex> verts_;
    std::vector<DrawBatch> batches_;
    float s_ = 1, ox_ = 0, oy_ = 0;
    int W_ = 640, H_ = 480;
    float mx_ = -1, my_ = -1, wheel_ = 0;
    bool down_ = false, pressed_ = false, released_ = false;
    std::vector<int> keys_;
    std::string typed_;
    std::string clicked_, hot_, lastHot_, pressedOn_;
    int caretBlink_ = 0;
public:
    float wheelDelta() const { return wheel_; }
    const std::string& typed() const { return typed_; }
    float scale() const { return s_; }
    // Drop cached textures with ids at or above `firstId` (released by the renderer after a mission).
    void forgetTextures(int firstId);
};

}  // namespace hg2
