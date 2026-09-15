//──────────────────────────────────────────────────────────────────────────
// SDL3 GPU renderer backend — skeleton
//──────────────────────────────────────────────────────────────────────────
// Implements the GPU/GPURENDERER_BACKEND vtable on SDL3's GPU API. The
// skeleton covers the device and frame plumbing — device creation from the
// host window, swapchain claim, offscreen colour+depth target, clearing,
// and the swapchain present — so a device is genuinely live when this
// backend is chosen. The draw path (3D batching and the present composite)
// is stubbed; GL remains the reference backend. Shaders are compiled to
// SPIR-V (and, when the cross-compilers are present, MSL/DXIL) at build
// time from the shared sources in LIB386/COMMON; the embedded
// blobs are catalogued here for the draw path that lands on top.
//──────────────────────────────────────────────────────────────────────────
#include <SDL3/SDL_gpu.h>

#include <SYSTEM/LOG.H>
#include <SYSTEM/WINDOW.H>
#include <GPU/GPURENDERER_BACKEND.H>

#include "sdl3gpu_shader_formats.h"

//──────────────────────────────────────────────────────────────────────────
// Embedded shaders produced by the build (LIB386/SDL3GPU/CMakeLists.txt).
// Each format may be compiled out when the toolchain is missing; the
// catalog below then carries a zero-length entry for it.
//──────────────────────────────────────────────────────────────────────────
typedef struct S3ShaderBlob {
    const char *name;
    const char *data;
    Uint32 size;
} S3ShaderBlob;

#if SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE
#include "present.vert.spv.h"
#include "present.frag.spv.h"
#include "clut.vert.spv.h"
#include "clut.frag.spv.h"
static const S3ShaderBlob s3Spv[] = {
    { "present.vert", PRESENT_VERT_SPV, sizeof(PRESENT_VERT_SPV) - 1 },
    { "present.frag", PRESENT_FRAG_SPV, sizeof(PRESENT_FRAG_SPV) - 1 },
    { "clut.vert", CLUT_VERT_SPV, sizeof(CLUT_VERT_SPV) - 1 },
    { "clut.frag", CLUT_FRAG_SPV, sizeof(CLUT_FRAG_SPV) - 1 },
};
#endif

#if SDL3GPU_SHADERFORMAT_MSL_AVAILABLE
#include "present.vert.metal.h"
#include "present.frag.metal.h"
#include "clut.vert.metal.h"
#include "clut.frag.metal.h"
static const S3ShaderBlob s3Msl[] = {
    { "present.vert", PRESENT_VERT_METAL, sizeof(PRESENT_VERT_METAL) - 1 },
    { "present.frag", PRESENT_FRAG_METAL, sizeof(PRESENT_FRAG_METAL) - 1 },
    { "clut.vert", CLUT_VERT_METAL, sizeof(CLUT_VERT_METAL) - 1 },
    { "clut.frag", CLUT_FRAG_METAL, sizeof(CLUT_FRAG_METAL) - 1 },
};
#endif

#if SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE
#include "present.vert.dxil.h"
#include "present.frag.dxil.h"
#include "clut.vert.dxil.h"
#include "clut.frag.dxil.h"
static const S3ShaderBlob s3Dxil[] = {
    { "present.vert", PRESENT_VERT_DXIL, sizeof(PRESENT_VERT_DXIL) - 1 },
    { "present.frag", PRESENT_FRAG_DXIL, sizeof(PRESENT_FRAG_DXIL) - 1 },
    { "clut.vert", CLUT_VERT_DXIL, sizeof(CLUT_VERT_DXIL) - 1 },
    { "clut.frag", CLUT_FRAG_DXIL, sizeof(CLUT_FRAG_DXIL) - 1 },
};
#endif

// Keeps the build-time shader blobs linked into the skeleton.
#if SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE || SDL3GPU_SHADERFORMAT_MSL_AVAILABLE || SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE
static const S3ShaderBlob *const s3AllFormats[] = {
#if SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE
    s3Spv,
#endif
#if SDL3GPU_SHADERFORMAT_MSL_AVAILABLE
    s3Msl,
#endif
#if SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE
    s3Dxil,
#endif
};
static const Uint32 s3AllFormatsCount =
    (Uint32)(sizeof(s3AllFormats) / sizeof(s3AllFormats[0]));
#endif

//──────────────────────────────────────────────────────────────────────────
// Backend state
//──────────────────────────────────────────────────────────────────────────
static SDL_Window *s_window = NULL;
static SDL_GPUDevice *s_device = NULL;
static SDL_GPUTextureFormat s_swapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;

static U32 s_resX = 0; /* present surface size (engine pixel space) */
static U32 s_resY = 0;

/* Offscreen colour+depth target (SSAA-free for now: native size). */
static SDL_GPUTexture *s_targetColor = NULL;
static SDL_GPUTexture *s_targetDepth = NULL;
static U32 s_targetW = 0;
static U32 s_targetH = 0;

