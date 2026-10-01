#pragma once
// Optional header-only C++17 helpers. The binary interface remains the C ABI.
// CPU, Call and Block views are valid only during their original callback.
#include "mod_native_api.h"
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace psx::mod {

class CPU {
    PSXNativeCPU* value_;
public:
    explicit CPU(PSXNativeCPU& value) noexcept : value_(&value) {}
    PSXNativeCPU* raw() const noexcept { return value_; }
    uint32_t& reg(unsigned index) const noexcept {
        assert(index > 0 && index < 32);
        return value_->gpr[index];
    }
#define PSX_MOD_REGISTER(name, index) \
    uint32_t& name() const noexcept { return value_->gpr[index]; }
    PSX_MOD_REGISTER(at, 1)
    PSX_MOD_REGISTER(v0, 2) PSX_MOD_REGISTER(v1, 3)
    PSX_MOD_REGISTER(a0, 4) PSX_MOD_REGISTER(a1, 5)
    PSX_MOD_REGISTER(a2, 6) PSX_MOD_REGISTER(a3, 7)
    PSX_MOD_REGISTER(t0, 8) PSX_MOD_REGISTER(t1, 9)
    PSX_MOD_REGISTER(t2, 10) PSX_MOD_REGISTER(t3, 11)
    PSX_MOD_REGISTER(t4, 12) PSX_MOD_REGISTER(t5, 13)
    PSX_MOD_REGISTER(t6, 14) PSX_MOD_REGISTER(t7, 15)
    PSX_MOD_REGISTER(s0, 16) PSX_MOD_REGISTER(s1, 17)
    PSX_MOD_REGISTER(s2, 18) PSX_MOD_REGISTER(s3, 19)
    PSX_MOD_REGISTER(s4, 20) PSX_MOD_REGISTER(s5, 21)
    PSX_MOD_REGISTER(s6, 22) PSX_MOD_REGISTER(s7, 23)
    PSX_MOD_REGISTER(t8, 24) PSX_MOD_REGISTER(t9, 25)
    PSX_MOD_REGISTER(k0, 26) PSX_MOD_REGISTER(k1, 27)
    PSX_MOD_REGISTER(gp, 28) PSX_MOD_REGISTER(sp, 29)
    PSX_MOD_REGISTER(fp, 30) PSX_MOD_REGISTER(ra, 31)
#undef PSX_MOD_REGISTER
    uint32_t& return_value() const noexcept { return v0(); }
    uint32_t& hi() const noexcept { return value_->hi; }
    uint32_t& lo() const noexcept { return value_->lo; }
    uint32_t& pc() const noexcept { return value_->pc; }
};

class Options {
    const PSXNativeHost* host_;
    bool read(const char* id, char* text, uint32_t capacity) const noexcept {
        return host_ && host_->abi_version == PSX_NATIVE_MOD_ABI &&
            host_->size >= offsetof(PSXNativeHost, call_guest) &&
            host_->option && id && host_->option(host_->context, id, text, capacity);
    }
public:
    explicit Options(const PSXNativeHost* host) noexcept : host_(host) {}
    bool get_bool(const char* id, bool fallback = false) const noexcept {
        char text[8]{};
        if (!read(id, text, sizeof text)) return fallback;
        if (!std::strcmp(text, "true")) return true;
        if (!std::strcmp(text, "false")) return false;
        return fallback;
    }
    int64_t get_int(const char* id, int64_t fallback = 0) const noexcept {
        char text[32]{};
        if (!read(id, text, sizeof text)) return fallback;
        int64_t value;
        const char* end = text + std::strlen(text);
        const auto parsed = std::from_chars(text, end, value);
        return parsed.ec == std::errc{} && parsed.ptr == end ? value : fallback;
    }
    std::string get_string(const char* id, const std::string& fallback = "",
                           uint32_t capacity = 4096) const {
        if (!capacity) return fallback;
        std::vector<char> text(capacity);
        return read(id, text.data(), capacity) ? std::string(text.data()) : fallback;
    }
};

struct GuestResult {
    int status = PSX_NATIVE_GUEST_UNAVAILABLE;
    uint32_t v0 = 0, v1 = 0;
    explicit operator bool() const noexcept { return status == PSX_NATIVE_GUEST_RETURNED; }
    bool nonlocal() const noexcept { return status == PSX_NATIVE_GUEST_NONLOCAL; }
};

struct Function {
    int status = PSX_NATIVE_FUNCTION_UNKNOWN;
    uint32_t address = 0;
    explicit operator bool() const noexcept { return status == PSX_NATIVE_FUNCTION_FOUND; }
    bool ambiguous() const noexcept { return status == PSX_NATIVE_FUNCTION_AMBIGUOUS; }
};

class Host {
    const PSXNativeHost* value_ = nullptr;
    static bool copy_name(std::string_view name, char (&out)[256]) noexcept {
        if (name.empty() || name.size() >= sizeof out || name.find('\0') != std::string_view::npos) return false;
        std::memcpy(out, name.data(), name.size());
        out[name.size()] = '\0';
        return true;
    }
public:
    Host() noexcept = default;
    explicit Host(const PSXNativeHost& value) noexcept : value_(&value) {}
    bool compatible() const noexcept {
        return value_ && value_->abi_version == PSX_NATIVE_MOD_ABI &&
            value_->size >= offsetof(PSXNativeHost, call_guest);
    }
    bool supports_guest_calls() const noexcept {
        return compatible() && value_->size >=
            offsetof(PSXNativeHost, call_guest) + sizeof(value_->call_guest) && value_->call_guest;
    }
    bool supports_named_functions() const noexcept {
        return compatible() && value_->size >=
            offsetof(PSXNativeHost, call_guest_named) + sizeof(value_->call_guest_named) &&
            value_->find_function && value_->call_guest_named;
    }
    Function find_function(std::string_view name) const {
        char text[256];
        if (!compatible() || value_->size < offsetof(PSXNativeHost, find_function) + sizeof(value_->find_function) ||
            !value_->find_function || !copy_name(name, text)) return {};
        Function found;
        found.status = value_->find_function(value_->context, text, &found.address);
        return found;
    }
    Options options() const noexcept { return Options(value_); }
    void log(const char* text) const noexcept {
        if (compatible() && value_->log) value_->log(value_->context, text);
    }
    uint8_t read_byte(uint32_t address) const noexcept {
        return compatible() && value_->read_byte ? value_->read_byte(address) : 0;
    }
    uint16_t read_half(uint32_t address) const noexcept {
        return compatible() && value_->read_half ? value_->read_half(address) : 0;
    }
    uint32_t read_word(uint32_t address) const noexcept {
        return compatible() && value_->read_word ? value_->read_word(address) : 0;
    }
    void write_byte(uint32_t address, uint8_t value) const noexcept {
        if (compatible() && value_->write_byte) value_->write_byte(address, value);
    }
    void write_half(uint32_t address, uint16_t value) const noexcept {
        if (compatible() && value_->write_half) value_->write_half(address, value);
    }
    void write_word(uint32_t address, uint32_t value) const noexcept {
        if (compatible() && value_->write_word) value_->write_word(address, value);
    }
    void advance_cycles(uint32_t cycles) const noexcept {
        if (compatible() && value_->advance_cycles) value_->advance_cycles(cycles);
    }
    bool game_started() const noexcept {
        return compatible() && value_->game_started && value_->game_started();
    }
    GuestResult call_guest(void* context, CPU cpu, uint32_t address,
                           const uint32_t* arguments, uint32_t count) const {
        PSXNativeGuestResult output{sizeof(output), 0, 0};
        if (!supports_guest_calls()) return {};
        const int status = value_->call_guest(context, cpu.raw(), address, arguments, count, &output);
        return {status, output.v0, output.v1};
    }
    GuestResult call_guest(void* context, CPU cpu, uint32_t address,
                           std::initializer_list<uint32_t> arguments = {}) const {
        return call_guest(context, cpu, address, arguments.begin(),
                          static_cast<uint32_t>(arguments.size()));
    }
    GuestResult call_guest(void* context, CPU cpu, std::string_view name,
                           const uint32_t* arguments, uint32_t count) const {
        char text[256];
        if (!supports_named_functions()) return {};
        if (!copy_name(name, text)) return {PSX_NATIVE_GUEST_UNKNOWN_FUNCTION, 0, 0};
        PSXNativeGuestResult output{sizeof(output), 0, 0};
        const int status = value_->call_guest_named(context, cpu.raw(), text, arguments, count, &output);
        return {status, output.v0, output.v1};
    }
    GuestResult call_guest(void* context, CPU cpu, std::string_view name,
                           std::initializer_list<uint32_t> arguments = {}) const {
        return call_guest(context, cpu, name, arguments.begin(), static_cast<uint32_t>(arguments.size()));
    }
};

class Call {
    Host host_;
    CPU cpu_;
    const PSXNativeCall* call_;
public:
    Call(Host host, PSXNativeCPU& cpu, const PSXNativeCall& call) noexcept
        : host_(host), cpu_(cpu), call_(&call) {}
    CPU cpu() const noexcept { return cpu_; }
    uint32_t address() const noexcept { return call_->address; }
    uint32_t return_address() const noexcept { return call_->return_address; }
    bool matches(std::string_view name) const {
        const auto found = host_.find_function(name);
        return found && !((found.address ^ address()) & 0x1fffffff);
    }
    bool next() const { return call_->next && call_->next(call_->context, cpu_.raw()); }
    void return_value(uint32_t v0, uint32_t v1 = 0) const noexcept {
        cpu_.v0() = v0; cpu_.v1() = v1; cpu_.pc() = return_address();
    }
    GuestResult call_guest(uint32_t address,
                           std::initializer_list<uint32_t> arguments = {}) const {
        return host_.call_guest(call_->context, cpu_, address, arguments);
    }
    GuestResult call_guest(uint32_t address, const uint32_t* arguments, uint32_t count) const {
        return host_.call_guest(call_->context, cpu_, address, arguments, count);
    }
    GuestResult call_guest(std::string_view name,
                           std::initializer_list<uint32_t> arguments = {}) const {
        return host_.call_guest(call_->context, cpu_, name, arguments);
    }
    GuestResult call_guest(std::string_view name, const uint32_t* arguments, uint32_t count) const {
        return host_.call_guest(call_->context, cpu_, name, arguments, count);
    }
};

class Block {
    Host host_;
    CPU cpu_;
    const PSXNativeBlock* block_;
    void* context() const noexcept {
        return block_->size >= offsetof(PSXNativeBlock, context) + sizeof(block_->context)
            ? block_->context : nullptr;
    }
public:
    Block(Host host, PSXNativeCPU& cpu, const PSXNativeBlock& block) noexcept
        : host_(host), cpu_(cpu), block_(&block) {}
    CPU cpu() const noexcept { return cpu_; }
    uint32_t address() const noexcept { return block_->address; }
    uint32_t resume_address() const noexcept { return block_->resume_address; }
    uint32_t byte_count() const noexcept { return block_->byte_count; }
    bool matches(std::string_view name) const {
        const auto found = host_.find_function(name);
        return found && !((found.address ^ address()) & 0x1fffffff);
    }
    GuestResult call_guest(uint32_t address,
                           std::initializer_list<uint32_t> arguments = {}) const {
        return host_.call_guest(context(), cpu_, address, arguments);
    }
    GuestResult call_guest(uint32_t address, const uint32_t* arguments, uint32_t count) const {
        return host_.call_guest(context(), cpu_, address, arguments, count);
    }
    GuestResult call_guest(std::string_view name,
                           std::initializer_list<uint32_t> arguments = {}) const {
        return host_.call_guest(context(), cpu_, name, arguments);
    }
    GuestResult call_guest(std::string_view name, const uint32_t* arguments, uint32_t count) const {
        return host_.call_guest(context(), cpu_, name, arguments, count);
    }
};

inline PSXNativeMod descriptor(void* userdata = nullptr) noexcept {
    PSXNativeMod result{};
    result.size = sizeof(result); result.abi_version = PSX_NATIVE_MOD_ABI;
    result.userdata = userdata;
    return result;
}

} // namespace psx::mod
