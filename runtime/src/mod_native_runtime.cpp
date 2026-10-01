#include "mod_native_runtime.h"
#include "mod_native_api.h"
#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_sha256.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* Supplied by the runtime (test fixtures implement a guest dispatcher). */
extern "C" void psx_native_mod_advance_cycles(uint32_t cycles);

namespace PSXRecompV4 {
namespace {
namespace fs = std::filesystem;
struct Loaded {
    ModResolution::NativeModule config;
    PSXNativeHost host{};
    const PSXNativeMod* mod = nullptr;
    void* library = nullptr;
    bool started = false;
    ~Loaded() {
        if (started && mod && mod->stop) {
            try { mod->stop(mod->userdata); } catch (...) { }
        }
        if (library) {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(library));
#else
            dlclose(library);
#endif
        }
    }
};
struct Session {
    std::vector<std::unique_ptr<Loaded>> modules;
    struct Binding { Loaded* loaded; const ModNativeHook* hook; };
    std::map<uint32_t, std::vector<Binding>> hooks;
    std::map<uint32_t, Binding> blocks;
    fs::path directory;
    bool active = false;
    uint32_t calls = 0;
    ~Session() {
        hooks.clear();
        while (!modules.empty()) modules.pop_back();
        std::error_code ec;
        if (!directory.empty()) fs::remove_all(directory, ec);
    }
};
std::unique_ptr<Session>& session() {
    static std::unique_ptr<Session> value;
    return value;
}
std::vector<std::unique_ptr<Session>>& retired_sessions() {
    static std::vector<std::unique_ptr<Session>> value;
    return value;
}
void collect_retired() {
    auto& retired = retired_sessions();
    retired.erase(std::remove_if(retired.begin(), retired.end(),
        [](const auto& item) { return item->calls == 0; }), retired.end());
}
void replace_session(std::unique_ptr<Session> replacement) {
    auto previous = std::move(session());
    session() = std::move(replacement);
    if (previous && previous->calls) retired_sessions().push_back(std::move(previous));
    collect_retired();
}
bool same_configuration(const Session& current, const ModResolution& plan) {
    if (current.modules.size() != plan.native_modules.size()) return false;
    for (size_t i = 0; i < current.modules.size(); ++i) {
        const auto& old = current.modules[i]->config;
        const auto& now = plan.native_modules[i];
        if (old.package_id != now.package_id || old.options != now.options ||
            old.module.feature_id != now.module.feature_id || old.module.id != now.module.id ||
            old.module.file != now.module.file || old.module.platform != now.module.platform ||
            old.module.sha256 != now.module.sha256 || old.module.hooks.size() != now.module.hooks.size()) return false;
        for (size_t h = 0; h < old.module.hooks.size(); ++h)
            if (old.module.hooks[h].address != now.module.hooks[h].address ||
                old.module.hooks[h].expected != now.module.hooks[h].expected ||
                old.module.hooks[h].resume_address != now.module.hooks[h].resume_address) return false;
    }
    return true;
}
/* Only the immediately following original dispatch bypasses hooks. Nested
 * guest calls, including recursion to the same address, can still be hooked.
 * Consumed before guest execution, so an IRQ longjmp leaves no stale bypass. */
thread_local bool bypass_once = false;

struct SymbolCatalog {
    const ModNativeSymbol* symbols = nullptr;
    uint32_t count = 0;
    ModNativeSymbolAvailable available = nullptr;
};
SymbolCatalog& symbol_catalog() {
    static SymbolCatalog value;
    return value;
}
struct FunctionReference {
    const ModNativeSymbol* symbol = nullptr;
    uint32_t address = 0, offset = 0;
    int status = PSX_NATIVE_FUNCTION_UNKNOWN;
};
bool parse_number(const char* begin, const char* end, uint32_t& value) {
    int base = 10;
    if (end - begin > 2 && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X')) {
        begin += 2; base = 16;
    }
    const auto parsed = std::from_chars(begin, end, value, base);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}
FunctionReference function_reference(const char* name) {
    FunctionReference result;
    if (!name) return result;
    size_t length = 0;
    while (length < 256 && name[length]) ++length;
    if (!length || length == 256) return result;
    const char* end = name + length;
    if (parse_number(name, end, result.address)) {
        if (!(result.address & 3u) && (result.address & 0x1fffffff) >= 0x10000u &&
            (result.address & 0x1fffffff) < 0x200000u) result.status = PSX_NATIVE_FUNCTION_FOUND;
        return result;
    }
    const char* plus = std::find(name, end, '+');
    if (plus != end && (!parse_number(plus + 1, end, result.offset) || (result.offset & 3u))) return result;
    char key[256];
    const size_t key_length = static_cast<size_t>(plus - name);
    std::memcpy(key, name, key_length);
    key[key_length] = '\0';
    const auto& catalog = symbol_catalog();
    if (!catalog.count) return result;
    const auto* finish = catalog.symbols + catalog.count;
    const auto* match = std::lower_bound(catalog.symbols, finish, key,
        [](const ModNativeSymbol& symbol, const char* text) { return std::strcmp(symbol.name, text) < 0; });
    if (match == finish || std::strcmp(key, match->name)) return result;
    if (match + 1 != finish && !std::strcmp(key, match[1].name)) {
        result.status = PSX_NATIVE_FUNCTION_AMBIGUOUS;
        return result;
    }
    const uint64_t address = uint64_t(match->address) + result.offset;
    if (address > UINT32_MAX || (address & 0x1fffffff) >= 0x200000u ||
        (address & 0xe0000000) != (match->address & 0xe0000000)) return result;
    result.symbol = match;
    result.address = static_cast<uint32_t>(address);
    result.status = PSX_NATIVE_FUNCTION_FOUND;
    return result;
}
int PSX_NATIVE_MOD_CALL find_function(void* context, const char* name, uint32_t* address) {
    if (!context || !address) return PSX_NATIVE_FUNCTION_UNKNOWN;
    const auto found = function_reference(name);
    if (found.status == PSX_NATIVE_FUNCTION_FOUND) *address = found.address;
    return found.status;
}

int PSX_NATIVE_MOD_CALL option(void* context, const char* id, char* out, uint32_t capacity) {
    if (!context || !id || !out || !capacity) return 0;
    const auto& options = static_cast<Loaded*>(context)->config.options;
    const auto it = options.find(id);
    if (it == options.end() || it->second.size() >= capacity) return 0;
    std::memcpy(out, it->second.c_str(), it->second.size() + 1);
    return 1;
}
void PSX_NATIVE_MOD_CALL log(void* context, const char* message) {
    if (context && message)
        std::fprintf(stderr, "native mod %s/%s: %s\n",
            static_cast<Loaded*>(context)->config.package_id.c_str(),
            static_cast<Loaded*>(context)->config.module.id.c_str(), message);
}
std::vector<uint8_t> authenticated_bytes(const ModNativeModule& module) {
    std::ifstream in(module.file, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() <= 0 || in.tellg() > 256 * 1024 * 1024)
        throw std::runtime_error("cannot read native module: " + module.file.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("cannot read complete native module");
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    std::ostringstream hash;
    for (auto byte : digest) hash << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    if (hash.str() != module.sha256) throw std::runtime_error("native module changed before loading");
    return bytes;
}
void export_cpu(const CPUState& from, PSXNativeCPU& to) {
    std::memcpy(to.gpr, from.gpr, sizeof(to.gpr));
    to.hi = from.hi; to.lo = from.lo; to.pc = from.pc;
}
void import_cpu(const PSXNativeCPU& from, CPUState& to) {
    std::memcpy(to.gpr, from.gpr, sizeof(from.gpr));
    to.gpr[0] = 0;
    to.hi = from.hi; to.lo = from.lo; to.pc = from.pc;
}
struct Chain {
    CPUState* guest;
    PSXNativeCPU* view;
    const std::vector<Session::Binding>* hooks;
    size_t index;
    uint32_t address, ra;
    bool called = false;
    bool escaped = false;
};
struct CallbackContext {
    CPUState* guest;
    PSXNativeCPU* view;
    Chain* chain;
    uint32_t return_pc;
    bool escaped = false;
};
void invoke(Chain& chain);
int PSX_NATIVE_MOD_CALL next(void* context, PSXNativeCPU* cpu) {
    auto& callback = *static_cast<CallbackContext*>(context);
    if (callback.escaped || !callback.chain) return 0;
    auto& chain = *callback.chain;
    if (chain.called || cpu != chain.view) return 0;
    chain.called = true;
    Chain following{chain.guest, chain.view, chain.hooks, chain.index + 1, chain.address, chain.ra};
    invoke(following);
    if (following.escaped) { callback.escaped = true; return 0; }
    return 1;
}
int call_guest_impl(void* context, PSXNativeCPU* view,
                    uint32_t address, const uint32_t* arguments,
                    uint32_t count, PSXNativeGuestResult* result,
                    const ModNativeSymbol* symbol) {
    if (!context || !view || !result || result->size < sizeof(*result) ||
        (count && !arguments) || !psx_mod_game_started() || g_psx_call_bail)
        return PSX_NATIVE_GUEST_UNAVAILABLE;
    auto& callback = *static_cast<CallbackContext*>(context);
    const uint32_t target = address & 0x1fffffff;
    if (callback.escaped || callback.view != view || !callback.guest ||
        (address & 3u) || target < 0x10000u || target >= 0x200000u)
        return PSX_NATIVE_GUEST_UNAVAILABLE;
    const uint32_t sp = view->gpr[29], physical_sp = sp & 0x1fffffff;
    uint32_t floor;
    if (physical_sp >= 0x10000u && physical_sp <= 0x200000u) floor = 0x10000u;
    else if (physical_sp >= 0x1f800000u && physical_sp <= 0x1f800400u) floor = 0x1f800000u;
    else return PSX_NATIVE_GUEST_UNAVAILABLE;
    const uint64_t argument_bytes = uint64_t(std::max(count, 4u)) * 4u;
    if ((sp & 3u) || argument_bytes > physical_sp - floor)
        return PSX_NATIVE_GUEST_UNAVAILABLE;
    const uint32_t frame_sp = (sp - static_cast<uint32_t>(argument_bytes)) & ~7u;
    if ((frame_sp & 0x1fffffff) < floor) return PSX_NATIVE_GUEST_UNAVAILABLE;
    const uint32_t frame_bytes = sp - frame_sp;
    std::vector<uint8_t> saved_frame;
    std::vector<uint32_t> words;
    try {
        saved_frame.resize(frame_bytes);
        words.resize(std::max(count, 4u));
    } catch (...) { return PSX_NATIVE_GUEST_UNAVAILABLE; }
    if (count) std::copy(arguments, arguments + count, words.begin());
    for (uint32_t i = 0; i < frame_bytes; ++i)
        saved_frame[i] = psx_mod_read_byte(frame_sp + i);
    const PSXNativeCPU saved = *view;
    CPUState& guest = *callback.guest;
    import_cpu(saved, guest);
    for (unsigned i = 0; i < 4; ++i) guest.gpr[4 + i] = words[i];
    guest.gpr[29] = frame_sp;
    /* Use a real guest continuation: generated callees poll interrupts before
     * returning, so an artificial host sentinel could leak into a saved EPC/TCB.
     * Dispatch catches this (PC, borrowed SP) boundary before executing it. */
    const uint32_t return_pc = callback.return_pc;
    guest.gpr[31] = return_pc;
    for (uint32_t i = 0; i < words.size(); ++i)
        psx_mod_write_word(frame_sp + i * 4u, words[i]);
    // A malformed stack can overlap code. Recheck after preparing arguments,
    // restoring the borrowed frame if that preparation changed the target.
    if (symbol && !symbol_catalog().available(symbol->identity)) {
        for (uint32_t i = 0; i < frame_bytes; ++i)
            psx_mod_write_byte(frame_sp + i, saved_frame[i]);
        import_cpu(saved, guest);
        *view = saved;
        return PSX_NATIVE_GUEST_FUNCTION_NOT_LOADED;
    }
    psx_dispatch_call(&guest, address, return_pc);
    if (g_psx_call_bail || guest.gpr[29] != frame_sp ||
        ((guest.gpr[31] ^ return_pc) & 0x1fffffff) ||
        (guest.pc && ((guest.pc ^ return_pc) & 0x1fffffff))) {
        g_psx_call_bail = 1;
        if (!guest.pc) guest.pc = guest.gpr[31];
        callback.escaped = true;
        export_cpu(guest, *view);
        return PSX_NATIVE_GUEST_NONLOCAL;
    }
    result->v0 = guest.gpr[2]; result->v1 = guest.gpr[3];
    for (uint32_t i = 0; i < frame_bytes; ++i)
        psx_mod_write_byte(frame_sp + i, saved_frame[i]);
    import_cpu(saved, guest);
    *view = saved;
    view->gpr[0] = 0;
    return PSX_NATIVE_GUEST_RETURNED;
}
int PSX_NATIVE_MOD_CALL call_guest(void* context, PSXNativeCPU* view,
                                  uint32_t address, const uint32_t* arguments,
                                  uint32_t count, PSXNativeGuestResult* result) {
    return call_guest_impl(context, view, address, arguments, count, result, nullptr);
}
int PSX_NATIVE_MOD_CALL call_guest_named(void* context, PSXNativeCPU* view,
                                        const char* name, const uint32_t* arguments,
                                        uint32_t count, PSXNativeGuestResult* result) {
    if (!context || !view || !result || result->size < sizeof(*result) ||
        (count && !arguments) || !psx_mod_game_started() || g_psx_call_bail)
        return PSX_NATIVE_GUEST_UNAVAILABLE;
    auto& callback = *static_cast<CallbackContext*>(context);
    if (callback.escaped || callback.view != view) return PSX_NATIVE_GUEST_UNAVAILABLE;
    const auto found = function_reference(name);
    if (found.status == PSX_NATIVE_FUNCTION_AMBIGUOUS) return PSX_NATIVE_GUEST_AMBIGUOUS_FUNCTION;
    if (found.status != PSX_NATIVE_FUNCTION_FOUND) return PSX_NATIVE_GUEST_UNKNOWN_FUNCTION;
    if (found.offset) return PSX_NATIVE_GUEST_UNAVAILABLE;
    if (found.symbol && (!symbol_catalog().available || !symbol_catalog().available(found.symbol->identity)))
        return PSX_NATIVE_GUEST_FUNCTION_NOT_LOADED;
    return call_guest_impl(context, view, found.address, arguments, count, result, found.symbol);
}
void invoke(Chain& chain) {
    if (chain.index == chain.hooks->size()) {
        import_cpu(*chain.view, *chain.guest);
        bypass_once = true;
        psx_dispatch_call(chain.guest, chain.address, chain.ra);
        bypass_once = false;
        export_cpu(*chain.guest, *chain.view);
        /* The dispatcher's pc=0 is a host-call return sentinel. A hooked
         * CPS callee must publish its guest continuation to the enclosing
         * trampoline, whose own stop address can belong to another frame. */
        if (chain.view->pc == 0) chain.view->pc = chain.ra;
        chain.escaped = g_psx_call_bail != 0;
        return;
    }
    auto* loaded = (*chain.hooks)[chain.index].loaded;
    CallbackContext context{chain.guest, chain.view, &chain, chain.address};
    PSXNativeCall call{sizeof(PSXNativeCall), chain.address, chain.ra, &context, next};
    loaded->mod->hook(loaded->mod->userdata, chain.view, &call);
    if (context.escaped) {
        chain.escaped = true;
        export_cpu(*chain.guest, *chain.view);
    }
}
}

bool mod_native_prepare(const ModResolution& plan, std::string* error) try {
    if (!plan.ok) throw std::runtime_error("cannot load an invalid native mod plan");
    /* A disc digest change alone must not reset mod state or unload a library
     * whose hook is still wrapping a guest call. resolve() has already checked
     * content/trust; reusing its authenticated loaded snapshot is safe. */
    if (session() && same_configuration(*session(), plan)) return true;
    auto pending = std::make_unique<Session>();
    /* Disc swaps recommit a plan during gameplay without another activation
     * call. A fresh launch stays inactive until activate_plugins(). */
    pending->active = session() && session()->active;
    if (!plan.native_modules.empty()) {
        /* Load an authenticated snapshot, never the mutable installed file.
         * Windows dependencies search System32 only; package authors statically
         * link other dependencies. POSIX uses eager, local symbol binding. */
        const auto directory = fs::temp_directory_path() / ("psx-native-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!fs::create_directory(directory))
            throw std::runtime_error("cannot create native session directory");
        pending->directory = directory;
        fs::permissions(pending->directory, fs::perms::owner_all, fs::perm_options::replace);
        for (const auto& config : plan.native_modules) {
            auto loaded = std::make_unique<Loaded>();
            loaded->config = config;
            const auto bytes = authenticated_bytes(config.module);
            const auto file = pending->directory / (std::to_string(pending->modules.size()) + config.module.file.extension().string());
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            out.close();
            if (!out) throw std::runtime_error("cannot snapshot native library");
            PSXNativeModEntry entry = nullptr;
#if defined(_WIN32)
            loaded->library = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (loaded->library)
                entry = reinterpret_cast<PSXNativeModEntry>(GetProcAddress(static_cast<HMODULE>(loaded->library), "psx_native_mod_v1"));
            if (!loaded->library) throw std::runtime_error("cannot load native library (Windows error " + std::to_string(GetLastError()) + ")");
#else
            loaded->library = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!loaded->library) throw std::runtime_error(std::string("cannot load native library: ") + dlerror());
            entry = reinterpret_cast<PSXNativeModEntry>(dlsym(loaded->library, "psx_native_mod_v1"));
#endif
            if (!entry) throw std::runtime_error("native library must export psx_native_mod_v1");
            loaded->mod = entry();
            if (!loaded->mod || loaded->mod->abi_version != PSX_NATIVE_MOD_ABI ||
                loaded->mod->size < offsetof(PSXNativeMod, block))
                throw std::runtime_error("incompatible native mod ABI/descriptor");
            loaded->host = {sizeof(PSXNativeHost), PSX_NATIVE_MOD_ABI, loaded.get(),
                loaded->config.package_id.c_str(), loaded->config.module.feature_id.c_str(),
                psx_mod_read_byte, psx_mod_read_half, psx_mod_read_word,
                psx_mod_write_byte, psx_mod_write_half, psx_mod_write_word,
                option, log, psx_native_mod_advance_cycles, psx_mod_game_started, call_guest,
                find_function, call_guest_named};
            for (const auto& hook : loaded->config.module.hooks) {
                const uint32_t phys = hook.address & 0x1fffffff;
                if (hook.resume_address) {
                    if (loaded->mod->size < sizeof(PSXNativeMod) || !loaded->mod->block)
                        throw std::runtime_error("partial native hook requires the block callback and extended descriptor");
                    auto next = pending->blocks.lower_bound(phys);
                    if ((next != pending->blocks.end() && next->first < phys + hook.expected.size()) ||
                        (next != pending->blocks.begin() && std::prev(next)->first + std::prev(next)->second.hook->expected.size() > phys))
                        throw std::runtime_error("partial native hook ranges overlap");
                    pending->blocks.emplace(phys, Session::Binding{loaded.get(), &hook});
                } else {
                    if (!loaded->mod->hook)
                        throw std::runtime_error("function hook requires the hook callback");
                    pending->hooks[phys].push_back({loaded.get(), &hook});
                }
            }
            pending->modules.push_back(std::move(loaded));
        }
        for (const auto& loaded : pending->modules) {
            loaded->started = true;
            if (loaded->mod->start && !loaded->mod->start(loaded->mod->userdata, &loaded->host))
                throw std::runtime_error(loaded->config.package_id + ": native mod rejected startup");
        }
    }
    replace_session(std::move(pending));
    return true;
} catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
} catch (...) {
    if (error) *error = "native mod threw across its C ABI";
    return false;
}
void mod_native_reset() { replace_session(nullptr); bypass_once = false; }
void mod_native_activate() { if (session()) session()->active = true; }
void mod_native_vblank() {
    if (!session() || !session()->active || !psx_mod_game_started()) return;
    Session* running = session().get();
    ++running->calls;
    for (auto& m : running->modules) if (m->mod->vblank) m->mod->vblank(m->mod->userdata);
    --running->calls;
    collect_retired();
}
void mod_native_savestate_loaded() {
    if (!session() || !session()->active) return;
    Session* running = session().get();
    ++running->calls;
    for (auto& m : running->modules) if (m->mod->savestate_loaded) m->mod->savestate_loaded(m->mod->userdata);
    --running->calls;
    collect_retired();
}
} // namespace PSXRecompV4

