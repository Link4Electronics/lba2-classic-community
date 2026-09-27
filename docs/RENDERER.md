# GPU renderer

The hardware-accelerated render path for the 3D scene and the final composite.
It is an option under construction; the engine still draws with the software
renderer by default, and the GPU path is switched in by the player from the
Display menu → Renderer (or the `Renderer` lba2.cfg key; see Status below).

## Where the behavior lives

`../LIB386/COMMON/GPURENDERER_COMMON.CPP` is the renderer's engine: it owns
every decision that is not drawing. It resolves each polygon type into a
`GpuDrawParams`, batches the packed 14-byte vertices, decides when to flush
(z-buffer fill surfaces, CLUT/fog steps, offscreen-copy and high-Z passes,
frame end), reimplements the software fill fallbacks for the batches that
cannot take the GL path, uploads the textures on the same schedule the
reference GL driver did, and runs the present sequence and the readback
post-process (depth → `aZ` inverse, Y-flip, SSAA downsample, ARGB →
palette-index). The GL backend below appended exactly this work to its draw
loop; here it is explicit and testable.

A backend is a thin wire: `GpuRendererBackendGL` and
`GpuRendererBackendSDL3GPU` (20 operations each) implement the vtable in
`../LIB386/H/GPU/GPURENDERER_BACKEND.H`. A backend binds its target, uploads
the batch, sets the uniforms from the already-resolved `GpuDrawParams`, draws,
and composites the two framebuffers onto the window. It decides nothing.

## Backends

The shaders live at the backend that needs them. The SDL3 GPU backend
cross-compiles the shared sources in `../LIB386/COMMON/*.glsl`, which mark
per-API blocks with `##ifdef GL_ES` / `##ifdef GL_CORE` / `##ifdef SDL3GPU` /
`##else` / `##endif` (double-hash so no shader compiler ever sees them).

- **OpenGL / GLES 2.0** (`../LIB386/GL/RENDER_GL.CPP`) is functional. It
  carries its shaders inline and picks the dialect at build time: `USE_GLES2`
  (defined by CMake for Android) selects the `#version 100` GLES header set,
  desktop keeps the `#version 110` GL 2.1 one. The source itself names no
  platform, so desktop and Android share the draw code.
- **SDL3 GPU** (`../LIB386/SDL3GPU/`) filters the shared sources and
  cross-compiles them to SPIR-V and (with shadercross/spirv-cross + dxc)
  MSL/DXIL at build time; a device selects its format at runtime and builds
  the CLUT and present pipelines from it. It runs the same draw path as GL
  with two API differences: SDL3 GPU has no 16-bit integer vertex formats, so
  the 14-byte packed `GpuVertex` is repacked to the float layout the shared
  shaders declare, and it has no separate swap call, so one command buffer per
  host frame carries the target draws, the texture uploads and the swapchain
  composite, submitting when the overlay quad is drawn. The frame's vertices
  accumulate in one arena and are replayed in a single render pass per texture
  change, so a frame does not reallocate its vertex buffer per batch. It is
  harmless without the translators, and fails at configure time on Apple, where
  Metal is the only driver.

## Build

`-DGPURENDERER=ON` (default) builds the three archives (`gpur`, `gly`,
`gs3`) and links them into the engine. The engine target compiles with
`USE_GPURENDERER`; a `GPURENDERER=OFF` build has no GPU references at all.

## How the engine plugs in

The game never calls the renderer directly; three seams keep the software
path untouched and the ASM equivalence tests free of GPU dependencies:

- **Polygon intercept** — `Fill_Poly` (`../LIB386/pol_work/POLY.CPP`) calls an
  optional hook (`Fill_Poly_SetHook`, default NULL) with the raw polygon
  arguments. `GpuRenderer_Init` registers `GpuRenderer_RenderTriangleList`, so
  every polygon the software rasterizer would draw is offered to the GPU path.
  A non-zero return means the hook consumed the polygon and the software fill
  does not run for it; zero leaves the polygon to the software path. The
  covered types are consumed; outside the engine's 3D pass (menus, holomap,
  NZW before the first terrain pass) body polys still fall back, so the
  software Log supplies what the GPU declines.
