#include "fs.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace hg2 {

std::string lower(std::string s) {
    for (auto& c : s) c = char(tolower((unsigned char)c));
    return s;
}
std::string upper(std::string s) {
    for (auto& c : s) c = char(toupper((unsigned char)c));
    return s;
}
std::string basename(const std::string& p) {
    size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}
std::string extension(const std::string& p) {
    std::string b = basename(p);
    size_t k = b.rfind('.');
    return k == std::string::npos ? "" : lower(b.substr(k + 1));
}

bool readFile(const std::string& path, Bytes& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size_t(n));
    size_t got = n ? fread(out.data(), 1, size_t(n), f) : 0;
    fclose(f);
    return got == size_t(n);
}

// FUN_1001d170
Bytes lzbDecompress(const uint8_t* d, size_t n) {
    Bytes out;
    out.reserve(n * 12);
    size_t p = 0;
    while (p + 3 <= n) {
        uint8_t type = d[p];
        int16_t bs = int16_t(d[p + 1] | (d[p + 2] << 8));
        if (type == 0x80 || bs < 3) break;
        const uint8_t* b = d + p + 3;
        size_t len = size_t(bs) - 3;
        p += size_t(bs);
        if (p > n) break;
        if (type == 0) {
            out.insert(out.end(), b, b + len);
            continue;
        }
        size_t i = 0;
        auto copyBack = [&](size_t off, size_t cnt) {
            size_t s = out.size() - off;
            for (size_t k = 0; k < cnt; k++) out.push_back(out[s + k]);
        };
        while (i < len) {
            uint8_t c = b[i];
            if (c & 0x80) {
                size_t off = (((c << 8) | b[i + 1]) & 0x7ff) + 1;
                copyBack(off, (c >> 3) - 13);
                i += 2;
            } else if (c < 0x40) {
                out.insert(out.end(), b + i + 1, b + i + 1 + c + 1);
                i += 1 + c + 1;
            } else if (c & 0x20) {
                size_t off = (((c << 8) | b[i + 1]) & 0x1fff) + 1;
                copyBack(off, size_t(b[i + 2]) + 4);
                i += 3;
            } else if (!(c & 0x10)) {
                out.insert(out.end(), size_t(uint8_t(c - 0x3d)), b[i + 1]);
                i += 2;
            } else if (!(c & 8)) {
                size_t cnt = (((c << 8) | b[i + 1]) + 0xb041) & 0xffff;
                out.insert(out.end(), b + i + 2, b + i + 2 + cnt);
                i += 2 + cnt;
            } else {
                size_t cnt = (((c << 8) | b[i + 1]) + 0xa813) & 0xffff;
                out.insert(out.end(), cnt, b[i + 2]);
                i += 3;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- PRJ
bool PrjArchive::open(const std::string& prjPath, const std::string& mapPath) {
    Bytes m;
    if (!readFile(mapPath, m) || m.size() < 16) return false;
    prjPath_ = prjPath;
    Reader r(m);
    uint32_t count = r.u32(), ntypes = r.u32();
    r.skip(8 * ntypes);
    r.u32();
    uint32_t ndll = r.u32();
    r.skip(16 * ndll);
    for (uint32_t k = 0; k < count && r.ok(20); k++) {
        char name[9] = {}, ext[5] = {};
        memcpy(name, r.p, 8);
        memcpy(ext, r.p + 8, 4);
        r.skip(12);
        uint32_t off = r.u32();
        r.skip(4);
        entries_[upper(std::string(name) + "." + std::string(ext))] = {off};
    }
    return !entries_.empty();
}

bool PrjArchive::has(const std::string& name) const { return entries_.count(upper(name)) != 0; }

std::vector<std::string> PrjArchive::names() const {
    std::vector<std::string> v;
    for (auto& e : entries_) v.push_back(e.first);
    std::sort(v.begin(), v.end());
    return v;
}

bool PrjArchive::read(const std::string& name, Bytes& out) const {
    auto it = entries_.find(upper(name));
    if (it == entries_.end()) return false;
    FILE* f = fopen(prjPath_.c_str(), "rb");
    if (!f) return false;
    uint8_t h[22];
    bool ok = false;
    if (fseek(f, long(it->second.offset), SEEK_SET) == 0 && fread(h, 1, 22, f) == 22 &&
        memcmp(h, "C:\\HG2\\", 7) == 0) {
        uint16_t method = uint16_t(h[12] | (h[13] << 8));
        uint32_t us, cs;
        memcpy(&us, h + 14, 4);
        memcpy(&cs, h + 18, 4);
        Bytes c(cs);
        if (fread(c.data(), 1, cs, f) == cs) {
            if (method == 0) {
                out = std::move(c);
                ok = true;
            } else {
                out.resize(us);
                uLongf dl = us;
                ok = uncompress(out.data(), &dl, c.data(), cs) == Z_OK && dl == us;
            }
        }
    }
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------- FileSystem
void FileSystem::indexDir(const std::string& dir, const std::string& rel, int depth) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string nm = e.path().filename().string();
        std::string r = rel.empty() ? nm : rel + "/" + nm;
        if (e.is_directory(ec)) {
            if (depth < 4) indexDir(e.path().string(), r, depth + 1);
        } else {
            files_[lower(r)] = e.path().string();
            byName_.emplace(lower(nm), e.path().string());
        }
    }
}

bool FileSystem::mount(const std::string& root) {
    root_ = root;
    files_.clear();
    byName_.clear();
    where_.clear();
    indexDir(root, "", 0);
    std::string prj = findLoose("simpack.prj"), map = findLoose("simpack.map");
    if (prj.empty() || map.empty() || !sim_.open(prj, map)) return false;
    std::string sp = findLoose("hg2shell.prj"), sm = findLoose("hg2shell.map");
    if (!sp.empty() && !sm.empty()) shell_.open(sp, sm);
    // "where": ext = .\dir1; .\dir2
    Bytes w;
    std::string wp = findLoose("where");
    if (!wp.empty() && readFile(wp, w)) {
        std::string s(w.begin(), w.end());
        size_t pos = 0;
        while (pos < s.size()) {
            size_t e = s.find('\n', pos);
            if (e == std::string::npos) e = s.size();
            std::string line = s.substr(pos, e - pos);
            pos = e + 1;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string t) {
                while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
                while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
                return t;
            };
            std::string ext = lower(trim(line.substr(0, eq)));
            std::string rest = line.substr(eq + 1);
            size_t q = 0;
            while (q < rest.size()) {
                size_t sc = rest.find(';', q);
                if (sc == std::string::npos) sc = rest.size();
                std::string d = trim(rest.substr(q, sc - q));
                q = sc + 1;
                for (auto& c : d)
                    if (c == '\\') c = '/';
                if (d.rfind("./", 0) == 0) d = d.substr(2);
                if (d == ".") d.clear();
                where_[ext].push_back(lower(d));
            }
        }
    }
    return true;
}

std::string FileSystem::findLoose(const std::string& relPath) const {
    std::string k = lower(relPath);
    for (auto& c : k)
        if (c == '\\') c = '/';
    while (k.rfind("./", 0) == 0) k = k.substr(2);
    auto it = files_.find(k);
    if (it != files_.end()) return it->second;
    // "~" fallback is handled by callers
    if (k.find('/') == std::string::npos) {
        auto wi = where_.find(extension(k));
        if (wi != where_.end())
            for (auto& d : wi->second) {
                auto f = files_.find(d.empty() ? k : d + "/" + k);
                if (f != files_.end()) return f->second;
            }
    }
    return "";
}

bool FileSystem::exists(const std::string& name) const {
    return sim_.has(basename(name)) || shell_.has(basename(name)) || !findLoose(name).empty();
}

bool FileSystem::read(const std::string& name, Bytes& out) const {
    std::string b = basename(name);
    if (sim_.read(b, out)) return true;
    if (shell_.read(b, out)) return true;
    std::string p = findLoose(name);
    if (!p.empty()) return readFile(p, out);
    // compressed variant: last char replaced by '~'
    std::string t = name;
    if (!t.empty()) {
        t.back() = '~';
        p = findLoose(t);
        if (p.empty() && t.find_first_of("/\\") == std::string::npos) {
            auto wi = where_.find(extension(name));
            if (wi != where_.end())
                for (auto& d : wi->second)
                    if (p.empty()) p = findLoose(d.empty() ? t : d + "/" + t);
        }
        Bytes c;
        if (!p.empty() && readFile(p, c)) {
            out = lzbDecompress(c.data(), c.size());
            return !out.empty();
        }
    }
    return false;
}

// ---------------------------------------------------------------- Reader
int32_t Reader::i32() {
    int32_t v = 0;
    if (ok(4)) memcpy(&v, p, 4);
    p += 4;
    return v;
}
uint16_t Reader::u16() {
    uint16_t v = 0;
    if (ok(2)) memcpy(&v, p, 2);
    p += 2;
    return v;
}
uint8_t Reader::u8() {
    uint8_t v = ok(1) ? *p : 0;
    p += 1;
    return v;
}
float Reader::f32() {
    float v = 0;
    if (ok(4)) memcpy(&v, p, 4);
    p += 4;
    return v;
}
double Reader::f64() {
    double v = 0;
    if (ok(8)) memcpy(&v, p, 8);
    p += 8;
    return v;
}
std::string Reader::str() {
    const uint8_t* s = p;
    while (p < end && *p) p++;
    std::string r(reinterpret_cast<const char*>(s), size_t(p - s));
    if (p < end) p++;
    return r;
}

uint32_t crc32hg(const uint8_t* d, size_t n) {
    static uint32_t tbl[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1;
            tbl[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = (c << 8) ^ tbl[(c >> 24) ^ d[i]];
    return ~c;
}

}  // namespace hg2
