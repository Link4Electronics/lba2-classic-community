//──────────────────────────────────────────────────────────────────────────
// GPU renderer: engine-facing API
//──────────────────────────────────────────────────────────────────────────
// The game renders through this API when a GPU backend is active; otherwise
// it falls through to the software rasterizer untouched. The implementation
// (LIB386/COMMON/GPURENDERER_COMMON.CPP) is driver-agnostic: it resolves
// polygon types, packs vertices and composes frames on top of one backend
// vtable (GPU/GPURENDERER_BACKEND.H), so the engine never names OpenGL or
// SDL3 GPU directly. Mirrors the commonrend/glrend/sdl3gpurend split of the
// BRender drivers.
//──────────────────────────────────────────────────────────────────────────
#ifndef LIB_GPURENDERER
#define LIB_GPURENDERER

//──────────────────────────────────────────────────────────────────────────
#include <SYSTEM/ADELINE_TYPES.H>
#include <POLYGON/POLY.H> // Struc_Point

//──────────────────────────────────────────────────────────────────────────
#ifdef __cplusplus
extern "C" {
#endif

//──────────────────────────────────────────────────────────────────────────
typedef enum GpuRendererBackendId {
    GPU_RENDERER_BACKEND_NONE = 0,  ///< Software rasterizer (always available)
    GPU_RENDERER_BACKEND_OPENGL = 1, ///< OpenGL / GLES 2.0 (LIB386/GL)
    GPU_RENDERER_BACKEND_SDL3GPU = 2, ///< SDL3 GPU (LIB386/SDL3GPU)
    GPU_RENDERER_BACKEND_COUNT
} GpuRendererBackendId;

//──────────────────────────────────────────────────────────────────────────
// Lifecycle. Init creates the backend's context on the existing window and
// returns TRUE when the backend is live; the engine decides to keep using
// the software path when it fails. Shutdown tears the renderer down, leaving
// the window (and software path) intact.
S32  GpuRenderer_Init(GpuRendererBackendId backend, U32 resX, U32 resY);
void GpuRenderer_Shutdown(void);
void GpuRenderer_Resize(U32 resX, U32 resY);

// Runtime state queries.
S32  GpuRenderer_IsActive(void);
GpuRendererBackendId GpuRenderer_ActiveBackend(void);
const char *GpuRenderer_BackendName(GpuRendererBackendId id);

//──────────────────────────────────────────────────────────────────────────
// Frame control. BeginFrame must precede any of ClearFBO/Present; ClearFBO
// is called before the terrain pass (affGrille) so a frame with no terrain
// keeps the previous frame's 3D content.
void GpuRenderer_BeginFrame(void);
void GpuRenderer_ClearFBO(void);
void GpuRenderer_FlushBatch(void);
void GpuRenderer_Present(const void *argbPixels, U32 pitch, U32 width,
                         U32 height, S32 filterLinear);
S32  GpuRenderer_RenderTriangleList(S32 typePoly, S32 colorPoly,
                                    S32 nbPoints, Struc_Point *ptrPoints);
void GpuRenderer_SetRenderQuality(U32 quality);

// Readback. Called after the exterior terrain pass to refill the software
// buffers the object pass reads: writes Screen (terrain colour) and PtrZBuffer
// (depth) from the offscreen target in one backend round trip. A no-op when no
// GPU backend is active; a backend without depth readback leaves PtrZBuffer
// untouched.
void GpuRenderer_ReadbackSceneBuffers(void);

#ifdef __cplusplus
}
#endif

#endif // LIB_GPURENDERER