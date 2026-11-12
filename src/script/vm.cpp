#include <SDL3/SDL.h>
#include "vm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hg2 {

namespace {
const uint32_t kTypeSize[6] = {0, 4, 4, 12, 4, 4};  // DAT_10160958
constexpr size_t kStackSize = 64 * 1024;
constexpr int kStepBudget = 20000;  // FUN_100c6ac0

std::map<std::pair<std::string, int>, NativeFn>& natives() {
    static std::map<std::pair<std::string, int>, NativeFn> m;
    return m;
}
std::unordered_map<int, std::pair<uint8_t*, uint32_t>> g_exports;
std::unordered_map<int, std::pair<std::shared_ptr<ScriptProgram>, uint32_t>> g_functions;

float asF(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
uint32_t fromF(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}
int32_t rd32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}
}  // namespace

// ------------------------------------------------------------------ values / strings
int32_t ScriptValue::i() const { return type == T_FLOAT ? int32_t(asF(d[0])) : int32_t(d[0]); }
float ScriptValue::f() const { return type == T_FLOAT ? asF(d[0]) : float(int32_t(d[0])); }
void ScriptValue::setFloat(float v) { type = T_FLOAT, big.clear(), d[0] = fromF(v); }
void ScriptValue::setVec(float x, float y, float z) {
    type = T_VECTOR, big.clear(), d[0] = fromF(x), d[1] = fromF(y), d[2] = fromF(z);
}

int StringTable::intern(const std::string& s) {
    auto it = ids_.find(s);
    if (it != ids_.end()) return it->second;
    strings_.push_back(s);
    return ids_[s] = int(strings_.size()) - 1;
}
const std::string& StringTable::get(int id) const {
    static const std::string empty;
    return id >= 0 && size_t(id) < strings_.size() ? strings_[size_t(id)] : empty;
}

void ScriptHost::log(const std::string& s) { fprintf(stderr, "[script] %s\n", s.c_str()); }
void ScriptHost::unimplemented(ScriptVM& vm, const std::string& name, int overload) {
    log(vm.where() + ": builtin not implemented: " + name + "/" + std::to_string(overload));
}

// ------------------------------------------------------------------ program loading
uint32_t ScriptProgram::sizeOfType(uint32_t t) const {
    if (t < 6) return kTypeSize[t];
    auto it = typeSize.find(t);
    return it == typeSize.end() ? 0 : it->second;
}

