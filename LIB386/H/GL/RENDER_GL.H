//──────────────────────────────────────────────────────────────────────────
// GPU renderer: OpenGL / GLES 2.0 backend (LIB386/GL)
//──────────────────────────────────────────────────────────────────────────
// Implements the GPU/GPURENDERER_BACKEND vtable (GpuRendererBackendGL): this
// TU is pure drawing — bind, set state, draw, composite — and every behaviour
// (polygon dispatch, GpuDrawParams resolution, batch flushes, texture-upload
// timing, the frame sequence and readback post-process) is owned by the
// common layer (LIB386/COMMON). The vtable lives in RENDER_GL.CPP; the
// shared renderer-state globals below are defined there as well and read by
// ANIMTEX and the common layer. gly is always built (stubs when GPURENDERER
// is off) so those consumers link a single definition; the vtable symbol is
// what the common layer tables at runtime.
//──────────────────────────────────────────────────────────────────────────
#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
//! Bilinear CLUT / overlay sampling in GL mode.
extern S32 DisplayTexFilter;
//! FBO supersample factor: 1 = native, 2 = SSAA 2x, 4 = SSAA 4x.
extern S32 RenderQuality;

// -----------------------------------------------------------------------------
#ifdef __cplusplus
}
#endif