- **3D pass** — while `AffScene` (`../SOURCES/OBJECT.CPP`) displays an
  exterior body — or, with `GpuScene` on, an interior body — it opens
  `GpuRenderer_Begin3DPass` / `_End3DPass` around `ObjectDisplay` (and the
  dart body draw). While open,   `GpuRenderer_RenderTriangleList` accepts
  non-Z-buffered body polys instead of declining them: they batch into the
  offscreen target with the rules types 0-15 already resolve to — `GEQUAL`
  depth-test against the terrain (exterior) or, interior under `GpuScene`,
  no depth at all (`GpuRenderer_SetSceneInterior` forces depth-test/write
  off because `TYPE_ISO` leaves `Pt_ZO` alone and would otherwise
  GEQUAL-test garbage against brick-written depth; front-wall re-blits still
  land in the Log), no depth-write, painter order from the software sort — so
  bodies render at the target's render-scaled resolution. The intake gate
  keys on FBO content: `s_fboHasTerrain` (exterior terrain depth, armed by
  `GpuRenderer_ClearFBO`) or its interior sibling `s_fboHasRoom` (set when
  `GpuRenderer_DrawBrick` enqueues a room quad). `GetZO3`/`GetZO4`
  (`../LIB386/OBJECT/AFF_OBJ.CPP`) fill the body clip-Z while the pass is
  open (`Fill_Flag_3DPass`, set only by the renderer, so equivalence tests
  and every other `ObjectDisplay` caller — menus' spinning models, holomap,
  credits — keep the software Log path).
- **Present seam** — `PresentFrame` (`../LIB386/SVGA/SDL.CPP`) hands its Log
  overlay to the renderer (`SetGpuPresentHooks`, default NULL) instead of the
  SDL streaming texture while a backend is active. With a GPU backend the
  hand-off is R8 palette indices (`GpuRenderer_Present(indexed=TRUE)`): the
  present shader resolves colour through the LUT, so the CPU no longer converts
  Log to ARGB. The overlay is staged into a present-owned buffer
  (`STAGING_BUFFER.CPP`) before the hand-off, because a GPU window is recreated
  without the SDL renderer/streaming texture (see the switch below), so the
  lock-texture path could not produce a frame there. `GL_GetPaletteLUT`
  exports the same 256-entry ARGB table so GPU-drawn content and Log always
  agree.
- **Atlas signal** — `AtlasTextureDirty` is raised by `DoTextureAnimation`
  (`../SOURCES/ANIMTEX.CPP`) whenever animated texture pages are baked in
  place, so the renderer re-uploads the atlas page it mirrored.
- **Depth readback** — `AffScene` (`../SOURCES/OBJECT.CPP`, `AFF_ALL`) calls
  `GpuRenderer_ReadbackDepthBuffer` after the exterior terrain pass to refill
  `PtrZBuffer` from the offscreen target. Content that stays in `Log` —
  sprites, 2D effects — still re-applies terrain occlusion with
  `ZBufBoxOverWrite2`
  (`../SOURCES/3DEXT/BOXZBUF.CPP`), which reads that depth; without the readback
  it holds nothing from the GPU-drawn terrain. Where terrain is in front,
  `ZBufBoxOverWrite2` writes colour 0, which the present composite blends
  transparent over the same target's terrain, so no colour readback is needed. (With `GpuScene`, world sprites and exterior flow particles are in the offscreen target and test its depth directly, so they drop out of this consumer list; `LineRain` still reads the readback depth directly.)
  Bodies in the 3D pass skip that step: the GPU `GEQUAL` test against the
  target's own depth does their terrain occlusion directly.
  The call is gated on `CubeMode == CUBE_EXTERIEUR`: interior cubes reuse
  `PtrZBuffer`'s backing store for `BufCube`/`BufferBrick` and draw through
  `DrawOverBrick`. OpenGL reads depth directly; SDL3 GPU downloads its
  `D16_UNORM` depth texture, reverses its top-down rows to the bottom-up order
  the common layer expects, and converts the depth to the GL window-depth
  convention the common aZ inverse expects.