bool ScriptProgram::load(const Bytes& data, const std::string& nm, StringTable& st, std::string* err) {
    name = nm;
    if (data.size() < 100) {
        if (err) *err = nm + ": too small";
        return false;
    }
    memcpy(h, data.data(), sizeof h);
    if (h[0] != 1 || 100 + size_t(h[2]) > data.size()) {
        if (err) *err = nm + ": bad header";
        return false;
    }
    blob.assign(data.begin() + 100, data.begin() + 100 + h[2]);
    const uint8_t* b = blob.data();
    auto inBlob = [&](uint32_t off, uint64_t n) { return uint64_t(off) + n <= blob.size(); };
    // string table: u8 length (incl. NUL) + chars
    if (!inBlob(h[20], 0)) return false;
    for (uint32_t i = 0, o = h[20]; i < h[19] && o < blob.size(); i++) {
        uint8_t n = b[o];
        std::string s(reinterpret_cast<const char*>(b + o + 1), n ? n - 1 : 0);
        strings.push_back(st.intern(s));
        o += 1 + n;
    }
    auto str = [&](uint32_t idx) { return idx < strings.size() ? strings[idx] : 0; };
    if (!inBlob(h[18], uint64_t(h[17]) * 12) || !inBlob(h[10], uint64_t(h[9]) * 16) ||
        !inBlob(h[12], uint64_t(h[11]) * 12) || !inBlob(h[16], uint64_t(h[15]) * 12) ||
        !inBlob(h[3], uint64_t(h[5]) * 12)) {
        if (err) *err = nm + ": table out of range";
        return false;
    }
    for (uint32_t i = 0; i < h[17]; i++) {
        const uint8_t* e = b + h[18] + 12 * i;
        uint16_t id;
        memcpy(&id, e + 4, 2);
        typeSize[id] = uint32_t(rd32(e + 8));
        typeIds[st.get(str(uint32_t(rd32(e))))] = id;
    }
    for (uint32_t i = 0; i < h[9]; i++) {
        const uint8_t* e = b + h[10] + 16 * i;
        Symbol s;
        s.name = str(uint32_t(rd32(e)));
        s.value = uint32_t(rd32(e + 4));
        s.resolved = e[8], s.used = e[9], s.native = e[10], s.overload = e[11];
        s.kind = uint32_t(rd32(e + 12));
        symbols.push_back(s);
    }
    for (uint32_t i = 0; i < h[11]; i++) {
        const uint8_t* e = b + h[12] + 12 * i;
        globals.push_back({str(uint32_t(rd32(e))), uint32_t(rd32(e + 4)), uint32_t(rd32(e + 8))});
    }
    for (uint32_t i = 0; i < h[15]; i++) {
        const uint8_t* e = b + h[16] + 12 * i;
        externs.push_back({str(uint32_t(rd32(e))), uint32_t(rd32(e + 4)), uint32_t(rd32(e + 8))});
    }
    for (uint32_t i = 0; i < h[5]; i++) {
        const uint8_t* e = b + h[3] + 12 * i;
        Event ev;
        ev.b0 = e[0], ev.flags = e[1];
        memcpy(&ev.id, e + 2, 2);
        ev.offset = uint32_t(rd32(e + 4));
        events.push_back(ev);
    }
    dataSize = h[7];
    // decode instructions (sizes follow the linker's operand fix-ups)
    uint32_t p = 0;
    auto immSize = [&](uint32_t q) { return 4 + sizeOfType(uint32_t(rd32(b + q))); };
    while (p < h[3]) {
        uint32_t w = uint32_t(rd32(b + p));
        ScriptInstruction in;
        in.offset = p;
        in.op = uint8_t((w >> 2) & 0x7f);
        in.variant = uint8_t(w & 3);
        uint32_t q = p + 4;
        in.operand = b + q;
        switch (in.op) {
            case 0: case 7: case 0x11: break;
            case 1: case 2: case 3: case 5: case 9: case 0xd: case 0x25: q += 4; break;
            case 6: q += 1; break;
            case 4: case 0xa: case 0xc: case 0x10: q += 4 + (in.variant ? 8 : immSize(q + 4)); break;
            case 8: q += in.variant ? 8 : immSize(q); break;
            case 0xf: q += 8; break;
            case 0x24: q += 8 + (in.variant ? 8 : immSize(q + 8)); break;
            default:
                if (in.op >= 0x12 && in.op <= 0x23) {
                    q += 1 + 8 + (in.variant ? 8 : immSize(q + 9));
                    break;
                }
                if (err) *err = nm + ": unknown opcode " + std::to_string(in.op);
                return false;
        }
        in.size = q - p;
        byOffset[p] = uint32_t(code.size());
        code.push_back(in);
        p = q;
    }
    if (p != h[3]) {
        if (err) *err = nm + ": code length mismatch";
        return false;
    }
    byOffset[p] = uint32_t(code.size());  // one-past-end target
    return true;
}

// ------------------------------------------------------------------ VM
void ScriptVM::bind(const std::string& name, int overload, NativeFn fn) { natives()[{name, overload}] = std::move(fn); }
bool ScriptVM::isBound(const std::string& name, int overload) { return natives().count({name, overload}) != 0; }
std::unordered_map<int, std::pair<uint8_t*, uint32_t>>& ScriptVM::exportedGlobals() { return g_exports; }
void ScriptVM::resetGlobals() {
    g_exports.clear();
    g_functions.clear();
}
void ScriptVM::exportFunctions(const std::shared_ptr<ScriptProgram>& p) {
    for (auto& sym : p->symbols)
        if (sym.resolved && !sym.native && sym.kind == 0) g_functions.emplace(sym.name, std::make_pair(p, sym.value));
}

