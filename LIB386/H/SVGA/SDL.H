#pragma once

#include <SVGA/VIDEO.H>

#ifdef __cplusplus
extern "C" {
#endif

// GPU present seam. The GPU renderer (GPURenderer_Init in LIB386/COMMON)
// registers itself here on init; the present path in SDL.CPP then hands its
// Log ARGB overlay to the renderer instead of the software streaming texture.
// Both pointers stay NULL on the software path, which the ASM equivalence
// tests link without any GPU dependency.
typedef void (*GpuPresentFn)(const void *argbPixels, U32 pitch, U32 width,
                             U32 height, S32 filterLinear);
typedef S32 (*GpuIsActiveFn)(void);
typedef void (*GpuVSyncFn)(S32 enabled);
void SetGpuPresentHooks(GpuPresentFn present, GpuIsActiveFn isActive);

// Present pacing seam. The renderer registers its setter on init and is handed
// whatever was chosen before it existed, so a config value loaded during boot
// still reaches the device. Kept apart from the present hooks, which only
// carry frames and must stay NULL on the software path.
void SetGpuVSyncHook(GpuVSyncFn setVSync);
void GpuPresentSetVSync(S32 enabled);

// TRUE while a GPU renderer owns the present path (either OpenGL or SDL3 GPU).
// The window layer asks this, not the GL backend's global, so both GPU
// backends get the same letterbox-aware mouse mapping and sys keeps no link
// dependency on a specific renderer.
S32 GpuPresentIsActive(void);

#ifdef __cplusplus
}
#endif
