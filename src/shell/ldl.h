// LDL: the shell's layout description language (hg2shell.prj *.LDL, parsed by shell.dll).
//   import "file.ldl"                      pull in another layout file
//   [template] name[:base1:base2...] {     a node; bases are other nodes (templates or ordinary nodes, a
//       int|bool|string key value          dotted path such as lows.back reaches a child) whose properties
//       child[:bases] { ... }              and children are inherited in order
//   }
//   # comment, $ ends the file
// Coordinates are pixels on the 640x480 shell screen: xpix/ypix relative to the parent, xanchor/yanchor
// "center"/"right"/"bottom" align within the parent, widthpct/heightpct/xpct/ypct are parent percentages.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../core/fs.h"

namespace hg2 {

struct LdlNode {
    std::string name;                         // as written (lowercase lookups)
    std::vector<std::string> bases;           // base references, lowercase
    std::map<std::string, std::string> props; // lowercase key -> raw value (strings unquoted)
    std::vector<std::unique_ptr<LdlNode>> children;
    LdlNode* parent = nullptr;
    bool isTemplate = false;
    const LdlNode* child(const std::string& n) const;  // own child only
};

class Ldl {
public:
    // Loads `file` (and its imports) from the shell archive / Shell\ldl folder.
    bool load(const FileSystem& fs, const std::string& file);
    // Top-level node or dotted path ("escape.bkgnd.splash"); inherited children are followed.
    const LdlNode* find(const std::string& path) const;
    // Property lookup through the node's own values, then each base (depth first, in order).
    const std::string* prop(const LdlNode* n, const std::string& key) const;
    int getInt(const LdlNode* n, const std::string& key, int def = 0) const;
    bool getBool(const LdlNode* n, const std::string& key, bool def = false) const;
    std::string getStr(const LdlNode* n, const std::string& key, const std::string& def = std::string()) const;
    bool has(const LdlNode* n, const std::string& key) const { return prop(n, key) != nullptr; }
    // Children: own, then inherited from bases (names already present are skipped).
    std::vector<const LdlNode*> children(const LdlNode* n) const;
    const LdlNode* childOf(const LdlNode* n, const std::string& name) const;
    const LdlNode* resolve(const std::string& ref) const;  // a base reference
    size_t files() const { return loaded_.size(); }

private:
    bool parse(const FileSystem& fs, const std::string& file, int depth);
    const std::string* propRec(const LdlNode* n, const std::string& key, int depth) const;
    void childrenRec(const LdlNode* n, std::vector<const LdlNode*>& out, std::vector<std::string>& seen, int depth) const;
    std::vector<std::unique_ptr<LdlNode>> roots_;
    std::map<std::string, LdlNode*> top_;  // lowercase name -> node (first definition wins)
    std::vector<std::string> loaded_;
};

}  // namespace hg2
