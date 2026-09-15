//──────────────────────────────────────────────────────────────────────────
// GPU renderer: OpenGL / GLES 2.0 backend (LIB386/GL)
//──────────────────────────────────────────────────────────────────────────
// The GL backend is one vtable symbol, GpuRendererBackendGL
// (GPU/GPURENDERER_BACKEND.H), implemented in RENDER_GL.CPP. Nothing outside
// the backend calls GL functions directly; the engine reaches the renderer
// through the common layer (GPU/GPURENDERER.H), which owns polygon dispatch,
// batching, frame composition and readbacks.
//
// The one standalone helper is GpuShader_FilterGL. Shaders are stored once,
// raw, in LIB386/COMMON/*.glsl, marked up with ##ifdef GL_ES / GL_CORE /
// SDL3GPU blocks. The SDL3 backend filters them at build time for SPIR-V and
// cross-compiles the result; the GL backend embeds the same raw sources and
// resolves GL_ES vs GL_CORE at runtime so the desktop and GLES dialects come
// from one file each.
//──────────────────────────────────────────────────────────────────────────
#ifndef LIB_GPU_RENDERER_GL
#define LIB_GPU_RENDERER_GL

#include <SYSTEM/ADELINE_TYPES.H>

#ifdef __cplusplus
extern "C" {
#endif

// Dialect selectors for GpuShader_FilterGL.
#define GPU_SHADER_GL_ES   1 /* #version 100 (GLES 2.0) */
#define GPU_SHADER_GL_CORE 2 /* #version 110 (desktop GL 2.1) */

// Resolve the ##ifdef blocks of a shared LIB386/COMMON shader for the GL
// backend. `srcSize` is the byte length of the embedded source (the embedded
// arrays are not NUL-terminated). Output is a NUL-terminated GLSL string;
// returns TRUE when it fit in `outSize`, FALSE on truncation.
S32 GpuShader_FilterGL(const char *src, U32 srcSize, S32 dialect,
                       char *out, U32 outSize);

#ifdef __cplusplus
}
#endif

#endif // LIB_GPU_RENDERER_GL