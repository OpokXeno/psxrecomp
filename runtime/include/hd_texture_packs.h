#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace PSXRecompV4 {

/* The user's HD texture pack list: external Beetle-PSX-HW style replacement
 * folders registered from the launcher. They are not .psxmod packages; the
 * folders stay where the user keeps them and only their paths are recorded,
 * in <mods root>/texture_packs.toml. List order is priority order. */
struct HdTexturePackEntry {
    std::string id;    /* stable, derived from the path */
    std::string name;  /* display name derived from the folder */
    std::string path;  /* folder the user picked */
    bool enabled = false;
    size_t image_count = 0; /* last successful probe; 0 when unavailable */
    std::string error;      /* last probe failure, empty when usable */
};

constexpr const char* kHdTexturePackPackageId = "psx.hd-texture-packs";

void hd_texture_packs_load(const std::filesystem::path& mods_root);
const std::vector<HdTexturePackEntry>& hd_texture_packs();
const HdTexturePackEntry* hd_texture_pack_find(const std::string& id);
/* Adds (enabled) or re-enables an already-listed folder. */
bool hd_texture_packs_add(const std::string& folder, std::string* out_id,
                          std::string* error);
bool hd_texture_packs_remove(const std::string& id, std::string* error);
bool hd_texture_packs_set_enabled(const std::string& id, bool enabled,
                                  std::string* error);
/* Hands the enabled folders to the renderer's replacement runtime. Called when
 * a game session starts; an empty selection disables replacement. */
bool hd_texture_packs_activate(std::string* error);
void hd_texture_packs_deactivate();

} // namespace PSXRecompV4
