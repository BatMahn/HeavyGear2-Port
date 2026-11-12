// Shell artwork: the ZFS3 containers the installer unpacks into Shell\shellart (rim565.zfs: 16-bit RGB565
// images "RIMF", alp.zfs: 8-bit alpha masks "ALPH"). The LDL layouts name the art as .TGA/.BMP files; the
// shell (shell.dll) looks the base name up as name.rim + name.alp. When the game is run straight from the
// CD's Setup\data folder the containers are still inside Setup\zip\zfs.zip, which is read directly.
#pragma once
#include <string>
#include <unordered_map>

#include "../assets/assets.h"
#include "fs.h"

namespace hg2 {

class ZfsArchive {
public:
    bool load(Bytes data);  // takes ownership
    bool has(const std::string& name) const { return dir_.count(lower(name)) != 0; }
    bool read(const std::string& name, const uint8_t*& p, size_t& n) const;
    size_t count() const { return dir_.size(); }
private:
    Bytes data_;
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> dir_;  // lower name -> offset, size
};

// Minimal reader for the installer's zip (stored / deflated entries).
bool zipExtract(const std::string& zipPath, const std::string& entry, Bytes& out);

class ShellArt {
public:
    bool open(const FileSystem& fs);
    bool ok() const { return rim_.count() > 0; }
    // "SBTN006U.TGA" -> RGBA (alpha from the matching .alp, opaque otherwise)
    bool image(const std::string& name, Image& out) const;
    std::string source() const { return source_; }
private:
    ZfsArchive rim_, alp_;
    std::string source_;
};

}  // namespace hg2