- **Exterior Log clear** — software `ClsTerrainZBuf`
  (`../SOURCES/3DEXT/TERRAIN.CPP`) fills `Log` with `FogCoul` so sky gaps show
  the fog colour. With a GPU backend active the terrain lives in the offscreen
  target, and only palette index 0 is transparent in the present composite:
  a `FogCoul` fill of `Log` would paint an opaque overlay over that target and
  hide the 3D scene. The same routine therefore clears `Log` to index 0 while a
  backend is live (unless `GpuRenderer_GetSoftwarePolys()` is set — a
  one-shot Screen-plate pass, which takes the `FogCoul` fill so the plate starts
  from the fog colour, not zeros), so the composite shows the GPU frame (HUD
  and sprites drawn into `Log` afterward still paint over it). The offscreen
  target itself is cleared to the scene `FogCoul` RGB
  (`GpuRenderer_SetSceneClearIndex` from `RefreshGrille`,
  `../SOURCES/INTEXT.CPP`) so pixels the GPU never covers — outside the drawn
  horizon, above the sky strip — match the software fill rather than black.
  Interior passes palette index 0, the software default.
- **Software-plate pass** — `GpuRenderer_SetSoftwarePolys(enabled)`
  (`../LIB386/H/GPU/GPURENDERER.H`) forces `Fill_Poly`'s hook to yield so a
  one-shot pass rasterizes into Log palette indices while the backend stays
  active (resets on Init/Shutdown). Used by the exterior inventory plate
  (`AffGrilleExt_BuildScreenPlate`, `../SOURCES/EXTFUNC.CPP`): after exterior
  `AFF_ALL` ends with `CopyScreen(Log, Screen)` while Log was cleared for the
  FBO composite, Screen holds zeros and `OpenInventory`'s
  `CopyScreen(Screen, Log)` would feed `ShadeBoxBlk` empty indices — the wheel
  would darken to a flat shade instead of the scene. The plate rebuilds Screen
  once (gated on backend active, exterior cube, not the cleanroom
  `--black-bg` golden), clears Log for the present composite, then the modal
  shades the real plate. The interior room plate
  (`AffGrille_BuildScreenPlate`, `../SOURCES/GRILLE.CPP`) needs no yield
  of its own — `s_swPlateBricks` forces AffGraph, which never enters
  `Fill_Poly` — and neither plate is built per frame any more: the
  inventory wheel and the behaviour (CTRL) modal (`MenuComportement`,
  `../SOURCES/COMPORTE.CPP`) each rebuild into `Screen` on open
  (exterior terrain plate, interior room plate under
  `GpuRenderer_IsSceneInterior()`, both skipped for the cleanroom
  `--black-bg` golden) then `CopyScreen(Screen, Log)`, so
  `BackupScreen(TRUE)` cannot paste zeros over the plate before the shade
  runs.
- **Clip-window scissor** — software `Fill_Poly` rasterizes only inside
  `ClipXMin..ClipYMax` (cinema bars, dialogue windows, `AFF_ALL_FLIP`
  unsetting the clip window). The GPU intake runs before that screen-space
  clip, so each batch carries the live window in `GpuDrawParams` and the
  backends scissor to the same rect (GL bottom-left Y-flip; SDL3 top-left),
  scaled through `screenScale` into target pixels.
- **Terrain/room snapshot/restore** — a camera cut (`AFF_OBJETS` in
  `AffScene`, `../SOURCES/OBJECT.CPP`) redraws only the flagged bodies and
  never the terrain (or, with `GpuScene`, the room); the offscreen target
  keeps the previous frame's colour and depth so the scene does not wipe to
  black, but that also keeps the previous frame's shadow spans, NZW animated
  polys and body paint, which re-stamp the same pixels into trails —
  `BoxClean` restores only the `Log` buffer. At the end of the exterior
  terrain pass — or, with `GpuScene`, the interior room pass —
  `GpuRenderer_SnapshotTerrain` copies the whole target into a persistent
  terrain target; at the head of each `AFF_OBJETS` frame under the same gate
  `GpuRenderer_RestoreTerrain` repaints it, before `DrawAnimatedPolys`
  (exterior: so that frame's animated polys land over the scrubbed terrain,
  mirroring software `BoxClean` + `DrawAnimatedPolys`; interior: no
  `DrawAnimatedPolys` — that path is exterior NZW), erasing the trails
  before the flagged-body pass draws. Depth is never written between the two
  calls — the NZW, shadow and body paths have depth-write off — so the
  restore repaints colour only and the `GEQUAL` depth tests keep working.
  Both are gated on the backend being active, and on
  `CubeMode == CUBE_EXTERIEUR` or (`GpuScene` and `CubeMode ==
  CUBE_INTERIEUR`). No `ReadbackDepthBuffer` runs for the interior: its
  depth stays on the CPU in `PtrZBuffer`. GL stores the copy with
  `glCopyTexSubImage2D` (colour + depth textures); SDL3 GPU replays it with
  texture-to-texture copies in one copy pass (`SDL_CopyGPUTextureToTexture`).
