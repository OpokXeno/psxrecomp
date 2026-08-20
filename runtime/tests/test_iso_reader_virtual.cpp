#include "mod_runtime.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

static std::array<uint8_t, 2352> virtual_existing;
static std::array<uint8_t, 2352> virtual_appended;
static int patch_calls;
static int failures;

extern "C" int mod_runtime_read_virtual_raw_sector(
    uint32_t lba, uint8_t* bytes, uint32_t size) {
    if (!bytes || size < 2352) return 0;
    const std::array<uint8_t, 2352>* sector = nullptr;
    if (lba == 0) sector = &virtual_existing;
    if (lba == 2) sector = &virtual_appended;
    if (!sector) return 0;
    std::memcpy(bytes, sector->data(), sector->size());
    return 1;
}

extern "C" void mod_runtime_patch_disc_sector(
    uint32_t, int raw_sector, uint8_t* bytes, uint32_t size) {
    patch_calls++;
    if (raw_sector && size >= 2352) {
        bytes[100] = 0xe1;
        bytes[24 + 7] = 0xe2;
    }
}

extern "C" uint32_t mod_runtime_effective_sector_count(uint32_t base_count) {
    return base_count < 3 ? 3 : base_count;
}

namespace PSXRecompV4 {
PS1::ISOReader* mod_runtime_take_verified_disc(const std::filesystem::path&) {
    return nullptr;
}

bool mod_runtime_requires_verified_disc() {
    return false;
}

bool mod_runtime_return_verified_disc(PS1::ISOReader*) {
    return false;
}
} // namespace PSXRecompV4

#include "../src/iso_reader_c.cpp"

static void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

int main() {
    check(iso_open(nullptr) == nullptr, "null image paths must be rejected");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "psxrecomp-iso-virtual.bin";
    std::array<uint8_t, 2 * 2352> stock{};
    stock[100] = 0x11;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(stock.data()), stock.size());
    out.close();

    virtual_existing.fill(0x31);
    virtual_existing[15] = 2;
    virtual_existing[18] = 0;
    virtual_existing[24 + 7] = 0xa0;
    virtual_appended.fill(0x42);
    virtual_appended[15] = 2;
    virtual_appended[18] = 0;
    virtual_appended[24 + 7] = 0xb0;

    void* iso = iso_open(path.string().c_str());
    check(iso != nullptr, "stock image must open");
    check(iso_sector_count(iso) == 3,
          "effective sector count must include appended virtual sectors");

    std::array<uint8_t, 2352> raw{};
    std::array<uint8_t, 2048> user{};
    patch_calls = 0;
    check(!iso_read_sector(iso, 0, nullptr, 2048) &&
              !iso_read_sector(iso, 0, user.data(), 2047) &&
              !iso_read_raw_sector(iso, 0, nullptr, 2352) &&
              !iso_read_raw_sector(iso, 0, raw.data(), 2351) &&
              patch_calls == 0,
          "virtual reads must reject null or undersized output buffers");

    patch_calls = 0;
    check(iso_read_raw_sector(iso, 0, raw.data(), raw.size()) &&
              raw[0] == 0x31 && raw[100] == 0xe1 && patch_calls == 1,
          "raw reads must prefer and then patch an existing-LBA virtual sector");
    raw.fill(0);
    check(iso_read_raw_sector(iso, 2, raw.data(), raw.size()) &&
              raw[0] == 0x42 && raw[100] == 0xe1 && patch_calls == 2,
          "raw reads must serve and then patch an appended virtual sector");

    patch_calls = 0;
    check(iso_read_sector(iso, 0, user.data(), user.size()) &&
              user[0] == 0x31 && user[7] == 0xe2 && patch_calls == 1,
          "logical reads must prefer a virtual raw sector and expose patched user data");
    user.fill(0);
    check(iso_read_sector(iso, 2, user.data(), user.size()) &&
              user[0] == 0x42 && user[7] == 0xe2 && patch_calls == 2,
          "logical reads must serve appended virtual user data after patching");

    iso_close(iso);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (failures) return 1;
    std::cout << "ISO virtual-sector bridge tests passed\n";
    return 0;
}