ScriptVM::ScriptVM(std::shared_ptr<ScriptProgram> prog, ScriptHost& host, StringTable& st, int owner)
    : prog_(std::move(prog)), host_(host), st_(st), owner_(owner) {
    data_.assign(prog_->dataSize + 16, 0);
    static_.assign(prog_->h[8] + 16, 0);
    stack_.assign(kStackSize, 0);
    for (auto& g : prog_->globals)
        if (g.offset < data_.size()) g_exports.emplace(g.name, std::make_pair(data_.data() + g.offset, g.type));
    for (auto& e : prog_->events)
        if (e.id < 64) events_[e.id] = {uint8_t(e.flags | 5), e.offset, true};
}

std::string ScriptVM::where() const {
    int line = -1;
    if (pc_ >= 0)
        for (int i = pc_; i >= 0; i--)
            if (code_->code[size_t(i)].op == 0x25) {
                line = rd32(code_->code[size_t(i)].operand);
                break;
            }
    if (!code_) return prog_->name;
    return code_->name + (line >= 0 ? ":" + std::to_string(line) : "");
}

uint32_t ScriptVM::typeId(const std::string& name) const {
    const ScriptProgram* p = code_ ? code_ : prog_.get();
    auto it = p->typeIds.find(name);
    return it == p->typeIds.end() ? 0 : it->second;
}

void ScriptVM::signal(int id) {
    if (id < 0 || id >= 64 || !events_[id].present) return;
    uint8_t& f = events_[id].flags;
    if (f & 0x1c) f |= 6;
}

void ScriptVM::enableState(int id, bool on) {
    if (id < 0 || id >= 64 || !events_[id].present) return;
    if (on) events_[id].flags |= 4;
    else events_[id].flags &= uint8_t(~4);
}

void ScriptVM::tick() {
    if (terminated_) return;
    // A handler interrupted by the step budget continues first.
    if (pc_ >= 0) {
        for (int n = 0; n < kStepBudget && pc_ >= 0 && !terminated_; n++) step();
        if (pc_ >= 0) return;
    }
    for (int id = 0; id < 64 && !terminated_; id++) {
        EventState& e = events_[id];
        if (!e.present || !(e.flags & 2)) continue;
        if (pc_ < 0) {
            if (getenv("HG2_SIGDBG")) SDL_Log("[vm] %s handler %d at %u", where().c_str(), id, e.offset);
            run(e.offset);
        }
        for (int n = 0; n < kStepBudget && pc_ >= 0 && !terminated_; n++) step();
        e.flags &= uint8_t(~2);
        if (pc_ >= 0) return;
    }
}

void ScriptVM::run(uint32_t offset) {
    code_ = prog_.get();
    auto it = prog_->byOffset.find(offset);
    pc_ = it == prog_->byOffset.end() ? -1 : int(it->second);
    sp_ = fp_ = pending_ = 0;
}

