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
    GPU_RENDERER_BACKEND_NONE = 0,    ///< Software rasterizer (always available)
    GPU_RENDERER_BACKEND_OPENGL = 1,  ///< OpenGL / GLES 2.0 (LIB386/GL)
    GPU_RENDERER_BACKEND_SDL3GPU = 2, ///< SDL3 GPU (LIB386/SDL3GPU)
    GPU_RENDERER_BACKEND_COUNT
} GpuRendererBackendId;

//──────────────────────────────────────────────────────────────────────────
// Lifecycle. Init creates the backend's context on the existing window and
// returns TRUE when the backend is live; the engine decides to keep using
// the software path when it fails. Shutdown tears the renderer down, leaving
// the window (and software path) intact.
S32 GpuRenderer_Init(GpuRendererBackendId backend, U32 resX, U32 resY);
void GpuRenderer_Shutdown(void);
void GpuRenderer_Resize(U32 resX, U32 resY);

// Runtime state queries.
S32 GpuRenderer_IsActive(void);
GpuRendererBackendId GpuRenderer_ActiveBackend(void);
const char *GpuRenderer_BackendName(GpuRendererBackendId id);

//──────────────────────────────────────────────────────────────────────────
// Frame control. BeginFrame must precede any of ClearFBO/Present; ClearFBO
// is called before the terrain pass (affGrille) so a frame with no terrain
// keeps the previous frame's 3D content.
void GpuRenderer_BeginFrame(void);
void GpuRenderer_ClearFBO(void);
// Palette index used as the offscreen clear colour (exterior FogCoul so sky
// gaps match the software ClsTerrainZBuf fill; 0 elsewhere). Applied on the
// next ClearFBO; the RGB is resolved from the live paletteLUT at that time.
void GpuRenderer_SetSceneClearIndex(U8 index);
// Interior vs exterior for this scene: set from CubeMode at the terrain pass
// (RefreshGrille, beside SetSceneClearIndex). Interior arms brick GEQUAL
// depth-write and forces body/NZW batches to no-depth (their TYPE_ISO ZO is
// untouched); exterior keeps the terrain-only scheme. Sticky across
// AFF_OBJETS frames; flushed if it changes mid-batch.
void GpuRenderer_SetSceneInterior(S32 interior);
// TRUE while the interior room lives in the FBO: backend active, GpuScene on,
// and the last SetSceneInterior mark was interior. Callers keep Log a
// transparent overlay under this rule (BoxClean clears rather than restores
// Screen) and rebuild the Screen re-blit plate AffGraph/CopyMask expect on
// AFF_ALL frames. Always false with the flag off or no backend.
S32 GpuRenderer_IsSceneInterior(void);
// Force subsequent Fill_Poly calls onto the software rasterizer while the
// backend stays active. Used for one-shot Screen-plate passes that must leave
// palette indices in Log (the inventory shade maps those indices through the
// CLUT; FBO colour cannot be mapped back). Save/restore around a plate pass;
// nested set is not supported. Resets on Init/Shutdown.
void GpuRenderer_SetSoftwarePolys(S32 enabled);
S32 GpuRenderer_GetSoftwarePolys(void);
void GpuRenderer_FlushBatch(void);
// Present the 2D overlay over the offscreen target and swap. pixels is either
// 0xAARRGGBB staging (indexed FALSE — console/touch may have drawn into it)
// or Log's raw palette indices (indexed TRUE, pitch = width): the R8 path
// skips the CPU Log→ARGB conversion and resolves colour in the present shader.
void GpuRenderer_Present(const void *pixels, U32 pitch, U32 width,
                         U32 height, S32 filterLinear, S32 indexed);
S32 GpuRenderer_RenderTriangleList(S32 typePoly, S32 colorPoly,
                                   S32 nbPoints, Struc_Point *ptrPoints);
void GpuRenderer_SetRenderQuality(U32 quality);

// Depth readback. Called after the exterior terrain pass so the object pass's
// ZBufBoxOverWrite2 tests against the terrain the GPU drew: fills PtrZBuffer
// from the offscreen depth target. A no-op when no GPU backend is active or
// the backend cannot read depth (the software Z-buffer then stays).
void GpuRenderer_ReadbackDepthBuffer(void);

