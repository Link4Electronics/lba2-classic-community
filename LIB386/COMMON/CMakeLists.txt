add_library(gpur GPURENDERER_COMMON.CPP)

target_include_directories(gpur PUBLIC ${CMAKE_SOURCE_DIR}/LIB386/H)
# Built unconditionally (see LIB386/CMakeLists.txt): USE_GPURENDERER selects
# the real renderer over the GpuRenderer_* stub set, gated the same way gly's
# half of RENDER_GL.CPP is gated.
if(GPURENDERER)
    target_compile_definitions(gpur PRIVATE USE_GPURENDERER)
endif()
target_link_libraries(gpur PUBLIC sys)