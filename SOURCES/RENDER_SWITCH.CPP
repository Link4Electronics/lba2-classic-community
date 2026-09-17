/*
 * Runtime renderer switching — orchestrator.  See RENDER_SWITCH.H + the GPU
 * renderer design doc at docs/RENDERER.md.
 */

#include "RENDER_SWITCH.H"

#include <stdlib.h>
#include <string.h>

#include <GPU/GPURENDERER.H> /* GpuRenderer Init/Shutdown (via RENDER_SWITCH.H) */
#include <SYSTEM/DEFFILE.H>  /* DefFileBufferInit / ReadValueDefault / SaveState */
#include <SYSTEM/EXIT.H>     /* PathConfigFile (extern char[ADELINE_MAX_PATH]) */
#include <SYSTEM/FILES.H>    /* ExistsFileOrDir */
#include <SYSTEM/LOG.H>
#include <SYSTEM/WINDOW.H> /* Window_RecreateWindowForGL/SW/GPU */
#include <SVGA/SCREEN.H>   /* ModeDesiredX/Y — the framebuffer the engine runs at */

/* The renderer the player chose (0 = software). Values match GpuRendererBackendId
   so the cfg row and the menu agree without a mapping table. */
S32 DisplayRenderer = GPU_RENDERER_BACKEND_NONE;

/* Scratch-buffer size for the transient, heap-free boot read of lba2.cfg below,
   mirroring RES_SWITCH.CPP's RES_BOOT_CFG_BUFSZ: a fully-populated valid cfg is
   ~6 KB, and a corrupt over-long line must not tip DefFileBufferInit into its
   "buffer too small" reject, which would silently drop the renderer choice. */
#define RENDER_BOOT_CFG_BUFSZ (64 * 1024)

/* The boot choice is a preference unless the LBA2_GPU_RENDERER env override says
   otherwise; the two have the same two acceptable values so the parse is shared.
   The env (like --resolution) is for one dev-gated run and is never persisted. */
GpuRendererBackendId Renderer_LoadBootChoice(void) {
    const char *env = getenv("LBA2_GPU_RENDERER");
    if (env != NULL && env[0] != 0) {
        if (strcmp(env, "opengl") == 0)
            return GPU_RENDERER_BACKEND_OPENGL;
        if (strcmp(env, "sdl3gpu") == 0)
            return GPU_RENDERER_BACKEND_SDL3GPU;
        Log_Warn("Renderer  ignoring unknown LBA2_GPU_RENDERER '%s'", env);
        return GPU_RENDERER_BACKEND_NONE;
    }

    /* Same transient, heap-free cfg read as Res_LoadBootDimensions /
       Res_LoadBootFullscreen: this must run before InitGraphics, which is before
       the engine heap and ScreenAux exist. The later ReadConfigFile ->
       Settings_ReadConfig pass reads the same key into DisplayRenderer; agreeing
       with it here keeps the run's choice and the player's stored preference the
       same value. */
    if (PathConfigFile[0] != '\0' && ExistsFileOrDir(PathConfigFile)) {
        static char tmpCfgBuf[RENDER_BOOT_CFG_BUFSZ];
        T_DEFFILE_STATE savedDef;
        DefFileBufferSaveState(&savedDef);
        if (DefFileBufferInit(PathConfigFile, tmpCfgBuf,
                              (S32)sizeof tmpCfgBuf)) {
            const long v =
                DefFileBufferReadValueDefault("Renderer",
                                              GPU_RENDERER_BACKEND_NONE);
            if (v == (long)GPU_RENDERER_BACKEND_OPENGL ||
                v == (long)GPU_RENDERER_BACKEND_SDL3GPU) {
                DefFileBufferRestoreState(&savedDef);
                return (GpuRendererBackendId)v;
            }
        }
        DefFileBufferRestoreState(&savedDef);
    }

    return GPU_RENDERER_BACKEND_NONE;
}

S32 Renderer_LoadBootQuality(void) {
    S32 quality = 1;
    if (PathConfigFile[0] != '\0' && ExistsFileOrDir(PathConfigFile)) {
        static char tmpCfgBuf[RENDER_BOOT_CFG_BUFSZ];
        T_DEFFILE_STATE savedDef;
        DefFileBufferSaveState(&savedDef);
        if (DefFileBufferInit(PathConfigFile, tmpCfgBuf,
                              (S32)sizeof tmpCfgBuf)) {
            const long value = DefFileBufferReadValueDefault("RenderQuality", 1);
            if (value >= 1 && value <= 4) {
                quality = value == 3 ? 4 : (S32)value;
            }
        }
        DefFileBufferRestoreState(&savedDef);
    }
    return quality;
}

/* Which window style a target renderer needs. The three presenters cannot share
   one window: the software path streams to an SDL_Renderer, the GL path swaps a
   GL context (requiring the window created with the OpenGL flag), and the SDL3
   GPU path claims the window for a GPU device — which refuses a window that
   already has a renderer or GL context. Each transition recreates the window. */
static bool Renderer_WindowFor(GpuRendererBackendId id, U32 resX, U32 resY,
                               bool fullscreen) {
    switch (id) {
    case GPU_RENDERER_BACKEND_OPENGL:
        return Window_RecreateWindowForGL(resX, resY, fullscreen);
    case GPU_RENDERER_BACKEND_SDL3GPU:
        return Window_RecreateWindowForGPU(resX, resY, fullscreen);
    default:
        return Window_RecreateWindowForSW(resX, resY, fullscreen);
    }
}

S32 Renderer_ApplyChoice(GpuRendererBackendId id) {
    if (id < 0 || id >= GPU_RENDERER_BACKEND_COUNT) {
        return FALSE;
    }

    /* Already there: nothing to tear down. A request for the live backend is
       a no-op even when the window's shape drifted from it (a GL init that
       failed at boot leaves a GL window on the software path), because the
       software present still works on that window. */
    if (GpuRenderer_ActiveBackend() == id) {
        DisplayRenderer = (S32)id;
        return TRUE;
    }

    const U32 resX = (U32)ModeDesiredX;
    const U32 resY = (U32)ModeDesiredY;
    const bool fullscreen = GetWindowFullscreen();

    /* Drop the backend first so no context/device holds the window while it is
       rebuilt. Shutdown also detaches the present/fill seams, so a half-built
       switch never drives a frame. */
    GpuRenderer_Shutdown();

    if (!Renderer_WindowFor(id, resX, resY, fullscreen)) {
        /* The target window could not be created: put the present path back on
           the software window the engine arrived on. */
        Log_Warn("Renderer  switching to backend %d failed; using software",
                 (int)id);
        Window_RecreateWindowForSW(resX, resY, fullscreen);
        DisplayRenderer = GPU_RENDERER_BACKEND_NONE;
        return FALSE;
    }

    if (id == GPU_RENDERER_BACKEND_NONE) {
        /* Software was the target: the SW window is already the whole present
           path. */
        DisplayRenderer = GPU_RENDERER_BACKEND_NONE;
        return TRUE;
    }

    if (GpuRenderer_Init(id, resX, resY)) {
        DisplayRenderer = (S32)id;
        return TRUE;
    }

    /* The backend accepted the window but could not come up (no GL context, no
       usable SDL3 GPU device): revert to software, the same fallback a boot-time
       init failure uses. */
    Log_Warn("Renderer  backend %d failed to start; using software", (int)id);
    Window_RecreateWindowForSW(resX, resY, fullscreen);
    DisplayRenderer = GPU_RENDERER_BACKEND_NONE;
    return FALSE;
}