- **Scene shadows** — the software path darkens `Log` through `ShadeBoxBlk`
  (`../SOURCES/FLOW_A.ASM`), but the exterior terrain is in the offscreen
  target, not `Log`, so there is nothing under the shadow to darken: index-0
  pixels would map to an opaque colour and the shadow would paint over the
  model. `DrawShadow` (`../SOURCES/BEZIER.CPP`) therefore hands its
  per-scanline spans to `GpuRenderer_ShadowBegin` / `_ShadowSpan` / `_ShadowEnd`
  when a backend is active and `CubeMode == CUBE_EXTERIEUR`; the common layer
  replays them as flat-black quads alpha-blended over the target, drawn at
  shadow time before the model, so painter's order keeps the shadow over the
  terrain and under the model. The alpha is the average luminance loss of the
  same CLUT row `ShadeBoxBlk` would use (`(15 - level)*256` of the current
  gouraud block, matched through the palette LUT) rather than a fixed
  `level/15`, so the two paths agree per palette and per fog level. Each span
  quad carries the shadow's ground-plane clip-Z, taken by `ProjectShadowExt`
  as the nearest of the four footprint corners — the minimum in `GET_ZO`
  space, where smaller is nearer, in the same space the terrain tiles
  carry — and depth-tests `GEQUAL/GREATER_OR_EQUAL` against the
  `D16` target the terrain pass left behind: terrain nearer than the nearest
  corner — a risen ridge or wall between camera and object — culls the shadow,
  mirroring the software `DrawRecover` occlusion, while the footprint's own
  ground never fails the test, so the shadow paints whole; depth-write stays
  off so later blends survive. Interior cubes and menus keep `ShadeBoxBlk`,
  since their terrain is drawn into `Log`. With `GpuScene` on, interiors
  split the darken: the room lives in the offscreen target, so
  `DrawShadow` (`../SOURCES/BEZIER.CPP`) opens `GpuRenderer_ShadowBeginFlat`
  (same CLUT-derived alpha, depth-test off — the interior FBO has no
  terrain depth to test) for the FBO spans, while `ShadeBoxBlkOverlay`
  (`../SOURCES/FLOW_A.CPP`) keeps darkening non-zero actor/HUD pixels
  still sitting in the Log overlay; index 0 is the overlay's transparent
  hole and is left alone so the FBO shows through (plain `ShadeBoxBlk`
  would map 0 through `clut[0]`, an opaque shade colour, painting the
  shadow span as solid holes over the room and body). Each buffer
  receives the darken its own pixels received when they shared one
  buffer.

