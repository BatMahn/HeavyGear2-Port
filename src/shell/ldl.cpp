#include "ldl.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdlib>

namespace hg2 {

const LdlNode* LdlNode::child(const std::string& n) const {
    std::string k = lower(n);
    for (auto& c : children)
        if (lower(c->name) == k) return c.get();
    return nullptr;
}

namespace {
struct Lexer {
    const std::string& s;
    size_t p = 0;
    explicit Lexer(const std::string& str) : s(str) {}
    // tokens: words (anything up to whitespace / brace / quote), "strings", { and }
    bool next(std::string& tok, bool& quoted) {
        quoted = false;
        for (;;) {
            while (p < s.size() && isspace((unsigned char)s[p])) p++;
            if (p >= s.size()) return false;
            if (s[p] == '#') {
                while (p < s.size() && s[p] != '\n') p++;
                continue;
            }
            break;
        }
        if (s[p] == '$') return false;
        if (s[p] == '{' || s[p] == '}') {
            tok.assign(1, s[p++]);
            return true;
        }
        if (s[p] == '"') {
            size_t e = s.find('"', p + 1);
            if (e == std::string::npos) e = s.size();
            tok = s.substr(p + 1, e - p - 1);
            p = e + 1;
            quoted = true;
            return true;
        }
        size_t b = p;
        while (p < s.size() && !isspace((unsigned char)s[p]) && s[p] != '{' && s[p] != '}' && s[p] != '"') p++;
        tok = s.substr(b, p - b);
        return true;
    }
};
std::string trim(std::string t) {
    while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
    while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
    return t;
}
}  // namespace

bool Ldl::load(const FileSystem& fs, const std::string& file) { return parse(fs, file, 0); }

bool Ldl::parse(const FileSystem& fs, const std::string& file, int depth) {
    std::string key = lower(file);
    for (auto& l : loaded_)
        if (l == key) return true;
    if (depth > 8) return false;
    Bytes b;
    if (!fs.read(file, b) && !fs.read("shell/ldl/" + file, b)) {
        SDL_Log("ldl: %s not found", file.c_str());
        return false;
    }
    loaded_.push_back(key);
    std::string src(b.begin(), b.end());
    Lexer lx(src);
    std::vector<LdlNode*> stack;
    std::vector<std::string> pending;  // words of the statement being read
    std::string tok;
    bool quoted;
    auto openNode = [&](bool isTemplate) {
        // pending: [template] name [: bases] [junk]  ->  join and split on ':'
        std::string all;
        for (auto& w : pending) all += (all.empty() ? "" : " ") + w;
        pending.clear();
        std::vector<std::string> parts;
        size_t q = 0;
        while (q <= all.size()) {
            size_t c = all.find(':', q);
            if (c == std::string::npos) c = all.size();
            parts.push_back(trim(all.substr(q, c - q)));
            q = c + 1;
        }
        auto n = std::make_unique<LdlNode>();
        n->name = parts.empty() ? std::string() : parts[0];
        n->isTemplate = isTemplate;
        for (size_t i = 1; i < parts.size(); i++)
            if (!parts[i].empty()) n->bases.push_back(lower(parts[i]));
        LdlNode* raw = n.get();
        if (stack.empty()) {
            std::string k = lower(raw->name);
            if (!top_.count(k)) top_[k] = raw;
            roots_.push_back(std::move(n));
        } else {
            raw->parent = stack.back();
            stack.back()->children.push_back(std::move(n));
        }
        stack.push_back(raw);
    };
    bool isTemplate = false;
    while (lx.next(tok, quoted)) {
        if (!quoted && tok == "{") {
            openNode(isTemplate);
            isTemplate = false;
            continue;
        }
        if (!quoted && tok == "}") {
            pending.clear();
            isTemplate = false;
            if (!stack.empty()) stack.pop_back();
            continue;
        }
        if (!quoted && pending.empty() && stack.empty() && lower(tok) == "import") {
            if (lx.next(tok, quoted)) parse(fs, tok, depth + 1);
            continue;
        }
        if (!quoted && pending.empty() && lower(tok) == "template") {
            isTemplate = true;
            continue;
        }
        std::string lt = lower(tok);
        if (!quoted && pending.empty() && !stack.empty() &&
            (lt == "int" || lt == "string" || lt == "bool" || lt == "float" || lt == "double")) {
            std::string k, v;
            bool q2;
            if (!lx.next(k, q2) || !lx.next(v, q2)) break;
            stack.back()->props[lower(k)] = v;
            continue;
        }
        pending.push_back(tok);
    }
    return true;
}

const LdlNode* Ldl::resolve(const std::string& ref) const { return find(ref); }

const LdlNode* Ldl::childOf(const LdlNode* n, const std::string& name) const {
    if (!n) return nullptr;
    for (const LdlNode* c : children(n))
        if (lower(c->name) == lower(name)) return c;
    return nullptr;
}

const LdlNode* Ldl::find(const std::string& path) const {
    std::string p = lower(path);
    size_t dot = p.find('.');
    auto it = top_.find(p.substr(0, dot));
    if (it == top_.end()) return nullptr;
    const LdlNode* n = it->second;
    while (dot != std::string::npos && n) {
        size_t e = p.find('.', dot + 1);
        n = childOf(n, p.substr(dot + 1, e == std::string::npos ? std::string::npos : e - dot - 1));
        dot = e;
    }
    return n;
}

const std::string* Ldl::propRec(const LdlNode* n, const std::string& key, int depth) const {
    if (!n || depth > 16) return nullptr;
    auto it = n->props.find(key);
    if (it != n->props.end()) return &it->second;
    for (auto& b : n->bases) {
        const LdlNode* bn = find(b);
        if (bn && bn != n)
            if (const std::string* v = propRec(bn, key, depth + 1)) return v;
    }
    return nullptr;
}

const std::string* Ldl::prop(const LdlNode* n, const std::string& key) const { return propRec(n, lower(key), 0); }

int Ldl::getInt(const LdlNode* n, const std::string& key, int def) const {
    const std::string* v = prop(n, key);
    return v ? atoi(v->c_str()) : def;
}
bool Ldl::getBool(const LdlNode* n, const std::string& key, bool def) const {
    const std::string* v = prop(n, key);
    if (!v) return def;
    std::string l = lower(*v);
    return l == "true" || l == "1" || l == "yes";
}
std::string Ldl::getStr(const LdlNode* n, const std::string& key, const std::string& def) const {
    const std::string* v = prop(n, key);
    return v ? *v : def;
}

void Ldl::childrenRec(const LdlNode* n, std::vector<const LdlNode*>& out, std::vector<std::string>& seen, int depth) const {
    if (!n || depth > 16) return;
    for (auto& c : n->children) {
        std::string k = lower(c->name);
        bool dup = false;
        for (auto& s : seen) dup |= s == k;
        if (dup) continue;
        seen.push_back(k);
        out.push_back(c.get());
    }
    for (auto& b : n->bases) {
        const LdlNode* bn = find(b);
        if (bn && bn != n) childrenRec(bn, out, seen, depth + 1);
    }
}

std::vector<const LdlNode*> Ldl::children(const LdlNode* n) const {
    std::vector<const LdlNode*> out;
    std::vector<std::string> seen;
    childrenRec(n, out, seen, 0);
    return out;
}

}  // namespace hg2
