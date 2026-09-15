# SDL3 GPU renderer backend (LIB386/SDL3GPU). Renders via SDL3's GPU API,
# whose backends each require a different shader format:
#   - Vulkan:  SPIR-V, produced by glslangValidator/glslang/glslc (-V) from the
#              filtered COMMON GLSL (existing pipeline).
#   - Metal:   MSL, cross-compiled from the SPIR-V with SDL_shadercross (or
#              spirv-cross --msl --msl-decoration-binding as a fallback), which
#              maps the [[buffer(N)]] / [[texture(N)]] indices onto the SDL3-GPU
#              slots and renames the entry point main -> main0.
#   - D3D12:   DXIL, cross-compiled from the SPIR-V with SDL_shadercross, or
#              spirv-cross (--hlsl --shader-model 60) + dxc as a fallback.
# SDL_shadercross is the preferred translator: it is the tool SDL3 itself uses,
# and its MSL/DXIL output matches the SDL3-GPU conventions by construction.
# Every available format is embedded at build time; RENDER_SDL3GPU.CPP selects
# the matching source per device (see sdl3gpu_shader_formats.h).
#
# MSL is mandatory on Apple: SDL3 GPU picks the Metal backend there, and a
# build that can only produce SPIR-V would not run without MoltenVK. Fail at
# configure time instead of at runtime.
find_program(GLSL_COMPILER NAMES glslangValidator glslang glslc)
if(GLSL_COMPILER)
    get_filename_component(GLSL_COMPILER_NAME ${GLSL_COMPILER} NAME_WE)
    message(STATUS "Found GLSL compiler: ${GLSL_COMPILER} (auto-compile .glsl -> .spv)")
else()
    message(STATUS "No GLSL compiler found; SDL3 GPU backend will embed no shaders")
endif()

find_program(SHADERCROSS_COMPILER NAMES shadercross)
if(SHADERCROSS_COMPILER)
    message(STATUS "Found SDL_shadercross: ${SHADERCROSS_COMPILER} (SPIR-V -> MSL / DXIL)")
endif()

find_program(SPIRV_CROSS_COMPILER NAMES spirv-cross)
if(SPIRV_CROSS_COMPILER)
    message(STATUS "Found spirv-cross: ${SPIRV_CROSS_COMPILER} (SPIR-V -> MSL / HLSL; fallback)")
endif()

find_program(DXC_COMPILER NAMES dxc)
if(DXC_COMPILER)
    message(STATUS "Found dxc: ${DXC_COMPILER} (HLSL -> DXIL for D3D12; fallback)")
endif()

# The skeleton backend is harmless without cross-compilers (it degrades to no
# embedded MSL/DXIL and the device creation falls back to Vulkan SPIR-V), but
# on Apple SPIR-V-only is unusable: Metal is the only driver. Mirror dethrace's
# fence so the failure is a configure error, not a runtime surprise.
if(APPLE AND NOT (SPIRV_CROSS_COMPILER OR SHADERCROSS_COMPILER))
    message(FATAL_ERROR
        "SDL_shadercross or spirv-cross is required to build the SDL3 GPU "
        "backend on Apple (MSL shaders for the Metal backend). Install "
        "SDL_shadercross (https://github.com/libsdl-org/SDL_shadercross) or spirv-cross.")
endif()

if(WIN32 AND NOT (SHADERCROSS_COMPILER OR (SPIRV_CROSS_COMPILER AND DXC_COMPILER)))
    message(WARNING
        "SDL_shadercross (or spirv-cross and dxc) not found; the SDL3 GPU "
        "backend will fall back to the Vulkan backend on Windows. Install "
        "SDL_shadercross (https://github.com/libsdl-org/SDL_shadercross) to "
        "enable the D3D12 (DXIL) backend.")
endif()

# Which shader formats does this build embed? Drives the device create mask in
# RENDER_SDL3GPU.CPP (via sdl3gpu_shader_formats.h). SPIR-V is compiled here
# from the COLIBMON GLSL when a GLSL compiler exists; there are no prebuilt
# blobs in the repo to fall back on, so it is available exactly when one does.
set(SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE 0)
set(SDL3GPU_SHADERFORMAT_MSL_AVAILABLE 0)
set(SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE 0)
if(GLSL_COMPILER)
    set(SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE 1)
    if(SPIRV_CROSS_COMPILER OR SHADERCROSS_COMPILER)
        set(SDL3GPU_SHADERFORMAT_MSL_AVAILABLE 1)
        # DXIL is only used by the D3D12 backend (Windows). SDL_shadercross
        # produces it without dxc; the spirv-cross fallback still needs dxc for
        # the final HLSL -> DXIL step.
        if((SHADERCROSS_COMPILER AND WIN32) OR DXC_COMPILER)
            set(SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE 1)
        endif()
    endif()
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdl3gpu_shader_formats.h"
    "#ifndef SDL3GPU_SHADER_FORMATS_H_\n"
    "#define SDL3GPU_SHADER_FORMATS_H_\n"
    "\n"
    "/* Auto-generated; describes which shader formats this build embeds. */\n"
    "#define SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE ${SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE}\n"
    "#define SDL3GPU_SHADERFORMAT_MSL_AVAILABLE ${SDL3GPU_SHADERFORMAT_MSL_AVAILABLE}\n"
    "#define SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE ${SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE}\n"
    "\n"
    "#endif\n")