- **`GpuScene` and the interior brick grid.** cfg key `GpuScene`, cvar
  `gpuscene`, default 0. With the flag on and a backend active, the base
  brick blits in `AffBrickBlock` / `AffBrickBlockOnly` go through
  `GpuRenderer_DrawBrick`: COMMON shelf-packs the cube's `BufferBrick`
  bank into one RG8 page (R = palette index, G = valid; AffGraph RLE skips
  leave G = 0 so the shader discards and the FBO keeps what was there),
  batches quads as polyMode 7 (nearest, no light), and each
  backend samples that page through `uAtlas` when polyMode is 7
  (`TexUpdateBricks` on the vtable; GL uses LUMINANCE_ALPHA, SDL3 GPU
  uses R8G8_UNORM). Each quad carries `GpuRenderer_IsoWorldZO(x,y,z)` —
  the fixed iso camera's `x+z` depth axis inverted into aZ — on all six
  vertices; while `GpuRenderer_SetSceneInterior` marks the cube interior
  (set from `CubeMode` in `RefreshGrille` beside the scene clear index),
  those quads depth-test `GEQUAL` and depth-write, so the nearer brick
  wins regardless of grid draw order and later sprites can test against
  the nearest room surface. Interior body/NZW batches under the same
  mark force depth-test and depth-write off (`TYPE_ISO` leaves `Pt_ZO`
  untouched; the front-wall re-blit remains the only body occlusion,
  submitted as a no-depth quad by `GpuRenderer_DrawBrickOver`).
  Exterior keeps the terrain-only depth scheme and never draws brick
  quads. `InitGrille` calls `GpuRenderer_SetBrickBank` after
  `LoadUsedBrick`; `FreeGrille` passes NULL. Successful enqueues arm
  `s_fboHasRoom` (cleared by `GpuRenderer_ClearFBO` and the lifecycle
  resets), the interior half of the body/NZW intake gate. With the flag
  off, or on the software path, every hook falls through to `AffGraph`
  unchanged. With the flag on, interior bodies and the 3D pass join the
  FBO (see 3D pass above); `DrawOverBrick`'s front-wall re-blits join them
  as no-depth brick quads (`GpuRenderer_DrawBrickOver`), while `CopyMask`
  — the fallback whenever the backend declines — plus Z-masks and HUD stay
  in the Log.

  Two Log/Screen rules keep that split correct. The re-blit no longer
  needs a plate: `CopyMask` copies wall pixels **from `Screen`** into
  `Log` over the body, and with the flag on `Cls` leaves `Log` empty while
  `RefreshGrille` paints only the FBO, so `AFF_ALL` used to rebuild a
  software plate every frame
  ([`AffGrille_BuildScreenPlate`](../SOURCES/GRILLE.CPP), one forced
  `AffGraph` pass under `GpuRenderer_IsSceneInterior()`). `DrawOverBrick`,
  `DrawOverBrick3` and `DrawOverBrickCage` now call
  [`GpuRenderer_DrawBrickOver`](../LIB386/COMMON/GPURENDERER_COMMON.CPP),
  which re-submits the brick's own quad —
  batched as `GPU_BRICKOVER_TYPE`: no depth test or write, so it lands
  over the body in batch order exactly as the mask-gated copy did — and
  returns FALSE, leaving `CopyMask` in charge, when the flag is off, no
  backend is live, or the pack fails. The mask bank is redundant for the
  quad: `CalcGraphMsk` derives it from the brick's own RLE spans, which
  the atlas valid channel already encodes in G, so the fragment's
  `G == 0` discard is the same gate. With the re-blit off `Screen`,
  `AFF_ALL` mirrors the empty `Log` into `Screen` (`CopyScreen(Log,
  Screen)`, the exterior contract), and the two 2D modals that shade
  through `Screen` rebuild a plate on demand: `AffGrille_BuildScreenPlate`
  from the inventory wheel and the behaviour modal, each gated on
  `GpuRenderer_IsSceneInterior()`. And `BoxClean` / `DefaultBoxOneClean`
  restores `Screen`→`Log` by default, which would paint that plate over
  FBO bodies in the dirty regions — `AffScene` sets `BoxOneClean` to
  `DefaultBoxOneClear` (clear to 0) under the same interior rule, back to
  the default otherwise, so mid-dialog `BoxClean` callers inherit it. Both
  rules are dead with the flag off.

- **`GpuScene` and opaque world sprites.** With the flag on, the three
  world-sprite sites in `AffOneObject` (`../SOURCES/OBJECT.CPP` —
  `TYPE_OBJ_SPRITE`, `TYPE_OBJ_ANIM_3DS`, and the opaque branch of
  `TYPE_EXTRA`) try `GpuRenderer_DrawSprite` first: COMMON shelf-packs
  that HQR entry into a separate RG8 page (same layout as the brick atlas;
  raw sprites encode color 0 as G = 0) and submits a polyMode-7 quad with
  `spriteAtlas = 1`, so the backend binds the sprite texture through
  `TexUpdateSprites`. Interior quads take no depth-test/write — software
  `AffGraph` never Z-tests, iso floor tiles under a standing sprite would
  fail `GEQUAL` against brick depth, and front-wall occlusion is already
  `DrawRecover3`'s re-blit into Log. Exterior quads depth-test `GEQUAL`
  with no depth-write against terrain depth (re-rotate the world point and
  stamp `GET_ZO(CameraZr - Z0)`). On success the clipped
  opaque footprint in Log is punched to palette index 0 so the present
  composite shows the FBO quad; interior still runs `DrawRecover3` for the
  front-wall re-blit. Scaled raw draws — `ScaleFactorSprite != 65536`, which
  is what `CalculeScaleFactorSprite` (`../SOURCES/EXTFUNC.CPP`) hands every
  exterior raw world sprite and most opaque `TYPE_EXTRA` sprites — take
  `GpuRenderer_DrawScaleSprite` instead of declining: `GpuBlend_Walk`
  reproduces `ScaleSprite`'s 16.16 sampling and `Screen*` publication into a
  scratch buffer (the walk is the software walk's line-for-line mirror,
  host-tested byte for byte against `ScaleSpriteTransp` in
  `tests/gpu_blend_walk`, which is ASM-anchored; `ScaleSprite` itself is
  ASM-anchored by `tests/SVGA/test_scalespi`), the walked rect is
  shelf-packed into the sprite page (R = walked source index, G = index != 0),
  and the quad batches exactly like `DrawSprite`'s — same interior/exterior
  depth rules — so terrain occlusion is the FBO `GEQUAL` test rather than a
  `ZBufBoxOverWrite2` pass over the Log footprint, which is punched only
  where the walk wrote a non-zero texel. HUD sites stay software; the
  `EXTRA_TRANSPARENT` branch takes the GPU path below.