// Colour readback of the offscreen target into a top-down 0x00RRGGBB buffer
// of size width x height (engine pixels). Flushes the pending batch first.
// Returns FALSE when no backend is active or the backend cannot read colour,
// so the caller can fall back to the software Log frame. Used by --screenshot
// so a GPU run captures the 3D the player sees, not only the Log overlay.
S32 GpuRenderer_ReadbackPresented(void *rgb, U32 width, U32 height);

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
// Interior room spans (GpuScene): same flat-black alpha as ShadowBegin,
// but no depth test — the interior has no terrain in the FBO to occlude
// against, matching the software ShadeBoxBlk path's plain darken.
void GpuRenderer_ShadowBeginFlat(S32 level);
void GpuRenderer_ShadowSpan(S32 x0, S32 y, S32 x1);
void GpuRenderer_ShadowEnd(void);

// Interior brick grid (GpuScene phase 1). SetBrickBank hands the cube's
// BufferBrick bank over (NULL on FreeGrille); DrawBrick submits one brick
// as a screen-space textured quad and returns TRUE when consumed — FALSE
// leaves the caller on the software AffGraph path (flag off, no backend,
// pack failure, out-of-range index). zo is the brick's iso view depth
// (GpuRenderer_IsoWorldZO) stamped on all six vertices; it is written even
// when the interior mark is off, so the vertex layout stays constant.
void GpuRenderer_SetBrickBank(const void *bank);
S32 GpuRenderer_DrawBrick(S32 brickIdx, S32 x, S32 y, U16 zo);

// Front-wall re-blit (DrawOverBrick*, GpuScene interiors): the very same
// brick quad the room pass drew, submitted again after the body so it paints
// over it, which is CopyMask's job without the Screen plate. The mask bank
// is redundant here — CalcGraphMsk derives it from the same RLE spans the
// atlas valid channel (G) encodes, so the fragment's G == 0 discard is
// already CopyMask's gate. Only consumes under the interior mark (the caller
// keeps CopyMask otherwise, and always when the flag is off); returns TRUE
// when consumed. Unlike DrawBrick it never publishes ScreenXMin/Max — the
// caller has just drawn a sprite and reads its rect back.
S32 GpuRenderer_DrawBrickOver(S32 brickIdx, S32 x, S32 y, U16 zo);

// Opaque world sprites (GpuScene phase 3). DrawSprite packs one sprite from
// bank into the sprite RG8 atlas (R = palette index, G = valid; transparent
// pixels and AffGraph RLE skips leave G = 0 so the shader discards) and
// submits a screen-space quad. rawFormat 0 is the AffGraph/SPRITES.HQR RLE
// layout; 1 is the ScaleSprite/SPRIRAW.HQR raw-pixel layout at 1:1 — scaled
// raw blits (factor != 65536) go through DrawScaleSprite instead. zo is
// stamped on all six vertices and only exterior depth-tests it (GET_ZO of
// the camera-relative clip-Z); interior takes no depth-test — AffGraph never
// Z-tests and DrawRecover3 owns front-wall occlusion. Returns TRUE when
// consumed — FALSE leaves the caller on the software AffGraph / ScaleSprite
// path (flag off, no backend, pack failure, out-of-range index). On success
// the clipped sprite rect in Log is punched to palette index 0 so the
// present composite shows the FBO quad through the overlay hole.
S32 GpuRenderer_DrawSprite(S32 spriteIdx, S32 x, S32 y, U16 zo,
                           const void *bank, S32 rawFormat);

// Scaled opaque world sprites (GpuScene phase 3): ScaleSprite's 16.16 blit
// with the quad depth-test. GpuBlend_Walk reproduces ScaleSprite's sampling
// and Screen* publication byte for byte (it mirrors the software walk line
// for line); the walked rect is packed into the sprite atlas (R = walked
// source index, G = index != 0), the clipped footprint in Log is punched
// where the walk wrote a non-zero texel — same hole Common_PunchSpriteLog
// opens — and a screen-space quad batches like DrawSprite's, so the depth
// rules are the batch's: exterior GEQUAL against terrain, interior none.
// factorx/factory are 16.16. Returns TRUE when consumed (Screen* published
// either way, sentinels included) — FALSE leaves the caller on the software
// ScaleSprite (flag off, no backend, allocation failure).
S32 GpuRenderer_DrawScaleSprite(S32 x, S32 y, S32 factorx, S32 factory,
                                U16 zo, const void *bank);

