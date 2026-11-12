#include "assets.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hg2 {

// ------------------------------------------------------------------ images
static bool decodeBMP(const Bytes& d, Image& out) {
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
    Reader r(d.data() + 10, d.size() - 10);
    uint32_t off = r.u32();
    r.u32();
    int32_t w = r.i32(), h = r.i32();
    r.u16();
    int bpp = r.u16();
    uint32_t comp = r.u32();
    if (comp != 0 || w <= 0 || h == 0) return false;
    bool flip = h > 0;
    h = std::abs(h);
    std::vector<uint8_t> pal;
    if (bpp == 8) {
        size_t po = 14 + 40;
        pal.assign(d.begin() + po, d.begin() + std::min(d.size(), po + 1024));
        pal.resize(1024);
    } else if (bpp != 24 && bpp != 32) {
        return false;
    }
    size_t stride = ((size_t(w) * bpp + 31) / 32) * 4;
    if (off + stride * h > d.size()) return false;
    out.w = w;
    out.h = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
        const uint8_t* s = d.data() + off + stride * (flip ? h - 1 - y : y);
        uint8_t* o = out.rgba.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; x++, o += 4) {
            if (bpp == 8) {
                const uint8_t* c = &pal[size_t(s[x]) * 4];
                o[0] = c[2], o[1] = c[1], o[2] = c[0], o[3] = 255;
            } else {
                const uint8_t* c = s + x * (bpp / 8);
                o[0] = c[2], o[1] = c[1], o[2] = c[0], o[3] = bpp == 32 ? c[3] : 255;
            }
        }
    }
    out.hasAlpha = bpp == 32;
    return true;
}

static bool decodeTGA(const Bytes& d, Image& out) {
    if (d.size() < 18) return false;
    int idlen = d[0], type = d[2];
    int w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8), bpp = d[16], desc = d[17];
    size_t p = 18 + idlen;
    if (type == 1 || type == 9) {  // colour-mapped, 8-bit indices
        int cmFirst = d[3] | (d[4] << 8), cmLen = d[5] | (d[6] << 8), cmBits = d[7];
        int cb = cmBits / 8;
        if (bpp != 8 || (cb != 3 && cb != 4) || p + size_t(cmLen) * cb > d.size()) return false;
        std::vector<uint8_t> pal(256 * 4, 255);
        for (int i = 0; i < cmLen && cmFirst + i < 256; i++) {
            const uint8_t* c = &d[p + size_t(i) * cb];
            uint8_t* o = &pal[size_t(cmFirst + i) * 4];
            o[0] = c[2], o[1] = c[1], o[2] = c[0], o[3] = cb == 4 ? c[3] : 255;
        }
        p += size_t(cmLen) * cb;
        std::vector<uint8_t> idx(size_t(w) * h);
        if (type == 1) {
            if (p + idx.size() > d.size()) return false;
            memcpy(idx.data(), d.data() + p, idx.size());
        } else {
            size_t o = 0;
            while (o < idx.size() && p < d.size()) {
                uint8_t c = d[p++];
                int n = (c & 0x7f) + 1;
                if (c & 0x80) {
                    uint8_t v = p < d.size() ? d[p++] : 0;
                    for (int i = 0; i < n && o < idx.size(); i++) idx[o++] = v;
                } else {
                    for (int i = 0; i < n && o < idx.size() && p < d.size(); i++) idx[o++] = d[p++];
                }
            }
        }
        bool top = desc & 0x20;
        out.w = w, out.h = h;
        out.rgba.resize(size_t(w) * h * 4);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                memcpy(&out.rgba[(size_t(y) * w + x) * 4], &pal[size_t(idx[size_t(top ? y : h - 1 - y) * w + x]) * 4], 4);
        out.hasAlpha = cb == 4;
        return true;
    }
    if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0) return false;
    int bpx = bpp / 8;
    std::vector<uint8_t> px(size_t(w) * h * bpx);
    if (type == 2) {
        if (p + px.size() > d.size()) return false;
        memcpy(px.data(), d.data() + p, px.size());
    } else {
        size_t o = 0;
        while (o < px.size() && p < d.size()) {
            uint8_t c = d[p++];
            int n = (c & 0x7f) + 1;
            if (c & 0x80) {
                if (p + bpx > d.size()) return false;
                for (int i = 0; i < n && o < px.size(); i++, o += bpx) memcpy(&px[o], &d[p], bpx);
                p += bpx;
            } else {
                size_t bytes = size_t(n) * bpx;
                if (p + bytes > d.size() || o + bytes > px.size()) return false;
                memcpy(&px[o], &d[p], bytes);
                p += bytes;
                o += bytes;
            }
        }
    }
    bool top = desc & 0x20;
    out.w = w;
    out.h = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
        const uint8_t* s = &px[size_t(top ? y : h - 1 - y) * w * bpx];
        uint8_t* o = &out.rgba[size_t(y) * w * 4];
        for (int x = 0; x < w; x++, s += bpx, o += 4) {
            o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = bpx == 4 ? s[3] : 255;
        }
    }
    out.hasAlpha = bpx == 4;
    return true;
}

