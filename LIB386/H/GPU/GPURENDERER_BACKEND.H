//──────────────────────────────────────────────────────────────────────────
// GPU renderer: backend vtable
//──────────────────────────────────────────────────────────────────────────
// A backend implements the raw API calls of one GPU API (OpenGL/GLES 2.0 or
// SDL3 GPU). The common layer (LIB386/COMMON) owns all behaviour that does
// not need a specific API: polygon-type dispatch, vertex packing, batch
// flushes, frame composition and the readback post-process (Y-flip, SSAA
// downscale, aZ inverse). A backend is pure drawing: bind, set state, call.
//
// One contract governs every row callback — coordinates. All draw and read
// targets are addressed in "engine pixel" space:
//   - The offscreen target (colour + depth) is NxM where N/M are the
//     present surface size (ModeDesiredX/Y). When the backend renders at a
//     higher scale (SSAA) it scales internally; the common layer never
//     changes the viewport dimensions it addresses.
//   - DrawTriangles receives clip-space vertex data derived from engine
//     (XE, YE) pixel pairs plus per-vertex texture/light/depth words; it
//     maps them with the screenScale pair in GpuDrawParams (2/resX, 2/resY).
//   - TargetReadDepth returns the depth buffer in bottom-up row order,
//     normalised from whatever the native API produces (GL reads bottom-up,
//     SDL3 GPU top-down). It is 16-bit unsigned in the GL window-depth
//     convention; the common layer performs the Y-flip, the SSAA downscale
//     and the aZ inverse centrally. A backend that cannot read depth reports
//     FALSE and the common layer leaves the software Z-buffer.
//──────────────────────────────────────────────────────────────────────────
#ifndef LIB_GPURENDERER_BACKEND
#define LIB_GPURENDERER_BACKEND

//──────────────────────────────────────────────────────────────────────────
#include <SYSTEM/ADELINE_TYPES.H>