extern "C" void mod_native_register_symbols(const ModNativeSymbol* symbols, uint32_t count,
                                             ModNativeSymbolAvailable available) {
    using namespace PSXRecompV4;
    // Catalog storage belongs to the game; a malformed table fails closed.
    if (!symbols || !available) { symbol_catalog() = {}; return; }
    for (uint32_t i = 0; i < count; ++i) {
        if (!symbols[i].name || !symbols[i].name[0] || !symbols[i].identity ||
            (symbols[i].address & 3u) || (symbols[i].address & 0x1fffffff) < 0x10000u ||
            (symbols[i].address & 0x1fffffff) >= 0x200000u ||
            (i && std::strcmp(symbols[i - 1].name, symbols[i].name) > 0)) {
            symbol_catalog() = {}; return;
        }
    }
    symbol_catalog() = {symbols, count, available};
}

extern "C" int mod_native_on_dispatch(CPUState* cpu, uint32_t address, uint32_t ra) {
    using namespace PSXRecompV4;
    if (bypass_once) { bypass_once = false; return 0; }
    if (!cpu || !session() || !session()->active || session()->hooks.empty() || !psx_mod_game_started()) return 0;
    const auto it = session()->hooks.find(address & 0x1fffffff);
    if (it == session()->hooks.end()) return 0;
    /* Overlay reuse/self-modification must not detour an unrelated function.
     * All chained hooks must have compatible live-byte guards. */
    for (const auto& binding : it->second) {
        const auto& hook = *binding.hook;
        for (size_t i = 0; i < hook.expected.size(); ++i)
            if (binding.loaded->host.read_byte(address + static_cast<uint32_t>(i)) != hook.expected[i]) return 0;
    }
    PSXNativeCPU view;
    export_cpu(*cpu, view);
    view.pc = address;
    /* Keep a pending native continuation mapped even if a disc swap/reset
     * changes the session while next() is executing guest code. This counter
     * deliberately has no C++ RAII object across guest longjmp boundaries: a
     * nonlocal guest unwind may retain a retired library until process exit,
     * but can never return into an unloaded module. */
    Session* running = session().get();
    ++running->calls;
    Chain chain{cpu, &view, &it->second, 0, address, ra};
    invoke(chain);
    import_cpu(view, *cpu);
    --running->calls;
    collect_retired();
    return 1;
}

