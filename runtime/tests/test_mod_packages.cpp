#include "mod_packages.h"
#include "crc32.h"
#include "psx_sha256.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

namespace fs = std::filesystem;
using namespace PSXRecompV4;

static int failures;

static void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

static void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

static std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    return out;
}

static void write_deflated_package(const fs::path& path) {
    static const char* compressed_hex =
        "4bcb2fca4d2c892f4b2d2acecccf53b05530e4ca4c01524a5599057ab9f929"
        "4a5c082925433d033d0325aebcc4dc541037ca3340c117a4a428b5383f07a80"
        "e2498929a9c93589458925996aac4151d5d9258949e5a121bcb950ed4140f313"
        "ad827345837c4353844890b00";
    std::vector<uint8_t> compressed;
    for (const char* p = compressed_hex; *p; p += 2)
        compressed.push_back((uint8_t)std::stoul(std::string(p, 2), nullptr, 16));
    std::vector<uint8_t> zip;
    auto le16 = [&](uint16_t v) {
        zip.push_back((uint8_t)v); zip.push_back((uint8_t)(v >> 8));
    };
    auto le32 = [&](uint32_t v) {
        le16((uint16_t)v); le16((uint16_t)(v >> 16));
    };
    const std::string name = "manifest.toml";
    le32(0x04034b50); le16(20); le16(0); le16(8); le16(0); le16(0);
    le32(0x7d8454e1); le32((uint32_t)compressed.size()); le32(127);
    le16((uint16_t)name.size()); le16(0);
    zip.insert(zip.end(), name.begin(), name.end());
    zip.insert(zip.end(), compressed.begin(), compressed.end());
    const uint32_t central_offset = (uint32_t)zip.size();
    le32(0x02014b50); le16(20); le16(20); le16(0); le16(8); le16(0); le16(0);
    le32(0x7d8454e1); le32((uint32_t)compressed.size()); le32(127);
    le16((uint16_t)name.size()); le16(0); le16(0); le16(0); le16(0);
    le32(0); le32(0);
    zip.insert(zip.end(), name.begin(), name.end());
    const uint32_t central_size = (uint32_t)zip.size() - central_offset;
    le32(0x06054b50); le16(0); le16(0); le16(1); le16(1);
    le32(central_size); le32(central_offset); le16(0);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)zip.data(), (std::streamsize)zip.size());
}

static void write_stored_package(
    const fs::path& path,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& entries) {
    struct CentralEntry {
        std::string name;
        uint32_t crc;
        uint32_t size;
        uint32_t offset;
    };
    std::vector<uint8_t> zip;
    std::vector<CentralEntry> central;
    auto le16 = [&](uint16_t v) {
        zip.push_back((uint8_t)v); zip.push_back((uint8_t)(v >> 8));
    };
    auto le32 = [&](uint32_t v) {
        le16((uint16_t)v); le16((uint16_t)(v >> 16));
    };
    for (const auto& [name, data] : entries) {
        const uint32_t crc = crc32_compute(data.data(), data.size());
        central.push_back({name, crc, (uint32_t)data.size(), (uint32_t)zip.size()});
        le32(0x04034b50); le16(20); le16(0); le16(0); le16(0); le16(0);
        le32(crc); le32((uint32_t)data.size()); le32((uint32_t)data.size());
        le16((uint16_t)name.size()); le16(0);
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), data.begin(), data.end());
    }
    const uint32_t central_offset = (uint32_t)zip.size();
    for (const CentralEntry& entry : central) {
        le32(0x02014b50); le16(20); le16(20); le16(0); le16(0); le16(0); le16(0);
        le32(entry.crc); le32(entry.size); le32(entry.size);
        le16((uint16_t)entry.name.size()); le16(0); le16(0); le16(0); le16(0);
        le32(0); le32(entry.offset);
        zip.insert(zip.end(), entry.name.begin(), entry.name.end());
    }
    const uint32_t central_size = (uint32_t)zip.size() - central_offset;
    le32(0x06054b50); le16(0); le16(0);
    le16((uint16_t)central.size()); le16((uint16_t)central.size());
    le32(central_size); le32(central_offset); le16(0);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)zip.data(), (std::streamsize)zip.size());
}

static std::string manifest(const std::string& id, const std::string& version,
                            const std::string& extra = {}) {
    return
        "format_version = 1\n"
        "id = \"" + id + "\"\n"
        "version = \"" + version + "\"\n"
        "name = \"" + id + "\"\n"
        "author = \"Test Author\"\n"
        "source_name = \"Upstream project\"\n"
        "source_url = \"https://example.com/project\"\n"
        "resolver = \"declarative\"\n"
        "[[author_link]]\n"
        "name = \"Test Author\"\n"
        "url = \"https://example.com/author\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-TEST\"\n" + extra;
}