bool decodeImage(const Bytes& data, const std::string& name, Image& out) {
    std::string e = extension(name);
    if (e == "tga") return decodeTGA(data, out) || decodeBMP(data, out);
    return decodeBMP(data, out) || decodeTGA(data, out);
}

// ------------------------------------------------------------------ models
static void orthonormal(const double in[12], double out[12]) {
    // FUN_1001d4f0: Z normalised, X = Y x Z, Y = Z x X (scale discarded)
    double Y[3] = {in[3], in[4], in[5]}, Z[3] = {in[6], in[7], in[8]};
    auto nrm = [](double* v) {
        double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
    };
    nrm(Z);
    double X[3] = {Y[1] * Z[2] - Y[2] * Z[1], Y[2] * Z[0] - Y[0] * Z[2], Y[0] * Z[1] - Y[1] * Z[0]};
    nrm(X);
    double Y2[3] = {Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0]};
    nrm(Y2);
    for (int i = 0; i < 3; i++) out[i] = X[i], out[3 + i] = Y2[i], out[6 + i] = Z[i], out[9 + i] = in[9 + i];
}

// BSP node: u32 n; n x u32 poly; i32 left; i32 right; then the left subtree (if left != -1)
// and the right subtree (if right != -1), depth-first. Walked iteratively (trees can be deep).
static void skipBsp(Reader& r) {
    size_t pending = 1;
    while (pending && r.ok(12)) {
        pending--;
        uint32_t n = r.u32();
        r.skip(size_t(n) * 4);
        int32_t a = r.i32(), b = r.i32();
        pending += (a != -1) + (b != -1);
    }
}

bool loadModel(const Bytes& data, Model& out) {
    Reader r(data);
    if (!r.ok(16)) return false;
    r.u32();  // crc
    r.u32();  // size
    const uint8_t* base = r.p;
    uint32_t matoff = r.u32();
    Reader m(base + matoff, size_t(r.end - (base + matoff)));
    if (base + matoff > r.end) return false;
    uint32_t nm = m.u32();
    for (uint32_t i = 0; i < nm && m.ok(36); i++) {
        Material mt;
        for (float& f : mt.params) f = m.f32();
        mt.flags = m.u32();
        mt.name = m.str();
        mt.texture = m.str();
        out.materials.push_back(mt);
    }
    r.u32();  // crc
    r.u32();  // size
    uint32_t nf = r.u32();
    for (uint32_t fi = 0; fi < nf && r.ok(4); fi++) {
        Frame f;
        f.name = r.str();
        f.type = r.u32();
        double raw[12];
        for (double& d : raw) d = r.f64();
        orthonormal(raw, f.m);
        f.child = r.i32() - 1;
        f.sibling = r.i32() - 1;
        int32_t hm = r.i32();
        if (hm != -1) {
            Mesh me;
            me.name = r.str();
            r.u32();
            r.skip(4 * 8 + 3 * 4);
            uint32_t n = r.u32();
            if (!r.ok(size_t(n) * 8)) return false;
            me.uv.resize(n);
            for (auto& t : me.uv) t[0] = r.f32(), t[1] = r.f32();
            n = r.u32();
            if (!r.ok(size_t(n) * 12)) return false;
            me.v.resize(n);
            for (auto& v : me.v) v.x = r.f32(), v.y = r.f32(), v.z = r.f32();
            n = r.u32();
            if (!r.ok(size_t(n) * 12)) return false;
            me.n.resize(n);
            for (auto& v : me.n) v.x = r.f32(), v.y = r.f32(), v.z = r.f32();
            uint32_t np = r.u32();
            if (np) {
                uint32_t nc = r.u32();
                for (uint32_t pi = 0; pi < np; pi++) {
                    Poly p;
                    p.id = r.u32();
                    for (float& f2 : p.plane) f2 = r.f32();
                    if (!r.ok(size_t(nc) * 12)) return false;
                    p.corners.resize(nc);
                    for (auto& c : p.corners) c.v = r.u32(), c.n = r.u32(), c.uv = r.u32();
                    p.material = r.i32() - 1;
                    me.polys.push_back(std::move(p));
                    if (pi + 1 < np) nc = r.u32();
                }
            }
            if (r.u32() == 1) skipBsp(r);
            f.mesh = int(out.meshes.size());
            out.meshes.push_back(std::move(me));
        }
        out.frames.push_back(std::move(f));
    }
    return r.p <= r.end && !out.frames.empty();
}

