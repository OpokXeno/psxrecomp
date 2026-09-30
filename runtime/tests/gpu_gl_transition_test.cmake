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
endif()
