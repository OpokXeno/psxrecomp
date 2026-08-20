#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include "mod_packages.h"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

namespace PS1 {
class ISOReader;
}

namespace PSXRecompV4 {

struct ModVirtualDisc {
    uint32_t sector_count = 0;
    uint32_t appended_start_lba = 0;
    std::map<uint32_t, std::array<uint8_t, 2352>> raw_sectors;
    std::vector<std::array<uint8_t, 2352>> appended_raw_sectors;
};

using ModIndexedFileHandler = bool (*)(
    PS1::ISOReader& disc,
    const std::vector<ModResolution::IndexedFile>& files,
    uint32_t base_sector_count,
    ModVirtualDisc& output,
    std::string* error);

bool mod_runtime_register_indexed_file_handler(
    const std::string& format, ModIndexedFileHandler handler);

bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            uint32_t game_entry_pc,
                            const std::filesystem::path& exe_path = {},
                            std::string* error = nullptr);
bool mod_runtime_commit(const std::filesystem::path& disc_path = {},
                        std::string* error = nullptr);
/* Drop the in-session mod plan for a netplay launch without rewriting the
 * user's persisted offline selection on disk. Netplay is always vanilla for
 * now (no synced mod plans). */
bool mod_runtime_clear_for_netplay(std::string* error = nullptr);
const std::string& mod_runtime_fingerprint();
const std::filesystem::path& mod_runtime_effective_disc_path();
bool mod_runtime_compute_disc_sha256(
    const std::filesystem::path& disc_path, std::string& digest,
    std::string* error = nullptr);
/* Transfers the exact ISOReader used to authenticate and build an indexed
 * virtual disc. A format-6 plan may not be mounted through a later reopen. */
PS1::ISOReader* mod_runtime_take_verified_disc(
    const std::filesystem::path& disc_path);
bool mod_runtime_return_verified_disc(PS1::ISOReader* disc);
bool mod_runtime_requires_verified_disc();

#if defined(RECOMP_LAUNCHER)
const ::RecompLauncherCModProvider* mod_runtime_launcher_provider();
#endif

} // namespace PSXRecompV4
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Called before a guest dispatch. Applies the complete main-EXE plan
 * transactionally on the first dispatch to the configured entry point. */
void mod_runtime_on_dispatch(uint32_t target);
/* A full-machine savestate restores guest RAM after the initial entry-point
 * application. Reapply the already-validated main-EXE plan so the current
 * enabled mod selection remains authoritative after the restore. */
void mod_runtime_on_savestate_loaded(void);
/* Invokes activation callbacks for the committed plan. Call after the final
 * launcher commit and before renderer/window initialization. */
void mod_runtime_activate_plugins(void);
void mod_runtime_on_vblank(void);
void mod_runtime_patch_disc_sector(uint32_t lba, int raw_sector,
                                   uint8_t* bytes, uint32_t size);
/* Returns a complete replacement raw sector when the committed indexed-file
 * plan owns this LBA. Existing stock sectors and appended virtual sectors use
 * the same path so table updates cannot race ordinary base reads. */
int mod_runtime_read_virtual_raw_sector(uint32_t lba, uint8_t* bytes,
                                        uint32_t size);
uint32_t mod_runtime_effective_sector_count(uint32_t base_sector_count);
void mod_runtime_enable_disc_patches(void);

#ifdef __cplusplus
}
#endif
