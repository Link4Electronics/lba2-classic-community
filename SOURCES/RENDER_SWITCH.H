#ifndef RENDER_SWITCH_H
#define RENDER_SWITCH_H

/* Runtime renderer switching: the Display submenu's backend choice, its lba2.cfg
   key, and the boot-time and runtime paths that act on it. The sibling of
   RES_SWITCH.CPP: that module owns the screen size, this one owns which present
   path draws it. */

#include <GPU/GPURENDERER.H> /* GpuRendererBackendId */

/* The renderer the player chose, as a GpuRendererBackendId value (0 = software).
   Owned by this module, written on a successful Renderer_ApplyChoice and at boot
   by ReadConfigFile; the cfg "Renderer" key binds to it. */
extern S32 DisplayRenderer;

/* The renderer to boot with: the LBA2_GPU_RENDERER env override when present,
   else the cfg "Renderer" value read with the same transient, heap-free
   DefFileBuffer pass InitGraphics needs the answer before. Must run before
   InitGraphics, because an OpenGL window has to be created with the flag set. */
GpuRendererBackendId Renderer_LoadBootChoice(void);
S32 Renderer_LoadBootQuality(void);

/* Switch the live present path to `id` now. Tears down the active backend, makes
   the window match the target (OpenGL flag for the GL backend, plain for the SDL
   GPU backend, SDL renderer + streaming texture for software), and re-inits. On
   any failure it falls back to the software path, the way a boot-time init
   failure does, so a backend that cannot start never strands the player. Returns
   TRUE when the target is live. */
S32 Renderer_ApplyChoice(GpuRendererBackendId id);

#endif // RENDER_SWITCH_H