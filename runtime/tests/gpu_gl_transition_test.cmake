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
    set_tests_properties(native_worker_scaling PROPERTIES TIMEOUT 30)
    set_tests_properties(native_gpu_scale PROPERTIES TIMEOUT 20
        ENVIRONMENT "SDL_VIDEODRIVER=offscreen;LP_NUM_THREADS=2")
endif()
