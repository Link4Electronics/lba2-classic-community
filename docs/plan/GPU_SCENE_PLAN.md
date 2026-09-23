# Moving scene drawing onto the GPU renderers

Status: Phase 1 Implemented (interior room bricks + flat interior shadows behind `GpuScene`, default off). Phases 2–4 are still Proposed; [RENDERER.md](../RENDERER.md) remains the reference for what the engine does today.

## Goal

Today only the exterior 3D scene reaches the GL / SDL3 GPU backends; interiors, sprites, HUD and text rasterize in the 1997 software path and composite over the GPU frame as the Log overlay. With one flag on — cfg key `GpuScene`, cvar `gpuscene`, default 0 — scene drawing moves onto the GPU for both backends, phase by phase. With the flag off, every new branch is dead: the default build stays byte-identical to today's hybrid, as the flag-gating rule requires.

Three costs are being removed, in the order they matter:

1. The interior room is thousands of CPU RLE sprite blits per frame (brick grid), scaling with the player's resolution while the exterior already scales on the GPU.
2. Every exterior frame drains the GPU pipeline for a depth readback (`GpuRenderer_ReadbackDepthBuffer`) so software sprites can test terrain occlusion.
3. Every frame converts the whole Log to ARGB on the CPU, stages it and uploads it, even where the Log covers most of the screen.

A note on the premise: the interior cube is not one texture. Bricks are individually RLE-encoded palette sprites (`AffGraph` in [GRAPH.CPP](../../LIB386/SVGA/GRAPH.CPP)), drawn in grid order, with walls in front of actors re-blitted through a second, mask-gated blit (`CopyMask` via `DrawOverBrick`). The migration turns the base grid blits into a batch of textured screen quads; the re-blit logic stays software in the first phases because the present order already makes it correct (Log composites over the FBO).

## Invariants the design keeps