/* In-flight frame: one command buffer, one acquired swapchain texture. */
static SDL_GPUCommandBuffer *s_cmdbuf = NULL;
static SDL_GPUTexture *s_swapchainTex = NULL;
static SDL_GPURenderPass *s_swapchainPass = NULL;

static void S3_TargetDestroy(void);

//──────────────────────────────────────────────────────────────────────────
// Context lifecycle
//──────────────────────────────────────────────────────────────────────────
static S32 S3_CreateContext(U32 resX, U32 resY) {
    s_window = Window_GetSDLWindow();
    if (s_window == NULL) {
        Log_Warn("[SDL3GPU] no host window to claim");
        return FALSE;
    }
    s_resX = resX;
    s_resY = resY;

    /* SDL3 GPU drivers require different shader bytecode; build the device
       create mask from the formats this build embedded. Formats compiled
       out (no cross-compiler at configure time) drop their driver. */
    SDL_GPUShaderFormat formats = 0;
#if SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE
    formats |= SDL_GPU_SHADERFORMAT_SPIRV;
#endif
#if SDL3GPU_SHADERFORMAT_MSL_AVAILABLE
    formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
#if SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE
    formats |= SDL_GPU_SHADERFORMAT_DXIL;
#endif

    /* Pick the driver automatically (Vulkan on Linux/Windows, Metal on
       Apple). Debug mode off. */
    s_device = SDL_CreateGPUDevice(formats, false, NULL);
    if (s_device == NULL) {
        Log_Warn("[SDL3GPU] SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return FALSE;
    }

    if (!SDL_ClaimWindowForGPUDevice(s_device, s_window)) {
        Log_Warn("[SDL3GPU] SDL_ClaimWindowForGPUDevice failed: %s",
                 SDL_GetError());
        SDL_DestroyGPUDevice(s_device);
        s_device = NULL;
        return FALSE;
    }

    s_swapchainFormat = SDL_GetGPUSwapchainTextureFormat(s_device, s_window);
    SDL_SetGPUSwapchainParameters(s_device, s_window,
                                  SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  SDL_GPU_PRESENTMODE_VSYNC);

    /* Instantiate the embedded shader blobs now so a misconfigured build
       (no shader format embedded) fails at select time, not first draw. */
#if SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE || SDL3GPU_SHADERFORMAT_MSL_AVAILABLE || SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE
    if (s3AllFormatsCount == 0) {
        Log_Warn("[SDL3GPU] no embedded shaders (is glslangValidator present "
                 "at configure time?)");
        SDL_ReleaseWindowFromGPUDevice(s_device, s_window);
        SDL_DestroyGPUDevice(s_device);
        s_device = NULL;
        return FALSE;
    }
#endif

    return TRUE;
}

static void S3_DestroyContext(void) {
    if (s_device == NULL) {
        return;
    }
    S3_TargetDestroy();
    if (s_window != NULL) {
        SDL_ReleaseWindowFromGPUDevice(s_device, s_window);
    }
    SDL_DestroyGPUDevice(s_device);
    s_device = NULL;
    s_window = NULL;
    s_swapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
}

//──────────────────────────────────────────────────────────────────────────
// Offscreen target
//──────────────────────────────────────────────────────────────────────────
static S32 S3_TargetCreate(U32 width, U32 height) {
    if (s_device == NULL) {
        return FALSE;
    }

    SDL_GPUTextureCreateInfo ci;
    SDL_zero(ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.width = width;
    ci.height = height;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    s_targetColor = SDL_CreateGPUTexture(s_device, &ci);
    if (s_targetColor == NULL) {
        Log_Warn("[SDL3GPU] color target create failed: %s", SDL_GetError());
        return FALSE;
    }

    ci.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    s_targetDepth = SDL_CreateGPUTexture(s_device, &ci);
    if (s_targetDepth == NULL) {
        Log_Warn("[SDL3GPU] depth target create failed: %s", SDL_GetError());
        SDL_ReleaseGPUTexture(s_device, s_targetColor);
        s_targetColor = NULL;
        return FALSE;
    }

    s_targetW = width;
    s_targetH = height;
    return TRUE;
}

static void S3_TargetDestroy(void) {
    if (s_device == NULL) {
        return;
    }
    if (s_targetColor != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_targetColor);
        s_targetColor = NULL;
    }
    if (s_targetDepth != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_targetDepth);
        s_targetDepth = NULL;
    }
    s_targetW = 0;
    s_targetH = 0;
}

static S32 S3_TargetIsValid(void) {
    return (s_targetColor != NULL && s_targetDepth != NULL) ? TRUE : FALSE;
}

