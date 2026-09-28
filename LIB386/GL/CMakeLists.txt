add_library(gly RENDER_GL.CPP)

target_include_directories(gly PUBLIC ${CMAKE_SOURCE_DIR}/LIB386/H)
# gly is always built: with GPURENDERER on it is the real backend (and the
# renderer state globals), with it off it compiles the stubs that keep every
# caller's references linkable. USE_GPURENDERER selects which half compiles.
if(GPURENDERER)
    target_compile_definitions(gly PRIVATE USE_GPURENDERER)
endif()
target_link_libraries(gly PUBLIC sys)

if(ANDROID OR LBA2_GLES2)
    # The GL dialect macro is a build decision, so the platform conditional
    # lives here in CMake and the renderer source stays platform-clean
    # (arch-check rule 3). GLES 2.0 is part of the NDK — no extra link needed.
    target_compile_definitions(gly PRIVATE USE_GLES2)
    if(NOT ANDROID)
        # Desktop forced-GLES build: the entry points must resolve through
        # libGLESv2, the library SDL's EGL path binds the context to —
        # resolving them through libGL would dispatch against GLX instead.
        # Linked by name because FindOpenGL's OpenGL::GLES2 target is newer
        # than this project's cmake_minimum_required.
        find_library(LBA2_GLES2_LIBRARY NAMES GLESv2)
        if(NOT LBA2_GLES2_LIBRARY)
            message(FATAL_ERROR
                    "LBA2_GLES2=ON needs libGLESv2 (install the GLES 2.0 "
                    "runtime, e.g. libgles2-mesa-dev / mesa-libGLES)")
        endif()
        target_link_libraries(gly PUBLIC ${LBA2_GLES2_LIBRARY})
    endif()
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(gly PUBLIC OpenGL::GL)
endif()

# Present + fill seams: SetGpuPresentHooks (SVGA/SDL.CPP) and the
# Fill_Poly_SetHook call that registers the triangle hook.
target_link_libraries(gly PUBLIC svg poly)
