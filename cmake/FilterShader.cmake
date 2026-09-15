# Filters ##ifdef blocks out of a shared GLSL source for a given backend.
#
# Usage:
#   cmake -DINPUT=in.glsl -DOUTPUT=out.glsl -DKEEP=SDL3GPU -P FilterShader.cmake
#
# The shared shaders in LIB386/COMMON/ mark per-backend sections with
# ##ifdef SDL3GPU / ##ifdef GL_ES / ##ifdef GL_CORE / ##else / ##endif markers
# (double-hash so that neither the GLSL compiler nor glslang ever sees them).
# The GL backend strips these at runtime (GpuShader_FilterGL) so it can pick
# GL_ES or GL_CORE per platform; this script does the same for the SDL3GPU
# backend so glslang only receives a plain shader with a literal-first
# #version line. KEEP is the block to retain; only "SDL3GPU" is used at build
# time (GL picks its variant at runtime).
#
# The line loop uses a string marker + while() rather than CMake lists, because
# list conversion mangles trailing backslashes (GLSL macro continuations) and
# semicolons.
#
# Adapted from the BRender-v1.3.2 drivers' cmake/FilterShader.cmake
# (Argonaut Software, MIT License).

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED KEEP)
    message(FATAL_ERROR "Usage: cmake -DINPUT=in.glsl -DOUTPUT=out.glsl -DKEEP=SDL3GPU -P FilterShader.cmake")
endif()

file(READ "${INPUT}" raw)
string(REPLACE "\r\n" "\n" raw "${raw}")
string(REPLACE "\r" "\n" raw "${raw}")
string(REPLACE "\n" "__VKNL__" work "${raw}")

set(state "none")
set(out "")

while(work)
    string(FIND "${work}" "__VKNL__" idx)
    if(idx EQUAL -1)
        set(line "${work}")
        set(work "")
    else()
        string(SUBSTRING "${work}" 0 ${idx} line)
        math(EXPR next "${idx} + 8")
        string(SUBSTRING "${work}" ${next} -1 work)
    endif()

    if(line STREQUAL "##ifdef SDL3GPU")
        set(state "SDL3GPU")
    elseif(line STREQUAL "##ifdef GL_ES")
        set(state "GL")
    elseif(line STREQUAL "##ifdef GL_CORE")
        set(state "GL")
    elseif(line STREQUAL "##else")
        if(state STREQUAL "SDL3GPU")
            set(state "SDL3GPU_ELSE")
        else()
            set(state "GL")
        endif()
    elseif(line STREQUAL "##endif")
        set(state "none")
    else()
        if(state STREQUAL "none")
            string(APPEND out "${line}\n")
        elseif(KEEP STREQUAL state)
            string(APPEND out "${line}\n")
        endif()
    endif()
endwhile()

file(WRITE "${OUTPUT}" "${out}")