# Shared shaders live in LIB386/COMMON with ##ifdef SDL3GPU/GL_ES/GL_CORE
# markers. Filter for SPIR-V (plain GLSL, literal-first #version 450) before
# compiling; the SDL3GPU marker keeps the set-1/2/3 binding layout that the
# SDL3-GPU pipeline layout uses.
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../COMMON")

set(SPV_GENERATED)
set(MSL_GENERATED)
set(DXIL_GENERATED)
foreach(spv_file
    present.vert.spv
    present.frag.spv
    clut.vert.spv
    clut.frag.spv
)
    get_filename_component(spv_base ${spv_file} NAME)
    string(REPLACE ".spv" "" spv_base ${spv_base})
    string(REPLACE ".spv" ".glsl" glsl_file ${spv_file})
    set(h_path "${CMAKE_CURRENT_BINARY_DIR}/${spv_file}.h")

    if(spv_base MATCHES "vert$")
        set(shader_stage vert)
        set(shadercross_stage vertex)
    else()
        set(shader_stage frag)
        set(shadercross_stage fragment)
    endif()

    if(GLSL_COMPILER)
        set(glsl_src "${SHADER_DIR}/${glsl_file}")
        set(glsl_filtered "${CMAKE_CURRENT_BINARY_DIR}/${glsl_file}")
        set(spv_path "${CMAKE_CURRENT_BINARY_DIR}/${spv_file}")
        add_custom_command(
            OUTPUT ${glsl_filtered}
            COMMAND ${CMAKE_COMMAND} "-DINPUT=${glsl_src}" "-DOUTPUT=${glsl_filtered}" "-DKEEP=SDL3GPU" -P "${CMAKE_SOURCE_DIR}/cmake/FilterShader.cmake"
            DEPENDS ${glsl_src}
            COMMENT "Filtering ${glsl_file} for SPIR-V"
        )
        if(GLSL_COMPILER_NAME STREQUAL "glslangValidator" OR GLSL_COMPILER_NAME STREQUAL "glslang")
            set(compile_cmd ${GLSL_COMPILER} -V -o ${spv_path} ${glsl_filtered})
        else()
            set(compile_cmd ${GLSL_COMPILER} -o ${spv_path} ${glsl_filtered})
        endif()
        add_custom_command(
            OUTPUT ${spv_path}
            COMMAND ${compile_cmd}
            DEPENDS ${glsl_filtered}
            COMMENT "Compiling ${glsl_file} to ${spv_file}"
        )
        set(embed_source_dir "${CMAKE_CURRENT_BINARY_DIR}")
        set(embed_depends ${spv_path})

        if(SHADERCROSS_COMPILER)
            # Metal: MSL source, compiled by Metal at runtime. SDL_shadercross
            # maps the SPIR-V binding numbers onto the MSL indices (matching
            # spirv-cross --msl-decoration-binding) and renames the entry point
            # main -> main0. The Metal backend always treats the shader as MSL,
            # so this source is embedded as bytes regardless.
            set(metal_path "${CMAKE_CURRENT_BINARY_DIR}/${spv_base}.metal")
            add_custom_command(
                OUTPUT ${metal_path}
                COMMAND ${SHADERCROSS_COMPILER} -s SPIRV -d MSL -t ${shadercross_stage} -e main -o ${metal_path} ${spv_path}
                DEPENDS ${spv_path}
                COMMENT "Cross-compiling ${spv_base} to MSL (SDL_shadercross)"
            )
            set(metal_h "${CMAKE_CURRENT_BINARY_DIR}/${spv_base}.metal.h")
            add_custom_command(
                OUTPUT ${metal_h}
                COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DFILE=${spv_base}.metal" -P "${CMAKE_SOURCE_DIR}/cmake/EmbedResource.cmake"
                DEPENDS ${metal_path}
                COMMENT "Embedding ${spv_base}.metal"
            )
            list(APPEND MSL_GENERATED ${metal_h})

            # DXIL (D3D12, Windows only): SDL_shadercross transpiles to HLSL
            # (SM6.0, with the register spaces SDL3's D3D12 backend expects) and
            # compiles to DXIL via the bundled DXC in one step.
            if(WIN32)
                set(dxil_path "${CMAKE_CURRENT_BINARY_DIR}/${spv_base}.dxil")
                add_custom_command(
                    OUTPUT ${dxil_path}
                    COMMAND ${SHADERCROSS_COMPILER} -s SPIRV -d DXIL -t ${shadercross_stage} -e main -o ${dxil_path} ${spv_path}
                    DEPENDS ${spv_path}
                    COMMENT "Compiling ${spv_base} to DXIL (SDL_shadercross)"
                )
                set(dxil_h "${CMAKE_CURRENT_BINARY_DIR}/${spv_base}.dxil.h")
                add_custom_command(
                    OUTPUT ${dxil_h}
                    COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DFILE=${spv_base}.dxil" -P "${CMAKE_SOURCE_DIR}/cmake/EmbedResource.cmake"
                    DEPENDS ${dxil_path}
                    COMMENT "Embedding ${spv_base}.dxil"
                )
                list(APPEND DXIL_GENERATED ${dxil_h})
            endif()
        elseif(SPIRV_CROSS_COMPILER)
            # Metal: MSL source, compiled by Metal at runtime.
            # --msl-decoration-binding maps the SPIR-V binding numbers onto the
            # MSL indices, which for these shaders are exactly the SDL3-GPU
            # uniform/sampler slots.
            set(metal_path "${CMAKE_CURRENT_BINARY_DIR}/${spv_base}.metal")
            add_custom_command(
                OUTPUT ${metal_path}
                COMMAND ${SPIRV_CROSS_COMPILER} ${spv_path} --msl --msl-decoration-binding --rename-entry-point main main0 ${shader_stage} --output ${metal_path}
                DEPENDS ${spv_path}
                COMMENT "Cross-compiling ${spv_base} to MSL"
            )
            set(metal_h "${CMAKE_CURRENT_BINARY_DIR}/${spv_base}.metal.h")
            add_custom_command(
                OUTPUT ${metal_h}
                COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DFILE=${spv_base}.metal" -P "${CMAKE_SOURCE_DIR}/cmake/EmbedResource.cmake"
                DEPENDS ${metal_path}
                COMMENT "Embedding ${spv_base}.metal"
            )
            list(APPEND MSL_GENERATED ${metal_h})

            # HLSL (SM6.0) then DXIL. spirv-cross already emits the register
            # spaces SDL3's D3D12 backend expects.
            set(hlsl_path "${CMAKE_CURRENT_BINARY_DIR}/${spv_base}.hlsl")
            add_custom_command(
                OUTPUT ${hlsl_path}
                COMMAND ${SPIRV_CROSS_COMPILER} ${spv_path} --hlsl --shader-model 60 --output ${hlsl_path}
                DEPENDS ${spv_path}
                COMMENT "Cross-compiling ${spv_base} to HLSL"
            )
            if(DXC_COMPILER)
                if(shader_stage STREQUAL "vert")
                    set(dxil_profile vs_6_0)
                else()
                    set(dxil_profile ps_6_0)
                endif()
                set(dxil_path "${CMAKE_CURRENT_BINARY_DIR}/${spv_base}.dxil")
                add_custom_command(
                    OUTPUT ${dxil_path}
                    COMMAND ${DXC_COMPILER} -T ${dxil_profile} -E main -Fo ${dxil_path} ${hlsl_path}
                    DEPENDS ${hlsl_path}
                    COMMENT "Compiling ${spv_base} to DXIL (${dxil_profile})"
                )
                set(dxil_h "${CMAKE_CURRENT_BINARY_DIR}/${spv_base}.dxil.h")
                add_custom_command(
                    OUTPUT ${dxil_h}
                    COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DFILE=${spv_base}.dxil" -P "${CMAKE_SOURCE_DIR}/cmake/EmbedResource.cmake"
                    DEPENDS ${dxil_path}
                    COMMENT "Embedding ${spv_base}.dxil"
                )
                list(APPEND DXIL_GENERATED ${dxil_h})
            endif()
        endif()
    endif()

    if(SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE)
        add_custom_command(
            OUTPUT ${h_path}
            COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${embed_source_dir}" "-DFILE=${spv_file}" -P "${CMAKE_SOURCE_DIR}/cmake/EmbedResource.cmake"
            DEPENDS ${embed_depends}
            COMMENT "Embedding ${spv_file}"
        )
        list(APPEND SPV_GENERATED ${h_path})
    endif()
endforeach()

add_library(gs3 RENDER_SDL3GPU.CPP ${SPV_GENERATED} ${MSL_GENERATED} ${DXIL_GENERATED})

target_include_directories(gs3 PUBLIC ${CMAKE_SOURCE_DIR}/LIB386/H)
target_include_directories(gs3 PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(gs3 PRIVATE SDL3::SDL3)

set_target_properties(gs3 PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED TRUE
        FOLDER "LIB386")