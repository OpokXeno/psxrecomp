#include "source_observation_plan.h"

#include <charconv>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace PSXRecomp {
namespace {

constexpr std::string_view kSchema = "psxrecomp-source-observation-plan-v5";

bool reject(std::string& error, size_t line, std::string_view message) {
    error = "source observation plan line " + std::to_string(line) + ": " +
            std::string(message);
    return false;
}

bool parse_hex32(std::string_view token, uint32_t& value) {
    if (token.size() != 8u) return false;
    const char* const begin = token.data();
    const char* const end = begin + token.size();
    const auto result = std::from_chars(begin, end, value, 16);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_width(std::string_view token, uint8_t& width) {
    unsigned parsed = 0u;
    const char* const begin = token.data();
    const char* const end = begin + token.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end || parsed > 255u)
        return false;
    width = static_cast<uint8_t>(parsed);
    return true;
}

bool is_control_transfer(uint32_t instruction) {
    const uint32_t opcode = instruction >> 26u;
    const uint32_t funct = instruction & 0x3fu;
    return opcode == 0x01u || opcode == 0x02u || opcode == 0x03u ||
           (opcode >= 0x04u && opcode <= 0x07u) ||
           (opcode >= 0x14u && opcode <= 0x17u) ||
           (opcode == 0u && (funct == 0x08u || funct == 0x09u));
}

std::optional<CodeGenConfig::SourceObservationOperation> parse_operation(
    std::string_view token) {
    if (token == "read")
        return CodeGenConfig::SourceObservationOperation::Read;
    if (token == "write")
        return CodeGenConfig::SourceObservationOperation::Write;
    if (token == "swc2")
        return CodeGenConfig::SourceObservationOperation::Swc2;
    if (token == "call")
        return CodeGenConfig::SourceObservationOperation::Call;
    if (token == "bucket")
        return CodeGenConfig::SourceObservationOperation::Bucket;
    return std::nullopt;
}

std::optional<CodeGenConfig::SourceObservationAuxiliary> parse_auxiliary(
    std::string_view token) {
    if (token == "effective-address")
        return CodeGenConfig::SourceObservationAuxiliary::EffectiveAddress;
    if (token == "none")
        return CodeGenConfig::SourceObservationAuxiliary::None;
    if (token == "result-register")
        return CodeGenConfig::SourceObservationAuxiliary::ResultRegister;
    return std::nullopt;
}

bool operation_contract_is_valid(
    CodeGenConfig::SourceObservationOperation operation,
    uint8_t width,
    CodeGenConfig::SourceObservationAuxiliary auxiliary) {
    switch (operation) {
    case CodeGenConfig::SourceObservationOperation::Read:
    case CodeGenConfig::SourceObservationOperation::Write:
    case CodeGenConfig::SourceObservationOperation::Swc2:
        return auxiliary ==
                   CodeGenConfig::SourceObservationAuxiliary::EffectiveAddress &&
               (width == 1u || width == 2u || width == 4u);
    case CodeGenConfig::SourceObservationOperation::Call:
        return auxiliary == CodeGenConfig::SourceObservationAuxiliary::None &&
               width == 0u;
    case CodeGenConfig::SourceObservationOperation::Bucket:
        return auxiliary ==
                   CodeGenConfig::SourceObservationAuxiliary::ResultRegister &&
               width == 0u;
    }
    return false;
}

std::optional<CodeGenConfig::RenderLifecycleRole> parse_lifecycle_role(
    std::string_view token) {
    if (token == "entry") return CodeGenConfig::RenderLifecycleRole::Entry;
    if (token == "capture") return CodeGenConfig::RenderLifecycleRole::Capture;
    if (token == "return") return CodeGenConfig::RenderLifecycleRole::Return;
    return std::nullopt;
}

}

