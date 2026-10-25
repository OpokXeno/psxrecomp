# The focused test includes the renderer's private transfer implementation.
# ELF section GC discards unrelated GL entry points, so no display is needed.
if(UNIX AND NOT APPLE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(gpu_gl_transition_test
        "${CMAKE_CURRENT_LIST_DIR}/test_gpu_gl_transition.c")
    target_include_directories(gpu_gl_transition_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../include" ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(gpu_gl_transition_test PRIVATE SDL_MAIN_HANDLED=1)
    target_compile_options(gpu_gl_transition_test PRIVATE
        -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(gpu_gl_transition_test PRIVATE -Wl,--gc-sections)
    target_link_libraries(gpu_gl_transition_test PRIVATE xg_semantic_presentation m)
    add_test(NAME gpu_gl_transition COMMAND gpu_gl_transition_test)

    # Logical scanout conversion runs on the CPU even with GPU presentation.
    add_executable(native_scanout_test "${CMAKE_CURRENT_LIST_DIR}/test_native_scanout.c")
    target_include_directories(native_scanout_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../include" ${PSX_SDL_INCLUDE_DIRS})
    target_compile_features(native_scanout_test PRIVATE c_std_11)
    target_compile_definitions(native_scanout_test PRIVATE SDL_MAIN_HANDLED=1 PSX_NO_DEBUG_TOOLS=1)
    if(PSX_SDL_BACKEND STREQUAL "SDL3")
        target_compile_definitions(native_scanout_test PRIVATE PSX_SDL3=1)
    endif()
    # PIC keeps renderer callback addresses out of merged constant pools, which
    # would otherwise retain the unrelated GL backend when section GC runs.
    target_compile_options(native_scanout_test PRIVATE -fPIC -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(native_scanout_test PRIVATE -Wl,--gc-sections)
    target_link_libraries(native_scanout_test PRIVATE xg_semantic_presentation m)
    add_test(NAME native_scanout COMMAND native_scanout_test)
    set_tests_properties(native_scanout PROPERTIES TIMEOUT 10)

    add_executable(native_raster_layout_test "${CMAKE_CURRENT_LIST_DIR}/test_native_raster_layout.c")
    target_include_directories(native_raster_layout_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../src" "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_compile_features(native_raster_layout_test PRIVATE c_std_11)
    target_link_libraries(native_raster_layout_test PRIVATE xg_semantic_presentation)
    add_test(NAME native_raster_layout COMMAND native_raster_layout_test)

    # GPU-only additions at 1x and exact word-texture reuse use real hidden GL.
    find_package(OpenGL REQUIRED)
    add_executable(native_gpu_scale_test
        "${CMAKE_CURRENT_LIST_DIR}/test_native_gpu_scale.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/host_parallel.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/host_cpu.c")
    target_include_directories(native_gpu_scale_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../include" ${PSX_SDL_INCLUDE_DIRS})
    target_compile_features(native_gpu_scale_test PRIVATE c_std_11)
    target_compile_definitions(native_gpu_scale_test PRIVATE
        SDL_MAIN_HANDLED=1 PSX_NO_DEBUG_TOOLS=1)
    if(PSX_SDL_BACKEND STREQUAL "SDL3")
        target_compile_definitions(native_gpu_scale_test PRIVATE PSX_SDL3=1)
    endif()
    target_compile_options(native_gpu_scale_test PRIVATE
        -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(native_gpu_scale_test PRIVATE
        -Wl,--gc-sections -Wl,--wrap=glTexSubImage2D)
    target_link_libraries(native_gpu_scale_test PRIVATE
        xg_semantic_presentation xg_render_ir ${PSX_SDL_LIBRARIES} OpenGL::GL m)
    add_test(NAME native_gpu_scale
        COMMAND native_gpu_scale_test)
    add_test(NAME native_worker_scaling
        COMMAND native_gpu_scale_test --workers-only)
    add_test(NAME native_motion_texture_identity
        COMMAND native_gpu_scale_test --motion-only)
    set_tests_properties(native_motion_texture_identity PROPERTIES TIMEOUT 10)
    set_tests_properties(native_worker_scaling PROPERTIES TIMEOUT 30)
    set_tests_properties(native_gpu_scale PROPERTIES TIMEOUT 20
        ENVIRONMENT "SDL_VIDEODRIVER=offscreen;LP_NUM_THREADS=2")

    # The FP32 regression uses public GL and the shared production shaders;
    # it does not depend on the renderer's private CPU/executor layout.
    set(_depth_shader_dir "${CMAKE_CURRENT_LIST_DIR}/../shaders")
    set(_depth_test_shaders
        "${_depth_shader_dir}/native_gpu.vert"
        "${_depth_shader_dir}/native_gpu.frag"
        "${_depth_shader_dir}/native_setup.comp"
        "${_depth_shader_dir}/native_capture_resolve.comp"
        "${_depth_shader_dir}/native_capture_linked.comp"
        "${CMAKE_CURRENT_LIST_DIR}/native_depth_probe.frag")
    set(_depth_header "${CMAKE_CURRENT_BINARY_DIR}/native_depth_test/depth_test_shaders.h")
    add_custom_command(OUTPUT "${_depth_header}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/embed_glsl.py"
            --out "${_depth_header}" ${_depth_test_shaders}
        DEPENDS ${_depth_test_shaders}
            "${_depth_shader_dir}/native_depth.glsl"
            "${_depth_shader_dir}/native_gpu_common.glsl"
            "${_depth_shader_dir}/native_raster.glsl"
            "${_depth_shader_dir}/native_prelude.glsl"
            "${_depth_shader_dir}/psx_raster.glsl"
            "${CMAKE_CURRENT_LIST_DIR}/../../tools/embed_glsl.py"
        VERBATIM)
    add_executable(native_depth_gpu_test "${CMAKE_CURRENT_LIST_DIR}/test_native_depth_gpu.c" "${_depth_header}")
    target_include_directories(native_depth_gpu_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../include" "${CMAKE_CURRENT_LIST_DIR}/../src"
        "${CMAKE_CURRENT_BINARY_DIR}/native_depth_test" ${PSX_SDL_INCLUDE_DIRS})
    target_compile_features(native_depth_gpu_test PRIVATE c_std_11)
    target_compile_definitions(native_depth_gpu_test PRIVATE SDL_MAIN_HANDLED=1)
    if(PSX_SDL_BACKEND STREQUAL "SDL3")
        target_compile_definitions(native_depth_gpu_test PRIVATE PSX_SDL3=1)
    endif()
    target_link_libraries(native_depth_gpu_test PRIVATE ${PSX_SDL_LIBRARIES} OpenGL::GL m)
    add_test(NAME native_depth_gpu COMMAND native_depth_gpu_test)
    set_tests_properties(native_depth_gpu PROPERTIES TIMEOUT 20
        ENVIRONMENT "SDL_VIDEODRIVER=offscreen;LP_NUM_THREADS=2")
    add_test(NAME native_depth_bounds
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_native_vk_depth_bounds.py")

    add_executable(native_resolution_gpu_test "${CMAKE_CURRENT_LIST_DIR}/test_native_resolution_gpu.c" "${_depth_header}")
    target_include_directories(native_resolution_gpu_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../include" "${CMAKE_CURRENT_BINARY_DIR}/native_depth_test" ${PSX_SDL_INCLUDE_DIRS})
    target_compile_features(native_resolution_gpu_test PRIVATE c_std_11)
    target_compile_definitions(native_resolution_gpu_test PRIVATE SDL_MAIN_HANDLED=1)
    if(PSX_SDL_BACKEND STREQUAL "SDL3")
        target_compile_definitions(native_resolution_gpu_test PRIVATE PSX_SDL3=1)
    endif()
    target_link_libraries(native_resolution_gpu_test PRIVATE ${PSX_SDL_LIBRARIES} OpenGL::GL m)
    add_test(NAME native_resolution_gpu COMMAND native_resolution_gpu_test)
    set_tests_properties(native_resolution_gpu PROPERTIES TIMEOUT 30
        ENVIRONMENT "SDL_VIDEODRIVER=offscreen;LP_NUM_THREADS=2")
endif()
