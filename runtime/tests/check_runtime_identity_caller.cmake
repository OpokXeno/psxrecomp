if(NOT DEFINED PSXRECOMP_ROOT)
    message(FATAL_ERROR "PSXRECOMP_ROOT is required")
endif()

set(_game_domain "psxrecomp-runtime:bios-only:game-identity:v1")
set(_manifest_domain "psxrecomp-runtime:bios-only:manifest-identity:v1")
string(SHA256 _game_sha256 "${_game_domain}")
string(SHA256 _manifest_sha256 "${_manifest_domain}")
string(LENGTH "${_game_sha256}" _game_length)
string(LENGTH "${_manifest_sha256}" _manifest_length)

if(NOT _game_length EQUAL 64 OR NOT _game_sha256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "BIOS-only game identity must be a lowercase 32-byte SHA-256 hex value")
endif()
if(NOT _manifest_length EQUAL 64 OR NOT _manifest_sha256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "BIOS-only manifest identity must be a lowercase 32-byte SHA-256 hex value")
endif()
if(_game_sha256 STREQUAL _manifest_sha256)
    message(FATAL_ERROR "BIOS-only game and manifest identities must stay distinct")
endif()

file(READ "${PSXRECOMP_ROOT}/runtime/runtime_bios_only_targets.cmake" _caller_contents)
foreach(_caller_name IN ITEMS psx-runtime psx-oracle)
    string(FIND "${_caller_contents}" "psxrecomp_add_runtime_target(${_caller_name}" _caller_start)
    if(_caller_name STREQUAL "psx-runtime")
        string(FIND "${_caller_contents}" "if(BUILD_TESTING)" _caller_end)
    else()
        string(LENGTH "${_caller_contents}" _caller_end)
    endif()
    if(_caller_start EQUAL -1 OR _caller_end EQUAL -1 OR _caller_end LESS _caller_start)
        message(FATAL_ERROR "standalone runtime caller block is missing")
    endif()
    math(EXPR _caller_length "${_caller_end} - ${_caller_start}")
    string(SUBSTRING "${_caller_contents}" ${_caller_start} ${_caller_length} _caller_block)
    string(FIND "${_caller_block}" "GAME_EXTRA_IDENTITY_SHA256" _caller_extra)
    string(FIND "${_caller_block}" "GAME_MANIFEST_DIGEST_SHA256" _caller_manifest)
    if(_caller_extra EQUAL -1 OR _caller_manifest EQUAL -1)
        message(FATAL_ERROR "${_caller_name} does not pass both identities")
    endif()
endforeach()

set(_smoke_root "/tmp/psxrecomp-runtime-identity-smoke")
file(REMOVE_RECURSE "${_smoke_root}")
file(MAKE_DIRECTORY "${_smoke_root}/good" "${_smoke_root}/bad_extra" "${_smoke_root}/bad_manifest")

set(_good_project "${_smoke_root}/good/CMakeLists.txt")
file(WRITE "${_good_project}" [=[
cmake_minimum_required(VERSION 3.20)
project(psx_runtime_identity_smoke C CXX)
set(PSXRECOMP_ROOT "@PSXRECOMP_ROOT@")
set(PSX_RECOMP_UI OFF)
include("@PSXRECOMP_ROOT@/runtime/runtime.cmake")
psxrecomp_add_runtime_target(identity_smoke
    DEBUG_PORT 4370
    WINDOW_TITLE "identity-smoke"
    GAME_EXTRA_IDENTITY_SHA256 "@GAME_SHA256@"
    GAME_MANIFEST_DIGEST_SHA256 "@MANIFEST_SHA256@"
)
]=])

set(_bad_extra_project "${_smoke_root}/bad_extra/CMakeLists.txt")
file(WRITE "${_bad_extra_project}" [=[
cmake_minimum_required(VERSION 3.20)
project(psx_runtime_identity_smoke_bad_extra C CXX)
set(PSXRECOMP_ROOT "@PSXRECOMP_ROOT@")
set(PSX_RECOMP_UI OFF)
include("@PSXRECOMP_ROOT@/runtime/runtime.cmake")
psxrecomp_add_runtime_target(identity_smoke_bad_extra
    DEBUG_PORT 4370
    WINDOW_TITLE "identity-smoke"
    GAME_MANIFEST_DIGEST_SHA256 "@MANIFEST_SHA256@"
)
]=])

set(_bad_manifest_project "${_smoke_root}/bad_manifest/CMakeLists.txt")
file(WRITE "${_bad_manifest_project}" [=[
cmake_minimum_required(VERSION 3.20)
project(psx_runtime_identity_smoke_bad_manifest C CXX)
set(PSXRECOMP_ROOT "@PSXRECOMP_ROOT@")
set(PSX_RECOMP_UI OFF)
include("@PSXRECOMP_ROOT@/runtime/runtime.cmake")
psxrecomp_add_runtime_target(identity_smoke_bad_manifest
    DEBUG_PORT 4370
    WINDOW_TITLE "identity-smoke"
    GAME_EXTRA_IDENTITY_SHA256 "@GAME_SHA256@"
)
]=])

foreach(_path IN ITEMS _good_project _bad_extra_project _bad_manifest_project)
    file(READ "${${_path}}" _content)
    string(REPLACE "@PSXRECOMP_ROOT@" "${PSXRECOMP_ROOT}" _content "${_content}")
    string(REPLACE "@GAME_SHA256@" "${_game_sha256}" _content "${_content}")
    string(REPLACE "@MANIFEST_SHA256@" "${_manifest_sha256}" _content "${_content}")
    file(WRITE "${${_path}}" "${_content}")
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_smoke_root}/good" -B "${_smoke_root}/good/build" -G Ninja
    RESULT_VARIABLE _good_rc
    OUTPUT_VARIABLE _good_out
    ERROR_VARIABLE _good_err)
if(NOT _good_rc EQUAL 0)
    message(FATAL_ERROR "good identity configure failed:\n${_good_out}\n${_good_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_smoke_root}/bad_extra" -B "${_smoke_root}/bad_extra/build" -G Ninja
    RESULT_VARIABLE _bad_extra_rc
    OUTPUT_VARIABLE _bad_extra_out
    ERROR_VARIABLE _bad_extra_err)
if(_bad_extra_rc EQUAL 0 OR NOT _bad_extra_err MATCHES "GAME_EXTRA_IDENTITY_SHA256")
    message(FATAL_ERROR "missing game identity did not fail as expected:\n${_bad_extra_out}\n${_bad_extra_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_smoke_root}/bad_manifest" -B "${_smoke_root}/bad_manifest/build" -G Ninja
    RESULT_VARIABLE _bad_manifest_rc
    OUTPUT_VARIABLE _bad_manifest_out
    ERROR_VARIABLE _bad_manifest_err)
if(_bad_manifest_rc EQUAL 0 OR NOT _bad_manifest_err MATCHES "GAME_MANIFEST_DIGEST_SHA256")
    message(FATAL_ERROR "missing manifest identity did not fail as expected:\n${_bad_manifest_out}\n${_bad_manifest_err}")
endif()

file(REMOVE_RECURSE "${_smoke_root}")
