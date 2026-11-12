// Heavy Gear II mission script VM.
//
// Scripts are compiled C-like programs (Scripting/*.o) executed by a threaded-code interpreter in
// hg2sim.dll. The instruction set (38 opcodes, two operand variants) comes from the linker at
// FUN_100c6700 and the handler/descriptor table at 0x101608b8; see docs/SCRIPTS.md.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/fs.h"

namespace hg2 {

enum ScriptType : uint32_t { T_VOID = 0, T_INT = 1, T_FLOAT = 2, T_VECTOR = 3, T_STRING = 4, T_HANDLE = 5 };

// Typed 16-byte value (registers and native-call return slot).
struct ScriptValue {
    uint32_t type = 0;
    uint32_t d[3] = {0, 0, 0};
    std::vector<uint8_t> big;  // payload for struct types (> 5)
    int32_t i() const;
    float f() const;
    uint8_t* data() { return big.empty() ? reinterpret_cast<uint8_t*>(d) : big.data(); }
    void setInt(int32_t v) { type = T_INT, big.clear(), d[0] = uint32_t(v); }
    void setFloat(float v);
    void setVec(float x, float y, float z);
};

// Global string table: script string literals and symbol names are interned here.
class StringTable {
public:
    int intern(const std::string& s);
    const std::string& get(int id) const;
private:
    std::vector<std::string> strings_{""};
    std::unordered_map<std::string, int> ids_;
};

class ScriptVM;
using NativeFn = std::function<void(ScriptVM&, const uint8_t* argTop, ScriptValue& ret)>;

// Engine side of the scripts: everything the builtins need from the game.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual float timeStep() { return 1.f / 30.f; }
    virtual void log(const std::string& s);
    // Fallback for builtins the port does not implement yet.
    virtual void unimplemented(ScriptVM& vm, const std::string& name, int overload);
};

struct ScriptInstruction {
    uint32_t offset = 0, size = 0;
    uint8_t op = 0, variant = 0;
    const uint8_t* operand = nullptr;  // points into the program's code
};

// One loaded .o file.
struct ScriptProgram {
    std::string name;
    Bytes blob;
    uint32_t h[25] = {};
    std::vector<ScriptInstruction> code;
    std::unordered_map<uint32_t, uint32_t> byOffset;  // byte offset -> instruction index
    std::vector<int> strings;                          // local string index -> global id
    struct Symbol {
        int name = 0;       // global string id
        uint32_t value = 0;
        uint8_t resolved = 0, used = 0, native = 0, overload = 0;
        uint32_t kind = 0;  // 1 = function
    };
    std::vector<Symbol> symbols;      // h[9]/h[10]
    struct Event { uint8_t b0, flags; uint16_t id; uint32_t offset; };
    std::vector<Event> events;        // h[5] entries following the code
    struct Global { int name; uint32_t offset; uint32_t type; };
    std::vector<Global> globals;      // h[11]/h[12] exported variables
    struct Extern { int name; uint32_t a, b; };
    std::vector<Extern> externs;      // h[15]/h[16] imported variables
    std::unordered_map<uint32_t, uint32_t> typeSize;  // struct types (h[17]/h[18])
    std::unordered_map<std::string, uint32_t> typeIds; // struct type name -> id
    uint32_t dataSize = 0;
    bool load(const Bytes& data, const std::string& name, StringTable& st, std::string* err);
    uint32_t sizeOfType(uint32_t t) const;
};

class ScriptVM {
public:
    ScriptVM(std::shared_ptr<ScriptProgram> prog, ScriptHost& host, StringTable& st, int owner);

    // Event slots 0..63 (signals, timers, states). Mirrors FUN_100c6bc0.
    void signal(int id);
    void enableState(int id, bool on);
    void markBreak(int id) {  // AutoBreak: slot fires when the unit's autopilot is interrupted (flag 8)
        if (id >= 0 && id < 64 && events_[id].present) events_[id].flags |= 8;
    }
    // AutoIgnore (FUN_100d7310) clears the break and abort flags (8, 0x10); AutoAbort (FUN_100d7370) sets 0x10.
    // A slot with either flag fires even while its state is disabled.
    void eventFlags(int id, uint8_t set, uint8_t clear) {
        if (id >= 0 && id < 64 && events_[id].present) events_[id].flags = uint8_t((events_[id].flags & ~clear) | set);
    }
    bool pending() const {  // a handler is running or signalled
        if (pc_ >= 0) return true;
        for (auto& e : events_)
            if (e.present && (e.flags & 2)) return true;
        return false;
    }
    bool persistent = false;  // Persistent(TRUE): the script outlives its unit (brain +0x194 bit 0)
    // Runs pending handlers (FUN_100c6ac0). Each run is limited to 20000 instructions per tick.
    void tick();

    bool terminated() const { return terminated_; }
    void terminate() { terminated_ = true; }
    int owner() const { return owner_; }
    ScriptHost& host() { return host_; }
    StringTable& strings() { return st_; }
    const ScriptProgram& program() const { return *prog_; }
    uint8_t* globalData() { return data_.data(); }
    std::string where() const;
    uint32_t typeId(const std::string& name) const;  // struct type of the executing program (0 if none)

    // Native bindings (name, overload) -> function. Unbound natives call ScriptHost::unimplemented.
    static void bind(const std::string& name, int overload, NativeFn fn);
    static bool isBound(const std::string& name, int overload);
    // Shared exported globals (all loaded scripts of the mission).
    static std::unordered_map<int, std::pair<uint8_t*, uint32_t>>& exportedGlobals();
    static void resetGlobals();
    // Functions exported by loaded scripts (symbols with the resolved flag), callable by name.
    static void exportFunctions(const std::shared_ptr<ScriptProgram>& p);

    uint64_t executed = 0;

private:
    struct Addr { uint32_t w0, w1; };
    uint8_t* resolve(const Addr& a);  // FUN_100cb4d0
    uint32_t typeOf(const Addr& a) const { return (a.w0 >> 16) & 0xfff; }
    uint32_t sizeOf(uint32_t t) const { return prog_->sizeOfType(t); }
    void run(uint32_t offset);
    bool step();
    void binop(const ScriptInstruction& in);
    void callNative(int sym);
    void ret(int32_t pop);
    int32_t localString(int32_t idx) const {
        return idx >= 0 && size_t(idx) < code_->strings.size() ? code_->strings[size_t(idx)] : 0;
    }

    std::shared_ptr<ScriptProgram> prog_;
    ScriptHost& host_;
    StringTable& st_;
    int owner_;
    std::vector<uint8_t> data_, static_;
    std::vector<uint8_t> stack_;
    uint32_t sp_ = 0, fp_ = 0, pending_ = 0;
    ScriptValue reg_[16];
    std::vector<ScriptValue> switchStack_;
    int pc_ = -1;  // instruction index, -1 = idle
    const ScriptProgram* code_ = nullptr;  // program whose code is executing (may be a library)
    std::map<uint32_t, std::pair<const ScriptProgram*, int>> returnAddr_;  // stack slot -> return point
    std::map<const ScriptProgram*, std::vector<uint8_t*>> externCache_;
    bool terminated_ = false;
    struct EventState { uint8_t flags; uint32_t offset; bool present; };
    EventState events_[64] = {};
};

}  // namespace hg2