uint8_t* ScriptVM::resolve(const Addr& a) {
    uint32_t idx = a.w0 & 0xffff, extra = a.w1 & 0xffff;
    switch ((a.w0 >> 28) & 7) {
        case 0:  // imported (extern) global
        {
            auto& cache = externCache_[code_];
            if (cache.empty()) {
                for (auto& e : code_->externs) {
                    auto it = g_exports.find(e.name);
                    cache.push_back(it == g_exports.end() ? nullptr : it->second.first);
                }
            }
            if (idx < cache.size() && cache[idx]) return cache[idx] + extra;
            host_.log(where() + ": unresolved extern variable " +
                      (idx < code_->externs.size() ? st_.get(code_->externs[idx].name) : std::to_string(idx)));
            return static_.data();
        }
        case 1: return idx + extra < data_.size() ? data_.data() + idx + extra : data_.data();
        case 2: return idx + extra < static_.size() ? static_.data() + idx + extra : static_.data();
        case 3: {
            int64_t o = int64_t(extra) + fp_ - idx;
            return o >= 0 && size_t(o) < stack_.size() ? stack_.data() + o : stack_.data();
        }
        case 4: {
            int64_t o = int64_t(extra) + sp_ - idx;
            return o >= 0 && size_t(o) < stack_.size() ? stack_.data() + o : stack_.data();
        }
        case 5: return reg_[idx & 15].data();
        default: return data_.data();
    }
}

// Typed copy into a register / memory slot with int<->float conversion (handlers 0x4, 0x6, 0x24).
static void convertInto(uint8_t* dst, uint32_t dstType, const uint8_t* src, uint32_t srcType, uint32_t size) {
    if (dstType == T_INT && srcType == T_FLOAT) {
        int32_t v = int32_t(asF(uint32_t(rd32(src))));
        memcpy(dst, &v, 4);
    } else if (dstType == T_FLOAT && srcType == T_INT) {
        float v = float(rd32(src));
        memcpy(dst, &v, 4);
    } else {
        memcpy(dst, src, size);
    }
}

static bool truthy(uint32_t type, const uint8_t* p) {
    switch (type) {
        case T_INT: return rd32(p) != 0;
        case T_FLOAT: return asF(uint32_t(rd32(p))) != 0.f;
        case T_VECTOR: return asF(uint32_t(rd32(p))) != 0.f || asF(uint32_t(rd32(p + 4))) != 0.f ||
                              asF(uint32_t(rd32(p + 8))) != 0.f;
        default: return true;
    }
}

void ScriptVM::ret(int32_t pop) {
    sp_ += uint32_t(pop);
    if (sp_ == 0) {
        fp_ = 0;
        pc_ = -1;
        return;
    }
    sp_ -= 4;
    fp_ = uint32_t(rd32(stack_.data() + sp_));
    if (sp_ == 0) {
        pc_ = -1;
        return;
    }
    sp_ -= 4;
    auto it = returnAddr_.find(sp_);
    if (it == returnAddr_.end()) {
        pc_ = -1;
        return;
    }
    code_ = it->second.first;
    pc_ = it->second.second;
}

void ScriptVM::callNative(int sym) {
    const auto& s = code_->symbols[size_t(sym)];
    const std::string& nm = st_.get(s.name);
    sp_ += pending_;
    pending_ = 0;
    ScriptValue& r0 = reg_[0];
    auto it = natives().find({nm, s.overload});
    static const bool trace = getenv("HG2_BTRACE") != nullptr;  // test: log every builtin call with its script line
    if (trace && nm != "GetTimeStep" && nm != "Wait") fprintf(stderr, "[bt] %s %s/%d\n", where().c_str(), nm.c_str(), s.overload);
    if (it != natives().end()) it->second(*this, stack_.data() + sp_, r0);
    else host_.unimplemented(*this, nm, s.overload);
}