- **`GpuScene` and blend sprites.** The `EXTRA_TRANSPARENT` branch of
  `AffOneObject` (`../SOURCES/OBJECT.CPP`) tries `GpuRenderer_DrawBlendSprite`
  first and falls back to software with `Log` untouched when it declines
  (no backend, no walk, or a failed scene copy). `GpuBlend_Walk` — host-tested
  byte-for-byte against the software walk (`tests/gpu_blend_walk`) —
  reproduces the source indices `ScaleSpriteTransp` would read, and the pack
  captures each rect texel's live `Log` index beside it. The backend uploads
  a rect-sized source/Log page (GL: `GL_LUMINANCE_ALPHA`, with unpack
  alignment forced to 1 so odd rect widths keep their rows; SDL3 GPU:
  tightly packed `R8G8`), the 256×256 `PtrTransPal` table, and a rect copy of
  the scene colour whose alpha byte is the destination palette index; only
  after that copy succeeds is the rect's `Log` footprint punched to index 0
  where the sprite writes, so the present shows the FBO result through the
  same overlay hole `Common_PunchSpriteLog` opens for opaque sprites. The
  mode-8 fragment (`clut.frag.glsl`) discards source index 0, lets the `Log`
  byte win over the scene alpha where the software path reads `Log`, looks
  the table up as row = source / column = destination, and writes the result
  index into the colour's alpha so the next blend sprite composes on top of
  it. Draws replace colour and alpha outright (`blend = 0`) with no
  depth-test or depth-write: software composes into `Log` unconditionally,
  and `DrawRecover3` never follows a blend sprite (the sentinel bounds leave
  the clip window empty after the caller's `SetClip`). The GL clip scissor
  floors its low edge and ceils its high edge when it maps an engine clip
  window through `screenScale`: a truncated edge dropped the top device row
  of any window whose mapped height landed off-integer, which is every blend
  rect — the topmost row of every blend sprite went undrawn until the
  rounding was fixed.

- **`GpuScene` and flow particles.** With the flag on and a backend live,
  `AffParticleFlow` (`../SOURCES/FLOW.CPP`) defers its exterior dots: each
  projected dot collects its screen position and colour, and after the loop
  the batch draws them one `GpuRenderer_DrawFlowDot` at a time. The depth
  each dot faces is `ZBufBoxOverWrite2`'s row-constant test over the flow's
  bounding box — `GetRecoverDepth` (the z0/z1 prelude `DrawRecover` already
  had, now its own function in `../SOURCES/INTEXT.CPP`) expanded with the
  software sweep's `Fill_ZBuffer_Factor`, `IncZ` across the published
  `ScreenYMin..ScreenYMax` bounds and `>>16` — so every dot row carries its
  own `zoTop`/`zoBot` and the dot is submitted as two row quads: a single
  quad would interpolate depth diagonally where the software test is
  constant per scanline row, and the two disagree at the box edge. Each dot
  shelf-packs a 2×2 texel pair into the sprite page (R = palette index,
  G = index != 0, so colour 0 still punches as a hole), batches as an
  opaque sprite (exterior `GEQUAL` against terrain depth, no depth-write)
  and punches the same 2×2 `Log` footprint `BoxFlow` would, so the present
  shows the FBO dot through the overlay hole. The object's `DrawRecover`
  box still runs — its `BoxMovingAdd` dirty-box contract does not depend on
  the dots. A dot `BoxFlow`'s clip would have rejected is consumed at that
  same test; an atlas shelf with no room falls back to software `BoxFlow`.
  Interior cubes under the flag draw immediately (`GpuRenderer_DrawFlowDot`
  with `zo` 0, which passes every `GEQUAL` test, same fallback), and with
  the flag off the hook declines so the dot takes `BoxFlow` exactly as
  before.

- **`GpuScene` and HUD text.** With the flag on and a backend live, the
  `AffScene` incrust loop's text sites (`INCRUST_NUM`, `INCRUST_TEXT`'s
  shadow + colour, `INCRUST_SYS_TEXT`, `INCRUST_CMPT` in
  `../SOURCES/OBJECT.CPP`) and `DrawBulle` (`../SOURCES/INCRUST.CPP`) try
  `TryGpuIncrustText` first and fall back to `Font` when it returns 0;
  `GpuRenderer_DrawHudText` (`../LIB386/COMMON/GPURENDERER_COMMON.CPP`)
  mirrors `Font` / `AffMask` / `ClippingMask` — a cursor walk for ink
  bounds and the `Screen*` publication a visible glyph leaves behind (a
  fully outside glyph leaves the previous publication, spaces advance by
  `InterSpace`), then each glyph's RLE decoded into one shelf-reserved
  sprite page (R = `ColMask`, G = 1 on written texels; the slot is zeroed
  first so a stale texel can never punch) and batched as an opaque sprite
  at `ZO` 0: exterior `GEQUAL` passes at the nearest depth with no
  depth-write, interior takes no test, so terrain and room never
  Z-occlude a HUD glyph. Only G = 1 texels inside clip ∩ bounds punch the
  `Log`, so the present shows the FBO glyph through the same hole
  software left. It returns 1 when consumed — spaces-only and fully
  clipped strings count, where `Font` would have drawn nothing — and 0
  otherwise (an atlas shelf with no room is one such 0), which is the
  fallback signal. The `INCRUST_SPRITE` `PtrAffGraph` site and `DrawBulle`
  take `TryGpuIncrustSprite` the same way: `sprite >= 100` reads the
  goodies bank (`HQRPtrSprite`, AffGraph RLE) and anything else the raw
  bank, exactly as `PtrAffGraph` splits them, with
  `ScaleFactorSprite != DEF_SCALE_FACTOR` routing through
  `GpuRenderer_DrawScaleSprite`. Both helpers decline with the flag off,
  with no backend live, or while a modal overlay rect is pinned
  (`GpuRenderer_IsOverlayOpaque` mirrors the present shader's
  `uOpaqueRect` test — the index-0-as-opaque-black rule inside the pinned
  rect would surface the punch as black instead of the glyph). Text that
  never passes through the incrust loop — the game menu's rows, the
  dialogue, the pause menu, the inventory and behaviour plates — stays in
  the Log.

