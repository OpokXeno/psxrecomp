cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)
include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# ---- SDL2 detection ----

if(MSVC)
    file(GLOB SDL2_MSVC_DIR "${CMAKE_SOURCE_DIR}/../sdl2-msvc/SDL2-*")
    if(SDL2_MSVC_DIR)
        set(SDL2_INCLUDE_DIRS "${SDL2_MSVC_DIR}/include")
        set(SDL2_LIBRARIES "${SDL2_MSVC_DIR}/lib/x64/SDL2.lib")
        message(STATUS "SDL2 MSVC: ${SDL2_MSVC_DIR}")
    else()
        message(FATAL_ERROR "SDL2 MSVC dev package not found. Run: bash tools/build_oracle.sh")
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

# ---- Native/oracle runtime targets ----

set(PSXRECOMP_ROOT "${CMAKE_SOURCE_DIR}/..")
include("${CMAKE_SOURCE_DIR}/runtime.cmake")

get_filename_component(XG_GAME_ROOT "${PSXRECOMP_ROOT}/.." ABSOLUTE)
if(NOT EXISTS "${XG_GAME_ROOT}/native_renderer/CMakeLists.txt")
    message(FATAL_ERROR
        "The runtime's Xenogears renderer provider is missing: "
        "${XG_GAME_ROOT}/native_renderer")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(psxrecomp_add_xg_native_renderer)
    # The renderer remains game-owned and its tests belong to the integrated
    # game build rather than this standalone framework build.
    set(BUILD_TESTING OFF)
    add_subdirectory("${XG_GAME_ROOT}/native_renderer"
                     "${CMAKE_BINARY_DIR}/native_renderer")
endfunction()
psxrecomp_add_xg_native_renderer()

include("${CMAKE_CURRENT_LIST_DIR}/runtime_bios_only_targets.cmake")

target_link_libraries(psx-runtime PRIVATE
    xg_visual_state
    xg_render_ir
    xg_render_static_auth
    xg_field_character_runtime)
target_include_directories(psx-runtime PRIVATE
    "${XG_GAME_ROOT}/native_renderer/include")
target_sources(psx-runtime PRIVATE
    "${XG_GAME_ROOT}/native_renderer/src/xg_render_unavailable_metadata.c")
if(TARGET psx-oracle)
    target_link_libraries(psx-oracle PRIVATE
        xg_visual_state
        xg_render_ir
        xg_render_static_auth
        xg_field_character_runtime)
    target_include_directories(psx-oracle PRIVATE
        "${XG_GAME_ROOT}/native_renderer/include")
    target_sources(psx-oracle PRIVATE
        "${XG_GAME_ROOT}/native_renderer/src/xg_render_unavailable_metadata.c")
endif()