void ScriptVM::binop(const ScriptInstruction& in) {
    const uint8_t* q = in.operand;
    uint8_t flags = q[0];
    Addr a1{uint32_t(rd32(q + 1)), uint32_t(rd32(q + 5))};
    uint32_t resType = typeOf(a1);
    uint8_t* memA = resolve(a1);
    uint32_t tA, tB;
    uint8_t bufA[12] = {}, bufB[12] = {};
    if (in.variant == 0) {
        uint32_t it = uint32_t(rd32(q + 9));
        const uint8_t* iv = q + 13;
        uint32_t isz = std::min<uint32_t>(12, sizeOf(it));
        int32_t strId = it == T_STRING ? localString(rd32(iv)) : 0;
        if (it == T_STRING) iv = reinterpret_cast<const uint8_t*>(&strId);
        // the operation is (first operand) op (second operand), computed below as B op A: B is the memory
        // operand unless flag 0x20 puts the immediate first ("x - 1", "count < 3", "UserAlive() > 0")
        if (!(flags & 0x20)) {
            tB = resType, memcpy(bufB, memA, std::min<uint32_t>(12, sizeOf(resType)));
            tA = it, memcpy(bufA, iv, isz);
        } else {
            tB = it, memcpy(bufB, iv, isz);
            tA = resType, memcpy(bufA, memA, std::min<uint32_t>(12, sizeOf(resType)));
        }
    } else {
        Addr a2{uint32_t(rd32(q + 9)), uint32_t(rd32(q + 13))};
        tB = resType, memcpy(bufB, memA, std::min<uint32_t>(12, sizeOf(resType)));
        tA = typeOf(a2), memcpy(bufA, resolve(a2), std::min<uint32_t>(12, sizeOf(typeOf(a2))));
    }
    // destination: register (low nibble) or in place
    uint8_t* dst;
    if (!(flags & 0x10)) {
        ScriptValue& r = reg_[flags & 15];
        r.type = resType;
        r.big.clear();
        if (resType > 5) r.big.assign(sizeOf(resType), 0);
        dst = r.data();
    } else {
        dst = memA;
    }
    // both operands are converted to the result type
    auto A_i = [&]() { return tA == T_FLOAT ? int32_t(asF(uint32_t(rd32(bufA)))) : rd32(bufA); };
    auto A_f = [&]() { return tA == T_FLOAT ? asF(uint32_t(rd32(bufA))) : float(rd32(bufA)); };
    int32_t Bi = tB == T_FLOAT ? int32_t(asF(uint32_t(rd32(bufB)))) : rd32(bufB);
    float Bf = tB == T_FLOAT ? asF(uint32_t(rd32(bufB))) : float(rd32(bufB));
    auto putI = [&](int32_t v) { memcpy(dst, &v, 4); };
    auto putF = [&](float v) { memcpy(dst, &v, 4); };
    auto arith = [&](auto fi, auto ff) {
        if (resType == T_INT) putI(fi(A_i(), Bi));
        else if (resType == T_FLOAT) putF(ff(A_f(), Bf));
        else if (resType == T_VECTOR)
            for (int k = 0; k < 3; k++) {
                float r = ff(asF(uint32_t(rd32(bufA + 4 * k))), asF(uint32_t(rd32(bufB + 4 * k))));
                memcpy(dst + 4 * k, &r, 4);
            }
    };
    // comparisons produce 1/0 in the result type's representation
    auto cmp = [&](auto fi, auto ff) {
        bool r = (resType == T_FLOAT || tA == T_FLOAT || tB == T_FLOAT) ? ff(A_f(), Bf) : fi(A_i(), Bi);
        if (resType == T_FLOAT) putF(r ? 1.f : 0.f);
        else putI(r ? 1 : 0);
    };
    switch (in.op) {
        case 0x12: arith([](int a, int b) { return a * b; }, [](float a, float b) { return a * b; }); break;
        case 0x13: arith([](int a, int b) { return a ? b / a : 0; }, [](float a, float b) { return b / a; }); break;
        case 0x14: { int32_t a = A_i(); putI(a ? Bi % a : 0); break; }
        case 0x15: arith([](int a, int b) { return a + b; }, [](float a, float b) { return a + b; }); break;
        case 0x16: arith([](int a, int b) { return b - a; }, [](float a, float b) { return b - a; }); break;
        case 0x17: putI(int32_t(uint32_t(Bi) << (A_i() & 31))); break;
        case 0x18: putI(Bi >> (A_i() & 31)); break;
        case 0x19: cmp([](int a, int b) { return b < a; }, [](float a, float b) { return b < a; }); break;
        case 0x1a: cmp([](int a, int b) { return a < b; }, [](float a, float b) { return a < b; }); break;
        case 0x1b: cmp([](int a, int b) { return b <= a; }, [](float a, float b) { return b <= a; }); break;
        case 0x1c: cmp([](int a, int b) { return a <= b; }, [](float a, float b) { return a <= b; }); break;
        case 0x1d: cmp([](int a, int b) { return a != b; }, [](float a, float b) { return a != b; }); break;
        case 0x1e: cmp([](int a, int b) { return a == b; }, [](float a, float b) { return a == b; }); break;
        case 0x1f: putI(A_i() & Bi); break;
        case 0x20: putI(A_i() ^ Bi); break;
        case 0x21: putI(A_i() | Bi); break;
        case 0x22: putI(truthy(tA, bufA) && truthy(tB, bufB) ? 1 : 0); break;
        case 0x23: putI(truthy(tA, bufA) || truthy(tB, bufB) ? 1 : 0); break;
    }
}

