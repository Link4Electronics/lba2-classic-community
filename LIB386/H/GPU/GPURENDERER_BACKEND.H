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
    S32  polyMode;      ///< 0=textured(CLUT) 1=flat 2=gouraud 3=gouraud-table 4=texel-is-index 5=fog-smooth
    S32  alphaMode;     ///< 0=opaque 1=TRANS(semi) 2=TRAME(stipple)
    float flatColor;     ///< palette index (modes 1/2/4) or CLUT column (mode 3)
    S32  bilinear;      ///< 1=smooth CLUT sampling (3D textured types)
    S32  perspCorrect;  ///< 1=perspective-correct UV/Z (types 16-24)
    S32  chromaKey;     ///< 1=discard palette index 0 (INCRUST types)
    float clutBaseRow;   ///< CLUT row offset captured at batch start
    float scaledFogNear; ///< Fill_ScaledFogNear (fog-smooth, mode 5)
    float fogRowScale;   ///< Fill_Fog_Factor / 65536 (fog-smooth, mode 5)
    S32  blend;         ///< 1=alpha blending (TRANS/TRAME)
    S32  depthTest;     ///< 0=off 1=GEQUAL 2=ALWAYS (NZW coplanar polys)
    S32  depthWrite;    ///< 1=write the offscreen depth target
    float screenScale[2]; ///< [2/resX, 2/resY] pixel -> clip
} GpuDrawParams;

//──────────────────────────────────────────────────────────────────────────
// Packed per-vertex payload the common layer builds for a batch. Matches the
// 14-byte stride the GL backend consumes: XE, YE, MapU, MapV, Light, ZO,
// RepMask (Pt_W and the realigned RepMask layout of Struc_Point are folded
// away by the packer).
#pragma pack(push, 1)
typedef struct GpuVertex {
    S16 XE;     ///< Screen coordinate X
    S16 YE;     ///< Screen coordinate Y
    U16 MapU;   ///< Texture coordinate U
    U16 MapV;   ///< Texture coordinate V
    U16 Light;  ///< Intensity (0.8.0)
    U16 ZO;     ///< Normalized Z (GET_ZO)
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
    S32  (*CreateContext)(U32 resX, U32 resY);
    void (*DestroyContext)(void);

    // The offscreen target (colour + depth). Created at context setup and
    // destroyed on teardown or on Resize (target size = present size).
    S32  (*TargetCreate)(U32 width, U32 height);
    void (*TargetDestroy)(void);
    S32  (*TargetIsValid)(void);
    void (*TargetBindClear)(void);
    U32  (*TargetWidth)(void);
    U32  (*TargetHeight)(void);
    // Depth readback (bottom-up, GL window-depth): 16-bit, or FALSE if
    // unreadable.
    S32  (*TargetReadDepth)(void *dst, U32 width, U32 height);

    // Present pacing. Called whenever the wanted vsync state changes; a
    // backend that has no pacing control may ignore it.
    void (*SetVSync)(S32 enabled);

    // Fixed-role textures owned by the backend.
    // Present: 32-bit ARGB staging surface (0xAARRGGBB), pitch in bytes.
    // Atlas/CLUT: 256x256 single-channel LUMINANCE (palette indices).
    // Palette: 256x1 ARGB (paletteLUT).
    void (*TexUpdatePresent)(const void *argb, U32 pitch, U32 width,
                             U32 height, S32 linear);
    void (*TexUpdateAtlas)(const void *lum);
    void (*TexUpdateCLUT)(const void *lum);
    void (*TexUpdatePalette)(const void *argb);

    // Draw a batch of triangles (fan already expanded). The backend maps
    // params to its shader uniforms/state and draws immediately.
    void (*DrawTriangles)(const GpuVertex *verts, U32 vertexCount,
                          const GpuDrawParams *params);

    // Composite to the default framebuffer. which: 0 == offscreen colour
    // texture, 1 == present texture. linear selects the sample filter.
    void (*ClearBackbuffer)(void);
    void (*DrawPresentQuad)(S32 which, S32 linear);
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