//──────────────────────────────────────────────────────────────────────────
#ifdef __cplusplus
extern "C" {
#endif

//──────────────────────────────────────────────────────────────────────────
// One polygon batch's render state, resolved from the polygon type by the
// common layer and consumed by the backend's draw program.
typedef struct GpuDrawParams {
    S32 polyMode;        ///< 0=textured(CLUT) 1=flat 2=gouraud 3=gouraud-table 4=texel-is-index 5=fog-smooth 6=scene-shadow 7=brick/sprite-quad
    S32 alphaMode;       ///< 0=opaque 1=TRANS(semi) 2=TRAME(stipple)
    float flatColor;     ///< palette index (modes 1/2/4), CLUT column (mode 3) or shadow alpha (mode 6)
    S32 bilinear;        ///< 1=smooth CLUT sampling (3D textured types)
    S32 perspCorrect;    ///< 1=perspective-correct UV/Z (types 16-24)
    S32 chromaKey;       ///< 1=discard palette index 0 (INCRUST types)
    float clutBaseRow;   ///< CLUT row offset captured at batch start
    float scaledFogNear; ///< Fill_ScaledFogNear (fog-smooth, mode 5)
    float fogRowScale;   ///< Fill_Fog_Factor / 65536 (fog-smooth, mode 5)
    S32 blend;           ///< 1=alpha blending (TRANS/TRAME)
    S32 depthTest;       ///< 0=off 1=GEQUAL (mirror of SW PtrZBuffer >= test)
    S32 depthWrite;      ///< 1=write the offscreen depth target
    // Mode 7 atlas page selector: 0 = brick RG8 (uBrickSize tracks the brick
    // texture), 1 = sprite RG8 (same shader path; the backend binds the sprite
    // texture and feeds its size as uBrickSize). Keeps sprites on polyMode 7
    // without a new fragment mode (and without an SDL3 SPIR-V layout change).
    S32 spriteAtlas;
    float screenScale[2]; ///< [2/resX, 2/resY] pixel -> clip
    // Software Fill_Poly clip window for this batch (engine pixels, inclusive).
    // The backends scissor to the same rect so GPU geometry never paints
    // outside the ClipX/ClipY window the software rasterizer would honour.
    S32 clipX0;
    S32 clipY0;
    S32 clipX1;
    S32 clipY1;
    // Mode 8 (blend sprite) lookup page sizes. blendLog is the packed rect
    // RG8 upload (R = walked source index, G = Log index) and blendDst the
    // same rect's target colour copy (A = destination index); both pages are
    // rect-sized with rect-local, engine-top-down rows, so the shader
    // divides local pixel coordinates by these sizes directly. Appended last
    // so the std140 block matches.
    float blendLogW;
    float blendLogH;
    float blendDstW;
    float blendDstH;
    // Mode-0 CLUT row override: >= 0 pins the gouraud row to this value
    // (the SW flat-textured fillers never look at Pt_Light); -1 keeps the
    // light-derived row. Appended last so the std140 block matches.
    float flatRow;
} GpuDrawParams;

//──────────────────────────────────────────────────────────────────────────
// Packed per-vertex payload the common layer builds for a batch. Matches the
// 14-byte stride the GL backend consumes: XE, YE, MapU, MapV, Light, ZO,
// RepMask (Pt_W and the realigned RepMask layout of Struc_Point are folded
// away by the packer).
#pragma pack(push, 1)
typedef struct GpuVertex {
    S16 XE;      ///< Screen coordinate X
    S16 YE;      ///< Screen coordinate Y
    U16 MapU;    ///< Texture coordinate U
    U16 MapV;    ///< Texture coordinate V
    U16 Light;   ///< Intensity (0.8.0)
    U16 ZO;      ///< Normalized Z (GET_ZO)
    U16 RepMask; ///< Texture repeat mask
} GpuVertex;
#pragma pack(pop)

//──────────────────────────────────────────────────────────────────────────
// Backend entry table. Every function pointer has the C ABI (backends are
// C++98 translation units that export an extern "C" table via the
// GpuRendererBackendGL / GpuRendererBackendSDL3GPU symbols below).
typedef struct GpuRendererBackend {
    const char *identifier;

    // Context lifecycle. CreateContext is called against an existing host
    // window (created in the backend's own present mode); it may create the
    // window surfaces and the draw context. Returns TRUE on success.
    S32 (*CreateContext)
    (U32 resX, U32 resY);
    void (*DestroyContext)(void);

    // The offscreen target (colour + depth). Created at context setup and
    // destroyed on teardown or on Resize (target size = present size).
    S32 (*TargetCreate)
    (U32 width, U32 height);
    void (*TargetDestroy)(void);
    S32 (*TargetIsValid)
    (void);
    // Bind the offscreen target and clear colour+depth. rgb is 0xRRGGBB —
    // the scene clear (exterior FogCoul) so sky gaps match software Cls.
    void (*TargetBindClear)(U32 rgb);
    U32 (*TargetWidth)
    (void);
    U32 (*TargetHeight)
    (void);
    // Depth readback (bottom-up, GL window-depth): 16-bit, or FALSE if
    // unreadable.
    S32 (*TargetReadDepth)
    (void *dst, U32 width, U32 height);
    // Colour readback of the offscreen target: U32 0x00RRGGBB, bottom-up
    // rows (GL's origin). width/height are the target's own size. FALSE if
    // the backend cannot read colour (the caller then falls back to Log).
    S32 (*TargetReadColor)
    (void *dst, U32 width, U32 height);

    // Terrain persistence (see GpuRenderer_SnapshotTerrain/RestoreTerrain).
    // The backend keeps a persistent "terrain target" (colour + depth) sized
    // like the offscreen target. Snapshot copies the offscreen target into it
    // (called at the end of the exterior terrain pass) and reports whether a
    // valid copy now exists; Restore copies the stored colour back over the
    // offscreen target (called at the head of each AFF_OBJETS exterior frame)
    // so the flagged-body pass starts from clean terrain instead of the
    // previous frame's shadow and NZW animated-poly paint. Restore never
    // touches the target's depth: nothing writes it between the two calls, so
    // it already is the terrain's.
    S32 (*TargetSnapshotTerrain)
    (void);
    void (*TargetRestoreTerrain)(void);

    // Present pacing. Called whenever the wanted vsync state changes; a
    // backend that has no pacing control may ignore it.
    void (*SetVSync)(S32 enabled);

    // Fixed-role textures owned by the backend.
    // Present: indexed FALSE uploads a 32-bit ARGB staging surface
    // (0xAARRGGBB); indexed TRUE uploads Log's raw palette indices (1 byte
    // per pixel, pitch in bytes) as an R8 texture — the present shader then
    // resolves colour through the palette LUT, which is what lets the engine
    // skip the CPU conversion. linear selects nearest vs bilinear on the
    // overlay (R8 bilinear is four index taps + four LUT lookups + mix, so it
    // matches hardware bilinear on the already-converted ARGB).
    // rectX/rectY/rectW/rectH bound the region of `pixels` that needs
    // uploading (a shrink of the per-frame cost the common layer computes
    // for indexed GpuScene frames; everything else sends the full surface).
    // A rect with zero width or height uploads nothing — and a backend that
    // (re)created its texture in this call uploads the full surface
    // regardless, because a fresh texture's contents are undefined.
    void (*TexUpdatePresent)(const void *pixels, U32 pitch, U32 width,
                             U32 height, S32 rectX, S32 rectY, S32 rectW,
                             S32 rectH, S32 linear, S32 indexed);
    void (*TexUpdateAtlas)(const void *lum);
    void (*TexUpdateCLUT)(const void *lum);
    void (*TexUpdatePalette)(const void *argb);
    // Brick atlas (GpuScene phase 1): RG8 page, R = palette index, G =
    // valid (0 = AffGraph RLE skip). Sized by width/height; the backend
    // creates or recreates its texture when either changes.
    void (*TexUpdateBricks)(const void *rg, U32 width, U32 height);
    // Sprite atlas (GpuScene phase 3): same RG8 layout as the brick page,
    // separate texture so a frame can hold both a room and its sprites.
    void (*TexUpdateSprites)(const void *rg, U32 width, U32 height);
    // Blend table (GpuScene phase 3): the 64 KB PtrTransPal page as R8,
    // row = source index, column = destination index. NULL entry = the
    // backend has no blend support (the common layer then declines mode 8).
    void (*TexUpdateTransPal)(const void *lum);
    // Blend source/Log page: full-screen RG8 (R = walked source index,
    // G = Log index), engine-top-down rows; the caller sends only the live
    // sprite rect through width/height at (0,0).
    void (*TexUpdateBlendLog)(const void *rg, U32 width, U32 height);
    // Copies the offscreen target's colour rect into the mode-8 destination
    // page (A = destination index) with engine-top-down rows, so a later
    // mode-8 draw samples the scene state exactly as the software blend
    // would have read Log. Returns FALSE if unavailable.
    S32 (*TargetCopyColorRect)
    (U32 x, U32 y, U32 width, U32 height);

    // Draw a batch of triangles (fan already expanded). The backend maps
    // params to its shader uniforms/state and draws immediately.
    void (*DrawTriangles)(const GpuVertex *verts, U32 vertexCount,
                          const GpuDrawParams *params);

    // Composite to the default framebuffer. which: 0 == offscreen colour
    // texture, 1 == present texture. linear selects the sample filter.
    // The overlay path also takes the modal full-2D UI rect (engine pixels,
    // x1<=x0 / y1<=y0 clears it): inside it index 0 / alpha 0 presents as
    // opaque black like the software present, outside it stays transparent.
    // The rect is a shader uniform so both the ARGB and R8 uploads honour it
    // without a CPU patch of the staging buffer.
    void (*ClearBackbuffer)(void);
    void (*DrawPresentQuad)(S32 which, S32 linear, S32 opaqueX0, S32 opaqueY0,
                            S32 opaqueX1, S32 opaqueY1);
} GpuRendererBackend;

//──────────────────────────────────────────────────────────────────────────
// The two built-in backends. Declared here (without the corresponding
// symbols in the binaries the engine links when hard dependencies are built
// out) so the common layer can build a fallback table.
extern const GpuRendererBackend GpuRendererBackendGL;
extern const GpuRendererBackend GpuRendererBackendSDL3GPU;

#ifdef __cplusplus
}
#endif

#endif // LIB_GPURENDERER_BACKEND