bool load_source_observation_plan(
    const std::filesystem::path& path,
    std::vector<CodeGenConfig::SourceObservationSite>& sites,
    std::vector<CodeGenConfig::NativeCutoverSite>& cutovers,
    std::vector<CodeGenConfig::RenderLifecycleSite>& lifecycle_sites,
    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "source observation plan is unreadable: " + path.string();
        return false;
    }

    std::string line;
    if (!std::getline(input, line) || line != kSchema)
        return reject(error, 1u, "unsupported or malformed schema");

    std::vector<CodeGenConfig::SourceObservationSite> parsed_sites;
    std::vector<CodeGenConfig::NativeCutoverSite> parsed_cutovers;
    std::vector<CodeGenConfig::RenderLifecycleSite> parsed_lifecycle_sites;
    std::set<std::pair<uint32_t, uint32_t>> identities;
    size_t line_number = 1u;
    while (std::getline(input, line)) {
        ++line_number;
        std::istringstream fields(line);
        std::string directive;
        std::string pc_token;
        std::string instruction_token;
        std::string operation_token;
        std::string width_token;
        std::string auxiliary_token;
        std::string trailing;
        if (!(fields >> directive >> pc_token >> instruction_token))
            return reject(error, line_number, "fields are not closed");
        if (directive == "cutover") {
            std::string transfer_token;
            std::string continuation_token;
            uint32_t pc = 0u;
            uint32_t instruction = 0u;
            uint32_t continuation = 0u;
            if (!(fields >> transfer_token >> continuation_token) ||
                fields >> trailing ||
                !parse_hex32(pc_token, pc) ||
                !parse_hex32(instruction_token, instruction) ||
                !parse_hex32(continuation_token, continuation))
                return reject(error, line_number,
                              "cutover fields are malformed");
            if ((pc & 0xE0000003u) != 0x80000000u ||
                (transfer_token == "local" &&
                 (continuation & 0xE0000003u) != 0x80000000u) ||
                (transfer_token != "local" && continuation != 0u) ||
                 (transfer_token != "local" && transfer_token != "observe" &&
                  transfer_token != "observe-after" &&
                  transfer_token != "return"))
                return reject(error, line_number,
                              "cutover transfer or continuation is invalid");
            if (!identities.emplace(pc, instruction).second)
                return reject(error, line_number,
                              "duplicate exact site identity");
            const auto transfer = transfer_token == "local"
                ? CodeGenConfig::NativeCutoverTransfer::Local
                : transfer_token == "observe"
                    ? CodeGenConfig::NativeCutoverTransfer::Observe
                    : transfer_token == "observe-after"
                        ? CodeGenConfig::NativeCutoverTransfer::ObserveAfter
                        : CodeGenConfig::NativeCutoverTransfer::Return;
            parsed_cutovers.push_back({pc, instruction, transfer, continuation});
            continue;
        }
        if (directive == "lifecycle") {
            std::string role_token;
            std::string delay_token;
            uint32_t pc = 0u;
            uint32_t instruction = 0u;
            uint32_t delay = 0u;
            if (!(fields >> role_token >> delay_token) || fields >> trailing ||
                !parse_hex32(pc_token, pc) ||
                !parse_hex32(instruction_token, instruction) ||
                !parse_hex32(delay_token, delay))
                return reject(error, line_number,
                              "lifecycle fields are malformed");
            const auto role = parse_lifecycle_role(role_token);
            const bool capture = role == CodeGenConfig::RenderLifecycleRole::Capture;
            if ((pc & 0xE0000003u) != 0x80000000u || !role.has_value() ||
                (capture && instruction >> 26u != 0x03u) ||
                (!capture && (delay != 0u ||
                              is_control_transfer(instruction))))
                return reject(error, line_number,
                              "lifecycle role or instruction contract is invalid");
            if (!identities.emplace(pc, instruction).second)
                return reject(error, line_number,
                              "duplicate exact site identity");
            parsed_lifecycle_sites.push_back({pc, instruction, *role, delay});
            continue;
        }
        if (directive != "site" ||
            !(fields >> operation_token >> width_token >> auxiliary_token) ||
            fields >> trailing)
            return reject(error, line_number, "fields are not closed");

        uint32_t pc = 0u;
        uint32_t instruction = 0u;
        uint8_t width = 0u;
        if (!parse_hex32(pc_token, pc) ||
            !parse_hex32(instruction_token, instruction) ||
            !parse_width(width_token, width)) {
            return reject(error, line_number, "numeric field is malformed");
        }
        if ((pc & 0xE0000003u) != 0x80000000u)
            return reject(error, line_number, "PC must be an aligned KSEG0 address");

        const auto operation = parse_operation(operation_token);
        const auto auxiliary = parse_auxiliary(auxiliary_token);
        if (!operation.has_value() || !auxiliary.has_value())
            return reject(error, line_number,
                          "operation or auxiliary token is unsupported");
        if (!operation_contract_is_valid(*operation, width, *auxiliary))
            return reject(error, line_number,
                          "operation, width, and auxiliary disagree");
        if (!identities.emplace(pc, instruction).second)
            return reject(error, line_number, "duplicate exact site identity");

        parsed_sites.push_back({pc, instruction, *operation, width, *auxiliary});
    }
    if (!input.eof()) {
        error = "source observation plan could not be read completely";
        return false;
    }
    if (parsed_sites.empty() && parsed_cutovers.empty() &&
        parsed_lifecycle_sites.empty())
        return reject(error, line_number + 1u,
                      "plan must contain at least one site or cutover");

    sites = std::move(parsed_sites);
    cutovers = std::move(parsed_cutovers);
    lifecycle_sites = std::move(parsed_lifecycle_sites);
    return true;
}

}