Init (`GpuRenderer_Init` after `InitGraphics` in `../SOURCES/INITADEL.C`,
through `Renderer_InitBootBackend` in `../SOURCES/RENDER_SWITCH.CPP`) and
shutdown (`atexit`) are wired; init failure falls back to software with a
warning. Because the SDL window must be created *as* an OpenGL window for a
GL context to attach (SDL has no add-the-flag-after-creation), the boot
choice is made before `InitGraphics` and forwarded through `CreateWindowSurface`.
The SDL3 GPU backend is the mirror case: its device refuses the window the
software path built (it carries an `SDL_Renderer`), so the boot helper recreates
that window plain — `Window_RecreateWindowForGPU` — before claiming it.
A runtime switch (Display menu → Renderer, `../SOURCES/RENDER_SWITCH.CPP`)
tears down the backend, recreates the window for the target
(`Window_RecreateWindowForGL` / `...ForSW` / `...ForGPU` in
`../LIB386/SYSTEM/WINDOW.CPP`) and re-inits, falling back to software on any
failure — the same fallback path a boot-time failure uses.

## Status

Engine wiring is in place and both GPU backends render. The GPU draws the
covered 3D polygons into its offscreen target and composites it under the
software Log overlay, which supplies the 2D HUD, the sprites and the polygons
the GPU path declines. The present path is a player choice from the Display
menu → Renderer (Software / OpenGL / SDL3 GPU), persisted to the lba2.cfg
`Renderer` key and honoured at boot; the `LBA2_GPU_RENDERER=opengl|sdl3gpu`
env is kept as a one-run dev override. Absent or unknown values keep the
software rasterizer, which is still the shipped default.

