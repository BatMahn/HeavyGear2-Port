#include "zfs.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace hg2 {

static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// ZFS3: "ZFS3", version, name length (16), entries per directory block (100), file count, 0, first block.
// Block: next block offset, then 100 x {name[16], offset, index, size, time, flags}.
bool ZfsArchive::load(Bytes data) {
    data_ = std::move(data);
    dir_.clear();
    if (data_.size() < 28 || memcmp(data_.data(), "ZFS3", 4) != 0) return false;
    uint32_t nameLen = rd32(&data_[8]), per = rd32(&data_[12]), total = rd32(&data_[16]), off = rd32(&data_[24]);
    if (nameLen != 16 || per == 0 || per > 10000) return false;
    size_t rec = nameLen + 20;
    while (off && off + 4 + per * rec <= data_.size() && dir_.size() < total) {
        uint32_t next = rd32(&data_[off]);
        for (uint32_t i = 0; i < per; i++) {
            const uint8_t* e = &data_[off + 4 + i * rec];
            std::string n(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), nameLen));
            if (n.empty()) continue;
            uint32_t o = rd32(e + nameLen), sz = rd32(e + nameLen + 8);
            if (uint64_t(o) + sz <= data_.size()) dir_[lower(n)] = {o, sz};
        }
        if (next <= off) break;
        off = next;
    }
    return !dir_.empty();
}

bool ZfsArchive::read(const std::string& name, const uint8_t*& p, size_t& n) const {
    auto it = dir_.find(lower(name));
    if (it == dir_.end()) return false;
    p = data_.data() + it->second.first;
    n = it->second.second;
    return true;
}

bool zipExtract(const std::string& zipPath, const std::string& entry, Bytes& out) {
    Bytes z;
    if (!readFile(zipPath, z) || z.size() < 22) return false;
    // end of central directory
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22 + 1; i-- > 0 && z.size() - i < 70000;)
        if (rd32(&z[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return false;
    uint32_t cdCount = rd16(&z[eocd + 10]), cdOff = rd32(&z[eocd + 16]);
    size_t p = cdOff;
    for (uint32_t i = 0; i < cdCount && p + 46 <= z.size(); i++) {
        if (rd32(&z[p]) != 0x02014b50) return false;
        uint16_t method = rd16(&z[p + 10]);
        uint32_t csize = rd32(&z[p + 20]), usize = rd32(&z[p + 24]);
        uint16_t nl = rd16(&z[p + 28]), xl = rd16(&z[p + 30]), cl = rd16(&z[p + 32]);
        uint32_t lho = rd32(&z[p + 42]);
        std::string n(reinterpret_cast<const char*>(&z[p + 46]), nl);
        p += 46 + nl + xl + cl;
        if (lower(n) != lower(entry)) continue;
        if (lho + 30 > z.size() || rd32(&z[lho]) != 0x04034b50) return false;
        size_t data = lho + 30 + rd16(&z[lho + 26]) + rd16(&z[lho + 28]);
        if (data + csize > z.size()) return false;
        if (method == 0) {
            out.assign(z.begin() + long(data), z.begin() + long(data + csize));
            return true;
        }
        if (method != 8) return false;
        out.resize(usize);
        z_stream s{};
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return false;
        s.next_in = &z[data];
        s.avail_in = csize;
        s.next_out = out.data();
        s.avail_out = usize;
        int r = inflate(&s, Z_FINISH);
        inflateEnd(&s);
        return r == Z_STREAM_END;
    }
    return false;
}

bool ShellArt::open(const FileSystem& fs) {
    auto loose = [&](const char* n, ZfsArchive& a) {
        std::string p = fs.findLoose(std::string("shell/shellart/") + n);
        if (p.empty()) p = fs.findLoose(n);
        Bytes b;
        return !p.empty() && readFile(p, b) && a.load(std::move(b));
    };
    if (loose("rim565.zfs", rim_)) {
        loose("alp.zfs", alp_);
        source_ = "shellart";
        return true;
    }
    // CD layout: Setup/data (mounted) next to Setup/zip/zfs.zip
    for (const char* rel : {"/../zip/zfs.zip", "/zip/zfs.zip", "/../Setup/zip/zfs.zip", "/Setup/zip/zfs.zip"}) {
        std::string zp = fs.root() + rel;
        Bytes b;
        if (zipExtract(zp, "rim565.zfs", b) && rim_.load(std::move(b))) {
            Bytes a;
            if (zipExtract(zp, "alp.zfs", a)) alp_.load(std::move(a));
            source_ = zp;
            return true;
        }
    }
    return false;
}

bool ShellArt::image(const std::string& name, Image& out) const {
    std::string base = lower(basename(name));
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    const uint8_t* p;
    size_t n;
    if (!rim_.read(base + ".rim", p, n) || n < 16 || memcmp(p, "RIMF", 4) != 0) return false;
    int w = rd16(p + 8), h = rd16(p + 10), pitch = rd16(p + 12);
    if (w <= 0 || h <= 0 || size_t(16) + size_t(pitch) * size_t(h - 1) + size_t(w) * 2 > n) return false;
    out.w = w, out.h = h;
    out.rgba.assign(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t v = rd16(p + 16 + size_t(y) * pitch + size_t(x) * 2);
            uint8_t* o = &out.rgba[(size_t(y) * w + x) * 4];
            int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            o[0] = uint8_t(r << 3 | r >> 2), o[1] = uint8_t(g << 2 | g >> 4), o[2] = uint8_t(b << 3 | b >> 2);
        }
    out.hasAlpha = false;
    const uint8_t* a;
    size_t an;
    if (alp_.read(base + ".alp", a, an) && an >= 16 && memcmp(a, "ALPH", 4) == 0) {
        int aw = rd16(a + 8), ah = rd16(a + 10), ap = rd16(a + 12);
        if (aw == w && ah == h && size_t(16) + size_t(ap) * size_t(h - 1) + size_t(w) <= an) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) out.rgba[(size_t(y) * w + x) * 4 + 3] = a[16 + size_t(y) * ap + x];
            out.hasAlpha = true;
        }
    }
    return true;
}

}  // namespace hg2
