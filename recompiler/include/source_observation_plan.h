#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "code_generator.h"

namespace PSXRecomp {

bool load_source_observation_plan(
    const std::filesystem::path& path,
    std::vector<CodeGenConfig::SourceObservationSite>& sites,
    std::vector<CodeGenConfig::NativeCutoverSite>& cutovers,
    std::vector<CodeGenConfig::RenderLifecycleSite>& lifecycle_sites,
    std::string& error);

}
