// Game data access: loose files (case-insensitive), PRJ archives, LZB (.xx~) files.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hg2 {

using Bytes = std::vector<uint8_t>;

std::string lower(std::string s);
std::string upper(std::string s);
std::string basename(const std::string& p);
std::string extension(const std::string& p);  // lowercase, without dot

bool readFile(const std::string& path, Bytes& out);
Bytes lzbDecompress(const uint8_t* d, size_t n);  // HG2 "~" file compression

// One PRJ archive (simpack / hg2shell): name "BAA10005.M" -> entry.
class PrjArchive {
public:
    bool open(const std::string& prjPath, const std::string& mapPath);
    bool has(const std::string& name) const;
    bool read(const std::string& name, Bytes& out) const;
    std::vector<std::string> names() const;
private:
    struct Entry { uint32_t offset; };
    std::string prjPath_;
    std::unordered_map<std::string, Entry> entries_;  // key: upper "NAME.EXT"
};

// Resolves game files the way the original does: archives first, then the
// directories listed in the "where" file for the extension, then the data root.
class FileSystem {
public:
    // root: directory holding hg2sim.dll / simpack.prj (installed game or CD Setup\data)
    bool mount(const std::string& root);
    const std::string& root() const { return root_; }
    bool read(const std::string& name, Bytes& out) const;          // by bare name or relative path
    bool exists(const std::string& name) const;
    std::string findLoose(const std::string& relPath) const;      // actual path or ""
    const PrjArchive& sim() const { return sim_; }
    const PrjArchive& shell() const { return shell_; }
private:
    void indexDir(const std::string& dir, const std::string& rel, int depth);
    std::string root_;
    std::unordered_map<std::string, std::string> files_;   // lower rel path -> real path
    std::unordered_map<std::string, std::string> byName_;  // lower basename -> real path (first)
    std::unordered_map<std::string, std::vector<std::string>> where_;  // ext -> dirs
    PrjArchive sim_, shell_;
};

// Little-endian cursor over token streams / binary files.
struct Reader {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    Reader() = default;
    Reader(const uint8_t* b, size_t n) : p(b), end(b + n) {}
    explicit Reader(const Bytes& b) : p(b.data()), end(b.data() + b.size()) {}
    bool ok(size_t n = 0) const { return p + n <= end; }
    size_t left() const { return size_t(end - p); }
    int32_t i32();
    uint32_t u32() { return uint32_t(i32()); }
    uint16_t u16();
    uint8_t u8();
    float f32();
    double f64();
    std::string str();
    void skip(size_t n) { p += n; }
};

uint32_t crc32hg(const uint8_t* d, size_t n);  // CRC-32/MPEG-2 style used by token blocks

}  // namespace hg2