bool ScriptVM::step() {
    if (!code_) code_ = prog_.get();
    if (pc_ < 0 || size_t(pc_) >= code_->code.size()) {
        pc_ = -1;
        return false;
    }
    const ScriptInstruction& in = code_->code[size_t(pc_)];
    const ScriptProgram* callee = code_;
    const uint8_t* q = in.operand;
    int next = pc_ + 1;
    auto jumpRel = [&](uint32_t base, int32_t rel) {  // byte-relative to `base`
        auto it = code_->byOffset.find(uint32_t(int64_t(base) + rel));
        return it == code_->byOffset.end() ? -1 : int(it->second);
    };
    executed++;
    switch (in.op) {
        case 0: case 7: case 0x25: break;
        case 1: case 2: {  // call script function (relative / via symbol)
            int target = -1;
            if (in.op == 1) target = jumpRel(in.offset + in.size, rd32(q));
            else {
                int sym = rd32(q);
                if (sym >= 0 && size_t(sym) < code_->symbols.size()) {
                    const auto& s = code_->symbols[size_t(sym)];
                    auto fn = g_functions.find(s.name);
                    if (s.resolved && !s.native) {
                        auto it = code_->byOffset.find(s.value);
                        target = it == code_->byOffset.end() ? -1 : int(it->second);
                    } else if (s.kind == 0 && fn != g_functions.end()) {  // function in another script
                        callee = fn->second.first.get();
                        auto it = callee->byOffset.find(fn->second.second);
                        target = it == callee->byOffset.end() ? -1 : int(it->second);
                    } else {
                        callNative(sym);  // linker turns natives into op 3
                        break;
                    }
                }
            }
            sp_ += pending_;
            pending_ = 0;
            uint32_t frame = sp_;
            returnAddr_[sp_] = {code_, next};
            sp_ += 4;
            memcpy(stack_.data() + sp_, &fp_, 4);
            sp_ += 4;
            fp_ = frame;
            next = target;
            code_ = callee;
            break;
        }
        case 3: callNative(rd32(q)); break;
        case 4: case 6: {
            if (in.op == 6) {  // move r0 -> rN
                reg_[q[0] & 15] = reg_[0];
                break;
            }
            ScriptValue& r0 = reg_[0];
            int32_t pop = rd32(q);
            if (in.variant == 0) {
                uint32_t t = uint32_t(rd32(q + 4));
                r0.type = t;
                r0.big.assign(t > 5 ? sizeOf(t) : 0, 0);
                memcpy(r0.data(), q + 8, std::min<uint32_t>(sizeOf(t), t > 5 ? sizeOf(t) : 12));
                if (t == T_STRING) r0.d[0] = uint32_t(localString(rd32(q + 8)));
            } else {
                Addr a{uint32_t(rd32(q + 4)), uint32_t(rd32(q + 8))};
                uint32_t t = typeOf(a);
                r0.type = t;
                r0.big.assign(t > 5 ? sizeOf(t) : 0, 0);
                memcpy(r0.data(), resolve(a), std::min<uint32_t>(sizeOf(t), t > 5 ? sizeOf(t) : 12));
            }
            ret(pop);
            next = pc_;
            break;
        }
        case 5: ret(rd32(q)); next = pc_; break;
        case 8: {  // push call argument
            uint8_t* dst = stack_.data() + sp_ + pending_;
            uint32_t t, sz;
            if (in.variant == 0) {
                t = uint32_t(rd32(q));
                sz = sizeOf(t);
                memcpy(dst, q + 4, sz);
                if (t == T_STRING) {  // linker remaps string literals to global string ids
                    int32_t id = localString(rd32(q + 4));
                    memcpy(dst, &id, 4);
                }
            } else {
                Addr a{uint32_t(rd32(q)), uint32_t(rd32(q + 4))};
                t = typeOf(a);
                sz = sizeOf(t);
                memcpy(dst, resolve(a), sz);
            }
            pending_ += sz;
            break;
        }
        case 9: sp_ += uint32_t(rd32(q)); break;
        case 0xa: case 0xc: {
            int32_t rel = rd32(q);
            bool v;
            if (in.variant == 0) v = truthy(uint32_t(rd32(q + 4)), q + 8);
            else {
                Addr a{uint32_t(rd32(q + 4)), uint32_t(rd32(q + 8))};
                v = truthy(typeOf(a), resolve(a));
            }
            if ((in.op == 0xa) ? !v : v) next = jumpRel(in.offset + in.size, rel);
            break;
        }
        case 0xd: next = jumpRel(in.offset + 8, rd32(q)); break;
        case 0xf: {
            Addr a{uint32_t(rd32(q)), uint32_t(rd32(q + 4))};
            ScriptValue v;
            v.type = typeOf(a);
            memcpy(v.d, resolve(a), std::min<uint32_t>(12, sizeOf(v.type)));
            switchStack_.push_back(v);
            break;
        }
        case 0x10: {
            int32_t rel = rd32(q);
            if (switchStack_.empty()) break;
            const ScriptValue& top = switchStack_.back();
            uint32_t t, raw;
            if (in.variant == 0) {
                t = uint32_t(rd32(q + 4)), raw = uint32_t(rd32(q + 8));
                if (t == T_STRING) raw = uint32_t(localString(int32_t(raw)));
            }
            else {
                Addr a{uint32_t(rd32(q + 4)), uint32_t(rd32(q + 8))};
                t = typeOf(a), raw = uint32_t(rd32(resolve(a)));
            }
            bool differ;
            if (t == T_FLOAT && top.type == T_INT) differ = float(int32_t(raw)) != asF(top.d[0]);
            else if (t == T_FLOAT) differ = asF(raw) != asF(top.d[0]);
            else differ = top.d[0] != raw;
            if (differ) next = jumpRel(in.offset + in.size, rel);
            break;
        }
        case 0x11: if (!switchStack_.empty()) switchStack_.pop_back(); break;
        case 0x24: {
            Addr a{uint32_t(rd32(q)), uint32_t(rd32(q + 4))};
            uint32_t dt = typeOf(a);
            uint8_t* dst = resolve(a);
            if (in.variant == 0) {
                uint32_t st = uint32_t(rd32(q + 8));
                if (st == T_STRING) {
                    int32_t id = localString(rd32(q + 12));
                    memcpy(dst, &id, 4);
                } else {
                    convertInto(dst, dt, q + 12, st, sizeOf(dt));
                }
            }
            else {
                Addr b{uint32_t(rd32(q + 8)), uint32_t(rd32(q + 12))};
                convertInto(dst, dt, resolve(b), typeOf(b), sizeOf(dt));
            }
            break;
        }
        default:
            if (in.op >= 0x12 && in.op <= 0x23) binop(in);
            break;
    }
    pc_ = next;
    return pc_ >= 0;
}

}  // namespace hg2