- **Present order is the constraint.** The GPU frame presents first, the Log overlay paints on top, palette index 0 transparent. Anything that must appear over scene content lives in the Log until everything beneath it has moved too — so phases migrate whole layers in an order where every intermediate split is order-correct, never screen-by-screen guesses.
- **All shared machinery lives in [GPURENDERER_COMMON.CPP](../../LIB386/COMMON/GPURENDERER_COMMON.CPP).** Batching, atlas state, quad submission, the darken-span mode: one implementation. The two backends only grow a vtable operation where GL and SDL3 GPU genuinely differ (buffer/texture creation). No quad path is written twice.
- **Software paths stay untouched when the flag is off.** Hooks are added at draw sites the flag gates; the ASM equivalence tests keep testing the software fillers exactly as now.
- **Pixel rules come from the software blitters.** A quad must reproduce `AffGraph` output: same positions (integer, `Map2Screen` + hotX/hotY), same palette mapping at present time (the brick atlas follows the poly path's CLUT scheme — palette-index texture resolved through the same LUT GPU content already uses, so palette fades keep affecting the room), nearest sampling, the RLE's skip blocks becoming atlas alpha. `CopyMask`'s mask bank is a separate 1-bit gate and only the re-blit uses it.
- **Bookkeeping never moves to the GPU.** `AffBrickBlock` both draws and buckets the brick into `ListBrickColon` for `DrawOverBrick`; the hook replaces only the blit, the bucketing stays.

## Why the phases are ordered this way

The Log-over-FBO present order decides what can move first:

- The room can move first: bodies, sprites, re-blit walls and HUD already draw after it, and they keep drawing into the Log, which presents on top — the same relative order as today. The re-blit walls duplicate room pixels that now live in the FBO, but they paint the same colors into the Log, so the composite is unchanged.
- Bodies can follow: front walls are still re-blitted over them in the Log, so the wall-in-front case needs no depth. Bodies keep the exterior scheme (painter order, `GEQUAL`, no depth write). The one known approximation is body-versus-sprite order when a sprite should paint behind a body — the same approximation the GPU exteriors already ship with.
- Sprites/HUD move last among scene layers, and that is the phase that deletes the depth readback: with terrain and room depth both in the FBO, sprite quads depth-test there directly. Only when the Log is empty of scene content can the per-frame staging upload shrink or go away.
- Interior shadows need no depth: the original interior `ShadeSpan` is a flat darken (`ShadeBoxBlk`) with no Z test, unlike the exterior's depth-tested spans. The split is pixel-preserving: the same spans darken the FBO (where the room now lives) through the existing `GpuRenderer_Shadow*` machinery in a new no-depth mode, while the unchanged `ShadeBoxBlk` darkens whatever actor pixels sit in the Log — each buffer receives exactly the darken its pixels received when they shared one buffer.
- Interior depth stays on the CPU throughout the first phases: the `PtrZBuffer` backing store is dual-use for brick/cube data and feeds `DrawOverBrick`'s Z-masks, so no readback is introduced for interiors until something actually needs it.

## Phases

**Phase 1 — interior room to the FBO (Implemented).** Flag-gated hooks at the base brick blit sites in [GRILLE.CPP](../../SOURCES/GRILLE.CPP) (`AffBrickBlock`, `AffBrickBlockOnly`) call `GpuRenderer_DrawBrick`, which packs the cube's `BufferBrick` bank into an RG8 atlas (R = palette index, G = valid; AffGraph RLE skips leave G = 0) and submits screen-space quads batched as polyMode 7. `InitGrille` hands the bank over after `LoadUsedBrick`; `FreeGrille` drops it. `RefreshGrille` already clears the FBO for both cube modes, and the `AFF_ALL` path's `Cls` keeps the Log clean for full frames. `DrawOverBrick`, `CopyMask`, Z-masks, bodies, sprites, GRM incrusts and HUD all stay in the Log. Interior shadow spans gain the no-depth COMMON mode (`GpuRenderer_ShadowBeginFlat`) with the dual `ShadeSpan` described above. Success criteria: flag off, byte-identical (host tests green); flag on, harness screenshots in an interior cube visually match flag off at the same tick, on both backends.

**Phase 2 — interior bodies and NZW polys to the FBO.** Open the 3D pass for `CUBE_INTERIEUR` while the flag is on (today's gate in `AffOneObject` is exterior-only), and let COMMON's intake key on "the FBO has room content this frame" instead of terrain-only (`GpuRenderer_RenderTriangleList`'s `s_fboHasTerrain` condition gains its interior sibling). No depth required — front-wall re-blits still land in the Log above the bodies. Success: screenshots match, including an actor standing in a doorway with a wall in front.

**Phase 3 — sprites and text to the FBO, readback deleted.** Sprite/font/HUD quads depth-test against FBO depth directly; with the flag on, `GpuRenderer_ReadbackDepthBuffer` and its consumers (`ZBufBoxOverWrite2`'s color-0 hole trick) are skipped, and the staging upload shrinks to whatever the Log still holds. The largest equivalence surface in the plan — font glyphs and `CopyBlock` blits must come out pixel-identical at integer positions, which nearest-sampled quads should give but screenshots must prove. A companion sub-phase, independent and byte-identical by construction, converts the Log present to an R8 palette-index texture with the LUT lookup in the present shader, deleting the CPU ARGB conversion for every screen that still uses the Log.

**Phase 4 — menus and text dialogs: evaluate, expect to defer.** Moving a menu screen requires the whole screen to move at once (backdrop, plasma, rows, sprites), and the win is small: menus are cheap today, the plasma is CPU-generated so it costs an upload either way, and font rasterization risk lands squarely on the highest-visibility UI. The in-game text dialog is a candidate only after Phase 3, drawn as a UI pass with depth testing off. The recommendation is to measure after Phase 3 and leave this phase unstarted unless the numbers argue for it.

## Flag and surfaces

- cfg key `GpuScene`, cvar `gpuscene`, default 0: a `BootSettings` row in [CONFIG_FILE.CPP](../../SOURCES/CONFIG_FILE.CPP) beside `RenderQuality`/`GpuTexFilter`, with the global beside `DisplayTexFilter`/`RenderQuality` in [RENDER_GL.CPP](../../LIB386/GL/RENDER_GL.CPP) — the established home for player-facing GPU toggles — read by COMMON and the SOURCES hook sites.
- A Display submenu row (Quality-style cycle or toggle) is an optional follow-up once the flag proves itself; cfg + cvar first.
- No backend-specific configuration: one flag, both backends, selected as `Renderer` already is.

## Verification

- Every phase: full build, `ctest -L host_quick`, `make arch-check`, `make docs-symbols`.
- Visual: the CLI harness (`--load <save> --tick N --screenshot --fixed-dt 16 --headless`) produces flag-off/flag-on pairs in the same interior/exterior spot on both backends; equivalence here is "identical to the eye and in diff", not byte-exact, because a flag-gated feature is allowed to prove itself rather than satisfy the ASM oracle — the software path with the flag off is what the oracle still guards.
- Each phase updates [RENDERER.md](../RENDERER.md) (and this plan's status) in the same commit; a phase is not done until the doc says what the flag does.

## Out of scope

- Menus, inventory and credits staying on the software path until Phase 4 says otherwise.
- Any change to the software rasterizers or their equivalence tests.
- Removing the readback before Phase 3 has moved every consumer of it.
- LBA1, editor builds and the holomap/credits body paths (they keep the software Log path, as they do now).
