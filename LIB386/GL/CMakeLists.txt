add_library(gly RENDER_GL.CPP)

target_include_directories(gly PUBLIC ${CMAKE_SOURCE_DIR}/LIB386/H
                                      ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(gly PRIVATE USE_GPURENDERER)
target_link_libraries(gly PUBLIC sys)

if(ANDROID)
    # GLES 2.0 is part of the NDK — no extra link needed. The GL dialect macro
    # is a build decision, so the platform conditional lives here in CMake and
    # the renderer source stays archive-clean (arch-check rule 3).
    target_compile_definitions(gly PRIVATE USE_GLES2)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(gly PUBLIC OpenGL::GL)
endif()

# Embed the shared raw shaders from LIB386/COMMON (the GL backend resolves the
# ##ifdef GL_ES/GL_CORE blocks at runtime with GpuShader_FilterGL, so it needs
# the unfiltered source, unlike the SDL3GPU backend which filters at build
# time and cross-compiles).
set(GL_SHARED_SHADERS present.vert present.frag clut.vert clut.frag)
set(GL_EMBEDDED_HEADERS)
foreach(shader_name IN LISTS GL_SHARED_SHADERS)
    set(shader_src "${CMAKE_CURRENT_SOURCE_DIR}/../COMMON/${shader_name}.glsl")
    set(shader_hdr "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.glsl.h")
    add_custom_command(
        OUTPUT "${shader_hdr}"
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../COMMON
            -DFILE=${shader_name}.glsl
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedResource.cmake
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS "${shader_src}" "${CMAKE_SOURCE_DIR}/cmake/EmbedResource.cmake"
        COMMENT "Embedding ${shader_name}.glsl"
        VERBATIM)
    list(APPEND GL_EMBEDDED_HEADERS "${shader_hdr}")
endforeach()

target_sources(gly PRIVATE ${GL_EMBEDDED_HEADERS})