static void S3_TargetBindClear(void) {
    if (s_device == NULL || s_targetColor == NULL) {
        return;
    }

    /* Clear-only render pass on the offscreen target. */
    SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(s_device);
    if (cmdbuf == NULL) {
        return;
    }

    SDL_FColor clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    SDL_GPUColorTargetInfo cti;
    SDL_zero(cti);
    cti.texture = s_targetColor;
    cti.clear_color = clearColor;
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo dti;
    SDL_zero(dti);
    dti.texture = s_targetDepth;
    dti.clear_depth = 0.0f;
    dti.load_op = SDL_GPU_LOADOP_CLEAR;
    dti.store_op = SDL_GPU_STOREOP_STORE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &cti, 1, &dti);
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmdbuf);
}

static U32 S3_TargetWidth(void) {
    return s_targetW;
}

static U32 S3_TargetHeight(void) {
    return s_targetH;
}

static S32 S3_TargetReadDepth(void *dst, U32 width, U32 height) {
    (void)dst; (void)width; (void)height;
    /* SDL3 GPU depth textures cannot be read back (they are not samplable
       and the API exposes no download for them). The GL backend is the
       reference readback path; the common layer falls back when FALSE. */
    return FALSE;
}

static void S3_TargetReadColor(void *dst, U32 width, U32 height) {
    (void)dst; (void)width; (void)height;
    /* Skeleton: no color readback. Common zeros the caller's buffer when
       nothing is written (see GPURENDERER_COMMON.CPP). */
}

//──────────────────────────────────────────────────────────────────────────
// Fixed-role textures (draw path not yet implemented).
//──────────────────────────────────────────────────────────────────────────
static void S3_TexUpdatePresent(const void *argb, U32 pitch, U32 width,
                                U32 height, S32 linear) {
    (void)argb; (void)pitch; (void)width; (void)height; (void)linear;
}

static void S3_TexUpdateAtlas(const void *lum) {
    (void)lum;
}

static void S3_TexUpdateCLUT(const void *lum) {
    (void)lum;
}

static void S3_TexUpdatePalette(const void *argb) {
    (void)argb;
}

//──────────────────────────────────────────────────────────────────────────
// 3D batch (stub: the CLUT pipeline lands here once the draw path exists)
//──────────────────────────────────────────────────────────────────────────
static void S3_DrawTriangles(const GpuVertex *verts, U32 vertexCount,
                             const GpuDrawParams *params) {
    (void)verts; (void)vertexCount; (void)params;
}

//──────────────────────────────────────────────────────────────────────────
// Frame composite. Present is implicit in SDL3 GPU: a command buffer that
// renders to an acquired swapchain texture presents it on submit. The two
// vtable slots delimit one host frame:
//   ClearBackbuffer  — acquire a command buffer, wait for and acquire the
//                      swapchain texture, begin a clear render pass.
//   DrawPresentQuad  — composites the FBO/docs texture into the pass (a
//                      no-op draw for now), then ends the pass and submits,
//                      which presents the swapchain.
//──────────────────────────────────────────────────────────────────────────
static void S3_ClearBackbuffer(void) {
    if (s_device == NULL || s_cmdbuf != NULL) {
        return;
    }

    SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(s_device);
    if (cmdbuf == NULL) {
        return;
    }

    SDL_GPUTexture *swapchainTex = NULL;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmdbuf, s_window, &swapchainTex,
                                               NULL, NULL)) {
        Log_Warn("[SDL3GPU] swapchain acquire failed: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmdbuf);
        return;
    }

    SDL_FColor color = { 0.0f, 0.0f, 0.0f, 1.0f };
    SDL_GPUColorTargetInfo cti;
    SDL_zero(cti);
    cti.texture = swapchainTex;
    cti.clear_color = color;
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmdbuf, &cti, 1, NULL);

    s_cmdbuf = cmdbuf;
    s_swapchainTex = swapchainTex;
    s_swapchainPass = pass;
}

static void S3_DrawPresentQuad(S32 which, S32 linear) {
    (void)which; (void)linear;
    /* Skeleton: the FBO/documents composite is not rendered yet. End the
       clear render pass and submit the command buffer, which presents the
       swapchain — SDL3 GPU has no separate swap call. */
    if (s_cmdbuf == NULL) {
        return;
    }
    if (s_swapchainPass != NULL) {
        SDL_EndGPURenderPass(s_swapchainPass);
        s_swapchainPass = NULL;
    }
    s_swapchainTex = NULL;
    SDL_SubmitGPUCommandBuffer(s_cmdbuf);
    s_cmdbuf = NULL;
}

//──────────────────────────────────────────────────────────────────────────
const GpuRendererBackend GpuRendererBackendSDL3GPU = {
    "sdl3gpu",
    S3_CreateContext,
    S3_DestroyContext,
    S3_TargetCreate,
    S3_TargetDestroy,
    S3_TargetIsValid,
    S3_TargetBindClear,
    S3_TargetWidth,
    S3_TargetHeight,
    S3_TargetReadDepth,
    S3_TargetReadColor,
    S3_TexUpdatePresent,
    S3_TexUpdateAtlas,
    S3_TexUpdateCLUT,
    S3_TexUpdatePalette,
    S3_DrawTriangles,
    S3_ClearBackbuffer,
    S3_DrawPresentQuad,
};