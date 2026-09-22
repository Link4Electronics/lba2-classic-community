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
`GpuRendererBackendSDL3GPU` (19 operations each) implement the vtable in
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
  covered types are consumed, and the body, NZW-before-terrain and menu cases
  fall back, so the software Log still supplies what the GPU declines.
- **Present seam** — `PresentFrame` (`../LIB386/SVGA/SDL.CPP`) hands its Log
  ARGB overlay to the renderer (`SetGpuPresentHooks`, default NULL) instead of
  the SDL streaming texture while a backend is active; the renderer composites
  it over its 3D framebuffer and swaps. The overlay is staged into a
  present-owned buffer (`STAGING_BUFFER.CPP`) before the hand-off, because a
  GPU window is recreated without the SDL renderer/streaming texture (see the
  switch below), so the lock-texture path could not produce a frame there.
  `GL_GetPaletteLUT` exports the same 256-entry ARGB table so GPU-drawn
  content and Log always agree.
- **Atlas signal** — `AtlasTextureDirty` is raised by `DoTextureAnimation`
  (`../SOURCES/ANIMTEX.CPP`) whenever animated texture pages are baked in
  place, so the renderer re-uploads the atlas page it mirrored.
- **Depth readback** — `AffScene` (`../SOURCES/OBJECT.CPP`, `AFF_ALL`) calls
  `GpuRenderer_ReadbackDepthBuffer` after the exterior terrain pass to refill
  `PtrZBuffer` from the offscreen target. The object pass draws bodies into the
  software `Log` and re-applies terrain occlusion with `ZBufBoxOverWrite2`
  (`../SOURCES/3DEXT/BOXZBUF.CPP`), which reads that depth; without the readback
  it holds nothing from the GPU-drawn terrain. Where terrain is in front,
  `ZBufBoxOverWrite2` writes colour 0, which the present composite blends
  transparent over the same target's terrain, so no colour readback is needed.
  The call is gated on `CubeMode == CUBE_EXTERIEUR`: interior cubes reuse
  `PtrZBuffer`'s backing store for `BufCube`/`BufferBrick` and draw through
  `DrawOverBrick`. OpenGL reads depth directly; SDL3 GPU downloads its
  `D16_UNORM` depth texture, reverses its top-down rows to the bottom-up order
  the common layer expects, and converts the depth to the GL window-depth
  convention the common aZ inverse expects.
- **Terrain snapshot/restore** — a camera cut (`AFF_OBJETS` in `AffScene`,
  `../SOURCES/OBJECT.CPP`) redraws only the flagged bodies and never the
  terrain; the offscreen target keeps the previous frame's colour and depth so
  the scene does not wipe to black, but that also keeps the previous frame's
  shadow spans and NZW animated polys, which re-stamp the same pixels into
  trails — `BoxClean` restores only the `Log` buffer. At the end of the
  exterior terrain pass `GpuRenderer_SnapshotTerrain` copies the whole target
  into a persistent terrain target; at the head of each `AFF_OBJETS` exterior
  frame `GpuRenderer_RestoreTerrain` repaints it, before `DrawAnimatedPolys`
  (so that frame's animated polys land over the scrubbed terrain, mirroring
  software `BoxClean` + `DrawAnimatedPolys`), erasing the trails before the
  flagged-body pass draws. Depth is never written between the two calls — the
  NZW and shadow paths have depth-write off — so the restore repaints colour
  only and the `GEQUAL` depth tests keep working. Both are gated on the backend
  being active and `CubeMode == CUBE_EXTERIEUR`. GL stores the copy with
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
  as the average of the four footprint corners (in the same `GET_ZO` space the
  terrain tiles carry), and depth-tests `GEQUAL/GREATER_OR_EQUAL` against the
  `D16` target the terrain pass left behind: terrain nearer than the plane — a
  risen ridge or wall between camera and object — culls the shadow, mirroring
  the software `DrawRecover` occlusion; depth-write stays off so later blends
  survive. Interior cubes and menus keep `ShadeBoxBlk`, since their terrain is
  drawn into `Log`.

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
  inventory slots) pins its rectangle with `GpuRenderer_SetOverlayOpaqueRect`
  (`../SOURCES/INVENT.CPP`, set at `MenuInventory` entry and cleared at exit);
  inside it the composite maps index 0 to opaque black, outside it the frozen
  3D scene still shows through the overlay's holes.

Remaining work, in order:

1. **Render-quality scaling** — the Display-menu tier list and
   `GpuRenderer_SetRenderQuality` (1–4×); the `Renderer` config key and the
   Display-menu toggle the original's "change quality" slot maps onto already
   landed.
2. **Resolution changes** — a runtime resolution switch while a backend is
   active is not handled yet; a switch should `GpuRenderer_Shutdown`/re-init
   cleanly.