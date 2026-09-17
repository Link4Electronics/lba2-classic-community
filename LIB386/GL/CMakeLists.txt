add_library(gly RENDER_GL.CPP)

target_include_directories(gly PUBLIC ${CMAKE_SOURCE_DIR}/LIB386/H)
# gly is always built: with GPURENDERER on it is the real backend (and the
# renderer state globals), with it off it compiles the stubs that keep every
# caller's references linkable. USE_GPURENDERER selects which half compiles.
if(GPURENDERER)
    target_compile_definitions(gly PRIVATE USE_GPURENDERER)
endif()
target_link_libraries(gly PUBLIC sys)

if(ANDROID)
    # GLES 2.0 is part of the NDK — no extra link needed. The GL dialect
    # macro is a build decision, so the platform conditional lives here in
    # CMake and the renderer source stays platform-clean (arch-check rule 3).
    target_compile_definitions(gly PRIVATE USE_GLES2)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(gly PUBLIC OpenGL::GL)
endif()

# Present + fill seams: SetGpuPresentHooks (SVGA/SDL.CPP) and the
# Fill_Poly_SetHook call that registers the triangle hook.
target_link_libraries(gly PUBLIC svg poly)
