# GPU renderer

The hardware-accelerated render path for the 3D scene and the final composite.
It is an option under construction; the engine still draws with the software
renderer by default, and none of this code is reachable from a normal run yet.

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

A backend is a thin wire: `GpuRendererBackendGL` (16 operations) implements
the vtable in `../LIB386/H/GPU/GPURENDERER_BACKEND.H`. It binds the FBO,
uploads the batch, sets the uniforms from the already-resolved
`GpuDrawParams`, draws, and composites the two framebuffers onto the window.
It decides nothing.

## Backends

Both backends share the four sources in `../LIB386/COMMON/*.glsl`, which mark
per-API blocks with `##ifdef GL_ES` / `##ifdef GL_CORE` / `##ifdef SDL3GPU` /
`##else` / `##endif` (double-hash so no shader compiler ever sees them).

- **OpenGL / GLES 2.0** (`../LIB386/GL/`) is functional. It embeds the raw
  shared sources at build time and resolves the dialect at runtime with
  `GpuShader_FilterGL`, so desktop and Android use identical code and only the
  `#version` header differs (`USE_GLES2` is set by CMake for Android; the
  source itself names no platform).
- **SDL3 GPU** (`../LIB386/SDL3GPU/`) is a skeleton. It filters the shared
  sources and cross-compiles them to SPIR-V and (with shadercross/spirv-cross
  + dxc) MSL/DXIL at build time; a device selects its format at runtime. It
  is harmless without the translators, and fails at configure time on Apple,
  where Metal is the only driver.

## Build

`-DGPURENDERER=ON` (default) builds the three archives (`gpur`, `gly`,
`gs3`) and links them into the engine. Enabling it does not change what a
run draws: nothing calls the renderer yet.

## Status

Not wired into the engine. The next milestone hooks the renderer in: the
polygon dispatches in `../SOURCES/3DEXT/TERRAIN.CPP` and `../LIB386/pol_work/POLY.CPP`
feed the common layer, the present branch in `../LIB386/SVGA/SDL.CPP` gets
`GL_GetPaletteLUT`, and init requests a GL window. After that, a config key
plus the Display-menu toggle exposes the choice to the player.