extern "C" int mod_native_blocks_intersect(uint32_t address, uint32_t size) {
    using namespace PSXRecompV4;
    if (!size || !session() || !session()->active || session()->blocks.empty() || !psx_mod_game_started()) return 0;
    const uint32_t lo = address & 0x1fffffff;
    const uint64_t hi = uint64_t(lo) + size;
    const auto& blocks = session()->blocks;
    auto next = blocks.lower_bound(lo);
    if (next != blocks.end() && next->first < hi) return 1;
    if (next == blocks.begin()) return 0;
    --next;
    return uint64_t(next->first) + next->second.hook->expected.size() > lo;
}

extern "C" int mod_native_has_block(uint32_t address) {
    return mod_native_blocks_intersect(address, 4) &&
        PSXRecompV4::session()->blocks.count(address & 0x1fffffff) != 0;
}

extern "C" int mod_native_on_block(CPUState* cpu, uint32_t address) {
    using namespace PSXRecompV4;
    if (!cpu || !mod_native_has_block(address)) return 0;
    const auto& binding = session()->blocks.at(address & 0x1fffffff);
    auto* loaded = binding.loaded;
    const auto& hook = *binding.hook;
    /* Exclude physical branch/load delay-slot addresses on every interpreter
     * path, including observed execution where load tracking can be flushed.
     * No flag is left armed across a guest exception's nonlocal unwind. */
    const uint32_t previous = loaded->host.read_word(address - 4u);
    const uint32_t op = previous >> 26, fn = previous & 63u, rs = (previous >> 21) & 31u;
    if ((op >= 1u && op <= 7u) ||
        (op == 0u && (fn == 8u || fn == 9u)) ||
        (op >= 0x10u && op <= 0x13u && rs == 8u) ||
        (op >= 0x20u && op <= 0x26u) || op == 0x32u ||
        (op >= 0x10u && op <= 0x13u && rs <= 2u)) return 0;
    for (size_t i = 0; i < hook.expected.size(); ++i)
        if (loaded->host.read_byte(address + static_cast<uint32_t>(i)) != hook.expected[i]) return 0;
    PSXNativeCPU view;
    export_cpu(*cpu, view);
    view.pc = address;
    const uint32_t resume = address + static_cast<uint32_t>(hook.expected.size());
    CallbackContext context{cpu, &view, nullptr, resume};
    const PSXNativeBlock block{sizeof(PSXNativeBlock), address, resume,
        static_cast<uint32_t>(hook.expected.size()), cpu->gpr[31], &context};
    Session* running = session().get();
    ++running->calls;
    loaded->mod->block(loaded->mod->userdata, &view, &block);
    if (context.escaped) export_cpu(*cpu, view);
    else view.pc = resume; // A partial replacement must preserve the guest suffix.
    import_cpu(view, *cpu);
    --running->calls;
    collect_retired();
    return 1;
}
