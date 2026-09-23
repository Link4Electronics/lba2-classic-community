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

// Depth readback. Called after the exterior terrain pass so the object pass's
// ZBufBoxOverWrite2 tests against the terrain the GPU drew: fills PtrZBuffer
// from the offscreen depth target. A no-op when no GPU backend is active or
// the backend cannot read depth (the software Z-buffer then stays).
void GpuRenderer_ReadbackDepthBuffer(void);

// The engine's 3D pass. AffScene opens it around each exterior body draw
// (ObjectDisplay, and the dart body draw) so non-Z-buffered body polys batch
// into the offscreen target with the terrain GEQUAL test instead of the Log
// fallback: bodies then render at the target's render-scaled resolution and
// are depth-occluded by terrain directly. Open/close is a flag pair at the
// Fill_Poly seam (Fill_Flag_3DPass); every other ObjectDisplay caller —
// menus' spinning models, holomap, credits — never opens it and keeps the
// software Log path, as do interiors. No-op while no backend is active.
void GpuRenderer_Begin3DPass(void);
void GpuRenderer_End3DPass(void);

// 3D scene shadows. In the exterior pass the terrain lives in the offscreen
// target, not Log, so the software ShadeBoxBlk has no terrain pixels to
// darken and produces opaque black over the models. The engine instead hands
// the same per-scanline spans here; they are replayed as flat black quads
// alpha-blended over the target at shadow time, before the model, so painter's
// order keeps the shadow over the terrain and under the model. The alpha is
// the average luminance loss of the same CLUT row ShadeBoxBlk would use
// ((15 - level)*256 of the current gouraud block), not a fixed level/15, so
// both fill paths agree per palette and per fog level. The spans additionally
// depth-test (GEQUAL) against the FBO the terrain pass left behind, with each
// span quad at the shadow's ground-plane clip-Z: terrain nearer than the plane
// (a risen ridge or wall between camera and object) culls the shadow, mirroring
// the software DrawRecover occlusion. zPlane is that ground clip-Z in GET_ZO
// space, produced by ProjectShadowExt as the nearest (minimum, since smaller
// GET_ZO is nearer) of the shadow footprint's four corners, so the
// footprint's own ground never culls the shadow. The engine keeps
// ShadeBoxBlk for paths that draw terrain into Log
// (interior, menus). Begin flushes the pending batch so the shadow lands on
// what precedes it.
void GpuRenderer_ShadowBegin(S32 level, S32 zPlane);
void GpuRenderer_ShadowSpan(S32 x0, S32 y, S32 x1);
void GpuRenderer_ShadowEnd(void);

// Terrain persistence for static-camera (AFF_OBJETS) frames. A camera cut
// redraws only the flagged bodies, never the terrain; the offscreen target
// keeps the previous frame's 3D colour and depth so the scene does not wipe
// to black (its ClearFBO runs at the terrain pass only). But that also keeps
// each previous frame's shadow spans and NZW animated polys, which the object
// pass re-stamps over the same pixels until they smear into trails — BoxClean
// restores only the Log buffer, not the FBO. SnapshotTerrain copies the whole
// target at the end of the terrain pass into a persistent terrain target;
// RestoreTerrain repaints it at the head of each AFF_OBJETS exterior frame,
// before the flagged-body pass, so shadows and animated polys start over clean
// terrain. Depth stays the target's own across the gap — the NZW, shadow and
// body paths never write it — so the restore repaints colour only and the
// ground-plane tests keep working. The engine calls both gated on the backend
// being active and CubeMode == CUBE_EXTERIEUR; RestoreTerrain is a no-op until
// a snapshot was taken.
void GpuRenderer_SnapshotTerrain(void);
void GpuRenderer_RestoreTerrain(void);

// Present pacing. Records the wanted state and applies it to the active
// backend; called before a backend exists it is remembered and applied at
// Init, so a config value loaded early still reaches the device.
void GpuRenderer_SetVSync(S32 enabled);

// Full-2D overlay regions (modal UI, e.g. the inventory opened mid-game).
// During gameplay the present composite leaves Log's palette index 0
// transparent so the offscreen 3D target shows through the overlay's holes.
// A modal UI that paints index-0 backgrounds (its slot squares, the software
// present's opaque black) pins x0<=x1 / y0<=y1 to the modal's rectangle: the
// composite then treats index 0 inside it as opaque black, while outside it
// the frozen 3D scene still shows through. Pin (0,0,0,0) to restore the
// transparent rule when the modal exits.
void GpuRenderer_SetOverlayOpaqueRect(S32 x0, S32 y0, S32 x1, S32 y1);

#ifdef __cplusplus
}
#endif

#endif // LIB_GPURENDERER