static int validate_catalog(int argc, char** argv) {
    const fs::path root =
        fs::temp_directory_path() / "psxrecomp-mod-catalog-validation";
    std::error_code ec;
    fs::remove_all(root, ec);
    mod_indexed_file_format_register("xenogears");
    ModPackageManager manager(root);
    std::string error;
    for (int i = 2; i < argc; ++i) {
        if (!manager.install_archive(argv[i], nullptr, nullptr, &error)) {
            std::cerr << "FAIL: " << argv[i] << ": " << error << "\n";
            fs::remove_all(root, ec);
            return 1;
        }
    }
    if (manager.packages().size() != static_cast<size_t>(argc - 2)) {
        std::cerr << "FAIL: catalog package identities are not unique\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const std::string disc1 =
        "74265236654985f8d5d76f79767ca62a9b2b6ba299c995211ff94588928a6235";
    const std::string disc2 =
        "b5fce68b407e9f4ae7474b3487a3d9a35ccd2c98e8b377374dd1fc1060450e30";
    std::vector<std::string> ids;
    for (const auto& [id, versions] : manager.packages()) {
        (void)versions;
        ids.push_back(id);
        if (!manager.set_feature_enabled(id, "perfect-works", true, &error)) {
            std::cerr << "FAIL: enabling " << id << ": " << error << "\n";
            fs::remove_all(root, ec);
            return 1;
        }
        const ModResolution first = manager.resolve("SLUS-00664", {}, disc1);
        const ModResolution second = manager.resolve("SLUS-00664", {}, disc2);
        if (!first.ok || !second.ok) {
            const ModResolution& failed = first.ok ? second : first;
            std::cerr << "FAIL: resolving " << id << ": "
                      << (failed.errors.empty() ? "unknown resolution error"
                                                : failed.errors.front()) << "\n";
            fs::remove_all(root, ec);
            return 1;
        }
        manager.set_feature_enabled(id, "perfect-works", false, &error);
    }

    const std::string prefix = "org.perfectworksbuild.individual.";
    const std::string story = prefix + "story-mode";
    const std::set<std::string> story_conflicts = {
        prefix + "half-encounters", prefix + "exp", prefix + "gold",
        prefix + "rebalanced-items", prefix + "rebalanced-enemies",
        prefix + "no-deathblow-levels", prefix + "no-damage-cap",
        prefix + "arena"};
    size_t compatible = 0;
    size_t incompatible = 0;
    for (size_t first_index = 0; first_index < ids.size(); ++first_index) {
        for (size_t second_index = first_index + 1;
             second_index < ids.size(); ++second_index) {
            const std::string& first_id = ids[first_index];
            const std::string& second_id = ids[second_index];
            const bool expected_conflict =
                (first_id == story && story_conflicts.count(second_id)) ||
                (second_id == story && story_conflicts.count(first_id));
            if (!manager.set_feature_enabled(
                    first_id, "perfect-works", true, &error) ||
                !manager.set_feature_enabled(
                    second_id, "perfect-works", true, &error)) {
                std::cerr << "FAIL: enabling catalog pair: " << error << "\n";
                fs::remove_all(root, ec);
                return 1;
            }
            const bool both_enabled = manager.feature_enabled(
                first_id, "perfect-works") && manager.feature_enabled(
                second_id, "perfect-works");
            if (expected_conflict) {
                if (both_enabled) {
                    std::cerr << "FAIL: declared conflict remained enabled\n";
                    fs::remove_all(root, ec);
                    return 1;
                }
                ++incompatible;
            } else {
                const ModResolution first =
                    manager.resolve("SLUS-00664", {}, disc1);
                const ModResolution second =
                    manager.resolve("SLUS-00664", {}, disc2);
                if (!both_enabled || !first.ok || !second.ok) {
                    const ModResolution& failed = first.ok ? second : first;
                    std::cerr << "FAIL: compatible pair " << first_id << " + "
                              << second_id << ": "
                              << (failed.errors.empty()
                                      ? "unknown resolution error"
                                      : failed.errors.front()) << "\n";
                    fs::remove_all(root, ec);
                    return 1;
                }
                ++compatible;
            }
            manager.set_feature_enabled(
                first_id, "perfect-works", false, &error);
            manager.set_feature_enabled(
                second_id, "perfect-works", false, &error);
        }
    }
    fs::remove_all(root, ec);
    if (compatible != 202 || incompatible != 8) {
        std::cerr << "FAIL: expected 202 compatible and 8 incompatible pairs, got "
                  << compatible << " and " << incompatible << "\n";
        return 1;
    }
    std::cout << "catalog validation passed: 21 packages, 202 compatible pairs, "
                 "8 conflicts\n";
    return 0;
}

static int install_catalog(int argc, char** argv) {
    ModPackageManager manager(argv[2]);
    std::string error;
    if (!manager.scan(&error)) {
        std::cerr << "FAIL: catalog install scan: " << error << "\n";
        return 1;
    }
    size_t installed = 0;
    for (int i = 3; i < argc; ++i) {
        if (!manager.install_archive(argv[i], nullptr, nullptr, &error)) {
            std::cerr << "FAIL: " << argv[i] << ": " << error << "\n";
            return 1;
        }
        ++installed;
    }
    std::cout << "installed catalog packages: " << installed << "\n";
    return 0;
}

static int enable_catalog(const fs::path& root) {
    ModPackageManager manager(root);
    std::string error;
    if (!manager.scan(&error) || !manager.load_state(&error)) {
        std::cerr << "FAIL: catalog state load: " << error << "\n";
        return 1;
    }
    size_t enabled = 0;
    for (const auto& [id, versions] : manager.packages()) {
        (void)versions;
        if (id.rfind("org.perfectworksbuild.individual.", 0) != 0) continue;
        const bool active =
            id != "org.perfectworksbuild.individual.story-mode";
        if (!manager.set_feature_enabled(
                id, "perfect-works", active, &error)) {
            std::cerr << "FAIL: catalog feature state: " << error << "\n";
            return 1;
        }
        enabled += active;
    }
    const auto option = [&](const char* package, const char* id,
                            const char* value) {
        return manager.set_feature_option(
            package, "perfect-works", id, value, &error);
    };
    if (enabled != 20 ||
        !option("org.perfectworksbuild.individual.arena", "mode", "expert") ||
        !option("org.perfectworksbuild.individual.exp", "multiplier", "2x") ||
        !option("org.perfectworksbuild.individual.gold", "multiplier", "2x") ||
        !option("org.perfectworksbuild.individual.portraits", "size", "resized") ||
        !manager.save_state(&error)) {
        std::cerr << "FAIL: catalog state save: " << error << "\n";
        return 1;
    }
    std::cout << "enabled catalog packages: " << enabled << "\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--enable-catalog")
        return enable_catalog(argv[2]);
    if (argc > 3 && std::string(argv[1]) == "--install-catalog")
        return install_catalog(argc, argv);
    if (argc > 2 && std::string(argv[1]) == "--catalog")
        return validate_catalog(argc, argv);
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-package-test";
    std::error_code ec;
    fs::remove_all(root, ec);

    {
        const uint8_t abc[] = {'a', 'b', 'c'};
        uint8_t one_shot[32], streamed[32];
        psx_sha256_compute(abc, sizeof(abc), one_shot);
        psx_sha256_ctx hash;
        psx_sha256_init(&hash);
        psx_sha256_update(&hash, abc, 1);
        psx_sha256_update(&hash, abc + 1, 2);
        psx_sha256_final(&hash, streamed);
        check(std::equal(one_shot, one_shot + 32, streamed),
              "streaming SHA-256 must match one-shot hashing");
    }

    write_text(root / "packages/base.mod/1.0.0/manifest.toml",
               manifest("base.mod", "1.0.0",
                   "\n[[option]]\n"
                   "id = \"difficulty\"\n"
                   "label = \"Difficulty\"\n"
                   "type = \"choice\"\n"
                   "default = \"normal\"\n"
                   "[[option.choice]]\nvalue = \"normal\"\nlabel = \"Normal\"\n"
                   "[[option.choice]]\nvalue = \"hard\"\nlabel = \"Hard\"\n"
                   "[[patch]]\n"
                   "target = \"main_exe\"\n"
                   "address = 2147487744\n"
                   "expected = \"01 02 03 04\"\n"
                   "replace = \"05 06 07 08\"\n"
                   "when_option = \"difficulty\"\n"
                   "when_value = \"hard\"\n"
                   "[[derived_disc]]\n"
                   "kind = \"vcdiff\"\n"
                   "patch = \"assets/base.xdelta3\"\n"
                   "patch_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
                   "output_size = 123456\n"
                   "output_sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"
                   "when_option = \"difficulty\"\n"
                   "when_value = \"hard\"\n"));
    write_text(root / "packages/base.mod/1.0.0/assets/base.xdelta3", "test");
    write_text(root / "packages/addon.mod/2.0.0/manifest.toml",
               manifest("addon.mod", "2.0.0",
                   "\n[[dependency]]\nid = \"base.mod\"\nversion = \"^1.0.0\"\n"));

    ModPackageManager manager(root);
    std::string error;
    check(manager.scan(&error), error.c_str());
    const ModPackage& metadata_package =
        manager.packages().at("base.mod").at("1.0.0");
    check(metadata_package.source_url == "https://example.com/project",
          "package source URL must be retained");
    check(metadata_package.author_links.size() == 1 &&
              metadata_package.author_links[0].name == "Test Author" &&
              metadata_package.author_links[0].url == "https://example.com/author",
          "package author links must be retained");
    write_deflated_package(root / "zip.psxmod");
    check(manager.install_archive(root / "zip.psxmod", nullptr, nullptr, &error),
          error.c_str());
    check(manager.packages().count("zip.mod") == 1,
          "deflated .psxmod must install");
    if (const char* external = std::getenv("PSXMOD_TEST_ARCHIVE");
        external && external[0]) {
        std::string installed_id, installed_version;
        check(manager.install_archive(external, &installed_id, &installed_version,
                                      &error),
              error.c_str());
        check(!installed_id.empty() && !installed_version.empty(),
              "external package must report installed identity");
    }
    check(manager.load_state(&error), error.c_str());
    check(manager.set_enabled("addon.mod", true, &error), error.c_str());
    ModResolution missing = manager.resolve("SLUS-TEST");
    check(!missing.ok, "missing dependency must fail resolution");
    check(manager.set_enabled("base.mod", true, &error), error.c_str());
    check(manager.set_option("base.mod", "difficulty", "hard", &error), error.c_str());
    check(!manager.set_option("base.mod", "difficulty", "impossible", &error),
          "invalid choice must be rejected");

    ModResolution resolved = manager.resolve("SLUS-TEST");
    check(resolved.ok, "valid dependency graph must resolve");
    check(resolved.ordered.size() == 2, "two packages should resolve");
    check(resolved.ordered.size() == 2 && resolved.ordered[0]->id == "base.mod",
          "dependency must precede dependent");
    check(resolved.writes.size() == 1, "selected declarative patch must resolve");
    check(resolved.writes.size() == 1 &&
              resolved.writes[0].location == 0x80001000ull &&
              resolved.writes[0].replacement[0] == 5,
          "resolved write must retain guest address and bytes");
    check(resolved.derived_discs.size() == 1 &&
              resolved.derived_discs[0].output_size == 123456,
          "selected derived-disc recipe must resolve");
    check(resolved.fingerprint.size() == 64, "plan fingerprint must be SHA-256 hex");
    const std::string fingerprint = resolved.fingerprint;

    check(manager.save_state(&error), error.c_str());
    ModPackageManager reload(root);
    check(reload.scan(&error), error.c_str());
    check(reload.load_state(&error), error.c_str());
    check(reload.resolve("SLUS-TEST").fingerprint == fingerprint,
          "saved state must resolve deterministically");
    check(!reload.remove_version("base.mod", "1.0.0", &error),
          "active package cannot be removed");
    check(reload.set_enabled("base.mod", false, &error), error.c_str());
    check(!reload.remove_version("base.mod", "1.0.0", &error),
          "enabled dependent must protect required version");
    check(reload.set_enabled("addon.mod", false, &error), error.c_str());
    check(reload.remove_version("base.mod", "1.0.0", &error), error.c_str());

    write_text(root / "packages/conflict.a/1.0.0/manifest.toml",
               "format_version = 1\n"
               "id = \"conflict.a\"\n"
               "version = \"1.0.0\"\n"
               "name = \"conflict.a\"\n"
               "resolver = \"declarative\"\n"
               "conflicts = [\"conflict.b\"]\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n");
    write_text(root / "packages/conflict.b/1.0.0/manifest.toml",
               manifest("conflict.b", "1.0.0"));
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("conflict.a", true, &error), error.c_str());
    check(reload.set_enabled("conflict.b", true, &error), error.c_str());
    check(!reload.selections().at("conflict.a").enabled &&
              reload.selections().at("conflict.b").enabled,
          "newly enabled package must disable an incompatible package");
    check(reload.conflict_blocker("conflict.a") == "conflict.b",
          "disabled package must report its active symmetric blocker");
    check(reload.resolve("SLUS-TEST").ok,
          "managed package exclusions must leave a valid resolution");
    check(reload.set_enabled("conflict.a", true, &error), error.c_str());
    check(reload.selections().at("conflict.a").enabled &&
              !reload.selections().at("conflict.b").enabled &&
              reload.conflict_blocker("conflict.b") == "conflict.a",
          "one-sided conflict declarations must behave symmetrically");
    const auto feature_conflict_manifest = [](const std::string& id,
                                              const std::string& conflicts) {
        return "format_version=3\nid=\"" + id +
            "\"\nversion=\"1.0.0\"\nname=\"" + id +
            "\"\nsave_compatibility=\"shared\"\n" + conflicts +
            "[[target]]\ngame_id=\"SLUS-TEST\"\n"
            "[[feature]]\nid=\"main\"\nname=\"Main\"\n";
    };
    write_text(
        root / "packages/feature-conflict.a/1.0.0/manifest.toml",
        feature_conflict_manifest(
            "feature-conflict.a",
            "conflicts=[\"feature-conflict.b\"]\n"));
    write_text(
        root / "packages/feature-conflict.b/1.0.0/manifest.toml",
        feature_conflict_manifest("feature-conflict.b", ""));
    check(reload.scan(&error), error.c_str());
    check(reload.set_feature_enabled(
              "feature-conflict.a", "main", true, &error) &&
              reload.set_feature_enabled(
                  "feature-conflict.b", "main", true, &error), error.c_str());
    check(!reload.feature_enabled("feature-conflict.a", "main") &&
              reload.feature_enabled("feature-conflict.b", "main") &&
              reload.conflict_blocker("feature-conflict.a") ==
                  "feature-conflict.b",
          "feature-style conflicts must auto-disable and expose their blocker");

    write_text(root / "packages/matrix.mod/1.0.0/manifest.toml",
               manifest("matrix.mod", "1.0.0",
                   "\n[[option]]\n"
                   "id = \"title\"\n"
                   "label = \"Title\"\n"
                   "type = \"choice\"\n"
                   "default = \"mega\"\n"
                   "[[option.choice]]\n"
                   "value = \"mega\"\n"
                   "label = \"Mega\"\n"
                   "[[option.choice]]\n"
                   "value = \"rockman\"\n"
                   "label = \"Rockman\"\n"
                   "\n[[option]]\n"
                   "id = \"script\"\n"
                   "label = \"Script\"\n"
                   "type = \"choice\"\n"
                   "default = \"original\"\n"
                   "[[option.choice]]\n"
                   "value = \"original\"\n"
                   "label = \"Original\"\n"
                   "[[option.choice]]\n"
                   "value = \"retranslation\"\n"
                   "label = \"Retranslation\"\n"
                   "\n[[derived_disc]]\n"
                   "kind = \"vcdiff\"\n"
                   "patch = \"assets/matrix.xdelta3\"\n"
                   "patch_sha256 = \"2222222222222222222222222222222222222222222222222222222222222222\"\n"
                   "output_size = 222222\n"
                   "output_sha256 = \"3333333333333333333333333333333333333333333333333333333333333333\"\n"
                   "when = { title = \"rockman\", script = \"retranslation\" }\n"));
    write_text(root / "packages/matrix.mod/1.0.0/assets/matrix.xdelta3", "test");
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("conflict.a", false, &error), error.c_str());
    check(reload.set_enabled("conflict.b", false, &error), error.c_str());
    check(reload.set_enabled("matrix.mod", true, &error), error.c_str());
    check(reload.set_option("matrix.mod", "title", "rockman", &error), error.c_str());
    check(reload.set_option("matrix.mod", "script", "retranslation", &error), error.c_str());
    ModResolution matrix = reload.resolve("SLUS-TEST");
    check(matrix.ok && matrix.derived_discs.size() == 1 &&
              matrix.derived_discs[0].output_size == 222222,
          "multi-option derived-disc condition must match selected values");

    mod_clear_builtin_resolvers_for_tests();
    bool resolver_context_seen = false;
    check(mod_register_builtin_resolver(
              "context-test",
              [&](const ModPackage& package, const ModSelection& selection,
                  const ModBuiltinResolverContext& context,
                  std::vector<ModResolution::Write>& writes,
                  std::vector<std::string>& errors) {
                  (void)writes;
                  (void)errors;
                  resolver_context_seen =
                      package.id == "context.consumer" &&
                      selection.enabled &&
                      context.active_packages &&
                      context.selections &&
                      context.active_packages->count("context.provider") == 1 &&
                      context.active_packages->count("context.consumer") == 1 &&
                      context.selections->at("context.provider").enabled &&
                      context.selections->at("context.consumer").enabled;
                  return resolver_context_seen;
              }),
          "test resolver must register");
    write_text(root / "packages/context.provider/1.0.0/manifest.toml",
               manifest("context.provider", "1.0.0"));
    write_text(root / "packages/context.consumer/1.0.0/manifest.toml",
               "format_version = 1\n"
               "id = \"context.consumer\"\n"
               "version = \"1.0.0\"\n"
               "name = \"context.consumer\"\n"
               "resolver = \"builtin:context-test\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n");
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("matrix.mod", false, &error), error.c_str());
    check(reload.set_enabled("context.provider", true, &error), error.c_str());
    check(reload.set_enabled("context.consumer", true, &error), error.c_str());
    ModResolution context_resolution = reload.resolve("SLUS-TEST");
    check(context_resolution.ok && resolver_context_seen,
          "built-in resolver must receive active package selection context");
    check(reload.set_enabled("context.consumer", false, &error), error.c_str());
    check(reload.set_enabled("context.provider", false, &error), error.c_str());
    mod_clear_builtin_resolvers_for_tests();

    mod_clear_plugins_for_tests();
    check(mod_register_vblank_plugin(
              "test.vblank", +[]() {}),
          "test plugin must register");
    write_text(root / "packages/plugin.mod/1.0.0/manifest.toml",
               "format_version = 5\n"
               "id = \"plugin.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Plugin Mod\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"vblank\"\n"
               "name = \"VBlank Plugin\"\n"
               "[[plugin]]\n"
               "feature = \"vblank\"\n"
               "id = \"test.vblank\"\n");
    check(reload.scan(&error), error.c_str());
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", true, &error), error.c_str());
    ModResolution plugin_resolution = reload.resolve("SLUS-TEST");
    check(plugin_resolution.ok && plugin_resolution.plugins.size() == 1 &&
              plugin_resolution.plugins[0].id == "test.vblank" &&
              plugin_resolution.plugins[0].package_id == "plugin.mod" &&
              plugin_resolution.plugins[0].feature_id == "vblank",
          "enabled trusted plugin must resolve with feature ownership");
    const std::string plugin_fingerprint = plugin_resolution.fingerprint;
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", false, &error), error.c_str());
    ModResolution plugin_disabled = reload.resolve("SLUS-TEST");
    check(plugin_disabled.ok && plugin_disabled.plugins.empty() &&
              plugin_disabled.fingerprint != plugin_fingerprint,
          "disabling a plugin feature must remove its activation and change "
          "the fingerprint");
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", true, &error), error.c_str());
    mod_clear_plugins_for_tests();
    ModResolution plugin_unavailable = reload.resolve("SLUS-TEST");
    check(!plugin_unavailable.ok &&
              std::any_of(
                  plugin_unavailable.errors.begin(),
                  plugin_unavailable.errors.end(),
                  [](const std::string& item) {
                      return item.find(
                          "trusted plugin is unavailable: test.vblank") !=
                          std::string::npos;
                  }),
          "enabled plugin must fail closed when its trusted implementation "
          "is unavailable");
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", false, &error), error.c_str());
    write_text(root / "packages/features.mod/1.0.0/manifest.toml",
               manifest("features.mod", "1.0.0",
                   "\n[[feature]]\n"
                   "id = \"title-screen\"\n"
                   "name = \"Title Screen\"\n"
                   "group = \"Localization\"\n"
                   "\n[[feature]]\n"
                   "id = \"retranslation\"\n"
                   "name = \"Retranslation\"\n"
                   "group = \"Localization\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-collision\"\n"
                   "name = \"Title Collision\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-identical\"\n"
                   "name = \"Title Identical\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-partial-compatible\"\n"
                   "name = \"Title Partial Compatible\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-partial-conflict\"\n"
                   "name = \"Title Partial Conflict\"\n"
                   "\n[[option]]\n"
                   "feature = \"title-screen\"\n"
                   "id = \"variant\"\n"
                   "label = \"Variant\"\n"
                   "type = \"choice\"\n"
                   "default = \"usa\"\n"
                   "[[option.choice]]\n"
                   "value = \"usa\"\n"
                   "label = \"Mega Man X6\"\n"
                   "[[option.choice]]\n"
                   "value = \"japan\"\n"
                   "label = \"Rockman X6\"\n"
                   "\n[[option]]\n"
                   "feature = \"retranslation\"\n"
                   "id = \"variant\"\n"
                   "label = \"Variant\"\n"
                   "type = \"boolean\"\n"
                   "default = \"true\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-screen\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495936\n"
                   "expected = \"0102\"\n"
                   "replace = \"a1a2\"\n"
                   "when = { variant = \"japan\" }\n"
                   "\n[[patch]]\n"
                   "feature = \"retranslation\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 23520\n"
                   "expected = \"03\"\n"
                   "replace = \"b3\"\n"
                   "when = { variant = \"true\" }\n"
                   "\n[[patch]]\n"
                   "feature = \"title-collision\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"02\"\n"
                   "replace = \"ff\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-identical\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495936\n"
                   "expected = \"0102\"\n"
                   "replace = \"a1a2\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-partial-compatible\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"0209\"\n"
                   "replace = \"a2c9\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-partial-conflict\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"ff09\"\n"
                   "replace = \"a2c9\"\n"));
    check(reload.scan(&error), error.c_str());
    check(!reload.set_enabled("features.mod", true, &error),
          "feature-style package must not expose package enablement");
    check(reload.set_feature_option(
              "features.mod", "title-screen", "variant", "japan", &error),
          error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "title-screen", true, &error), error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "retranslation", true, &error), error.c_str());
    ModResolution features = reload.resolve("SLUS-TEST");
    check(features.ok && features.writes.size() == 2,
          "independently enabled features must compose their operations");
    check(features.ok && features.writes[0].feature_id == "title-screen" &&
              features.writes[1].feature_id == "retranslation",
          "resolved writes must retain feature ownership");
    check(reload.set_feature_enabled(
              "features.mod", "title-collision", true, &error), error.c_str());
    ModResolution collision = reload.resolve("SLUS-TEST");
    check(!collision.ok && collision.diagnostics.size() == 1,
          "overlapping feature writes must produce a structured diagnostic");
    check(!collision.diagnostics.empty() &&
              collision.diagnostics[0].feature_id == "title-collision" &&
              collision.diagnostics[0].other_feature_id == "title-screen" &&
              !collision.diagnostics[0].resource.empty(),
          "collision diagnostic must identify both features and the resource");
    check(reload.set_feature_enabled(
              "features.mod", "title-collision", false, &error), error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "title-identical", true, &error), error.c_str());
    ModResolution identical = reload.resolve("SLUS-TEST");
    check(identical.ok && identical.writes.size() == 2,
          "truly identical writes must coalesce deterministically");
    check(reload.set_feature_enabled(
              "features.mod", "title-partial-compatible", true, &error),
          error.c_str());
    ModResolution partial_compatible = reload.resolve("SLUS-TEST");
    check(partial_compatible.ok && partial_compatible.writes.size() == 3,
          "partially overlapping writes with matching expected and replacement "
          "bytes must compose");
    check(reload.set_feature_enabled(
              "features.mod", "title-partial-conflict", true, &error),
          error.c_str());
    ModResolution partial_conflict = reload.resolve("SLUS-TEST");
    check(!partial_conflict.ok && !partial_conflict.diagnostics.empty() &&
              partial_conflict.diagnostics[0].resource ==
                  "main_exe:0x80003001-0x80003002",
          "one differing expected byte in a partial overlap must identify "
          "the exact contested byte");
    check(reload.set_feature_enabled(
              "features.mod", "title-partial-conflict", false, &error),
          error.c_str());
    check(reload.save_state(&error), error.c_str());
    ModPackageManager feature_reload(root);
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.load_state(&error), error.c_str());
    check(feature_reload.feature_enabled("features.mod", "title-screen") &&
              feature_reload.feature_enabled("features.mod", "retranslation") &&
              !feature_reload.feature_enabled("features.mod", "title-collision"),
          "per-feature enabled state must survive save/reload");
    check(feature_reload.feature_option_value(
              "features.mod", "title-screen", "variant") == "japan",
          "feature-scoped option values must survive save/reload");
    check(feature_reload.resolve("SLUS-TEST").fingerprint ==
              partial_compatible.fingerprint,
          "feature state must resolve deterministically after reload");

    write_text(root / "packages/parametric.mod/1.0.0/manifest.toml",
               "format_version = 3\n"
               "id = \"parametric.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Parametric\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"numeric\"\n"
               "name = \"Numeric\"\n"
               "[[feature]]\n"
               "id = \"numeric-collision\"\n"
               "name = \"Numeric Collision\"\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"byte\"\n"
               "label = \"Byte\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 255\n"
               "step = 1\n"
               "default = 7\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"word\"\n"
               "label = \"Word\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 65534\n"
               "step = 1\n"
               "default = 4660\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"dword\"\n"
               "label = \"Dword\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 4294967295\n"
               "step = 1\n"
               "default = 305419896\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"split\"\n"
               "label = \"Split\"\n"
               "type = \"integer\"\n"
               "min = 200000\n"
               "max = 600000\n"
               "step = 1\n"
               "default = 425984\n"
               "[[option]]\n"
               "feature = \"numeric-collision\"\n"
               "id = \"byte\"\n"
               "label = \"Byte\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 255\n"
               "default = 8\n"
               "[[constraint]]\n"
               "feature = \"numeric\"\n"
               "kind = \"ordered_integer\"\n"
               "direction = \"nondecreasing\"\n"
               "options = [\"byte\", \"word\", \"dword\"]\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500032\n"
               "expected = \"00\"\n"
               "replace_from = { option = \"byte\", encoding = \"u8\" }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500033\n"
               "expected = \"07\"\n"
               "replace_from = { option = \"byte\", encoding = \"u8\" }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500034\n"
               "expected = \"0000\"\n"
               "replace_from = { option = \"word\", encoding = \"u16le\", addend = 1 }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500036\n"
               "expected = \"00000000\"\n"
               "replace_from = { option = \"dword\", encoding = \"u32le\" }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500040\n"
               "expected = \"0000aabb\"\n"
               "replace_from = { option = \"word\", encoding = \"u16le\", offset = 0 }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500048\n"
               "expected = \"0600013c00802134\"\n"
               "replace_from = { option = \"split\", encoding = \"mips_lui_ori_u32\", omit_when_default = true }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500056\n"
               "expected = \"0400013c00202134\"\n"
               "replace_from = { option = \"split\", encoding = \"mips_lui_ori_u32\", omit_when_default = true }\n"
               "[[patch]]\n"
               "feature = \"numeric-collision\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500032\n"
               "expected = \"00\"\n"
               "replace_from = { option = \"byte\", encoding = \"u8\" }\n");
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", true, &error), error.c_str());
    check(!feature_reload.set_feature_option(
              "parametric.mod", "numeric", "byte", "+7", &error),
          "integer options must reject a leading plus");
    check(!feature_reload.set_feature_option(
              "parametric.mod", "numeric", "byte", "07", &error),
          "integer options must reject noncanonical leading zeroes");
    ModResolution parametric = feature_reload.resolve("SLUS-TEST");
    const auto numeric_write = [&](uint64_t location)
        -> const ModResolution::Write* {
        const auto found = std::find_if(
            parametric.writes.begin(), parametric.writes.end(),
            [&](const ModResolution::Write& write) {
                return write.package_id == "parametric.mod" &&
                       write.location == location;
            });
        return found == parametric.writes.end() ? nullptr : &*found;
    };
    const ModResolution::Write* byte_write = numeric_write(0x80004000ull);
    const ModResolution::Write* noop_write = numeric_write(0x80004001ull);
    const ModResolution::Write* word_write = numeric_write(0x80004002ull);
    const ModResolution::Write* dword_write = numeric_write(0x80004004ull);
    const ModResolution::Write* guarded_word_write =
        numeric_write(0x80004008ull);
    check(parametric.ok && byte_write &&
              byte_write->replacement == std::vector<uint8_t>({7}),
          "u8 replace_from must encode the selected value");
    check(!noop_write,
          "replace_from equal to the stock guard must elide the no-op write");
    check(word_write &&
              word_write->replacement == std::vector<uint8_t>({0x35, 0x12}),
          "u16le replace_from must apply addend and encode little-endian");
    check(dword_write &&
              dword_write->replacement ==
                  std::vector<uint8_t>({0x78, 0x56, 0x34, 0x12}),
          "u32le replace_from must encode little-endian");
    check(guarded_word_write &&
              guarded_word_write->replacement ==
                  std::vector<uint8_t>({0x34, 0x12, 0xaa, 0xbb}),
          "replace_from must preserve guarded bytes outside its value field");
    check(!numeric_write(0x80004010ull) &&
              !numeric_write(0x80004018ull),
          "omit_when_default must suppress every split-immediate site");
    const std::string parametric_fingerprint = parametric.fingerprint;
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "byte", "9", &error),
          error.c_str());
    check(!feature_reload.set_feature_option(
              "parametric.mod", "numeric", "word", "5", &error),
          "enabled ordered integer features must reject inverted values");
    ModResolution changed_parametric = feature_reload.resolve("SLUS-TEST");
    check(changed_parametric.ok &&
              changed_parametric.fingerprint != parametric_fingerprint,
          "changing a generated integer must change the plan fingerprint");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "split", "200000", &error),
          error.c_str());
    ModResolution split_parametric = feature_reload.resolve("SLUS-TEST");
    const auto split_write = [&](uint64_t location)
        -> const ModResolution::Write* {
        const auto found = std::find_if(
            split_parametric.writes.begin(), split_parametric.writes.end(),
            [&](const ModResolution::Write& write) {
                return write.package_id == "parametric.mod" &&
                       write.location == location;
            });
        return found == split_parametric.writes.end() ? nullptr : &*found;
    };
    check(split_write(0x80004010ull) &&
              split_write(0x80004010ull)->replacement ==
                  std::vector<uint8_t>({
                      0x03, 0x00, 0x01, 0x3c,
                      0x40, 0x0d, 0x21, 0x34}) &&
              split_write(0x80004018ull) &&
              split_write(0x80004018ull)->replacement ==
                  std::vector<uint8_t>({
                      0x03, 0x00, 0x01, 0x3c,
                      0x40, 0x0d, 0x21, 0x34}),
          "typed MIPS split encodings must update every guarded pair");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "split", "270336", &error),
          error.c_str());
    ModResolution partial_stock_split =
        feature_reload.resolve("SLUS-TEST");
    check(std::count_if(
              partial_stock_split.writes.begin(),
              partial_stock_split.writes.end(),
              [](const ModResolution::Write& write) {
                  return write.package_id == "parametric.mod" &&
                         (write.location == 0x80004010ull ||
                          write.location == 0x80004018ull);
              }) == 2,
          "a nondefault split value must retain ownership of a pair whose "
          "replacement happens to equal stock");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "split", "425984", &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric-collision", true, &error),
          error.c_str());
    check(!feature_reload.resolve("SLUS-TEST").ok,
          "different generated values at one guarded byte must collide");
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric-collision", false, &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", false, &error), error.c_str());
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "word", "0", &error),
          "disabled features may retain an invalid draft");
    check(!feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", true, &error),
          "an invalid ordered integer draft must block feature enablement");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "word", "4660", &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", true, &error), error.c_str());
    check(feature_reload.save_state(&error), error.c_str());
    ModPackageManager parametric_reload(root);
    check(parametric_reload.scan(&error), error.c_str());
    check(parametric_reload.load_state(&error), error.c_str());
    check(parametric_reload.feature_option_value(
              "parametric.mod", "numeric", "byte") == "9" &&
              parametric_reload.resolve("SLUS-TEST").fingerprint ==
                  changed_parametric.fingerprint,
          "generated integer state and fingerprint must survive reload");

    write_text(root / "packages/requires.mod/1.0.0/manifest.toml",
               "format_version = 4\n"
               "id = \"requires.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Requires\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"prereq\"\n"
               "name = \"Prerequisite\"\n"
               "[[feature]]\n"
               "id = \"dependent\"\n"
               "name = \"Dependent\"\n"
               "[[feature]]\n"
               "id = \"optioned-dependent\"\n"
               "name = \"Optioned Dependent\"\n"
               "[[option]]\n"
               "feature = \"prereq\"\n"
               "id = \"availability\"\n"
               "label = \"Available in\"\n"
               "type = \"choice\"\n"
               "default = \"main\"\n"
               "[[option.choice]]\n"
               "value = \"main\"\n"
               "label = \"Main Stages\"\n"
               "[[option.choice]]\n"
               "value = \"everywhere\"\n"
               "label = \"Everywhere\"\n"
               "[[constraint]]\n"
               "feature = \"dependent\"\n"
               "kind = \"requires_feature\"\n"
               "requires_feature = \"prereq\"\n"
               "[[constraint]]\n"
               "feature = \"optioned-dependent\"\n"
               "kind = \"requires_feature\"\n"
               "requires_feature = \"prereq\"\n"
               "requires_option = \"availability\"\n"
               "requires_value = \"everywhere\"\n");
    check(parametric_reload.scan(&error), error.c_str());
    check(parametric_reload.set_feature_enabled(
              "requires.mod", "dependent", true, &error), error.c_str());
    check(parametric_reload.feature_enabled("requires.mod", "prereq") &&
              parametric_reload.feature_enabled("requires.mod", "dependent"),
          "enabling a dependent feature must auto-enable its prerequisite");
    check(parametric_reload.set_feature_enabled(
              "requires.mod", "optioned-dependent", true, &error),
          error.c_str());
    check(parametric_reload.feature_enabled(
              "requires.mod", "optioned-dependent") &&
              parametric_reload.feature_option_value(
                  "requires.mod", "prereq", "availability") == "everywhere",
          "enabling an optioned dependent must auto-select the required "
          "prerequisite value");
    check(parametric_reload.set_feature_option(
              "requires.mod", "prereq", "availability", "main", &error),
          error.c_str());
    check(parametric_reload.feature_enabled("requires.mod", "prereq") &&
              parametric_reload.feature_enabled("requires.mod", "dependent") &&
              !parametric_reload.feature_enabled(
                  "requires.mod", "optioned-dependent"),
          "weakening a prerequisite option must disable invalid dependents");
    check(parametric_reload.set_feature_enabled(
              "requires.mod", "prereq", false, &error), error.c_str());
    check(!parametric_reload.feature_enabled("requires.mod", "prereq") &&
              !parametric_reload.feature_enabled("requires.mod", "dependent"),
          "disabling a prerequisite must disable downstream dependents");

    const auto reject_parametric_manifest =
        [&](const std::string& name, const std::string& body) {
            const fs::path path = root / (name + ".toml");
            write_text(path, body);
            ModPackage rejected;
            return !ModPackageManager::read_manifest(path, rejected, &error);
        };
    const std::string dynamic_prelude =
        "id=\"bad.dynamic\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
        "[[target]]\ngame_id=\"SLUS-TEST\"\n"
        "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
        "[[option]]\nfeature=\"bad\"\nid=\"value\"\nlabel=\"Value\"\n"
        "type=\"integer\"\nmin=0\nmax=255\ndefault=1\n";
    check(reject_parametric_manifest(
              "dynamic-v1",
              "format_version=1\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\"}\n"),
          "format 1 manifests must reject replace_from");
    check(reject_parametric_manifest(
              "dynamic-both",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\nreplace=\"01\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\"}\n"),
          "a patch must reject simultaneous replace and replace_from");
    check(reject_parametric_manifest(
              "dynamic-width",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"0000\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\",offset=2}\n"),
          "replace_from value must stay inside the expected guard");
    check(reject_parametric_manifest(
              "dynamic-overflow",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\",addend=1}\n"),
          "the full option range plus addend must fit its encoding");
    check(reject_parametric_manifest(
              "dynamic-unknown",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\",shift=1}\n"),
          "replace_from must reject unknown transform fields");
    check(reject_parametric_manifest(
              "dynamic-mips-v2",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"0600013c00802134\"\n"
                  "replace_from={option=\"value\","
                  "encoding=\"mips_lui_ori_u32\"}\n"),
          "typed MIPS pairs must require package format 3");
    check(reject_parametric_manifest(
              "dynamic-mips-unlinked",
              "format_version=3\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"0600013c00802234\"\n"
                  "replace_from={option=\"value\","
                  "encoding=\"mips_lui_ori_u32\"}\n"),
          "typed MIPS pairs must reject unlinked registers");
    check(reject_parametric_manifest(
              "constraint-inverted-default",
              "format_version=3\n"
              "id=\"bad.dynamic\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
              "[[target]]\ngame_id=\"SLUS-TEST\"\n"
              "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
              "[[option]]\nfeature=\"bad\"\nid=\"low\"\nlabel=\"Low\"\n"
              "type=\"integer\"\nmin=0\nmax=10\ndefault=8\n"
              "[[option]]\nfeature=\"bad\"\nid=\"high\"\nlabel=\"High\"\n"
              "type=\"integer\"\nmin=0\nmax=10\ndefault=2\n"
              "[[constraint]]\nfeature=\"bad\"\n"
              "kind=\"ordered_integer\"\ndirection=\"nondecreasing\"\n"
              "options=[\"low\",\"high\"]\n"),
          "ordered integer defaults must satisfy their constraint");
    check(reject_parametric_manifest(
              "dynamic-step-default",
              "format_version=2\n"
              "id=\"bad.dynamic\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
              "[[target]]\ngame_id=\"SLUS-TEST\"\n"
              "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
              "[[option]]\nfeature=\"bad\"\nid=\"value\"\nlabel=\"Value\"\n"
              "type=\"integer\"\nmin=0\nmax=10\nstep=2\ndefault=3\n"),
          "integer defaults must align to their declared step");

    write_text(root / "packages/sparse.mod/1.0.0/manifest.toml",
               "format_version = 4\n"
               "id = \"sparse.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Sparse Fields\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"timing\"\n"
               "name = \"Timing\"\n"
               "[[feature]]\n"
               "id = \"cancellable\"\n"
               "name = \"Cancellable\"\n"
               "[[feature]]\n"
               "id = \"collision\"\n"
               "name = \"Collision\"\n"
               "[[feature]]\n"
               "id = \"guard-mismatch\"\n"
               "name = \"Guard Mismatch\"\n"
               "[[feature]]\n"
               "id = \"predicates\"\n"
               "name = \"Predicates\"\n"
               "[[option]]\n"
               "feature = \"timing\"\n"
               "id = \"frames\"\n"
               "label = \"Frames\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 99\n"
               "default = 2\n"
               "[[option]]\n"
               "feature = \"predicates\"\n"
               "id = \"value\"\n"
               "label = \"Value\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 10\n"
               "default = 5\n"
               "[[patch]]\n"
               "feature = \"timing\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 0, option = \"frames\", encoding = \"u8\" }]\n"
               "when_integer = { option = \"frames\", op = \"gt\", value = 0 }\n"
               "[[patch]]\n"
               "feature = \"timing\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 0, replace = \"01\" }, "
               "{ offset = 2, replace = \"00\" }]\n"
               "when_integer = { option = \"frames\", op = \"eq\", value = 0 }\n"
               "[[patch]]\n"
               "feature = \"cancellable\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 1, replace = \"42\" }]\n"
               "[[patch]]\n"
               "feature = \"collision\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 0, replace = \"09\" }]\n"
               "[[patch]]\n"
               "feature = \"guard-mismatch\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"03000132\"\n"
               "fields = [{ offset = 3, replace = \"33\" }]\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508480\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"eq\", value = 5 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508481\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"ne\", value = 5 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508482\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"lt\", value = 6 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508483\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"le\", value = 5 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508484\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"gt\", value = 4 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508485\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"ge\", value = 5 }\n");
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "timing", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "cancellable", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "predicates", true, &error), error.c_str());
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "5", &error),
          error.c_str());
    ModResolution sparse_positive = feature_reload.resolve("SLUS-TEST");
    const auto sparse_writes_at = [&](const ModResolution& plan,
                                      uint64_t location) {
        return std::count_if(
            plan.writes.begin(), plan.writes.end(),
            [&](const ModResolution::Write& write) {
                return write.package_id == "sparse.mod" &&
                       write.location == location;
            });
    };
    check(sparse_positive.ok &&
              sparse_writes_at(sparse_positive, 0x80006000ull) == 2,
          "adjacent sparse fields in one guarded record must compose");
    const auto timing_write = std::find_if(
        sparse_positive.writes.begin(), sparse_positive.writes.end(),
        [](const ModResolution::Write& write) {
            return write.package_id == "sparse.mod" &&
                   write.feature_id == "timing" &&
                   write.location == 0x80006000ull;
        });
    check(timing_write != sparse_positive.writes.end() &&
              timing_write->expected ==
                  std::vector<uint8_t>({2, 0, 1, 0x32}) &&
              timing_write->replacement.empty() &&
              timing_write->fields.size() == 1 &&
              timing_write->fields[0].offset == 0 &&
              timing_write->fields[0].replacement ==
                  std::vector<uint8_t>({5}),
          "sparse resolution must retain the complete guard but own only "
          "declared fields");
    check(std::count_if(
              sparse_positive.writes.begin(),
              sparse_positive.writes.end(),
              [](const ModResolution::Write& write) {
                  return write.package_id == "sparse.mod" &&
                         write.feature_id == "predicates";
              }) == 5,
          "eq/ne/lt/le/gt/ge predicates must resolve with typed integer "
          "semantics");
    const std::string sparse_positive_fingerprint =
        sparse_positive.fingerprint;
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "0", &error),
          error.c_str());
    ModResolution sparse_zero = feature_reload.resolve("SLUS-TEST");
    const auto zero_timing = std::find_if(
        sparse_zero.writes.begin(), sparse_zero.writes.end(),
        [](const ModResolution::Write& write) {
            return write.package_id == "sparse.mod" &&
                   write.feature_id == "timing";
        });
    check(sparse_zero.ok && zero_timing != sparse_zero.writes.end() &&
              zero_timing->fields.size() == 2 &&
              zero_timing->fields[0].offset == 0 &&
              zero_timing->fields[0].replacement ==
                  std::vector<uint8_t>({1}) &&
              zero_timing->fields[1].offset == 2 &&
              zero_timing->fields[1].replacement ==
                  std::vector<uint8_t>({0}) &&
              sparse_zero.fingerprint != sparse_positive_fingerprint,
          "zero and nonzero conditional sparse plans must own their exact "
          "distinct fields and fingerprints");
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "2", &error),
          error.c_str());
    ModResolution sparse_stock = feature_reload.resolve("SLUS-TEST");
    check(sparse_stock.ok &&
              sparse_writes_at(sparse_stock, 0x80006000ull) == 1,
          "stock-equal sparse fields must elide only their own no-op while "
          "an adjacent feature remains active");
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "5", &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "collision", true, &error), error.c_str());
    check(!feature_reload.resolve("SLUS-TEST").ok,
          "different sparse replacements for one owned byte must collide");
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "collision", false, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "guard-mismatch", true, &error), error.c_str());
    check(!feature_reload.resolve("SLUS-TEST").ok,
          "overlapping complete guards with incompatible expected bytes "
          "must fail before runtime");
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "guard-mismatch", false, &error),
          error.c_str());

    const std::string sparse_prelude =
        "id=\"bad.sparse\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
        "[[target]]\ngame_id=\"SLUS-TEST\"\n"
        "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
        "[[option]]\nfeature=\"bad\"\nid=\"value\"\nlabel=\"Value\"\n"
        "type=\"integer\"\nmin=0\nmax=10\nstep=2\ndefault=2\n";
    const std::string sparse_patch =
        "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
        "address=2147487744\nexpected=\"00000000\"\n";
    check(reject_parametric_manifest(
              "sparse-v3",
              "format_version=3\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"),
          "sparse fields must require format 4");
    check(reject_parametric_manifest(
              "sparse-empty",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[]\n"),
          "sparse fields must not be empty");
    check(reject_parametric_manifest(
              "sparse-both",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "replace=\"01000000\"\n"
                  "fields=[{offset=0,replace=\"01\"}]\n"),
          "sparse fields must be mutually exclusive with full replace");
    check(reject_parametric_manifest(
              "sparse-overlap",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"0102\"},"
                  "{offset=1,replace=\"03\"}]\n"),
          "sparse fields must reject overlapping owned ranges");
    check(reject_parametric_manifest(
              "sparse-bounds",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=4,replace=\"01\"}]\n"),
          "sparse fields must stay inside the complete guard");
    check(reject_parametric_manifest(
              "sparse-mixed",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\",option=\"value\","
                  "encoding=\"u8\"}]\n"),
          "one sparse field must not mix literal and dynamic forms");
    check(reject_parametric_manifest(
              "sparse-overflow",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,option=\"value\",encoding=\"u8\","
                  "addend=250}]\n"),
          "sparse dynamic field ranges plus addends must fit encoding");
    check(reject_parametric_manifest(
              "sparse-predicate-op",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"value\",op=\"between\",value=2}\n"),
          "typed integer predicates must reject unknown operations");
    check(reject_parametric_manifest(
              "sparse-predicate-feature",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"missing\",op=\"eq\",value=2}\n"),
          "typed integer predicates must reference same-feature integers");
    check(reject_parametric_manifest(
              "sparse-predicate-bounds",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"value\",op=\"gt\",value=11}\n"),
          "typed integer predicate constants must stay in option bounds");
    check(reject_parametric_manifest(
              "sparse-predicate-step",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"value\",op=\"eq\",value=3}\n"),
          "typed equality predicates must use selectable values");

    const std::vector<uint8_t> overlay_a = {1, 2, 3, 4};
    const std::vector<uint8_t> overlay_b = {8, 9};
    const std::vector<uint8_t> overlay_c = {3, 4, 7};
    const std::string overlay_disc_hash(64, '4');
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/a.bin", overlay_a);
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/b.bin", overlay_b);
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/c.bin", overlay_c);
    write_text(root / "packages/overlay.mod/1.0.0/manifest.toml",
               manifest("overlay.mod", "1.0.0",
                   "disc_sha256 = \"" + overlay_disc_hash + "\"\n"
                   "[[feature]]\n"
                   "id = \"asset-a\"\n"
                   "name = \"Asset A\"\n"
                   "[[feature]]\n"
                   "id = \"asset-b\"\n"
                   "name = \"Asset B\"\n"
                   "[[feature]]\n"
                   "id = \"asset-c\"\n"
                   "name = \"Asset C\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-a\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 100\n"
                   "file = \"assets/a.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_a) + "\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-b\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 102\n"
                   "file = \"assets/b.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_b) + "\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-c\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 102\n"
                   "file = \"assets/c.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_c) + "\"\n"));
    check(feature_reload.scan(&error), error.c_str());
    const ModPackage* overlay_package =
        feature_reload.selected_package("overlay.mod");
    check(overlay_package && overlay_package->overlays.size() == 3 &&
              overlay_package->overlays[0].size == overlay_a.size(),
          "manifest scan must verify and retain overlay metadata");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-a", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-c", true, &error), error.c_str());
    ModResolution overlay_compatible =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(overlay_compatible.ok && overlay_compatible.overlays.size() == 2,
          "partially overlapping file overlays with matching payload bytes "
          "must compose");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-b", true, &error), error.c_str());
    ModResolution overlay_collision =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(!overlay_collision.ok &&
              overlay_collision.diagnostics.size() == 1 &&
              overlay_collision.diagnostics[0].feature_id == "asset-b" &&
              overlay_collision.diagnostics[0].other_feature_id == "asset-a",
          "overlapping file overlays must identify both owning features");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-b", false, &error), error.c_str());
    ModResolution one_overlay =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(one_overlay.ok && one_overlay.overlays.size() == 2 &&
              one_overlay.overlays[0].payload == overlay_a &&
              one_overlay.overlays[1].payload == overlay_c,
          "compatible enabled overlay payloads must remain in the plan");

    write_text(root / "feature-derived.toml",
               manifest("bad.derived", "1.0.0",
                   "\n[[feature]]\n"
                   "id = \"bad\"\n"
                   "name = \"Bad\"\n"
                   "[[derived_disc]]\n"
                   "patch = \"bad.xdelta3\"\n"
                   "patch_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
                   "output_size = 1\n"
                   "output_sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"));
    ModPackage feature_derived;
    check(!ModPackageManager::read_manifest(
              root / "feature-derived.toml", feature_derived, &error),
          "feature-style packages must reject derived-disc operations");

    ModPackage invalid;
    write_text(root / "bad.toml",
               "format_version=1\nid=\"../bad\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
               "[[target]]\ngame_id=\"SLUS-TEST\"\n");
    check(!ModPackageManager::read_manifest(root / "bad.toml", invalid, &error),
          "unsafe package id must be rejected");
    write_text(root / "wrapped-version.toml",
               "format_version=4294967302\nid=\"wrapped.version\"\n"
               "version=\"1.0.0\"\nname=\"Wrapped\"\n"
               "[[target]]\ngame_id=\"SLUS-TEST\"\n");
    check(!ModPackageManager::read_manifest(
              root / "wrapped-version.toml", invalid, &error),
          "format versions must be validated before narrowing to uint32");

    const std::string scoped_disc_a(64, 'a');
    const std::string scoped_disc_b(64, 'b');
    const auto scoped_manifest = [&](int format_version) {
        return
            "format_version=" + std::to_string(format_version) + "\n"
            "id=\"scoped.patch\"\nversion=\"1.0.0\"\n"
            "name=\"Scoped Patch\"\n"
            "[[target]]\ngame_id=\"SLUS-SCOPED\"\ndisc_sha256=\"" +
            scoped_disc_a + "\"\n"
            "[[target]]\ngame_id=\"SLUS-SCOPED\"\ndisc_sha256=\"" +
            scoped_disc_b + "\"\n"
            "[[feature]]\nid=\"controls\"\nname=\"Controls\"\n"
            "[[feature]]\nid=\"fmv\"\nname=\"FMV\"\n"
            "[[patch]]\nfeature=\"controls\"\ntarget=\"main_exe\"\n"
            "disc_sha256=\"" + scoped_disc_a + "\"\n"
            "address=0x80001000\nexpected=\"00\"\nreplace=\"01\"\n"
            "when_features=[{package=\"scoped.patch\",feature=\"fmv\",enabled=false}]\n"
            "[[patch]]\nfeature=\"controls\"\ntarget=\"main_exe\"\n"
            "disc_sha256=\"" + scoped_disc_a + "\"\n"
            "address=0x80001000\nexpected=\"04\"\nreplace=\"01\"\n"
            "when_features=[{package=\"scoped.patch\",feature=\"fmv\",enabled=true}]\n"
            "[[patch]]\nfeature=\"controls\"\ntarget=\"main_exe\"\n"
            "disc_sha256=\"" + scoped_disc_b + "\"\n"
            "address=0x80001000\nexpected=\"02\"\nreplace=\"03\"\n";
    };
    write_text(root / "scoped-v7.toml", scoped_manifest(7));
    check(!ModPackageManager::read_manifest(
              root / "scoped-v7.toml", invalid, &error),
          "disc-scoped and cross-feature patches require format 8");
    const fs::path scoped_root = root / "scoped-root";
    write_text(
        scoped_root / "packages/scoped.patch/1.0.0/manifest.toml",
        scoped_manifest(8));
    ModPackageManager scoped_manager(scoped_root);
    check(scoped_manager.scan(&error) && scoped_manager.load_state(&error),
          error.c_str());
    check(scoped_manager.set_feature_enabled(
              "scoped.patch", "controls", true, &error), error.c_str());
    const ModResolution scoped_a = scoped_manager.resolve(
        "SLUS-SCOPED", {}, scoped_disc_a);
    const ModResolution scoped_b = scoped_manager.resolve(
        "SLUS-SCOPED", {}, scoped_disc_b);
    check(scoped_a.ok && scoped_a.writes.size() == 1 &&
              scoped_a.writes[0].expected == std::vector<uint8_t>{0x00} &&
              scoped_b.ok && scoped_b.writes.size() == 1 &&
              scoped_b.writes[0].expected == std::vector<uint8_t>{0x02},
          "format-8 patches must select guards by mounted disc");
    check(scoped_manager.set_feature_enabled(
              "scoped.patch", "fmv", true, &error), error.c_str());
    const ModResolution scoped_fmv = scoped_manager.resolve(
        "SLUS-SCOPED", {}, scoped_disc_a);
    check(scoped_fmv.ok && scoped_fmv.writes.size() == 1 &&
              scoped_fmv.writes[0].expected ==
                  std::vector<uint8_t>{0x04} &&
              scoped_fmv.fingerprint != scoped_a.fingerprint,
          "format-8 patch feature predicates must select the FMV guard");

    const fs::path indexed_root = root / "indexed-root";
    const fs::path indexed_package =
        indexed_root / "packages/indexed.mod/1.0.0";
    const std::vector<uint8_t> indexed_a = {0x10, 0x20, 0x30};
    const std::vector<uint8_t> indexed_b = {0x40, 0x50};
    const std::vector<uint8_t> indexed_c = {0x60};
    const std::string indexed_disc_hash(64, 'a');
    const std::string indexed_other_disc_hash(64, 'd');
    const std::string expected_a(64, 'b');
    const std::string expected_b(64, 'c');
    write_bytes(indexed_package / "assets/a.bin", indexed_a);
    write_bytes(indexed_package / "assets/b.bin", indexed_b);
    write_bytes(indexed_package / "assets/c.bin", indexed_c);
    const auto indexed_manifest = [&](const std::string& expected) {
        return
            "format_version = 6\n"
            "id = \"indexed.mod\"\n"
            "version = \"1.0.0\"\n"
            "name = \"Indexed Files\"\n"
            "[[target]]\n"
            "game_id = \"SLUS-INDEXED\"\n"
            "disc_sha256 = \"" + indexed_disc_hash + "\"\n"
            "[[target]]\n"
            "game_id = \"SLUS-INDEXED\"\n"
            "disc_sha256 = \"" + indexed_other_disc_hash + "\"\n"
            "[[feature]]\n"
            "id = \"primary\"\n"
            "name = \"Primary\"\n"
            "[[feature]]\n"
            "id = \"duplicate\"\n"
            "name = \"Duplicate\"\n"
            "[[feature]]\n"
            "id = \"conditional\"\n"
            "name = \"Conditional\"\n"
            "[[feature]]\n"
            "id = \"collision\"\n"
            "name = \"Collision\"\n"
            "[[feature]]\n"
            "id = \"unavailable\"\n"
            "name = \"Unavailable\"\n"
            "[[option]]\n"
            "feature = \"conditional\"\n"
            "id = \"active\"\n"
            "label = \"Active\"\n"
            "type = \"boolean\"\n"
            "default = \"false\"\n"
            "[[indexed_file]]\n"
            "feature = \"primary\"\n"
            "format = \"test-index\"\n"
            "index = 7\n"
            "disc_sha256 = \"" + indexed_disc_hash + "\"\n"
            "file = \"assets/a.bin\"\n"
            "sha256 = \"" + sha256_hex(indexed_a) + "\"\n"
            "expected_sha256 = \"" + expected + "\"\n"
            "order = 0\n"
            "[[indexed_file]]\n"
            "feature = \"duplicate\"\n"
            "format = \"test-index\"\n"
            "index = 7\n"
            "disc_sha256 = \"" + indexed_disc_hash + "\"\n"
            "file = \"assets/a.bin\"\n"
            "sha256 = \"" + sha256_hex(indexed_a) + "\"\n"
            "expected_sha256 = \"" + expected + "\"\n"
            "[[indexed_file]]\n"
            "feature = \"conditional\"\n"
            "format = \"test-index\"\n"
            "index = 8\n"
            "disc_sha256 = \"" + indexed_disc_hash + "\"\n"
            "file = \"assets/c.bin\"\n"
            "sha256 = \"" + sha256_hex(indexed_c) + "\"\n"
            "expected_sha256 = \"" + expected_a + "\"\n"
            "when = { active = \"true\" }\n"
            "[[indexed_file]]\n"
            "feature = \"collision\"\n"
            "format = \"test-index\"\n"
            "index = 7\n"
            "disc_sha256 = \"" + indexed_disc_hash + "\"\n"
            "file = \"assets/b.bin\"\n"
            "sha256 = \"" + sha256_hex(indexed_b) + "\"\n"
            "expected_sha256 = \"" + expected + "\"\n"
            "[[indexed_file]]\n"
            "feature = \"unavailable\"\n"
            "format = \"missing-index\"\n"
            "index = 9\n"
            "disc_sha256 = \"" + indexed_disc_hash + "\"\n"
            "file = \"assets/c.bin\"\n"
            "sha256 = \"" + sha256_hex(indexed_c) + "\"\n"
            "expected_sha256 = \"" + expected_a + "\"\n";
    };
    write_text(indexed_package / "manifest.toml",
               indexed_manifest(expected_a));
    ModPackage parsed_indexed;
    check(ModPackageManager::read_manifest(
              indexed_package / "manifest.toml", parsed_indexed, &error) &&
              parsed_indexed.indexed_files.size() == 5 &&
              parsed_indexed.indexed_files[0].format == "test-index" &&
              parsed_indexed.indexed_files[0].index == 7 &&
              parsed_indexed.indexed_files[0].disc_sha256 == indexed_disc_hash &&
              parsed_indexed.indexed_files[0].size == indexed_a.size(),
          "format-6 indexed files must parse verified metadata");
    check(!mod_indexed_file_format_register("Bad Format"),
          "indexed file registry must reject unstable format ids");
    check(mod_indexed_file_format_register("test-index") &&
              mod_indexed_file_format_registered("test-index") &&
              !mod_indexed_file_format_register("test-index"),
          "indexed file formats must register once by stable id");

    const std::string install_manifest = indexed_manifest(expected_a);
    const fs::path indexed_archive = root / "indexed-install.psxmod";
    write_stored_package(indexed_archive, {
        {"manifest.toml", std::vector<uint8_t>(
            install_manifest.begin(), install_manifest.end())},
        {"assets/a.bin", indexed_a},
        {"assets/b.bin", indexed_b},
        {"assets/c.bin", indexed_c},
    });
    const fs::path install_root = root / "indexed-install-root";
    ModPackageManager install_manager(install_root);
    check(install_manager.install_archive(
              indexed_archive, nullptr, nullptr, &error), error.c_str());
    check(install_manager.set_feature_enabled(
              "indexed.mod", "primary", true, &error), error.c_str());
    const ModResolution installed_indexed = install_manager.resolve(
        "SLUS-INDEXED", {}, indexed_disc_hash);
    const fs::path installed_root =
        install_root / "packages/indexed.mod/1.0.0";
    const ModPackage& installed_package =
        install_manager.packages().at("indexed.mod").at("1.0.0");
    check(installed_indexed.ok &&
              installed_indexed.indexed_files.size() == 1 &&
              installed_indexed.indexed_files[0].payload == indexed_a &&
              installed_package.root == installed_root &&
              installed_package.indexed_files[0].file ==
                  installed_root / "assets/a.bin",
          "format-6 install must resolve published assets without a scan");

    ModPackageManager indexed_manager(indexed_root);
    check(indexed_manager.scan(&error), error.c_str());
    check(indexed_manager.load_state(&error), error.c_str());
    check(indexed_manager.set_feature_enabled(
              "indexed.mod", "primary", true, &error), error.c_str());
    check(indexed_manager.set_feature_enabled(
              "indexed.mod", "duplicate", true, &error), error.c_str());
    ModResolution indexed_resolved = indexed_manager.resolve(
        "SLUS-INDEXED", {}, indexed_disc_hash);
    check(indexed_resolved.ok && indexed_resolved.indexed_files.size() == 1 &&
              indexed_resolved.indexed_files[0].format == "test-index" &&
              indexed_resolved.indexed_files[0].index == 7 &&
              indexed_resolved.indexed_files[0].payload == indexed_a &&
              indexed_resolved.indexed_files[0].expected_sha256 == expected_a,
          "exact indexed-file claims must coalesce after payload reverification");
    const ModResolution indexed_other_disc = indexed_manager.resolve(
        "SLUS-INDEXED", {}, indexed_other_disc_hash);
    check(indexed_other_disc.ok && indexed_other_disc.indexed_files.empty(),
          "disc-specific indexed files must be omitted on another declared target");
    const std::string indexed_fingerprint = indexed_resolved.fingerprint;
    write_text(indexed_package / "manifest.toml",
               indexed_manifest(expected_b));
    check(indexed_manager.scan(&error), error.c_str());
    ModResolution indexed_semantic_change = indexed_manager.resolve(
        "SLUS-INDEXED", {}, indexed_disc_hash);
    check(indexed_semantic_change.ok &&
              indexed_semantic_change.fingerprint != indexed_fingerprint,
          "indexed-file expected stock hashes must affect the fingerprint");
    check(indexed_manager.set_feature_enabled(
              "indexed.mod", "conditional", true, &error), error.c_str());
    check(indexed_manager.resolve(
              "SLUS-INDEXED", {}, indexed_disc_hash).indexed_files.size() == 1,
          "false indexed-file conditions must omit their payload");
    check(indexed_manager.set_feature_option(
              "indexed.mod", "conditional", "active", "true", &error),
          error.c_str());
    ModResolution indexed_conditional = indexed_manager.resolve(
        "SLUS-INDEXED", {}, indexed_disc_hash);
    check(indexed_conditional.ok &&
              indexed_conditional.indexed_files.size() == 2 &&
              indexed_conditional.fingerprint !=
                  indexed_semantic_change.fingerprint,
          "selected indexed-file conditions and claims must affect the plan");

    write_bytes(indexed_package / "assets/a.bin", {0x99});
    ModResolution indexed_changed_payload = indexed_manager.resolve(
        "SLUS-INDEXED", {}, indexed_disc_hash);
    check(!indexed_changed_payload.ok &&
              indexed_changed_payload.indexed_files.empty(),
          "changed indexed payloads must fail closed and clear the resolved list");
    write_bytes(indexed_package / "assets/a.bin", indexed_a);
    check(indexed_manager.set_feature_enabled(
              "indexed.mod", "collision", true, &error), error.c_str());
    ModResolution indexed_collision = indexed_manager.resolve(
        "SLUS-INDEXED", {}, indexed_disc_hash);
    check(!indexed_collision.ok && indexed_collision.indexed_files.empty() &&
              indexed_collision.diagnostics.size() == 1 &&
              indexed_collision.diagnostics[0].resource ==
                  "indexed_file:test-index:7" &&
              indexed_collision.diagnostics[0].feature_id == "collision" &&
              indexed_collision.diagnostics[0].other_feature_id == "primary",
          "differing indexed-file claims must report both owners and clear output");
    check(indexed_manager.set_feature_enabled(
              "indexed.mod", "collision", false, &error), error.c_str());
    check(indexed_manager.set_feature_enabled(
              "indexed.mod", "unavailable", true, &error), error.c_str());
    ModResolution indexed_unavailable = indexed_manager.resolve(
        "SLUS-INDEXED", {}, indexed_disc_hash);
    check(!indexed_unavailable.ok && indexed_unavailable.indexed_files.empty() &&
              std::any_of(
                  indexed_unavailable.errors.begin(),
                  indexed_unavailable.errors.end(),
                  [](const std::string& item) {
                      return item.find(
                          "indexed file format is unavailable: missing-index") !=
                          std::string::npos;
                  }),
          "unregistered indexed-file formats must fail resolution closed");

    const fs::path composition_root = root / "composition-root";
    const fs::path composition_base =
        composition_root / "packages/compose.base/1.0.0";
    const fs::path composition_other =
        composition_root / "packages/compose.other/1.0.0";
    write_bytes(composition_base / "assets/a.bin", indexed_a);
    write_bytes(composition_other / "assets/b.bin", indexed_b);
    write_text(
        composition_base / "manifest.toml",
        "format_version=7\nid=\"compose.base\"\nversion=\"1.0.0\"\n"
        "name=\"Composition Base\"\n[[target]]\ngame_id=\"SLUS-COMPOSE\"\n"
        "disc_sha256=\"" + indexed_disc_hash + "\"\n"
        "[[feature]]\nid=\"main\"\nname=\"Main\"\n"
        "[[option]]\nfeature=\"main\"\nid=\"variant\"\nlabel=\"Variant\"\n"
        "type=\"choice\"\ndefault=\"a\"\n"
        "[[option.choice]]\nvalue=\"a\"\nlabel=\"A\"\n"
        "[[option.choice]]\nvalue=\"b\"\nlabel=\"B\"\n"
        "[[indexed_file]]\nfeature=\"main\"\nformat=\"test-index\"\nindex=7\n"
        "disc_sha256=\"" + indexed_disc_hash + "\"\nfile=\"assets/a.bin\"\n"
        "sha256=\"" + sha256_hex(indexed_a) + "\"\nexpected_sha256=\"" +
        expected_a + "\"\ncompose=\"three-way\"\n"
        "[[indexed_file]]\nfeature=\"main\"\nformat=\"test-index\"\nindex=8\n"
        "disc_sha256=\"" + indexed_disc_hash + "\"\nfile=\"assets/a.bin\"\n"
        "sha256=\"" + sha256_hex(indexed_a) + "\"\nexpected_sha256=\"" +
        expected_a + "\"\n");
    write_text(
        composition_other / "manifest.toml",
        "format_version=7\nid=\"compose.other\"\nversion=\"1.0.0\"\n"
        "name=\"Composition Other\"\n[[target]]\ngame_id=\"SLUS-COMPOSE\"\n"
        "disc_sha256=\"" + indexed_disc_hash + "\"\n"
        "[[feature]]\nid=\"main\"\nname=\"Main\"\n"
        "[[indexed_file]]\nfeature=\"main\"\nformat=\"test-index\"\nindex=7\n"
        "disc_sha256=\"" + indexed_disc_hash + "\"\nfile=\"assets/b.bin\"\n"
        "sha256=\"" + sha256_hex(indexed_b) + "\"\nexpected_sha256=\"" +
        expected_a + "\"\ncompose=\"three-way\"\n"
        "when_features=[{package=\"compose.base\",feature=\"main\",enabled=true,"
        "option=\"variant\",value=\"a\"}]\n"
        "[[indexed_file]]\nfeature=\"main\"\nformat=\"test-index\"\nindex=8\n"
        "disc_sha256=\"" + indexed_disc_hash + "\"\nfile=\"assets/b.bin\"\n"
        "sha256=\"" + sha256_hex(indexed_b) + "\"\nexpected_sha256=\"" +
        expected_a + "\"\nsupersedes=[\"compose.base\"]\n");
    ModPackageManager composition_manager(composition_root);
    check(composition_manager.scan(&error) &&
              composition_manager.load_state(&error), error.c_str());
    check(composition_manager.set_feature_enabled(
              "compose.other", "main", true, &error), error.c_str());
    check(composition_manager.resolve(
              "SLUS-COMPOSE", {}, indexed_disc_hash).indexed_files.size() == 1,
          "external feature conditions must omit claims while their provider is disabled");
    check(composition_manager.set_feature_enabled(
              "compose.base", "main", true, &error), error.c_str());
    const ModResolution composed_resolution = composition_manager.resolve(
        "SLUS-COMPOSE", {}, indexed_disc_hash);
    check(composed_resolution.ok && composed_resolution.indexed_files.size() == 3 &&
              std::count_if(
                  composed_resolution.indexed_files.begin(),
                  composed_resolution.indexed_files.end(),
                  [](const ModResolution::IndexedFile& item) {
                      return item.index == 7 && item.compose == "three-way";
                  }) == 2,
          "matching format-7 three-way claims must reach the indexed handler together");
    const auto superseded = std::find_if(
        composed_resolution.indexed_files.begin(),
        composed_resolution.indexed_files.end(),
        [](const ModResolution::IndexedFile& item) { return item.index == 8; });
    check(superseded != composed_resolution.indexed_files.end() &&
              superseded->package_id == "compose.other" &&
              superseded->payload == indexed_b,
          "an explicit format-7 superseder must replace only its named owner");

    const fs::path supersession_root = root / "supersession-root";
    const auto write_claim_package = [&](const std::string& id,
                                         uint32_t index,
                                         const std::vector<uint8_t>& payload,
                                         const std::string& metadata) {
        const fs::path package =
            supersession_root / "packages" / id / "1.0.0";
        write_bytes(package / "assets/payload.bin", payload);
        write_text(
            package / "manifest.toml",
            "format_version=7\nid=\"" + id + "\"\nversion=\"1.0.0\"\n"
            "name=\"Supersession\"\n[[target]]\ngame_id=\"SLUS-COMPOSE\"\n"
            "disc_sha256=\"" + indexed_disc_hash + "\"\n"
            "[[feature]]\nid=\"main\"\nname=\"Main\"\n"
            "[[indexed_file]]\nfeature=\"main\"\nformat=\"test-index\"\n"
            "index=" + std::to_string(index) + "\ndisc_sha256=\"" +
            indexed_disc_hash + "\"\nfile=\"assets/payload.bin\"\nsha256=\"" +
            sha256_hex(payload) + "\"\nexpected_sha256=\"" + expected_a +
            "\"\n" + metadata);
    };
    write_claim_package("late.a", 9, indexed_a, "");
    write_claim_package("late.b", 9, indexed_b, "");
    write_claim_package(
        "late.c", 9, {0xC3},
        "supersedes=[\"late.a\",\"late.b\"]\n");
    const fs::path self_package =
        supersession_root / "packages/self.hybrid/1.0.0";
    write_bytes(self_package / "assets/a.bin", indexed_a);
    write_text(
        self_package / "manifest.toml",
        "format_version=7\nid=\"self.hybrid\"\nversion=\"1.0.0\"\n"
        "name=\"Self Hybrid\"\n[[target]]\ngame_id=\"SLUS-COMPOSE\"\n"
        "disc_sha256=\"" + indexed_disc_hash + "\"\n"
        "[[feature]]\nid=\"main\"\nname=\"Main\"\n"
        "[[indexed_file]]\nfeature=\"main\"\nformat=\"test-index\"\nindex=10\n"
        "disc_sha256=\"" + indexed_disc_hash + "\"\nfile=\"assets/a.bin\"\n"
        "sha256=\"" + sha256_hex(indexed_a) + "\"\nexpected_sha256=\"" +
        expected_a + "\"\ncompose=\"three-way\"\n"
        "[[indexed_file]]\nfeature=\"main\"\nformat=\"test-index\"\nindex=10\n"
        "disc_sha256=\"" + indexed_disc_hash + "\"\nfile=\"assets/a.bin\"\n"
        "sha256=\"" + sha256_hex(indexed_a) + "\"\nexpected_sha256=\"" +
        expected_a + "\"\ncompose=\"three-way\"\n"
        "supersedes=[\"self.hybrid\",\"self.other\"]\n");
    write_claim_package("self.other", 10, indexed_b, "compose=\"three-way\"\n");
    write_claim_package(
        "mutual.a", 11, indexed_a,
        "compose=\"three-way\"\nsupersedes=[\"mutual.b\"]\n");
    write_claim_package(
        "mutual.b", 11, indexed_b,
        "compose=\"three-way\"\nsupersedes=[\"mutual.a\"]\n");
    write_claim_package(
        "cycle.a", 12, indexed_a,
        "compose=\"three-way\"\nsupersedes=[\"cycle.b\"]\n");
    write_claim_package(
        "cycle.b", 12, indexed_b,
        "compose=\"three-way\"\nsupersedes=[\"cycle.c\"]\n");
    write_claim_package(
        "cycle.c", 12, {0xC3},
        "compose=\"three-way\"\nsupersedes=[\"cycle.a\"]\n");
    write_claim_package("cycle.keep", 12, {0xD4}, "compose=\"three-way\"\n");
    ModPackageManager supersession_manager(supersession_root);
    check(supersession_manager.scan(&error) &&
              supersession_manager.load_state(&error), error.c_str());
    for (const char* id : {"late.a", "late.b", "late.c",
                           "self.hybrid", "self.other"})
        check(supersession_manager.set_feature_enabled(
                  id, "main", true, &error), error.c_str());
    const ModResolution late_resolution = supersession_manager.resolve(
        "SLUS-COMPOSE", {}, indexed_disc_hash);
    check(late_resolution.ok && late_resolution.indexed_files.size() == 2 &&
              std::any_of(
                  late_resolution.indexed_files.begin(),
                  late_resolution.indexed_files.end(),
                  [](const ModResolution::IndexedFile& item) {
                      return item.index == 9 && item.package_id == "late.c";
                  }) &&
              std::any_of(
                  late_resolution.indexed_files.begin(),
                  late_resolution.indexed_files.end(),
                  [](const ModResolution::IndexedFile& item) {
                      return item.index == 10 &&
                          item.package_id == "self.hybrid";
                  }),
          "late and identical self-superseders must remove every named base claim");
    const std::string stable_supersession_fingerprint = late_resolution.fingerprint;
    write_claim_package(
        "late.c", 9, {0xC3},
        "supersedes=[\"late.b\",\"late.a\"]\n");
    check(supersession_manager.scan(&error), error.c_str());
    check(supersession_manager.resolve(
              "SLUS-COMPOSE", {}, indexed_disc_hash).fingerprint ==
              stable_supersession_fingerprint,
          "supersedes declaration order must not affect the plan fingerprint");
    check(supersession_manager.set_feature_enabled(
              "mutual.a", "main", true, &error) &&
              supersession_manager.set_feature_enabled(
                  "mutual.b", "main", true, &error), error.c_str());
    const ModResolution mutual_resolution = supersession_manager.resolve(
        "SLUS-COMPOSE", {}, indexed_disc_hash);
    check(!mutual_resolution.ok && std::any_of(
              mutual_resolution.errors.begin(), mutual_resolution.errors.end(),
              [](const std::string& item) {
                  return item.find("mutually supersede") != std::string::npos;
              }),
          "mutual supersession must fail even when compositor IDs agree");
    check(supersession_manager.set_feature_enabled(
              "mutual.a", "main", false, &error) &&
              supersession_manager.set_feature_enabled(
                  "mutual.b", "main", false, &error), error.c_str());
    for (const char* id : {"cycle.a", "cycle.b", "cycle.c", "cycle.keep"})
        check(supersession_manager.set_feature_enabled(
                  id, "main", true, &error), error.c_str());
    const ModResolution cyclic_resolution = supersession_manager.resolve(
        "SLUS-COMPOSE", {}, indexed_disc_hash);
    check(!cyclic_resolution.ok && std::any_of(
              cyclic_resolution.errors.begin(), cyclic_resolution.errors.end(),
              [](const std::string& item) {
                  return item.find("supersession cycle") != std::string::npos;
              }),
          "a supersession cycle must fail even when another claim survives");

    const auto reject_indexed_manifest =
        [&](const std::string& name, const std::string& body) {
            const fs::path path = indexed_package / (name + ".toml");
            write_text(path, body);
            ModPackage rejected;
            return !ModPackageManager::read_manifest(path, rejected, &error);
        };
    const std::string indexed_prelude =
        "id=\"bad.indexed\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
        "[[target]]\ngame_id=\"SLUS-INDEXED\"\ndisc_sha256=\"" +
        indexed_disc_hash + "\"\n"
        "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n";
    const std::string indexed_entry =
        "[[indexed_file]]\nfeature=\"bad\"\nformat=\"test-index\"\n"
        "index=0\nfile=\"assets/a.bin\"\nsha256=\"" +
        sha256_hex(indexed_a) + "\"\nexpected_sha256=\"" + expected_a +
        "\"\n";
    check(reject_indexed_manifest(
              "indexed-v5", "format_version=5\n" + indexed_prelude +
                  indexed_entry),
          "indexed files must require package format 6");
    check(reject_indexed_manifest(
              "indexed-negative", "format_version=6\n" + indexed_prelude +
                  "[[indexed_file]]\nfeature=\"bad\"\n"
                  "format=\"test-index\"\nindex=-1\n"
                  "file=\"assets/a.bin\"\nsha256=\"" +
                  sha256_hex(indexed_a) + "\"\nexpected_sha256=\"" +
                  expected_a + "\"\n"),
          "indexed file indexes must reject negative values");
    check(reject_indexed_manifest(
              "indexed-unknown-field", "format_version=6\n" +
                  indexed_prelude + indexed_entry + "whenn={}\n"),
          "indexed files must reject unknown fields");
    check(reject_indexed_manifest(
              "indexed-unknown-disc", "format_version=6\n" +
                  indexed_prelude +
                  "[[indexed_file]]\nfeature=\"bad\"\nformat=\"test-index\"\n"
                  "index=0\ndisc_sha256=\"" + indexed_other_disc_hash +
                  "\"\nfile=\"assets/a.bin\"\nsha256=\"" +
                  sha256_hex(indexed_a) + "\"\nexpected_sha256=\"" +
                  expected_a + "\"\n"),
          "indexed-file disc guards must name a declared package target");
    check(reject_indexed_manifest(
              "indexed-unguarded",
              "format_version=6\nid=\"bad.indexed\"\n"
              "version=\"1.0.0\"\nname=\"Bad\"\n"
              "[[target]]\ngame_id=\"SLUS-INDEXED\"\n"
              "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n" +
                  indexed_entry),
          "indexed files must require exact disc hashes on every target");
    check(reject_indexed_manifest(
              "indexed-legacy",
              "format_version=6\nid=\"bad.indexed\"\n"
              "version=\"1.0.0\"\nname=\"Bad\"\n"
              "[[target]]\ngame_id=\"SLUS-INDEXED\"\n"
              "disc_sha256=\"" + indexed_disc_hash + "\"\n" +
                  indexed_entry),
          "indexed files must require explicit feature ownership");

    fs::remove_all(root, ec);
    if (failures) {
        std::cerr << failures << " mod package test(s) failed\n";
        return 1;
    }
    std::cout << "mod package tests passed\n";
    return 0;
}
