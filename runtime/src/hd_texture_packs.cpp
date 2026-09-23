#include "hd_texture_packs.h"

#include "hd_texture_runtime.h"
#include "toml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace PSXRecompV4 {
namespace {

struct PackList {
    fs::path root;
    std::vector<HdTexturePackEntry> entries;
    /* Roots handed to the replacement runtime; re-activating the same set
     * keeps its upload tracker instead of forgetting resident textures. */
    std::vector<std::string> active_roots;
    bool active_valid = false;
};

PackList& packs() {
    static PackList value;
    return value;
}

std::string quote(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back((char)c);
    }
    out.push_back('"');
    return out;
}

std::string normalized(const std::string& folder) {
    std::error_code ec;
    fs::path path = fs::absolute(fs::u8path(folder), ec);
    if (ec) path = fs::u8path(folder);
    std::string text = path.lexically_normal().u8string();
    while (text.size() > 1 && (text.back() == '/' || text.back() == '\\'))
        text.pop_back();
    return text;
}

std::string id_for(const std::string& path) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : path) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "pack-%016llx", (unsigned long long)hash);
    return text;
}

std::string strip_suffix(std::string name) {
    static const std::string suffix = "-texture-replacements";
    if (name.size() >= suffix.size()) {
        std::string tail = name.substr(name.size() - suffix.size());
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (tail == suffix) name.erase(name.size() - suffix.size());
    }
    return name;
}

std::string name_for(const std::string& path) {
    const fs::path folder = fs::u8path(path);
    std::string name = strip_suffix(folder.filename().u8string());
    /* Packs ship their replacement folder named after a placeholder disc
     * ("[your .cue file name here]-texture-replacements"); the folder around
     * it carries the pack's real name. */
    if ((name.empty() || name[0] == '[') && folder.has_parent_path())
        name = strip_suffix(folder.parent_path().filename().u8string());
    return name.empty() ? path : name;
}

void probe(HdTexturePackEntry& entry) {
    char error[512];
    size_t count = 0;
    if (hd_texture_pack_probe(entry.path.c_str(), &count, nullptr, 0,
                              error, sizeof(error))) {
        entry.image_count = count;
        entry.error.clear();
    } else {
        entry.image_count = 0;
        entry.error = error[0] ? error : "folder is not usable";
    }
}

bool save(std::string* error) {
    PackList& list = packs();
    if (list.root.empty()) return true;
    std::error_code ec;
    fs::create_directories(list.root, ec);
    const fs::path temp = list.root / "texture_packs.toml.tmp";
    const fs::path final = list.root / "texture_packs.toml";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + temp.u8string();
            return false;
        }
        out << "# HD texture packs registered from the launcher. Earlier entries win.\n";
        out << "format_version = 1\n";
        for (const HdTexturePackEntry& entry : list.entries) {
            out << "\n[[pack]]\n";
            out << "path = " << quote(entry.path) << "\n";
            out << "enabled = " << (entry.enabled ? "true" : "false") << "\n";
        }
        if (!out) {
            if (error) *error = "cannot write " + temp.u8string();
            return false;
        }
    }
    fs::rename(temp, final, ec);
    if (ec) {
        if (error) *error = "cannot replace " + final.u8string() + ": " + ec.message();
        return false;
    }
    return true;
}

HdTexturePackEntry* find_mutable(const std::string& id) {
    for (HdTexturePackEntry& entry : packs().entries)
        if (entry.id == id) return &entry;
    return nullptr;
}

} // namespace

void hd_texture_packs_load(const fs::path& mods_root) {
    PackList& list = packs();
    list.root = mods_root;
    list.entries.clear();
    const fs::path path = mods_root / "texture_packs.toml";
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    try {
        const toml::value cfg = toml::parse(path.string());
        if (!cfg.contains("pack")) return;
        for (const toml::value& value : toml::find(cfg, "pack").as_array()) {
            HdTexturePackEntry entry;
            entry.path = normalized(toml::find<std::string>(value, "path"));
            entry.enabled = toml::find_or<bool>(value, "enabled", true);
            entry.id = id_for(entry.path);
            entry.name = name_for(entry.path);
            if (find_mutable(entry.id)) continue;
            probe(entry);
            list.entries.push_back(std::move(entry));
        }
    } catch (const std::exception&) {
        /* An unreadable list is rewritten from scratch by the next change. */
        list.entries.clear();
    }
}

const std::vector<HdTexturePackEntry>& hd_texture_packs() {
    return packs().entries;
}

const HdTexturePackEntry* hd_texture_pack_find(const std::string& id) {
    return find_mutable(id);
}

bool hd_texture_packs_add(const std::string& folder, std::string* out_id,
                          std::string* error) {
    if (folder.empty()) {
        if (error) *error = "no folder selected";
        return false;
    }
    HdTexturePackEntry entry;
    entry.path = normalized(folder);
    entry.id = id_for(entry.path);
    entry.name = name_for(entry.path);
    entry.enabled = true;
    probe(entry);
    if (!entry.error.empty()) {
        if (error) *error = "Not a texture pack: " + entry.error;
        return false;
    }
    if (HdTexturePackEntry* existing = find_mutable(entry.id)) {
        *existing = entry;
    } else {
        packs().entries.push_back(entry);
    }
    if (out_id) *out_id = entry.id;
    return save(error);
}

bool hd_texture_packs_remove(const std::string& id, std::string* error) {
    auto& entries = packs().entries;
    const auto it = std::find_if(entries.begin(), entries.end(),
        [&](const HdTexturePackEntry& entry) { return entry.id == id; });
    if (it == entries.end()) {
        if (error) *error = "unknown texture pack";
        return false;
    }
    entries.erase(it);
    return save(error);
}

bool hd_texture_packs_set_enabled(const std::string& id, bool enabled,
                                  std::string* error) {
    HdTexturePackEntry* entry = find_mutable(id);
    if (!entry) {
        if (error) *error = "unknown texture pack";
        return false;
    }
    if (enabled) probe(*entry);
    entry->enabled = enabled;
    return save(error);
}

bool hd_texture_packs_activate(std::string* error) {
    std::vector<std::string> selected;
    for (HdTexturePackEntry& entry : packs().entries) {
        if (!entry.enabled) continue;
        probe(entry);
        /* A pack whose folder moved must not keep the game from starting;
         * it contributes nothing this session and the launcher shows why. */
        if (!entry.error.empty()) continue;
        selected.push_back(entry.path);
    }
    PackList& list = packs();
    if (list.active_valid && list.active_roots == selected) return true;
    std::vector<const char*> roots;
    for (const std::string& root : selected) roots.push_back(root.c_str());
    char message[512];
    list.active_valid = false;
    list.active_roots.clear();
    if (!hd_texture_runtime_configure(roots.data(), roots.size(), message,
                                      sizeof(message))) {
        if (error) *error = std::string("texture packs unavailable: ") + message;
        return false;
    }
    list.active_roots = selected;
    list.active_valid = true;
    return true;
}

void hd_texture_packs_deactivate() {
    PackList& list = packs();
    hd_texture_runtime_configure(nullptr, 0, nullptr, 0);
    list.active_roots.clear();
    list.active_valid = true;
}

} // namespace PSXRecompV4