The Display submenu also carries the two GPU-renderer options. **Bilinear**
(drives `GpuTexFilter` on a GPU backend, the software `TextureFilter`'s
bilinear 4-tap setting on the software rasterizer) toggles both the CLUT/overlay
sampling in the 3D pass and the present filter for the Log layer — bodies,
sprites and HUD — wherever the present scales it. **Quality** (lba2.cfg
`RenderQuality`, cvar `quality`) cycles the render scale 1x→2x→4x:
`GpuRenderer_SetRenderQuality` (`../LIB386/GPU/GPURENDERER_COMMON.CPP`)
rebuilds the offscreen target at the new size. The tier scales only what the
GPU draws: body polygons decline the intake (`GpuRenderer_RenderTriangleList`
returns 0 while `Fill_Flag_ZBuffer` is clear) and rasterize into the software
`Log`, so actors keep native resolution regardless of the tier — the Log layer
follows the Bilinear present filter instead. The row only exists while a GPU
backend is live — the software rasterizer has no scale to tier. The boot path
read (`Renderer_LoadBootQuality`, `../SOURCES/RENDER_SWITCH.CPP`) folds a
stored 3 into 4, and a runtime backend switch replays the stored quality,
because `GpuRenderer_Init` starts a fresh backend at native scale.

The frame boundary is owned by the renderer: `GpuRenderer_BeginFrame` runs at
the top of `AffScene` (`../SOURCES/OBJECT.CPP`) and `GpuRenderer_ClearFBO` at
the top of `RefreshGrille` (`../SOURCES/INTEXT.CPP`). A frame that redraws
terrain clears the offscreen target; a frame that only redraws objects
(`AFF_OBJETS`) keeps the previous 3D colour and depth, matching the software
Z-buffer, so an idle scene is not wiped to black. That paused colour would
otherwise keep each static-camera frame's shadow and animated-poly paint as an
accumulating smear, so the object pass restores the terrain snapshot from the
end of the terrain pass (see the seam above). After the terrain pass the
software `Screen`/`PtrZBuffer` buffers are refilled from the target (see the
readback seam above) so object occlusion works under a GPU backend.

Two composite rules keep the GPU output matching the software present, whose
only overlay mapping is `palette index 0 = opaque black`:

- **NZW depth test.** Batches under a `Fill_Flag_NZW` filler depth-test
  `GEQUAL` with no depth write, the mirror of the software fillers' per-pixel
  `PtrZBuffer[offset] >= zInt` test. An exterior body flagged
  `OBJ_ZBUFFER`/`OBJ_IN_WATER` (e.g. a crab or dragonfly, drawn to the target)
  is therefore culled where terrain is nearer than it instead of overpainted
  over walls; heavier occlusion still writes depth, and coplanar NZW polys
  cannot fight one another because the batch never writes depth.
- **Opaque overlay regions.** A modal UI that paints index-0 black (the
  inventory slots; the behaviour modal's cadre strokes and empty life/magic
  bars) pins its rectangle with `GpuRenderer_SetOverlayOpaqueRect`
  (`../SOURCES/INVENT.CPP` at `MenuInventory` entry, `../SOURCES/COMPORTE.CPP`
  at `MenuComportement` entry; each clears at exit); inside it the present
  shader maps index 0 to opaque black (`indexedTexel` in
  `present.frag.glsl`), outside it the frozen 3D scene still shows through the
  overlay's holes. The inventory wheel and name box, and the behaviour box plus
  its info bar, are inside those rects; the plate above keeps the shaded
  content non-zero so the shade maps real scene indices through the CLUT.

Remaining work, in order:

1. **Resolution changes** — a runtime resolution switch while a backend is
   active is not handled yet; a switch should `GpuRenderer_Shutdown`/re-init
   cleanly.