// ------------------------------------------------------------------ animation
bool loadAnimSets(const Bytes& data, std::vector<AnimSet>& out) {
    Reader r(data);
    if (!r.ok(12)) return false;
    r.u32();
    r.u32();
    uint32_t n = r.u32();
    for (uint32_t si = 0; si < n && r.ok(4); si++) {
        AnimSet set;
        uint32_t t = r.u32();
        while (t == 0xd && r.ok()) {  // Animation
            AnimTrack tr;
            tr.frame = lower(r.str());
            t = r.u32();
            while (t == 0xe && r.ok(8)) {  // AnimationKey
                uint32_t type = r.u32(), nk = r.u32();
                int w = type == 0 ? 4 : 3;
                if (type > 2 || !r.ok(size_t(nk) * (4 + 4 * w))) return false;
                auto& dst = type == 0 ? tr.rot : type == 1 ? tr.scale : tr.pos;
                for (uint32_t k = 0; k < nk; k++) {
                    AnimKey key{};
                    key.time = r.i32();
                    for (int c = 0; c < w; c++) key.v[c] = r.f32();
                    set.length = std::max(set.length, key.time);
                    dst.push_back(key);
                }
                t = r.u32();
            }
            set.tracks.push_back(std::move(tr));
            t = r.u32();  // after the animation's closing token
        }
        out.push_back(std::move(set));
    }
    return r.p <= r.end;
}

static void sampleKeys(const std::vector<AnimKey>& keys, float t, int w, float out[4], bool quat) {
    if (keys.size() == 1 || t <= keys.front().time) {
        memcpy(out, keys.front().v, sizeof(float) * 4);
        return;
    }
    if (t >= keys.back().time) {
        memcpy(out, keys.back().v, sizeof(float) * 4);
        return;
    }
    size_t i = 1;
    while (i < keys.size() && keys[i].time < t) i++;
    const AnimKey &a = keys[i - 1], &b = keys[i];
    float f = (t - a.time) / float(std::max(1, b.time - a.time));
    if (!quat) {
        for (int c = 0; c < w; c++) out[c] = a.v[c] + (b.v[c] - a.v[c]) * f;
        return;
    }
    float d = a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3];
    float s = d < 0 ? -1.f : 1.f;
    d = std::fabs(d);
    float k0 = 1 - f, k1 = f;
    if (d < 0.9995f) {
        float th = std::acos(d), sn = std::sin(th);
        k0 = std::sin((1 - f) * th) / sn;
        k1 = std::sin(f * th) / sn;
    }
    float l = 0;
    for (int c = 0; c < 4; c++) out[c] = a.v[c] * k0 + b.v[c] * k1 * s, l += out[c] * out[c];
    l = std::sqrt(l);
    for (int c = 0; c < 4; c++) out[c] /= l;
}

Mat4 animLocal(const AnimTrack* tr, const Mat4& base, float t) {
    if (!tr) return base;
    Mat4 m = base;
    if (!tr->rot.empty()) {
        float q[4];
        sampleKeys(tr->rot, t, 4, q, true);
        // keys are (w,x,y,z); the engine applies the conjugate in row-vector form, which is the
        // standard column-vector rotation matrix transposed (verified visually on gear rest poses).
        float w = q[0], x = -q[1], y = -q[2], z = -q[3];
        m.m[0][0] = 1 - 2 * (y * y + z * z), m.m[0][1] = 2 * (x * y + z * w), m.m[0][2] = 2 * (x * z - y * w);
        m.m[1][0] = 2 * (x * y - z * w), m.m[1][1] = 1 - 2 * (x * x + z * z), m.m[1][2] = 2 * (y * z + x * w);
        m.m[2][0] = 2 * (x * z + y * w), m.m[2][1] = 2 * (y * z - x * w), m.m[2][2] = 1 - 2 * (x * x + y * y);
    }
    if (!tr->pos.empty()) {
        float p[4];
        sampleKeys(tr->pos, t, 3, p, false);
        m.m[3][0] = p[0], m.m[3][1] = p[1], m.m[3][2] = p[2];
    }
    return m;
}