// Flow particles (GpuScene phase 3): one DOT_DISPLAY dot's 2x2 BoxFlow block
// as sprite-page quads. The dot's palette index packs as R with G = colour
// != 0, so a colour-0 dot discards in the mode-7 fragment and the FBO keeps
// what was behind it — BoxFlow's colour-0 write is a hole too. Each of the
// two rows is its own uniform-ZO quad: ZBufBoxOverWrite2 tests depth
// row-constant across the flow's bounding box, while the fragment shader
// interpolates depth across a quad, so a single quad cannot reproduce the
// software test. zoTop/zoBot are that interpolated z at the dot's two rows
// (GetRecoverDepth's z0/z1 through the ZBufBoxOverWrite2 IncZ math); the
// caller passes 0/0 where no depth-test runs (interior). BoxFlow publishes
// no Screen* (AffParticleFlow does that itself), so neither does this.
// Returns TRUE when consumed — including the fully-outside clip BoxFlow
// skips too — FALSE leaves the caller on BoxFlow (flag off, no backend, page
// exhausted). On success the clipped 2x2 Log footprint is punched to palette
// index 0 unconditionally: BoxFlow wrote all four texels even at colour 0.
S32 GpuRenderer_DrawFlowDot(S32 x, S32 y, U8 coul, U16 zoTop, U16 zoBot);

// Blend sprites (GpuScene phase 3): ScaleSpriteTransp's transpTable compose
// on the GPU. GpuBlend_Walk (GPUBLEND_WALK.H) reproduces the software
// walk's sampling and Screen* publication; the backend uploads the walk's
// source indices beside the live Log index, copies the scene colour rect
// (alpha byte = destination palette index) and draws a quad whose shader
// performs the table lookup per texel — chained sprites see the previous
// result as their destination, as composing into Log would. factorx/factory
// are 16.16; unlike DrawSprite the scaled walk is supported. The sprite
// footprint in Log is punched to palette index 0 so the present shows the
// FBO result through the overlay hole. Returns TRUE when consumed (Screen*
// published either way, sentinels included) — FALSE leaves the caller on
// ScaleSpriteTransp (flag off, no backend, no bank or table).
S32 GpuRenderer_DrawBlendSprite(S32 x, S32 y, S32 factorx, S32 factory,
                                const void *bank, const void *transPal);

// HUD/incrust text (GpuScene phase 3): one Font() glyph run as a single
// sprite-page quad. The string packs as R = ColMask with G = 1 exactly on
// the pixels AffMask would write — the cursor walk, glyph RLE, clip and
// Screen* publication mirror Font / AffMask / ClippingMask — the clipped
// footprint in Log is punched where G = 1, and the quad batches like
// DrawSprite's with ZO 0: exterior GEQUAL passes at the nearest depth and
// the sprite batch never depth-writes, interior takes no test, so a HUD
// overlay is never Z-occluded (Font draws over the finished scene). Two
// Font calls (shadow then colour) interleave like the software run because
// each call packs and submits in order. Returns TRUE when consumed — FALSE
// leaves the caller on software Font: flag off, no backend, no font, a
// pinned modal overlay rect (the present's index-0-as-opaque-black rule
// inside it would surface the punch as black), or page exhausted.
S32 GpuRenderer_DrawHudText(S32 x, S32 y, const char *str);

// Interior iso view depth. Map2Screen's ground axis is (x+z): larger is
// nearer on screen, so invert into aZ (smaller is nearer under GEQUAL with
// the depth clear at 0) and clamp the S16 grid sum to the 16-bit field.
// y is screen-vertical only and does not enter the depth. Pure and
// header-only so host tests can exercise the whole domain.
static inline U16 GpuRenderer_IsoWorldZO(S32 x, S32 y, S32 z) {
    S32 n = x + z;
    (void)y;
    if (n < 0) {
        n = 0;
    }
    if (n > 65535) {
        n = 65535;
    }
    return (U16)(65535 - n);
}

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

// TRUE while a modal rect is pinned (present treats Log's index 0 as opaque
// black inside it). Hooks that punch Log to expose an FBO quad ask this
// first: the punch would surface as black inside the pinned frame.
S32 GpuRenderer_IsOverlayOpaque(void);

#ifdef __cplusplus
}
#endif

#endif // LIB_GPURENDERER