if(BUILD_TESTING)
    add_executable(launcher_device_roundtrip_test
        tests/test_launcher_device_roundtrip.cpp)
    target_include_directories(launcher_device_roundtrip_test PRIVATE include)
    add_test(NAME launcher_device_roundtrip_test
             COMMAND launcher_device_roundtrip_test)

    add_executable(psx_cyc_batch_test tests/test_psx_cyc_batch.c)
    target_include_directories(psx_cyc_batch_test PRIVATE include)
    target_compile_definitions(psx_cyc_batch_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME psx_cyc_batch_test COMMAND psx_cyc_batch_test)

    add_executable(psx_icache_fastpath_test
        tests/test_psx_icache_fastpath.c
        src/psx_icache.c
    )
    target_include_directories(psx_icache_fastpath_test PRIVATE include)
    target_compile_definitions(psx_icache_fastpath_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1
        PSX_OVERLAY_DLL_BUILD=1)
    add_test(NAME psx_icache_fastpath_test COMMAND psx_icache_fastpath_test)

    add_executable(gte_register_access_test
        tests/test_gte_register_access.cpp
        src/gte.cpp
        src/gte_attribution.cpp
    )
    target_include_directories(gte_register_access_test PRIVATE include)
    target_compile_definitions(gte_register_access_test PRIVATE
        PSX_NO_DEBUG_TOOLS=1
        PSX_GTE_REGISTER_TEST=1
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME gte_register_access_test COMMAND gte_register_access_test)

    add_executable(gte_attribution_test
        tests/test_gte_attribution.cpp
        src/gte.cpp
        src/gte_attribution.cpp)
    target_include_directories(gte_attribution_test PRIVATE include)
    target_compile_definitions(gte_attribution_test PRIVATE
        PSX_NO_DEBUG_TOOLS=1
        GTE_ATTRIBUTION_CONTEXT_CAPACITY=3
        GTE_ATTRIBUTION_SITE_CAPACITY=4
        GTE_ATTRIBUTION_COUNTER_MAX=4)
    add_test(NAME gte_attribution_test COMMAND gte_attribution_test)

    add_executable(irq_cop2_boundary_test
        tests/test_irq_cop2_boundary.c
        tests/test_irq_cop2_boundary_stubs.c
        src/interrupts.c
        src/gte.cpp
        src/gte_attribution.cpp)
    target_include_directories(irq_cop2_boundary_test PRIVATE include)
    target_compile_definitions(irq_cop2_boundary_test PRIVATE
        PSX_NO_DEBUG_TOOLS=1
        PSX_ENABLE_BLOCK_CYCLES=1)
    if(MSVC)
        target_compile_options(irq_cop2_boundary_test PRIVATE /Gy)
        target_link_options(irq_cop2_boundary_test PRIVATE /OPT:REF)
    elseif(APPLE)
        target_compile_options(irq_cop2_boundary_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(irq_cop2_boundary_test PRIVATE -Wl,-dead_strip)
    else()
        target_compile_options(irq_cop2_boundary_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(irq_cop2_boundary_test PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME irq_cop2_boundary_test COMMAND irq_cop2_boundary_test)

    add_executable(psx_instr_cost_test tests/test_psx_instr_cost.cpp)
    target_include_directories(psx_instr_cost_test PRIVATE include)
    add_test(NAME psx_instr_cost_test COMMAND psx_instr_cost_test)

    add_executable(game_identity_test
        tests/test_game_identity.c
        src/game_identity.c
        src/boot_state.c)
    target_include_directories(game_identity_test PRIVATE include)
    target_compile_definitions(game_identity_test PRIVATE
        PSX_GAME_EXTRA_IDENTITY_SHA256="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        PSX_GAME_MANIFEST_DIGEST_SHA256="202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f")
    add_test(NAME game_identity_test COMMAND game_identity_test)

    add_executable(game_identity_unconfigured_test
        tests/test_game_identity_unconfigured.c
        src/game_identity.c)
    target_include_directories(game_identity_unconfigured_test PRIVATE include)
    add_test(NAME game_identity_unconfigured_test
             COMMAND game_identity_unconfigured_test)

    add_executable(native_render_baseline_test
        tests/test_native_render_baseline.c
        src/native_render_baseline.c
        src/native_render_baseline_runtime.c
        src/gte_attribution.cpp)
    target_include_directories(native_render_baseline_test PRIVATE include)
    target_compile_definitions(native_render_baseline_test PRIVATE
        PSX_NATIVE_RENDER_BASELINE_TEST=1
        GTE_ATTRIBUTION_COUNTER_MAX=4)
    add_test(NAME native_render_baseline_test COMMAND native_render_baseline_test)

    add_executable(overlay_capture_retry_test
        tests/test_overlay_capture_retry.cpp
        src/overlay_capture.c)
    target_include_directories(overlay_capture_retry_test PRIVATE
        include ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(overlay_capture_retry_test PRIVATE
        SDL_MAIN_HANDLED PSX_OVERLAY_CAPTURE_TEST=1)
    target_link_libraries(overlay_capture_retry_test PRIVATE ${SDL2_LIBRARIES})
    add_test(NAME overlay_capture_retry_test COMMAND overlay_capture_retry_test)

    add_executable(overlay_path_canon_test
        tests/test_overlay_path_canon.c
        src/overlay_path_canon.c)
    target_include_directories(overlay_path_canon_test PRIVATE include)
    add_test(NAME overlay_path_canon_test COMMAND overlay_path_canon_test)

    # Either member of a disc dump (.cue or .bin) is a valid pick: both mount
    # the same disc and identify off the same data track.
    add_executable(disc_path_resolve_test
        tests/test_disc_path_resolve.cpp
        src/disc_path.cpp
        src/cue_sheet.cpp
        src/iso_reader.cpp)
    target_include_directories(disc_path_resolve_test PRIVATE include)
    add_test(NAME disc_path_resolve_test COMMAND disc_path_resolve_test)

    # Both BIOS filename conventions (bare vs region-qualified) resolve, in
    # both directions, so a config and a dump folder may disagree.
    add_executable(bios_rom_alias_test tests/test_bios_rom_alias.cpp)
    target_include_directories(bios_rom_alias_test PRIVATE
        ${PSXRECOMP_ROOT}/recompiler/include)
    add_test(NAME bios_rom_alias_test COMMAND bios_rom_alias_test)

    # Multi-file cue TOC mapping (data track + CD-DA in separate bins).
    add_executable(iso_reader_cdda_test
        tests/test_iso_reader_cdda.cpp
        src/iso_reader.cpp
        src/cue_sheet.cpp)
    target_include_directories(iso_reader_cdda_test PRIVATE include)
    add_test(NAME iso_reader_cdda_test COMMAND iso_reader_cdda_test)

    if(WIN32)
        add_executable(autocompile_publication_test
            tests/test_autocompile_publication.c
            src/autocompile.c)
        target_include_directories(autocompile_publication_test PRIVATE include)
        target_compile_definitions(autocompile_publication_test PRIVATE
            PSX_AUTOCOMPILE_TEST=1)
        add_test(NAME autocompile_publication_test
                 COMMAND autocompile_publication_test)
    else()
        add_executable(autocompile_posix_test
            tests/test_autocompile_posix.c
            src/autocompile.c)
        target_include_directories(autocompile_posix_test PRIVATE include)
        target_link_libraries(autocompile_posix_test PRIVATE Threads::Threads)
        add_test(NAME autocompile_posix_test COMMAND autocompile_posix_test)
    endif()

    add_executable(gpu_render_transaction_test
        tests/test_gpu_render_transaction.c
        src/gpu_render.c)
    target_include_directories(gpu_render_transaction_test PRIVATE include)
    target_compile_definitions(gpu_render_transaction_test PRIVATE
        GPU_RENDER_TRANSACTION_TESTING=1)
    add_test(NAME gpu_render_transaction COMMAND gpu_render_transaction_test)

    add_executable(gpu_semantic_workload_test
        tests/test_gpu_semantic_workload.c
        src/gpu_semantic_workload.c)
    target_include_directories(gpu_semantic_workload_test PRIVATE include)
    add_test(NAME gpu_semantic_workload COMMAND gpu_semantic_workload_test)

    add_executable(gpu_gl_mask_order_test
        tests/test_gpu_gl_mask_order.c
        tests/native_render_baseline_gpu_test_double.c
        src/color_lut.c
        src/gpu.c
        src/gpu_render_oracle.c
        src/guest_render_native_stream.c
        src/guest_render_transaction.c
        src/gpu_sw_renderer.c
        src/gpu_render.c
        src/gpu_semantic_workload.c
        src/gpu_gl_renderer.c)
    target_include_directories(gpu_gl_mask_order_test PRIVATE
        include ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(gpu_gl_mask_order_test PRIVATE
        SDL_MAIN_HANDLED=1
        PSX_NO_DEBUG_TOOLS=1)
    target_link_libraries(gpu_gl_mask_order_test PRIVATE ${SDL2_LIBRARIES})
    if(WIN32 OR MINGW)
        target_link_libraries(gpu_gl_mask_order_test PRIVATE opengl32)
        add_test(NAME gpu_gl_mask_order_test COMMAND gpu_gl_mask_order_test)
    else()
        find_package(OpenGL REQUIRED)
        find_program(XVFB_RUN_EXECUTABLE xvfb-run REQUIRED)
        target_link_libraries(gpu_gl_mask_order_test PRIVATE
            ${CMAKE_DL_LIBS} OpenGL::GL m)
        add_test(NAME gpu_gl_mask_order_test
                 COMMAND ${XVFB_RUN_EXECUTABLE} -a $<TARGET_FILE:gpu_gl_mask_order_test>)
    endif()

    add_executable(gpu_gl_peek_order_test
        tests/test_gpu_gl_peek_order.c
        tests/native_render_baseline_gpu_test_double.c
        src/color_lut.c
        src/gpu.c
        src/gpu_render_oracle.c
        src/guest_render_native_stream.c
        src/guest_render_transaction.c
        src/gpu_sw_renderer.c
        src/gpu_render.c
        src/gpu_semantic_workload.c
        src/gpu_gl_renderer.c)
    target_include_directories(gpu_gl_peek_order_test PRIVATE
        include ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(gpu_gl_peek_order_test PRIVATE
        SDL_MAIN_HANDLED=1
        PSX_NO_DEBUG_TOOLS=1)
    target_link_libraries(gpu_gl_peek_order_test PRIVATE ${SDL2_LIBRARIES})
    if(WIN32 OR MINGW)
        target_link_libraries(gpu_gl_peek_order_test PRIVATE opengl32)
        add_test(NAME gpu_gl_peek_order_test COMMAND gpu_gl_peek_order_test)
    else()
        find_package(OpenGL REQUIRED)
        find_program(XVFB_RUN_EXECUTABLE xvfb-run REQUIRED)
        target_link_libraries(gpu_gl_peek_order_test PRIVATE
            ${CMAKE_DL_LIBS} OpenGL::GL m)
        add_test(NAME gpu_gl_peek_order_test
                 COMMAND ${XVFB_RUN_EXECUTABLE} -a $<TARGET_FILE:gpu_gl_peek_order_test>)
    endif()

    add_executable(xg_render_gl_transaction_test
        tests/test_xg_render_gl_transaction.c
        tests/native_render_baseline_gpu_test_double.c
        src/color_lut.c
        src/gpu.c
        src/gpu_render_oracle.c
        src/guest_render_native_stream.c
        src/guest_render_transaction.c
        src/gpu_sw_renderer.c
        src/gpu_render.c
        src/gpu_semantic_workload.c
        src/gpu_gl_renderer.c)
    target_include_directories(xg_render_gl_transaction_test PRIVATE
        include ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(xg_render_gl_transaction_test PRIVATE
        SDL_MAIN_HANDLED=1
        PSX_NO_DEBUG_TOOLS=1
        PSX_DEBUG_OVERLAY=1
        PSX_GL_TRANSACTION_TESTING=1)
    target_link_libraries(xg_render_gl_transaction_test PRIVATE ${SDL2_LIBRARIES})
    if(WIN32 OR MINGW)
        target_link_libraries(xg_render_gl_transaction_test PRIVATE opengl32)
        add_test(NAME xg_render_gl_transaction
                 COMMAND xg_render_gl_transaction_test)
    else()
        target_link_libraries(xg_render_gl_transaction_test PRIVATE
            ${CMAKE_DL_LIBS} OpenGL::GL m)
        add_test(NAME xg_render_gl_transaction
                 COMMAND ${XVFB_RUN_EXECUTABLE} -a $<TARGET_FILE:xg_render_gl_transaction_test>)
    endif()

    add_executable(guest_render_bridge_test
        tests/test_guest_render_bridge.c
        src/guest_render_bridge.c)
    target_include_directories(guest_render_bridge_test PRIVATE
        include ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(guest_render_bridge_test PRIVATE
        GUEST_RENDER_BRIDGE_TESTING=1)
    target_link_libraries(guest_render_bridge_test PRIVATE ${SDL2_LIBRARIES})
    add_test(NAME guest_render_bridge COMMAND guest_render_bridge_test)

    add_executable(native_render_mode_control_test
        tests/test_native_render_mode_control.c
        src/native_render_mode_control.c)
    target_include_directories(native_render_mode_control_test PRIVATE include)
    add_test(NAME native_render_mode_control
             COMMAND native_render_mode_control_test)

    add_executable(guest_render_transaction_test
        tests/test_guest_render_transaction.c
        src/guest_render_native_stream.c
        src/guest_render_transaction.c)
    target_include_directories(guest_render_transaction_test PRIVATE include)
    target_compile_definitions(guest_render_transaction_test PRIVATE
        GUEST_RENDER_TRANSACTION_TESTING=1
        GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY=4
        GUEST_RENDER_TRANSACTION_WORD_CAPACITY=16
        GUEST_RENDER_TRANSACTION_BINDING_CAPACITY=2
        GUEST_RENDER_TRANSACTION_PENDING_CAPACITY=2)
    add_test(NAME guest_render_transaction COMMAND guest_render_transaction_test)

    add_executable(guest_render_native_stream_test
        tests/test_guest_render_native_stream.c
        src/guest_render_native_stream.c)
    target_include_directories(guest_render_native_stream_test PRIVATE include)
    target_compile_definitions(guest_render_native_stream_test PRIVATE
        GUEST_RENDER_NATIVE_STREAM_TESTING=1
        GUEST_RENDER_NATIVE_STREAM_CAPACITY=4)
    add_test(NAME guest_render_native_stream
             COMMAND guest_render_native_stream_test)

    add_executable(xg_render_oracle_core_test
        tests/test_gpu_render_oracle.c
        src/gpu_render_oracle.c)
    target_include_directories(xg_render_oracle_core_test PRIVATE include)
    target_compile_definitions(xg_render_oracle_core_test PRIVATE
        GPU_RENDER_ORACLE_TESTING=1)
    add_test(NAME xg_render_oracle_core COMMAND xg_render_oracle_core_test)

    add_executable(xg_render_oracle_integration_test
        tests/test_gpu_render_oracle_integration.c
        tests/gpu_render_oracle_integration_stubs.c
        src/color_lut.c
        src/dma.c
        src/gpu.c
        src/gpu_render.c
        src/gpu_render_oracle.c
        src/guest_render_native_stream.c
        src/guest_render_transaction.c
        src/gpu_sw_renderer.c
        src/memory.c)
    target_include_directories(xg_render_oracle_integration_test PRIVATE
        include src ${SDL2_INCLUDE_DIRS})
    target_link_libraries(xg_render_oracle_integration_test PRIVATE ${SDL2_LIBRARIES})
    if(MSVC)
        target_compile_options(xg_render_oracle_integration_test PRIVATE /Gy)
        target_link_options(xg_render_oracle_integration_test PRIVATE /OPT:REF)
    elseif(APPLE)
        target_compile_options(xg_render_oracle_integration_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(xg_render_oracle_integration_test PRIVATE -Wl,-dead_strip)
    else()
        target_compile_options(xg_render_oracle_integration_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(xg_render_oracle_integration_test PRIVATE -Wl,--gc-sections)
        target_link_libraries(xg_render_oracle_integration_test PRIVATE m)
    endif()
    add_test(NAME xg_render_oracle_integration
             COMMAND xg_render_oracle_integration_test)

    add_executable(gpu_atomic_checkpoint_test
        tests/test_gpu_atomic_checkpoint.c
        tests/gpu_render_oracle_integration_stubs.c
        src/color_lut.c
        src/dma.c
        src/gpu.c
        src/gpu_render.c
        src/gpu_render_oracle.c
        src/guest_render_native_stream.c
        src/guest_render_transaction.c
        src/gpu_sw_renderer.c
        src/memory.c)
    target_include_directories(gpu_atomic_checkpoint_test PRIVATE
        include src ${SDL2_INCLUDE_DIRS})
    target_compile_definitions(gpu_atomic_checkpoint_test PRIVATE
        GPU_RENDER_TRANSACTION_TESTING=1
        GUEST_RENDER_TRANSACTION_TESTING=1
        GPU_ATOMIC_CHECKPOINT_TESTING=1)
    target_link_libraries(gpu_atomic_checkpoint_test PRIVATE ${SDL2_LIBRARIES})
    if(MSVC)
        target_compile_options(gpu_atomic_checkpoint_test PRIVATE /Gy)
        target_link_options(gpu_atomic_checkpoint_test PRIVATE /OPT:REF)
    elseif(APPLE)
        target_compile_options(gpu_atomic_checkpoint_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(gpu_atomic_checkpoint_test PRIVATE -Wl,-dead_strip)
    else()
        target_compile_options(gpu_atomic_checkpoint_test PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(gpu_atomic_checkpoint_test PRIVATE -Wl,--gc-sections)
        target_link_libraries(gpu_atomic_checkpoint_test PRIVATE m)
    endif()
    add_test(NAME gpu_atomic_checkpoint COMMAND gpu_atomic_checkpoint_test)

    add_executable(xg_render_slot_substitution_test
        tests/test_xg_render_slot_substitution.c
        tests/gpu_render_oracle_integration_stubs.c
        src/color_lut.c
        src/dma.c
        src/gpu.c
        src/gpu_render.c
        src/gpu_render_oracle.c
        src/guest_render_native_stream.c
        src/guest_render_transaction.c
        src/gpu_sw_renderer.c
        src/memory.c)
    target_include_directories(xg_render_slot_substitution_test PRIVATE
        include src ${SDL2_INCLUDE_DIRS})
    set_target_properties(xg_render_slot_substitution_test PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS YES)
    target_compile_definitions(xg_render_slot_substitution_test PRIVATE
        GPU_RENDER_TRANSACTION_TESTING=1
        GUEST_RENDER_TRANSACTION_TESTING=1
        GPU_ATOMIC_CHECKPOINT_TESTING=1
        GUEST_RENDER_TRANSACTION_COMMAND_CAPACITY=4
        GUEST_RENDER_TRANSACTION_WORD_CAPACITY=64
        GUEST_RENDER_TRANSACTION_BINDING_CAPACITY=4
        GUEST_RENDER_TRANSACTION_PENDING_CAPACITY=4
        DMA2_LINKED_LIST_MAX_NODES=8)
    target_link_libraries(xg_render_slot_substitution_test PRIVATE
        ${SDL2_LIBRARIES})
    if(MSVC)
        target_compile_options(xg_render_slot_substitution_test PRIVATE
            /W4 /WX /Gy)
        target_link_options(xg_render_slot_substitution_test PRIVATE /OPT:REF)
    elseif(APPLE)
        target_compile_options(xg_render_slot_substitution_test PRIVATE
            -Wall -Wextra -Werror
            -Wno-error=unused-function -Wno-error=unused-variable
            -Wno-error=misleading-indentation -Wno-error=comment
            -ffunction-sections -fdata-sections)
        target_link_options(xg_render_slot_substitution_test PRIVATE
            -Wl,-dead_strip)
    else()
        target_compile_options(xg_render_slot_substitution_test PRIVATE
            -Wall -Wextra -Werror
            -Wno-error=unused-function -Wno-error=unused-variable
            -Wno-error=misleading-indentation -Wno-error=comment
            -ffunction-sections -fdata-sections)
        target_link_options(xg_render_slot_substitution_test PRIVATE
            -Wl,--gc-sections)
        target_link_libraries(xg_render_slot_substitution_test PRIVATE m)
    endif()
    add_test(NAME xg_render_slot_substitution
             COMMAND xg_render_slot_substitution_test)

endif()

# ---- psx-beetle target (Beetle PSX libretro standalone, MinGW) ----
#
# Standalone binary that runs Beetle PSX (mednafen-psx libretro core) with
# a debug server exposing the SAME wire-protocol commands as psx-runtime,
# on a different port (4380). No recomp code linked.
#
# Requires beetle-psx built via:
#   cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1 HAVE_LIGHTREC=0

set(BEETLE_LIB "${CMAKE_SOURCE_DIR}/../beetle-psx/libmednafen_psx.a")

if(NOT MSVC AND EXISTS "${BEETLE_LIB}")
    message(STATUS "psx-beetle: found ${BEETLE_LIB}")

    set(BEETLE_DIR "${CMAKE_SOURCE_DIR}/../beetle-psx")
    set(LRC "${BEETLE_DIR}/libretro-common")
    set(LIBRETRO_COMMON_SOURCES
        ${LRC}/streams/file_stream.c
        ${LRC}/streams/file_stream_transforms.c
        ${LRC}/vfs/vfs_implementation.c
        ${LRC}/file/file_path.c
        ${LRC}/compat/compat_strl.c
        ${LRC}/compat/fopen_utf8.c
        ${LRC}/compat/compat_posix_string.c
        ${LRC}/string/stdstring.c
        ${LRC}/lists/string_list.c
        ${LRC}/encodings/encoding_utf.c
        ${LRC}/time/rtime.c
        ${LRC}/rthreads/rthreads.c
        ${LRC}/memmap/memalign.c
        ${LRC}/hash/rhash.c
    )

    add_executable(psx-beetle
        src/beetle_main.cpp
        src/beetle_libretro.cpp
        src/beetle_debug_server.c
        src/frame_pacing.c
        src/parity_trace.c
        src/device_trace.c
        src/audio_trace.c
        ${LIBRETRO_COMMON_SOURCES}
    )

    target_include_directories(psx-beetle PRIVATE
        include
        ${SDL2_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/../beetle-psx/libretro-common/include
        ${CMAKE_SOURCE_DIR}/../beetle-psx
    )

    target_compile_definitions(psx-beetle PRIVATE
        PSX_BEETLE_BUILD=1
        DEFAULT_DEBUG_PORT=4380
    )
    if(NOT PSX_DEBUG_TOOLS)
        target_compile_definitions(psx-beetle PRIVATE PSX_NO_DEBUG_TOOLS=1)
    endif()

    target_link_libraries(psx-beetle PRIVATE
        "${BEETLE_LIB}"
        ${SDL2_LIBRARIES}
    )

    if(WIN32 OR MINGW)
        target_link_libraries(psx-beetle PRIVATE ws2_32 -lpthread)
    endif()

    if(MINGW)
        target_link_options(psx-beetle PRIVATE -Wl,--stack,67108864)
    endif()
elseif(NOT MSVC)
    message(STATUS "psx-beetle: libmednafen_psx.a not found - skipping psx-beetle")
endif()