// ------------------------------------------------------------------ terrain
bool loadTerrain(const Bytes& data, Terrain& out) {
    Reader r(data);
    uint32_t nt = r.u32();
    if (nt > 56) nt = 56;
    for (uint32_t i = 0; i < nt; i++) {
        const char* s = reinterpret_cast<const char*>(r.p);
        out.textures.emplace_back(s, strnlen(s, 128));
        r.skip(128);
    }
    r.skip(16);
    out.tiles.resize(128 * 128);
    for (auto& t : out.tiles) {
        if (!r.ok(116)) return false;
        t.x0 = r.i32(), t.z0 = r.i32(), t.x1 = r.i32(), t.z1 = r.i32();
        r.p -= 16;
        for (int32_t& f : t.f) f = r.i32();
        r.skip(32);
        int cnt = t.f[11];
        t.dim = t.f[12];
        t.step = t.f[15];
        if (cnt <= 0 || cnt > 4096 || t.dim * t.dim != cnt || t.step <= 0 || !r.ok(size_t(cnt) * 4)) return false;
        t.h.resize(size_t(cnt));
        t.attr.resize(size_t(cnt));
        for (auto& h : t.h) h = r.u16();
        for (auto& a : t.attr) a = r.u16();
    }
    for (auto& t : out.tiles)
        for (int32_t& x : t.tex) x = r.i32();
    // The file's first axis is the world's Z: mission objects only sit on the ground when the grid is read
    // transposed (checked on every mission). Swap to world X/Z here so everything downstream is plain.
    std::vector<TerrainTile> tt(out.tiles.size());
    for (int a = 0; a < 128; a++)
        for (int b = 0; b < 128; b++) {
            TerrainTile t = out.tiles[size_t(a) * 128 + b];
            std::swap(t.x0, t.z0), std::swap(t.x1, t.z1);
            auto tr = [&](std::vector<uint16_t>& v) {
                std::vector<uint16_t> o(v.size());
                for (int i = 0; i < t.dim; i++)
                    for (int j = 0; j < t.dim; j++) o[size_t(j) * t.dim + i] = v[size_t(i) * t.dim + j];
                v.swap(o);
            };
            tr(t.h), tr(t.attr);
            tt[size_t(b) * 128 + a] = std::move(t);
        }
    out.tiles.swap(tt);
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok(12); i++) out.table.push_back({r.f32(), r.f32(), r.f32()});
    return true;
}

// After loadTerrain's transpose: tiles[tx*128 + tz] covers world x = [x0,x1], z = [z0,z1]; h[ix*dim + iz].
float Terrain::height(int gx, int gz) const {
    gx = std::max(0, std::min(256, gx));
    gz = std::max(0, std::min(256, gz));
    int tx = std::min(127, gx / 2), tz = std::min(127, gz / 2);
    int lx = gx - tx * 2, lz = gz - tz * 2;
    return tiles[size_t(tx) * 128 + tz].heightAt(lx * 16, lz * 16);
}

float Terrain::sample(float x, float z) const {
    x = std::max(0.f, std::min(4095.99f, x));
    z = std::max(0.f, std::min(4095.99f, z));
    int tx = int(x) / 32, tz = int(z) / 32;
    const TerrainTile& t = tiles[size_t(tx) * 128 + tz];
    float lx = (x - t.x0) / t.step, lz = (z - t.z0) / t.step;
    int ix = std::min(t.dim - 2, int(lx)), iz = std::min(t.dim - 2, int(lz));
    float fx = lx - ix, fz = lz - iz;
    auto H = [&](int a, int b) { return t.h[size_t(a) * t.dim + b] * 0.01f; };
    return (H(ix, iz) * (1 - fx) + H(ix + 1, iz) * fx) * (1 - fz) + (H(ix, iz + 1) * (1 - fx) + H(ix + 1, iz + 1) * fx) * fz;
}

int Terrain::surfaceAt(float x, float z) const {
    if (tiles.size() < 128 * 128) return 0;
    x = std::max(0.f, std::min(4095.99f, x));
    z = std::max(0.f, std::min(4095.99f, z));
    const TerrainTile& t = tiles[size_t(int(x) / 32) * 128 + size_t(int(z) / 32)];
    if (t.attr.empty() || t.step <= 0) return 0;
    int ix = std::min(t.dim - 1, int((x - float(t.x0)) / float(t.step) + 0.5f));
    int iz = std::min(t.dim - 1, int((z - float(t.z0)) / float(t.step) + 0.5f));
    size_t k = size_t(std::max(0, ix)) * size_t(t.dim) + size_t(std::max(0, iz));
    return k < t.attr.size() ? (t.attr[k] & 0x700) >> 8 : 0;
}

float TerrainTile::heightAt(int lx, int lz) const {
    int ix = std::min(dim - 1, std::max(0, lx / step)), iz = std::min(dim - 1, std::max(0, lz / step));
    return h[size_t(ix) * dim + iz] * 0.01f;
}

}  // namespace hg2
