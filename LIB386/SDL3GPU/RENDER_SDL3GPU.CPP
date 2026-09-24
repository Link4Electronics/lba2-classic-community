//──────────────────────────────────────────────────────────────────────────
// SDL3 GPU renderer backend
//──────────────────────────────────────────────────────────────────────────
// Implements the GPU/GPURENDERER_BACKEND vtable on SDL3's GPU API: device
// and swapchain plumbing, the offscreen colour+depth target, the CLUT 3D
// pipeline and the two-quad present composite. Shaders are compiled to
// SPIR-V (and, when the cross-compilers are present, MSL/DXIL) at build
// time from the shared sources in LIB386/COMMON; the embedded blobs are
// catalogued below and the matching format is selected per device.
//
// Vertex data arrives packed as the GL backend's 14-byte GpuVertex. SDL3 GPU
// has no 16-bit integer vertex formats, so the frame's fresh vertices are
// repacked to the 28-byte float layout the shared shaders declare (vec2/vec2/
// float/float/float) and appended to a CPU arena. The batches are uploaded in
// one copy pass and replayed in one render pass when a texture upload or the
// present closes the target pass; uploading per batch instead would recycle
// the vertex buffer on every copy pass and make SDL allocate GPU buffers
// repeatedly. That repack is the only place the two backends' vertex paths
// differ.
//
// SDL3 GPU has no separate swap call: a command buffer that renders to an
// acquired swapchain texture presents it on submit. One host frame shares a
// single command buffer, from the first upload or triangle draw through the
// overlay composite, which is where the frame is submitted.
//──────────────────────────────────────────────────────────────────────────
#include <SDL3/SDL_gpu.h>

#include <SYSTEM/LOG.H>
#include <SYSTEM/WINDOW.H>
#include <GPU/GPURENDERER_BACKEND.H>

#include <string.h>
#include <stdlib.h>

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

//──────────────────────────────────────────────────────────────────────────
// Layout constants
//──────────────────────────────────────────────────────────────────────────
#define S3_BATCH_MAX_VERTS 4096
#define S3_VERTEX_STRIDE 28 /* 7 floats: pos.xy, uv, light, z, repmask */

#define S3_CLUT_PIPELINE_COUNT 12 /* 3 depth-test modes x 2 writes x 2 blends */

// CLUT shader uniforms. Must match the std140 layout of clut.vert/frag.glsl:
// scalars, all naturally 4-byte aligned, block sizes padded to 16.
typedef struct S3ScreenParams {
    float screenScale[2];
    float pad[2];
} S3ScreenParams;

typedef struct S3GpuParams {
    S32 polyMode;
    S32 alphaMode;
    float flatColor;
    S32 bilinear;
    S32 perspCorrect;
    S32 chromaKey;
    float clutBaseRow;
    float scaledFogNear;
    float fogRowScale;
    float brickW; /* mode 7: brick atlas page size (std140 match GLSL) */
    float brickH;
    float pad;
} S3GpuParams;

//──────────────────────────────────────────────────────────────────────────
// Backend state
//──────────────────────────────────────────────────────────────────────────
static SDL_Window *s_window = NULL;
static SDL_GPUPresentMode s_presentMode = SDL_GPU_PRESENTMODE_VSYNC;
static SDL_GPUDevice *s_device = NULL;
static SDL_GPUTextureFormat s_swapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
static SDL_GPUShaderFormat s_shaderFormat = 0;
static const S3ShaderBlob *s_shaderBlobs = NULL;

static U32 s_resX = 0; /* present surface size (engine pixel space) */
static U32 s_resY = 0;
static U32 s_presentW = 0; /* content size the composite letterboxes */
static U32 s_presentH = 0;

/* Offscreen colour+depth target (native present size). */
static SDL_GPUTexture *s_targetColor = NULL;
static SDL_GPUTexture *s_targetDepth = NULL;
static U32 s_targetW = 0;
static U32 s_targetH = 0;

/* Persistent terrain target (see GpuRenderer_SnapshotTerrain/RestoreTerrain):
   colour + depth textures sized like the offscreen target. Filled by texture
   copies at the end of the exterior terrain pass; the colour copy is replayed
   over the offscreen target at the head of each AFF_OBJETS exterior frame so a
   static camera scrubs the previous frame's shadow/animated-poly paint. The
   depth copy stores a faithful post-terrain-pass state even though restore
   never reads it back (the target's own depth is unwritten between the two). */
static SDL_GPUTexture *s_terrainColor = NULL;
static SDL_GPUTexture *s_terrainDepth = NULL;

/* Fixed-role textures. */
static SDL_GPUTexture *s_atlasTex = NULL;
static SDL_GPUTexture *s_clutTex = NULL;
static SDL_GPUTexture *s_paletteTex = NULL;
static SDL_GPUTexture *s_brickTex = NULL; /* RG8 brick atlas (GpuScene) */
static U32 s_brickTexW = 0;
static U32 s_brickTexH = 0;
static SDL_GPUTexture *s_overlayTex = NULL;
static U32 s_overlayW = 0;
static U32 s_overlayH = 0;

/* Samplers: nearest for the paletted lookups, linear for the composite. */
static SDL_GPUSampler *s_samplerNearest = NULL;
static SDL_GPUSampler *s_samplerLinear = NULL;

/* Vertex buffers: the frame's repacked stream and the composite quad. */
static SDL_GPUBuffer *s_batchVbo = NULL;
static U32 s_batchVboBytes = 0;
static SDL_GPUBuffer *s_quadVbo = NULL;

/* One staging transfer buffer shared by every upload. SDL cycles it per map so
   a copy the GPU is still reading is not overwritten; growing it is rare. */
static SDL_GPUTransferBuffer *s_staging = NULL;
static U32 s_stagingBytes = 0;

/* Download transfer buffers for the combined colour+depth readback. It submits
   and waits, so a buffer is never overwritten while still in flight. */
static SDL_GPUTransferBuffer *s_readbackDepthTb = NULL;
static U32 s_readbackDepthTbBytes = 0;

/* Pipelines. */
static SDL_GPUGraphicsPipeline *s_clutPipelines[S3_CLUT_PIPELINE_COUNT];
static SDL_GPUGraphicsPipeline *s_presentOpaque = NULL;
static SDL_GPUGraphicsPipeline *s_presentBlend = NULL;

/* Last content uploaded to each fixed texture, to skip redundant uploads.
   The common layer offers the atlas/CLUT/palette on every polygon; the GL
   backend re-uploads each time, which is wasteful here because every copy
   pass forces the target pass closed. Compare instead. */
static U8 s_atlasCache[256 * 256];
static U8 s_clutCache[256 * 256];
static U32 s_paletteCache[256];
static S32 s_atlasCacheValid = FALSE;
static S32 s_clutCacheValid = FALSE;
static S32 s_paletteCacheValid = FALSE;

/* In-flight host frame: one command buffer shared by every draw, plus the
   acquired swapchain and its render pass near the end of the frame. */
static SDL_GPUCommandBuffer *s_frameCmd = NULL;
static SDL_GPUTexture *s_swapchainTex = NULL;
static SDL_GPURenderPass *s_swapchainPass = NULL;
static U32 s_swapchainW = 0;
static U32 s_swapchainH = 0;

/* Open offscreen pass (kept short: uploads need a copy pass, which cannot
   overlap a render pass) and whether it still needs its first clear. */
static SDL_GPURenderPass *s_targetPass = NULL;
static S32 s_targetNeedsClear = TRUE;
/* 0xRRGGBB for the next deferred target clear (scene FogCoul). */
static U32 s_targetClearRgb = 0;

/* Letterbox rect computed by ClearBackbuffer for both composite quads. */
static S32 s_vpX = 0;
static S32 s_vpY = 0;
static U32 s_vpW = 0;
static U32 s_vpH = 0;

/* Repacked batch vertices (7 floats per vertex). The whole frame's draws are
   appended to the CPU arena below and uploaded in one copy pass when a texture
   upload closes the target pass (rare, the uploads are deduped) or at present.
   Uploading per batch instead recycles the vertex buffer on every copy pass,
   which makes SDL allocate a fresh GPU buffer hundreds of times a frame. */
static float *s_frameVerts = NULL;
static U32 s_frameVertCap = 0; /* capacity in vertices */
static U32 s_frameVertCount = 0;

typedef struct S3DrawCmd {
    U32 pipelineIndex;
    U32 firstVertex;
    U32 vertexCount;
    float screenScale[2];
    S32 clipX0;
    S32 clipY0;
    S32 clipX1;
    S32 clipY1;
    S3GpuParams params;
} S3DrawCmd;

#define S3_DRAW_CMD_MAX 8192
static S3DrawCmd s_draws[S3_DRAW_CMD_MAX];
static U32 s_drawCount = 0;

/* 2D overlay quad. SDL3 GPU texture coordinates have the top-left at (0,0)
   with +Y down, matching game row 0, so V is flipped relative to OpenGL:
   NDC top (+1) samples V=0. The offscreen target uses the same mapping
   because the CLUT vertex shader maps game Y=0 to NDC +1. */
static const float s_quadVerts[16] = {
    /* pos             texcoord */
    -1.0f, -1.0f, 0.0f, 1.0f, /* bottom-left  */
     1.0f, -1.0f, 1.0f, 1.0f, /* bottom-right */
    -1.0f,  1.0f, 0.0f, 0.0f, /* top-left     */
     1.0f,  1.0f, 1.0f, 0.0f, /* top-right    */
};

static void S3_TargetDestroy(void);
static void S3_DestroyContext(void);
static void S3_FlushDraws(void);

//──────────────────────────────────────────────────────────────────────────
// Shader and pipeline creation
//──────────────────────────────────────────────────────────────────────────
static SDL_GPUShaderFormat S3_SelectShaderFormat(void) {
    SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(s_device);
#if SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE
    if ((supported & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
        s_shaderBlobs = s3Spv;
        return SDL_GPU_SHADERFORMAT_SPIRV;
    }
#endif
#if SDL3GPU_SHADERFORMAT_MSL_AVAILABLE
    if ((supported & SDL_GPU_SHADERFORMAT_MSL) != 0) {
        s_shaderBlobs = s3Msl;
        return SDL_GPU_SHADERFORMAT_MSL;
    }
#endif
#if SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE
    if ((supported & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
        s_shaderBlobs = s3Dxil;
        return SDL_GPU_SHADERFORMAT_DXIL;
    }
#endif
    return 0;
}

static const S3ShaderBlob *S3_FindBlob(const char *name) {
    U32 i;
    if (s_shaderBlobs == NULL) {
        return NULL;
    }
    for (i = 0; i < 4; i++) {
        if (strcmp(s_shaderBlobs[i].name, name) == 0) {
            return &s_shaderBlobs[i];
        }
    }
    return NULL;
}

static SDL_GPUShader *S3_CreateShader(const char *name,
                                      SDL_GPUShaderStage stage,
                                      U32 numSamplers, U32 numUniformBuffers) {
    const S3ShaderBlob *blob = S3_FindBlob(name);
    SDL_GPUShaderCreateInfo ci;
    if (blob == NULL || blob->size == 0) {
        Log_Warn("[SDL3GPU] shader %s not embedded", name);
        return NULL;
    }
    SDL_zero(ci);
    ci.code_size = (size_t)blob->size;
    ci.code = (const Uint8 *)blob->data;
    /* SPIRV-Cross renames the MSL entry point to main0. */
    ci.entrypoint = (s_shaderFormat == SDL_GPU_SHADERFORMAT_MSL) ? "main0" : "main";
    ci.format = s_shaderFormat;
    ci.stage = stage;
    ci.num_samplers = numSamplers;
    ci.num_uniform_buffers = numUniformBuffers;
    {
        SDL_GPUShader *sh = SDL_CreateGPUShader(s_device, &ci);
        if (sh == NULL) {
            Log_Warn("[SDL3GPU] shader %s create failed: %s", name,
                     SDL_GetError());
        }
        return sh;
    }
}

static void S3_FillBlendState(SDL_GPUColorTargetBlendState *bs, S32 blend) {
    SDL_zero(*bs);
    bs->src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    bs->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    bs->color_blend_op = SDL_GPU_BLENDOP_ADD;
    bs->src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    bs->dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    bs->alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    bs->enable_blend = (blend != 0) ? true : false;
}

static SDL_GPUGraphicsPipeline *S3_CreateClutPipeline(SDL_GPUShader *vs,
                                                      SDL_GPUShader *fs,
                                                      S32 depthTest,
                                                      S32 depthWrite, S32 blend) {
    SDL_GPUVertexBufferDescription vbd;
    SDL_GPUVertexAttribute attrs[5];
    SDL_GPUVertexInputState vin;
    SDL_GPUColorTargetBlendState blendState;
    SDL_GPUColorTargetDescription ctd;
    SDL_GPUDepthStencilState ds;
    SDL_GPURasterizerState rs;
    SDL_GPUMultisampleState ms;
    SDL_GPUGraphicsPipelineTargetInfo ti;
    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_GPUGraphicsPipeline *pipe;

    SDL_zero(vbd);
    vbd.slot = 0;
    vbd.pitch = S3_VERTEX_STRIDE;
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_zero(attrs);
    attrs[0].location = 0;
    attrs[0].buffer_slot = 0;
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; /* XE, YE  */
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].buffer_slot = 0;
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; /* MapU, V */
    attrs[1].offset = 8;
    attrs[2].location = 2;
    attrs[2].buffer_slot = 0;
    attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT; /* Light   */
    attrs[2].offset = 16;
    attrs[3].location = 3;
    attrs[3].buffer_slot = 0;
    attrs[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT; /* ZO      */
    attrs[3].offset = 20;
    attrs[4].location = 4;
    attrs[4].buffer_slot = 0;
    attrs[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT; /* RepMask */
    attrs[4].offset = 24;

    SDL_zero(vin);
    vin.vertex_buffer_descriptions = &vbd;
    vin.num_vertex_buffers = 1;
    vin.vertex_attributes = attrs;
    vin.num_vertex_attributes = 5;

    S3_FillBlendState(&blendState, blend);

    SDL_zero(ctd);
    ctd.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ctd.blend_state = blendState;

    SDL_zero(ds);
    ds.enable_depth_test = (depthTest != 0) ? true : false;
    ds.enable_depth_write = (depthTest != 0 && depthWrite != 0) ? true : false;
    ds.compare_op = (depthTest == 2) ? SDL_GPU_COMPAREOP_ALWAYS
                                     : SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;

    SDL_zero(rs);
    rs.fill_mode = SDL_GPU_FILLMODE_FILL;
    rs.cull_mode = SDL_GPU_CULLMODE_NONE;
    rs.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    rs.enable_depth_clip = true;

    SDL_zero(ms);
    ms.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_zero(ti);
    ti.color_target_descriptions = &ctd;
    ti.num_color_targets = 1;
    ti.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    ti.has_depth_stencil_target = true;

    SDL_zero(ci);
    ci.vertex_shader = vs;
    ci.fragment_shader = fs;
    ci.vertex_input_state = vin;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state = rs;
    ci.multisample_state = ms;
    ci.depth_stencil_state = ds;
    ci.target_info = ti;

    pipe = SDL_CreateGPUGraphicsPipeline(s_device, &ci);
    if (pipe == NULL) {
        Log_Warn("[SDL3GPU] CLUT pipeline (dt=%d dw=%d bl=%d) failed: %s",
                 (int)depthTest, (int)depthWrite, (int)blend, SDL_GetError());
    }
    return pipe;
}

static SDL_GPUGraphicsPipeline *S3_CreatePresentPipeline(SDL_GPUShader *vs,
                                                         SDL_GPUShader *fs,
                                                         S32 blend) {
    SDL_GPUVertexBufferDescription vbd;
    SDL_GPUVertexAttribute attrs[2];
    SDL_GPUVertexInputState vin;
    SDL_GPUColorTargetBlendState blendState;
    SDL_GPUColorTargetDescription ctd;
    SDL_GPUDepthStencilState ds;
    SDL_GPURasterizerState rs;
    SDL_GPUMultisampleState ms;
    SDL_GPUGraphicsPipelineTargetInfo ti;
    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_GPUGraphicsPipeline *pipe;

    SDL_zero(vbd);
    vbd.slot = 0;
    vbd.pitch = 16;
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_zero(attrs);
    attrs[0].location = 0;
    attrs[0].buffer_slot = 0;
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].buffer_slot = 0;
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[1].offset = 8;

    SDL_zero(vin);
    vin.vertex_buffer_descriptions = &vbd;
    vin.num_vertex_buffers = 1;
    vin.vertex_attributes = attrs;
    vin.num_vertex_attributes = 2;

    S3_FillBlendState(&blendState, blend);

    SDL_zero(ctd);
    ctd.format = s_swapchainFormat;
    ctd.blend_state = blendState;

    SDL_zero(ds); /* no depth target */

    SDL_zero(rs);
    rs.fill_mode = SDL_GPU_FILLMODE_FILL;
    rs.cull_mode = SDL_GPU_CULLMODE_NONE;
    rs.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    rs.enable_depth_clip = true;

    SDL_zero(ms);
    ms.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_zero(ti);
    ti.color_target_descriptions = &ctd;
    ti.num_color_targets = 1;
    ti.has_depth_stencil_target = false;

    SDL_zero(ci);
    ci.vertex_shader = vs;
    ci.fragment_shader = fs;
    ci.vertex_input_state = vin;
    /* The composite quad is a 4-vertex strip (bottom-left, bottom-right,
       top-left, top-right), exactly like the GL backend's glDrawArrays
       GL_TRIANGLE_STRIP. As a list it would emit only the first triangle and
       leave the top-right half black. */
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    ci.rasterizer_state = rs;
    ci.multisample_state = ms;
    ci.depth_stencil_state = ds;
    ci.target_info = ti;

    pipe = SDL_CreateGPUGraphicsPipeline(s_device, &ci);
    if (pipe == NULL) {
        Log_Warn("[SDL3GPU] present pipeline (blend=%d) failed: %s",
                 (int)blend, SDL_GetError());
    }
    return pipe;
}

static S32 S3_CreatePipelines(void) {
    SDL_GPUShader *clutVS, *clutFS, *presVS, *presFS;
    S32 dt, dw, bl;
    S32 ok = TRUE;

    clutVS = S3_CreateShader("clut.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    clutFS = S3_CreateShader("clut.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
    presVS = S3_CreateShader("present.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    presFS = S3_CreateShader("present.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    if (clutVS == NULL || clutFS == NULL || presVS == NULL || presFS == NULL) {
        ok = FALSE;
    }

    for (dt = 0; dt <= 2 && ok; dt++) {
        for (dw = 0; dw <= 1 && ok; dw++) {
            for (bl = 0; bl <= 1 && ok; bl++) {
                U32 idx = (U32)(dt * 4 + dw * 2 + bl);
                s_clutPipelines[idx] = S3_CreateClutPipeline(
                    clutVS, clutFS, dt, dw, bl);
                if (s_clutPipelines[idx] == NULL) {
                    ok = FALSE;
                }
            }
        }
    }

    if (ok) {
        s_presentOpaque = S3_CreatePresentPipeline(presVS, presFS, 0);
        s_presentBlend = S3_CreatePresentPipeline(presVS, presFS, 1);
        if (s_presentOpaque == NULL || s_presentBlend == NULL) {
            ok = FALSE;
        }
    }

    if (clutVS != NULL) SDL_ReleaseGPUShader(s_device, clutVS);
    if (clutFS != NULL) SDL_ReleaseGPUShader(s_device, clutFS);
    if (presVS != NULL) SDL_ReleaseGPUShader(s_device, presVS);
    if (presFS != NULL) SDL_ReleaseGPUShader(s_device, presFS);
    return ok;
}

//──────────────────────────────────────────────────────────────────────────
// Resource creation
//──────────────────────────────────────────────────────────────────────────
static S32 S3_CreateSamplers(void) {
    SDL_GPUSamplerCreateInfo ci;

    SDL_zero(ci);
    ci.min_filter = SDL_GPU_FILTER_NEAREST;
    ci.mag_filter = SDL_GPU_FILTER_NEAREST;
    ci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ci.min_lod = 0.0f;
    ci.max_lod = 0.0f;
    s_samplerNearest = SDL_CreateGPUSampler(s_device, &ci);
    if (s_samplerNearest == NULL) {
        Log_Warn("[SDL3GPU] nearest sampler failed: %s", SDL_GetError());
        return FALSE;
    }

    ci.min_filter = SDL_GPU_FILTER_LINEAR;
    ci.mag_filter = SDL_GPU_FILTER_LINEAR;
    s_samplerLinear = SDL_CreateGPUSampler(s_device, &ci);
    if (s_samplerLinear == NULL) {
        Log_Warn("[SDL3GPU] linear sampler failed: %s", SDL_GetError());
        return FALSE;
    }
    return TRUE;
}

static SDL_GPUTexture *S3_CreateTexture(SDL_GPUTextureFormat format, U32 w,
                                        U32 h) {
    SDL_GPUTextureCreateInfo ci;
    SDL_zero(ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = format;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = w;
    ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(s_device, &ci);
}

static S32 S3_CreateTextures(void) {
    /* Atlas and CLUT are 8-bit index/light maps sampled as .r; the palette is
       the ARGB colours they resolve to. */
    s_atlasTex = S3_CreateTexture(SDL_GPU_TEXTUREFORMAT_R8_UNORM, 256, 256);
    s_clutTex = S3_CreateTexture(SDL_GPU_TEXTUREFORMAT_R8_UNORM, 256, 256);
    s_paletteTex =
        S3_CreateTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 256, 1);
    if (s_atlasTex == NULL || s_clutTex == NULL || s_paletteTex == NULL) {
        Log_Warn("[SDL3GPU] fixed texture create failed: %s", SDL_GetError());
        return FALSE;
    }
    return TRUE;
}

/* Maps the shared staging transfer buffer (growing it if needed) so `size`
   bytes can be written. SDL cycles the buffer so a copy the GPU is still
   reading is not overwritten. Write then call S3_UnmapStaging. */
static U8 *S3_MapStaging(U32 size) {
    SDL_GPUTransferBufferCreateInfo tci;

    if (s_device == NULL) {
        return NULL;
    }
    if (size > s_stagingBytes) {
        U32 newSize = (size + 0xFFFFu) & ~0xFFFFu;
        if (s_staging != NULL) {
            SDL_ReleaseGPUTransferBuffer(s_device, s_staging);
            s_staging = NULL;
        }
        SDL_zero(tci);
        tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tci.size = newSize;
        s_staging = SDL_CreateGPUTransferBuffer(s_device, &tci);
        if (s_staging == NULL) {
            s_stagingBytes = 0;
            return NULL;
        }
        s_stagingBytes = newSize;
    }
    return (U8 *)SDL_MapGPUTransferBuffer(s_device, s_staging, true);
}

static void S3_UnmapStaging(void) {
    if (s_staging != NULL) {
        SDL_UnmapGPUTransferBuffer(s_device, s_staging);
    }
}

/* Uploads into the frame command buffer (a copy pass). The caller must not
   have a render pass open; the helper closes it defensively. */
static S32 S3_UploadTextureData(SDL_GPUTexture *tex, const void *data, U32 width,
                                U32 height, U32 bpp, U32 srcPitch) {
    U32 rowBytes = width * bpp;
    U32 size = rowBytes * height;
    U8 *mapped;
    SDL_GPUCopyPass *cp;
    SDL_GPUTextureTransferInfo src;
    SDL_GPUTextureRegion dst;

    if (s_device == NULL || tex == NULL || data == NULL) {
        return FALSE;
    }

    /* Render the batches accumulated so far with the *current* texture before
       replacing it: a mid-frame texture change (per-tile fog, a new atlas
       page) must not retroactively repaint already-batched geometry. */
    S3_FlushDraws();

    if (s_frameCmd == NULL) {
        s_frameCmd = SDL_AcquireGPUCommandBuffer(s_device);
        if (s_frameCmd == NULL) {
            Log_Warn("[SDL3GPU] command buffer acquire failed: %s",
                     SDL_GetError());
            return FALSE;
        }
    }
    if (s_targetPass != NULL) {
        SDL_EndGPURenderPass(s_targetPass);
        s_targetPass = NULL;
    }

    mapped = S3_MapStaging(size);
    if (mapped == NULL) {
        Log_Warn("[SDL3GPU] transfer buffer create failed: %s", SDL_GetError());
        return FALSE;
    }
    if (srcPitch == 0 || srcPitch == rowBytes) {
        memcpy(mapped, data, size);
    } else {
        U32 y;
        for (y = 0; y < height; y++) {
            memcpy(mapped + y * rowBytes, (const U8 *)data + y * srcPitch,
                   rowBytes);
        }
    }
    S3_UnmapStaging();

    cp = SDL_BeginGPUCopyPass(s_frameCmd);
    SDL_zero(src);
    src.transfer_buffer = s_staging;
    src.offset = 0;
    src.pixels_per_row = width;
    src.rows_per_layer = height;
    SDL_zero(dst);
    dst.texture = tex;
    dst.w = width;
    dst.h = height;
    dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    return TRUE;
}

/* One-shot upload of a whole GPU buffer (used for the static quad). Runs on
   its own command buffer and presents nothing. */
static S32 S3_UploadBufferOnce(SDL_GPUBuffer *dst, const void *data,
                               U32 size) {
    SDL_GPUCommandBuffer *cmdbuf;
    SDL_GPUTransferBufferCreateInfo tci;
    SDL_GPUTransferBuffer *tb;
    void *mapped;
    SDL_GPUCopyPass *cp;
    SDL_GPUTransferBufferLocation src;
    SDL_GPUBufferRegion region;

    SDL_zero(tci);
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = size;
    tb = SDL_CreateGPUTransferBuffer(s_device, &tci);
    if (tb == NULL) {
        return FALSE;
    }
    mapped = SDL_MapGPUTransferBuffer(s_device, tb, false);
    if (mapped == NULL) {
        SDL_ReleaseGPUTransferBuffer(s_device, tb);
        return FALSE;
    }
    memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(s_device, tb);

    cmdbuf = SDL_AcquireGPUCommandBuffer(s_device);
    if (cmdbuf == NULL) {
        SDL_ReleaseGPUTransferBuffer(s_device, tb);
        return FALSE;
    }
    cp = SDL_BeginGPUCopyPass(cmdbuf);
    SDL_zero(src);
    src.transfer_buffer = tb;
    src.offset = 0;
    SDL_zero(region);
    region.buffer = dst;
    region.offset = 0;
    region.size = size;
    SDL_UploadToGPUBuffer(cp, &src, &region, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmdbuf);
    SDL_ReleaseGPUTransferBuffer(s_device, tb);
    return TRUE;
}

/* Grows a download transfer buffer to at least `bytes`. */
static S32 S3_EnsureReadbackTb(SDL_GPUTransferBuffer **tb, U32 *capacity,
                               U32 bytes) {
    SDL_GPUTransferBufferCreateInfo tci;

    if (*capacity >= bytes) {
        return TRUE;
    }
    if (*tb != NULL) {
        SDL_ReleaseGPUTransferBuffer(s_device, *tb);
        *tb = NULL;
        *capacity = 0;
    }
    SDL_zero(tci);
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tci.size = bytes;
    *tb = SDL_CreateGPUTransferBuffer(s_device, &tci);
    if (*tb == NULL) {
        Log_Warn("[SDL3GPU] readback transfer buffer create failed: %s",
                 SDL_GetError());
        return FALSE;
    }
    *capacity = bytes;
    return TRUE;
}

/* Copies one row-flipped (bottom-up) plane out of a mapped readback buffer.
   SDL3 GPU rows are top-down (negative-height viewport); the common readback
   flips them assuming GL's bottom-up glReadPixels order, so reverse here. */
static void S3_CopyReadbackPlane(void *dst, const void *mapped, U32 width,
                                 U32 height, U32 bpp) {
    const U32 stride = width * bpp;
    U32 y;

    for (y = 0; y < height; y++) {
        memcpy((U8 *)dst + y * stride,
               (const U8 *)mapped + (height - 1 - y) * stride, stride);
    }
}

/* Downloads the depth target and converts it to the GL window-depth
   convention the common layer inverts. Waits on this submit's fence rather
   than a full GPU idle so the swapchain's present is not serialised with it.
   Returns FALSE when depth is unavailable, leaving the engine buffer alone. */
static S32 S3_TargetReadDepth(void *depthDst, U32 width, U32 height) {
    U32 depthBytes;
    SDL_GPUCopyPass *cp;
    SDL_GPUTextureRegion src;
    SDL_GPUTextureTransferInfo dstInfo;
    SDL_GPUFence *fence;
    const void *mapped;
    U16 *samples;
    U32 count;
    U32 i;

    if (s_device == NULL || s_targetDepth == NULL || depthDst == NULL ||
        width == 0 || height == 0) {
        return FALSE;
    }

    /* The target pass must be closed before a copy pass can touch the frame's
       command buffer; rendering the queued terrain also fills the target. */
    S3_FlushDraws();
    if (s_frameCmd == NULL) {
        s_frameCmd = SDL_AcquireGPUCommandBuffer(s_device);
        if (s_frameCmd == NULL) {
            Log_Warn("[SDL3GPU] command buffer acquire failed: %s",
                     SDL_GetError());
            return FALSE;
        }
    }
    if (s_targetPass != NULL) {
        SDL_EndGPURenderPass(s_targetPass);
        s_targetPass = NULL;
    }

    depthBytes = width * height * 2;
    if (!S3_EnsureReadbackTb(&s_readbackDepthTb, &s_readbackDepthTbBytes,
                             depthBytes)) {
        return FALSE;
    }

    cp = SDL_BeginGPUCopyPass(s_frameCmd);
    SDL_zero(src);
    src.texture = s_targetDepth;
    src.w = width;
    src.h = height;
    src.d = 1;
    SDL_zero(dstInfo);
    dstInfo.transfer_buffer = s_readbackDepthTb;
    dstInfo.offset = 0;
    dstInfo.pixels_per_row = width;
    dstInfo.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(cp, &src, &dstInfo);
    SDL_EndGPUCopyPass(cp);

    /* The frame's command buffer ends at the readback and is reacquired for
       whatever still draws this frame. */
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(s_frameCmd);
    s_frameCmd = NULL;
    s_targetPass = NULL;
    if (fence == NULL) {
        Log_Warn("[SDL3GPU] readback submit failed: %s", SDL_GetError());
        return FALSE;
    }
    if (!SDL_WaitForGPUFences(s_device, true, &fence, 1)) {
        Log_Warn("[SDL3GPU] readback wait failed: %s", SDL_GetError());
        SDL_ReleaseGPUFence(s_device, fence);
        return FALSE;
    }
    SDL_ReleaseGPUFence(s_device, fence);

    mapped = SDL_MapGPUTransferBuffer(s_device, s_readbackDepthTb, false);
    if (mapped == NULL) {
        Log_Warn("[SDL3GPU] depth readback map failed: %s", SDL_GetError());
        return FALSE;
    }
    samples = (U16 *)depthDst;
    S3_CopyReadbackPlane(samples, mapped, width, height, 2);
    SDL_UnmapGPUTransferBuffer(s_device, s_readbackDepthTb);

    /* Raw D16 is the clip-space depth the shader wrote, 1 - aZ/65535, so
       sample = 65535 - aZ. The common layer applies the GL aZ inverse
       (aZ = 131070 - 2*glDepth) to whatever this returns; convert to that
       GL window-depth convention here (glDepth = 65535 - aZ/2) so one inverse
       serves both backends. */
    count = width * height;
    for (i = 0; i < count; i++) {
        samples[i] = (U16)((samples[i] + 65535u) >> 1);
    }
    return TRUE;
}

/* Colour readback: U32 0x00RRGGBB, bottom-up (flip SDL's top-down download
   with the same helper as depth). The colour target is B8G8R8A8; on LE that
   is already [B,G,R,A] in memory, mask to 0x00RRGGBB after the flip. */
static S32 S3_TargetReadColor(void *colorDst, U32 width, U32 height) {
    U32 colorBytes;
    SDL_GPUCopyPass *cp;
    SDL_GPUTextureRegion src;
    SDL_GPUTextureTransferInfo dstInfo;
    SDL_GPUFence *fence;
    const void *mapped;
    U32 *pixels;
    U32 count;
    U32 i;

    if (s_device == NULL || s_targetColor == NULL || colorDst == NULL ||
        width == 0 || height == 0) {
        return FALSE;
    }

    S3_FlushDraws();
    if (s_frameCmd == NULL) {
        s_frameCmd = SDL_AcquireGPUCommandBuffer(s_device);
        if (s_frameCmd == NULL) {
            Log_Warn("[SDL3GPU] command buffer acquire failed: %s",
                     SDL_GetError());
            return FALSE;
        }
    }
    if (s_targetPass != NULL) {
        SDL_EndGPURenderPass(s_targetPass);
        s_targetPass = NULL;
    }

    colorBytes = width * height * 4;
    if (!S3_EnsureReadbackTb(&s_readbackDepthTb, &s_readbackDepthTbBytes,
                             colorBytes)) {
        return FALSE;
    }

    cp = SDL_BeginGPUCopyPass(s_frameCmd);
    SDL_zero(src);
    src.texture = s_targetColor;
    src.w = width;
    src.h = height;
    src.d = 1;
    SDL_zero(dstInfo);
    dstInfo.transfer_buffer = s_readbackDepthTb;
    dstInfo.offset = 0;
    dstInfo.pixels_per_row = width;
    dstInfo.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(cp, &src, &dstInfo);
    SDL_EndGPUCopyPass(cp);

    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(s_frameCmd);
    s_frameCmd = NULL;
    s_targetPass = NULL;
    if (fence == NULL) {
        Log_Warn("[SDL3GPU] colour readback submit failed: %s",
                 SDL_GetError());
        return FALSE;
    }
    if (!SDL_WaitForGPUFences(s_device, true, &fence, 1)) {
        Log_Warn("[SDL3GPU] colour readback wait failed: %s",
                 SDL_GetError());
        SDL_ReleaseGPUFence(s_device, fence);
        return FALSE;
    }
    SDL_ReleaseGPUFence(s_device, fence);

    mapped = SDL_MapGPUTransferBuffer(s_device, s_readbackDepthTb, false);
    if (mapped == NULL) {
        Log_Warn("[SDL3GPU] colour readback map failed: %s",
                 SDL_GetError());
        return FALSE;
    }
    S3_CopyReadbackPlane(colorDst, mapped, width, height, 4);
    SDL_UnmapGPUTransferBuffer(s_device, s_readbackDepthTb);

    pixels = (U32 *)colorDst;
    count = width * height;
    for (i = 0; i < count; i++) {
        pixels[i] &= 0x00FFFFFFu;
    }
    return TRUE;
}

/* Copies the offscreen target (colour + depth) into the persistent terrain
   target. Texture-to-texture copies of the same format, recorded in one copy
   pass after whatever the target still holds has been rendered. */
static S32 S3_TargetSnapshotTerrain(void) {
    SDL_GPUCopyPass *cp;
    SDL_GPUTextureLocation src;
    SDL_GPUTextureLocation dst;

    if (s_device == NULL || s_targetColor == NULL || s_targetDepth == NULL ||
        s_terrainColor == NULL || s_terrainDepth == NULL) {
        return FALSE;
    }

    /* Render the terrain tiles queued so far so the target holds the complete
       pass before it is copied. */
    S3_FlushDraws();

    if (s_frameCmd == NULL) {
        s_frameCmd = SDL_AcquireGPUCommandBuffer(s_device);
        if (s_frameCmd == NULL) {
            Log_Warn("[SDL3GPU] command buffer acquire failed (terrain "
                     "snapshot): %s", SDL_GetError());
            return FALSE;
        }
    }
    if (s_targetPass != NULL) {
        SDL_EndGPURenderPass(s_targetPass);
        s_targetPass = NULL;
    }

    cp = SDL_BeginGPUCopyPass(s_frameCmd);
    SDL_zero(src);
    src.texture = s_targetColor;
    SDL_zero(dst);
    dst.texture = s_terrainColor;
    SDL_CopyGPUTextureToTexture(cp, &src, &dst, s_targetW, s_targetH, 1, false);
    src.texture = s_targetDepth;
    dst.texture = s_terrainDepth;
    SDL_CopyGPUTextureToTexture(cp, &src, &dst, s_targetW, s_targetH, 1, false);
    SDL_EndGPUCopyPass(cp);
    return TRUE;
}

/* Copies the stored terrain colour back over the offscreen target. This runs
   at the head of each AFF_OBJETS exterior frame, *before* the frame's queued
   draws (animated polys, shadows) are replayed by the next S3_FlushDraws, so
   they paint over the scrubbed terrain instead of being wiped by it. */
static void S3_TargetRestoreTerrain(void) {
    SDL_GPUCopyPass *cp;
    SDL_GPUTextureLocation src;
    SDL_GPUTextureLocation dst;

    if (s_device == NULL || s_targetColor == NULL || s_terrainColor == NULL) {
        return;
    }

    if (s_frameCmd == NULL) {
        s_frameCmd = SDL_AcquireGPUCommandBuffer(s_device);
        if (s_frameCmd == NULL) {
            Log_Warn("[SDL3GPU] command buffer acquire failed (terrain "
                     "restore): %s", SDL_GetError());
            return;
        }
    }
    if (s_targetPass != NULL) {
        SDL_EndGPURenderPass(s_targetPass);
        s_targetPass = NULL;
    }

    cp = SDL_BeginGPUCopyPass(s_frameCmd);
    SDL_zero(src);
    src.texture = s_terrainColor;
    SDL_zero(dst);
    dst.texture = s_targetColor;
    SDL_CopyGPUTextureToTexture(cp, &src, &dst, s_targetW, s_targetH, 1, false);
    SDL_EndGPUCopyPass(cp);
}

static S32 S3_CreateBuffers(void) {
    SDL_GPUBufferCreateInfo ci;

    SDL_zero(ci);
    ci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    ci.size = (U32)(S3_BATCH_MAX_VERTS * S3_VERTEX_STRIDE);
    s_batchVbo = SDL_CreateGPUBuffer(s_device, &ci);
    if (s_batchVbo == NULL) {
        Log_Warn("[SDL3GPU] batch vertex buffer failed: %s", SDL_GetError());
        return FALSE;
    }
    s_batchVboBytes = ci.size;

    ci.size = (U32)sizeof(s_quadVerts);
    s_quadVbo = SDL_CreateGPUBuffer(s_device, &ci);
    if (s_quadVbo == NULL) {
        Log_Warn("[SDL3GPU] quad vertex buffer failed: %s", SDL_GetError());
        return FALSE;
    }
    if (!S3_UploadBufferOnce(s_quadVbo, s_quadVerts,
                             (U32)sizeof(s_quadVerts))) {
        Log_Warn("[SDL3GPU] quad upload failed: %s", SDL_GetError());
        return FALSE;
    }
    return TRUE;
}

//──────────────────────────────────────────────────────────────────────────
// Context lifecycle
//──────────────────────────────────────────────────────────────────────────
static S32 S3_CreateContext(U32 resX, U32 resY) {
    SDL_GPUShaderFormat formats = 0;

    s_window = Window_GetSDLWindow();
    if (s_window == NULL) {
        Log_Warn("[SDL3GPU] no host window to claim");
        return FALSE;
    }
    s_resX = resX;
    s_resY = resY;
    s_presentW = resX;
    s_presentH = resY;

    /* SDL3 GPU drivers require different shader bytecode; build the device
       create mask from the formats this build embedded. Formats compiled
       out (no cross-compiler at configure time) drop their driver. */
#if SDL3GPU_SHADERFORMAT_SPIRV_AVAILABLE
    formats |= SDL_GPU_SHADERFORMAT_SPIRV;
#endif
#if SDL3GPU_SHADERFORMAT_MSL_AVAILABLE
    formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
#if SDL3GPU_SHADERFORMAT_DXIL_AVAILABLE
    formats |= SDL_GPU_SHADERFORMAT_DXIL;
#endif
    if (formats == 0) {
        Log_Warn("[SDL3GPU] no embedded shaders (is glslangValidator present "
                 "at configure time?)");
        return FALSE;
    }

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
        S3_DestroyContext();
        return FALSE;
    }

    s_swapchainFormat = SDL_GetGPUSwapchainTextureFormat(s_device, s_window);
    SDL_SetGPUSwapchainParameters(s_device, s_window,
                                  SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  s_presentMode);

    s_shaderFormat = S3_SelectShaderFormat();
    if (s_shaderFormat == 0) {
        Log_Warn("[SDL3GPU] device supports no embedded shader format");
        S3_DestroyContext();
        return FALSE;
    }

    if (!S3_CreateSamplers() || !S3_CreateTextures() || !S3_CreateBuffers() ||
        !S3_CreatePipelines()) {
        S3_DestroyContext();
        return FALSE;
    }

    Log_Debug("[SDL3GPU] context ready (%ux%u, swapchain 0x%X)",
              (unsigned)resX, (unsigned)resY, (unsigned)s_swapchainFormat);
    return TRUE;
}

/* Present pacing. IMMEDIATE uncaps the frame rate; VSYNC blocks the acquire
   on the display. Recorded even before the device exists so Init can apply
   the mode the config asked for. */
static void S3_SetVSync(S32 enabled) {
    s_presentMode = enabled ? SDL_GPU_PRESENTMODE_VSYNC
                            : SDL_GPU_PRESENTMODE_IMMEDIATE;
    if (s_device != NULL && s_window != NULL) {
        SDL_SetGPUSwapchainParameters(s_device, s_window,
                                      SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      s_presentMode);
    }
}

static void S3_DestroyContext(void) {
    U32 i;

    if (s_device == NULL) {
        return;
    }

    if (s_frameCmd != NULL) {
        SDL_CancelGPUCommandBuffer(s_frameCmd);
        s_frameCmd = NULL;
    }
    s_swapchainPass = NULL;
    s_targetPass = NULL;
    s_swapchainTex = NULL;

    S3_TargetDestroy();

    if (s_overlayTex != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_overlayTex);
        s_overlayTex = NULL;
    }
    if (s_atlasTex != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_atlasTex);
        s_atlasTex = NULL;
    }
    if (s_clutTex != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_clutTex);
        s_clutTex = NULL;
    }
    if (s_paletteTex != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_paletteTex);
        s_paletteTex = NULL;
    }
    if (s_brickTex != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_brickTex);
        s_brickTex = NULL;
        s_brickTexW = 0;
        s_brickTexH = 0;
    }
    s_overlayW = 0;
    s_overlayH = 0;

    if (s_samplerNearest != NULL) {
        SDL_ReleaseGPUSampler(s_device, s_samplerNearest);
        s_samplerNearest = NULL;
    }
    if (s_samplerLinear != NULL) {
        SDL_ReleaseGPUSampler(s_device, s_samplerLinear);
        s_samplerLinear = NULL;
    }

    for (i = 0; i < S3_CLUT_PIPELINE_COUNT; i++) {
        if (s_clutPipelines[i] != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(s_device, s_clutPipelines[i]);
            s_clutPipelines[i] = NULL;
        }
    }
    if (s_presentOpaque != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_presentOpaque);
        s_presentOpaque = NULL;
    }
    if (s_presentBlend != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_presentBlend);
        s_presentBlend = NULL;
    }

    if (s_batchVbo != NULL) {
        SDL_ReleaseGPUBuffer(s_device, s_batchVbo);
        s_batchVbo = NULL;
    }
    s_batchVboBytes = 0;
    if (s_quadVbo != NULL) {
        SDL_ReleaseGPUBuffer(s_device, s_quadVbo);
        s_quadVbo = NULL;
    }
    if (s_staging != NULL) {
        SDL_ReleaseGPUTransferBuffer(s_device, s_staging);
        s_staging = NULL;
    }
    s_stagingBytes = 0;
    if (s_readbackDepthTb != NULL) {
        SDL_ReleaseGPUTransferBuffer(s_device, s_readbackDepthTb);
        s_readbackDepthTb = NULL;
    }
    s_readbackDepthTbBytes = 0;
    if (s_frameVerts != NULL) {
        free(s_frameVerts);
        s_frameVerts = NULL;
    }
    s_frameVertCap = 0;
    s_frameVertCount = 0;
    s_drawCount = 0;

    s_atlasCacheValid = FALSE;
    s_clutCacheValid = FALSE;
    s_paletteCacheValid = FALSE;

    if (s_window != NULL) {
        SDL_ReleaseWindowFromGPUDevice(s_device, s_window);
    }
    SDL_DestroyGPUDevice(s_device);
    s_device = NULL;
    s_window = NULL;
    s_swapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    s_shaderFormat = 0;
    s_shaderBlobs = NULL;
    s_targetNeedsClear = TRUE;
}

//──────────────────────────────────────────────────────────────────────────
// Offscreen target
//──────────────────────────────────────────────────────────────────────────
static S32 S3_TargetCreate(U32 width, U32 height) {
    SDL_GPUTextureCreateInfo ci;

    if (s_device == NULL) {
        return FALSE;
    }

    SDL_zero(ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    /* B8G8R8A8 matches the CLUT pipeline's colour target and the palette and
       overlay textures, so every sampled and stored channel order agrees. */
    ci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ci.width = width;
    ci.height = height;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    /* SAMPLER is required for the composite to read it back as which=0. */
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
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

    /* Terrain snapshot textures at the same size and with the same formats, so
       the texture-to-texture copies (colour→colour, depth→depth) are legal. */
    ci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    s_terrainColor = SDL_CreateGPUTexture(s_device, &ci);
    if (s_terrainColor == NULL) {
        Log_Warn("[SDL3GPU] terrain color target create failed: %s",
                 SDL_GetError());
        SDL_ReleaseGPUTexture(s_device, s_targetColor);
        SDL_ReleaseGPUTexture(s_device, s_targetDepth);
        s_targetColor = NULL;
        s_targetDepth = NULL;
        return FALSE;
    }

    ci.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    s_terrainDepth = SDL_CreateGPUTexture(s_device, &ci);
    if (s_terrainDepth == NULL) {
        Log_Warn("[SDL3GPU] terrain depth target create failed: %s",
                 SDL_GetError());
        SDL_ReleaseGPUTexture(s_device, s_terrainColor);
        SDL_ReleaseGPUTexture(s_device, s_targetColor);
        SDL_ReleaseGPUTexture(s_device, s_targetDepth);
        s_terrainColor = NULL;
        s_targetColor = NULL;
        s_targetDepth = NULL;
        return FALSE;
    }

    s_targetW = width;
    s_targetH = height;
    /* Fresh textures hold undefined colour and depth; the first pass clears. */
    s_targetNeedsClear = TRUE;
    return TRUE;
}

static void S3_TargetDestroy(void) {
    if (s_device == NULL) {
        return;
    }
    /* Batches queued against this target have nowhere to land now; the next
       frame rebuilds them. */
    s_drawCount = 0;
    s_frameVertCount = 0;
    if (s_targetPass != NULL) {
        SDL_EndGPURenderPass(s_targetPass);
        s_targetPass = NULL;
    }
    if (s_targetColor != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_targetColor);
        s_targetColor = NULL;
    }
    if (s_targetDepth != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_targetDepth);
        s_targetDepth = NULL;
    }
    if (s_terrainColor != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_terrainColor);
        s_terrainColor = NULL;
    }
    if (s_terrainDepth != NULL) {
        SDL_ReleaseGPUTexture(s_device, s_terrainDepth);
        s_terrainDepth = NULL;
    }
    s_targetW = 0;
    s_targetH = 0;
}

static S32 S3_TargetIsValid(void) {
    return (s_targetColor != NULL && s_targetDepth != NULL) ? TRUE : FALSE;
}

static void S3_TargetBindClear(U32 rgb) {
    /* The clear is deferred to the first triangle pass of the frame so it
       shares the frame command buffer with the draws it precedes. */
    s_targetClearRgb = rgb;
    s_targetNeedsClear = TRUE;
}

static U32 S3_TargetWidth(void) {
    return s_targetW;
}

static U32 S3_TargetHeight(void) {
    return s_targetH;
}

//──────────────────────────────────────────────────────────────────────────
// Fixed-role textures
//──────────────────────────────────────────────────────────────────────────
static void S3_TexUpdateAtlas(const void *lum) {
    if (lum == NULL) {
        return;
    }
    if (s_atlasCacheValid && memcmp(s_atlasCache, lum, sizeof(s_atlasCache)) == 0) {
        return;
    }
    if (S3_UploadTextureData(s_atlasTex, lum, 256, 256, 1, 256)) {
        memcpy(s_atlasCache, lum, sizeof(s_atlasCache));
        s_atlasCacheValid = TRUE;
    }
}

static void S3_TexUpdateCLUT(const void *lum) {
    if (lum == NULL) {
        return;
    }
    if (s_clutCacheValid && memcmp(s_clutCache, lum, sizeof(s_clutCache)) == 0) {
        return;
    }
    if (S3_UploadTextureData(s_clutTex, lum, 256, 256, 1, 256)) {
        memcpy(s_clutCache, lum, sizeof(s_clutCache));
        s_clutCacheValid = TRUE;
    }
}

static void S3_TexUpdatePalette(const void *argb) {
    U32 paletteCopy[256];
    if (argb == NULL) {
        return;
    }
    /* Palette index 0 is the transparent colour (sprites/UI chroma key), so
       its alpha is forced to 0 exactly as the GL backend does. */
    memcpy(paletteCopy, argb, sizeof(paletteCopy));
    paletteCopy[0] &= 0x00FFFFFFu;

    if (s_paletteCacheValid &&
        memcmp(s_paletteCache, paletteCopy, sizeof(s_paletteCache)) == 0) {
        return;
    }
    if (S3_UploadTextureData(s_paletteTex, paletteCopy, 256, 1, 4, 0)) {
        memcpy(s_paletteCache, paletteCopy, sizeof(s_paletteCache));
        s_paletteCacheValid = TRUE;
    }
}

/* Brick atlas: R8G8_UNORM (R = palette index, G = valid). Recreated when
   the pack size changes; nearest sampler at bind time. */
static void S3_TexUpdateBricks(const void *rg, U32 width, U32 height) {
    if (rg == NULL || width == 0 || height == 0) {
        return;
    }
    if (s_brickTex != NULL &&
        (s_brickTexW != width || s_brickTexH != height)) {
        SDL_ReleaseGPUTexture(s_device, s_brickTex);
        s_brickTex = NULL;
    }
    if (s_brickTex == NULL) {
        s_brickTex =
            S3_CreateTexture(SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, width, height);
        if (s_brickTex == NULL) {
            Log_Warn("[SDL3GPU] brick texture create failed: %s",
                     SDL_GetError());
            s_brickTexW = 0;
            s_brickTexH = 0;
            return;
        }
        s_brickTexW = width;
        s_brickTexH = height;
    }
    S3_UploadTextureData(s_brickTex, rg, width, height, 2, width * 2);
}

static void S3_TexUpdatePresent(const void *argb, U32 pitch, U32 width,
                                U32 height, S32 linear) {
    (void)linear; /* chosen at composite bind time */

    if (s_device == NULL || argb == NULL || width == 0 || height == 0) {
        return;
    }

    s_presentW = width;
    s_presentH = height;

    if (s_overlayTex == NULL || s_overlayW != width || s_overlayH != height) {
        if (s_overlayTex != NULL) {
            SDL_ReleaseGPUTexture(s_device, s_overlayTex);
            s_overlayTex = NULL;
        }
        s_overlayTex = S3_CreateTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                                        width, height);
        if (s_overlayTex == NULL) {
            Log_Warn("[SDL3GPU] overlay texture create failed: %s",
                     SDL_GetError());
            s_overlayW = 0;
            s_overlayH = 0;
            return;
        }
        s_overlayW = width;
        s_overlayH = height;
    }

    S3_UploadTextureData(s_overlayTex, argb, width, height, 4, pitch);
}

//──────────────────────────────────────────────────────────────────────────
// 3D batch
//──────────────────────────────────────────────────────────────────────────
static U32 S3_ClutPipelineIndex(const GpuDrawParams *p) {
    S32 dt = p->depthTest;
    S32 dw;
    S32 bl;
    if (dt < 0) {
        dt = 0;
    } else if (dt > 2) {
        dt = 2;
    }
    dw = (dt != 0 && p->depthWrite) ? 1 : 0;
    bl = p->blend ? 1 : 0;
    return (U32)(dt * 4 + dw * 2 + bl);
}

/* Grows the CPU vertex arena so `extra` more vertices fit. */
static S32 S3_EnsureFrameVerts(U32 extra) {
    U32 need = s_frameVertCount + extra;
    U32 cap;
    float *grown;

    if (need <= s_frameVertCap) {
        return TRUE;
    }
    cap = (s_frameVertCap == 0) ? S3_BATCH_MAX_VERTS : s_frameVertCap;
    while (cap < need) {
        cap *= 2;
    }
    grown = (float *)realloc(s_frameVerts, (size_t)cap * 7 * sizeof(float));
    if (grown == NULL) {
        return FALSE;
    }
    s_frameVerts = grown;
    s_frameVertCap = cap;
    return TRUE;
}

/* Grows the batch vertex buffer when a frame holds more than it can. The old
   buffer stays valid for the passes already recorded against it; SDL defers
   its release until the GPU is done. */
static S32 S3_EnsureBatchBuffer(U32 bytes) {
    SDL_GPUBufferCreateInfo ci;
    U32 newSize;
    SDL_GPUBuffer *grown;

    if (bytes <= s_batchVboBytes) {
        return TRUE;
    }
    newSize = (s_batchVboBytes == 0)
                  ? (U32)(S3_BATCH_MAX_VERTS * S3_VERTEX_STRIDE)
                  : s_batchVboBytes;
    while (newSize < bytes) {
        newSize *= 2;
    }
    SDL_zero(ci);
    ci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    ci.size = newSize;
    grown = SDL_CreateGPUBuffer(s_device, &ci);
    if (grown == NULL) {
        Log_Warn("[SDL3GPU] batch buffer grow failed: %s", SDL_GetError());
        return FALSE;
    }
    if (s_batchVbo != NULL) {
        SDL_ReleaseGPUBuffer(s_device, s_batchVbo);
    }
    s_batchVbo = grown;
    s_batchVboBytes = newSize;
    return TRUE;
}

static void S3_BeginTargetPass(void) {
    SDL_GPUColorTargetInfo cti;
    SDL_GPUDepthStencilTargetInfo dti;
    SDL_GPULoadOp loadOp;

    if (s_targetPass != NULL || s_targetColor == NULL || s_targetDepth == NULL) {
        return;
    }
    if (s_frameCmd == NULL) {
        s_frameCmd = SDL_AcquireGPUCommandBuffer(s_device);
        if (s_frameCmd == NULL) {
            return;
        }
    }

    loadOp = s_targetNeedsClear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;

    SDL_zero(cti);
    cti.texture = s_targetColor;
    /* Scene clear RGB (exterior FogCoul): sky gaps outside the drawn
       horizon must match the software Cls fill, not black. */
    cti.clear_color.r = (float)((s_targetClearRgb >> 16) & 0xFF) / 255.0f;
    cti.clear_color.g = (float)((s_targetClearRgb >> 8) & 0xFF) / 255.0f;
    cti.clear_color.b = (float)(s_targetClearRgb & 0xFF) / 255.0f;
    cti.clear_color.a = 1.0f;
    cti.load_op = loadOp;
    cti.store_op = SDL_GPU_STOREOP_STORE;

    SDL_zero(dti);
    dti.texture = s_targetDepth;
    dti.clear_depth = 0.0f;
    dti.load_op = loadOp;
    dti.store_op = SDL_GPU_STOREOP_STORE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    s_targetPass = SDL_BeginGPURenderPass(s_frameCmd, &cti, 1, &dti);
    s_targetNeedsClear = FALSE;
}

/* Uploads the frame's accumulated vertices and replays its draw list in one
   copy pass and one render pass. Runs when a texture upload forces the target
   pass closed (rare; the uploads are deduped) and once more at present. */
static void S3_FlushDraws(void) {
    U32 bytes;
    U32 i;
    U8 *mapped;
    SDL_GPUCopyPass *cp;
    SDL_GPUTransferBufferLocation src;
    SDL_GPUBufferRegion dst;
    SDL_GPUViewport vp;
    SDL_Rect scissor;
    SDL_GPUBufferBinding vb;
    SDL_GPUTextureSamplerBinding samplers[3];

    if (s_drawCount == 0) {
        return;
    }
    if (s_device == NULL) {
        s_drawCount = 0;
        s_frameVertCount = 0;
        return;
    }
    if (s_frameCmd == NULL) {
        s_frameCmd = SDL_AcquireGPUCommandBuffer(s_device);
        if (s_frameCmd == NULL) {
            s_drawCount = 0;
            s_frameVertCount = 0;
            return;
        }
    }

    bytes = s_frameVertCount * S3_VERTEX_STRIDE;
    if (!S3_EnsureBatchBuffer(bytes)) {
        s_drawCount = 0;
        s_frameVertCount = 0;
        return;
    }

    mapped = S3_MapStaging(bytes);
    if (mapped == NULL) {
        Log_Warn("[SDL3GPU] batch staging failed: %s", SDL_GetError());
        s_drawCount = 0;
        s_frameVertCount = 0;
        return;
    }
    memcpy(mapped, s_frameVerts, bytes);
    S3_UnmapStaging();

    cp = SDL_BeginGPUCopyPass(s_frameCmd);
    SDL_zero(src);
    src.transfer_buffer = s_staging;
    src.offset = 0;
    SDL_zero(dst);
    dst.buffer = s_batchVbo;
    dst.offset = 0;
    dst.size = bytes;
    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    SDL_EndGPUCopyPass(cp);

    S3_BeginTargetPass();
    if (s_targetPass == NULL) {
        s_drawCount = 0;
        s_frameVertCount = 0;
        return;
    }

    SDL_zero(vp);
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.w = (float)s_targetW;
    vp.h = (float)s_targetH;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    SDL_SetGPUViewport(s_targetPass, &vp);

    SDL_zero(scissor);
    scissor.w = (int)s_targetW;
    scissor.h = (int)s_targetH;
    SDL_SetGPUScissor(s_targetPass, &scissor);

    SDL_zero(vb);
    vb.buffer = s_batchVbo;
    vb.offset = 0;
    SDL_BindGPUVertexBuffers(s_targetPass, 0, &vb, 1);

    samplers[0].texture = s_atlasTex;
    samplers[0].sampler = s_samplerNearest;
    samplers[1].texture = s_clutTex;
    samplers[1].sampler = s_samplerNearest;
    samplers[2].texture = s_paletteTex;
    samplers[2].sampler = s_samplerNearest;
    SDL_BindGPUFragmentSamplers(s_targetPass, 0, samplers, 3);

    {
        /* Mode 7 samples the brick page through uAtlas; the bind is per
           draw so a frame that mixes brick quads with other polys (shadows
           use no atlas, terrain never coexists with an interior room) still
           samples the right texture for each. */
        S32 boundBrick = 0;
        for (i = 0; i < s_drawCount; i++) {
            const S3DrawCmd *cmd = &s_draws[i];
            S3ScreenParams sp;
            S32 wantBrick = (cmd->params.polyMode == 7);

            if (wantBrick != boundBrick) {
                samplers[0].texture = wantBrick ? s_brickTex : s_atlasTex;
                samplers[0].sampler = s_samplerNearest;
                SDL_BindGPUFragmentSamplers(s_targetPass, 0, samplers, 3);
                boundBrick = wantBrick;
            }
            /* Scissor to the software Fill_Poly clip window (engine pixels
               through screenScale into target pixels). SDL3 GPU render
               targets use a top-left origin, same as the engine rows. */
            {
                const float engW = 2.0f / cmd->screenScale[0];
                const float engH = 2.0f / cmd->screenScale[1];
                const float sx = (float)s_targetW / engW;
                const float sy = (float)s_targetH / engH;
                const float x0 = (float)cmd->clipX0 * sx;
                const float y0 = (float)cmd->clipY0 * sy;
                const float w = (float)(cmd->clipX1 - cmd->clipX0 + 1) * sx;
                const float h = (float)(cmd->clipY1 - cmd->clipY0 + 1) * sy;
                scissor.x = (int)x0;
                scissor.y = (int)y0;
                scissor.w = (w > 0.0f) ? (int)w : 0;
                scissor.h = (h > 0.0f) ? (int)h : 0;
                SDL_SetGPUScissor(s_targetPass, &scissor);
            }
            sp.screenScale[0] = cmd->screenScale[0];
            sp.screenScale[1] = cmd->screenScale[1];
            sp.pad[0] = 0.0f;
            sp.pad[1] = 0.0f;
            SDL_BindGPUGraphicsPipeline(s_targetPass,
                                        s_clutPipelines[cmd->pipelineIndex]);
            SDL_PushGPUVertexUniformData(s_frameCmd, 0, &sp, sizeof(sp));
            SDL_PushGPUFragmentUniformData(s_frameCmd, 0, &cmd->params,
                                           sizeof(cmd->params));
            SDL_DrawGPUPrimitives(s_targetPass, cmd->vertexCount, 1,
                                  cmd->firstVertex, 0);
        }
    }

    SDL_EndGPURenderPass(s_targetPass);
    s_targetPass = NULL;

    s_drawCount = 0;
    s_frameVertCount = 0;
}

static void S3_DrawTriangles(const GpuVertex *verts, U32 vertexCount,
                             const GpuDrawParams *params) {
    U32 i;
    U32 first;
    U32 pipelineIndex;
    S3DrawCmd *cmd;

    if (s_device == NULL || verts == NULL || params == NULL ||
        vertexCount == 0) {
        return;
    }
    if (vertexCount > S3_BATCH_MAX_VERTS) {
        vertexCount = S3_BATCH_MAX_VERTS;
    }

    pipelineIndex = S3_ClutPipelineIndex(params);
    if (s_clutPipelines[pipelineIndex] == NULL) {
        return;
    }

    /* The draw list is one render pass; flush it early if a single frame
       somehow outgrows the table. */
    if (s_drawCount >= S3_DRAW_CMD_MAX) {
        S3_FlushDraws();
    }
    if (!S3_EnsureFrameVerts(vertexCount)) {
        return;
    }

    /* Repack the 14-byte packed vertex into the float layout the shaders
       consume (SDL3 GPU has no 16-bit integer vertex formats). */
    first = s_frameVertCount;
    for (i = 0; i < vertexCount; i++) {
        const GpuVertex *v = &verts[i];
        float *o = &s_frameVerts[(first + i) * 7];
        o[0] = (float)v->XE;
        o[1] = (float)v->YE;
        o[2] = (float)v->MapU;
        o[3] = (float)v->MapV;
        o[4] = (float)v->Light;
        o[5] = (float)v->ZO;
        o[6] = (float)v->RepMask;
    }
    s_frameVertCount += vertexCount;

    cmd = &s_draws[s_drawCount++];
    cmd->pipelineIndex = pipelineIndex;
    cmd->firstVertex = first;
    cmd->vertexCount = vertexCount;
    cmd->screenScale[0] = params->screenScale[0];
    cmd->screenScale[1] = params->screenScale[1];
    cmd->clipX0 = params->clipX0;
    cmd->clipY0 = params->clipY0;
    cmd->clipX1 = params->clipX1;
    cmd->clipY1 = params->clipY1;
    cmd->params.polyMode = params->polyMode;
    cmd->params.alphaMode = params->alphaMode;
    cmd->params.flatColor = params->flatColor;
    cmd->params.bilinear = params->bilinear;
    cmd->params.perspCorrect = params->perspCorrect;
    cmd->params.chromaKey = params->chromaKey;
    cmd->params.clutBaseRow = params->clutBaseRow;
    cmd->params.scaledFogNear = params->scaledFogNear;
    cmd->params.fogRowScale = params->fogRowScale;
    cmd->params.brickW = (float)s_brickTexW;
    cmd->params.brickH = (float)s_brickTexH;
    cmd->params.pad = 0.0f;
}

//──────────────────────────────────────────────────────────────────────────
// Frame composite
//──────────────────────────────────────────────────────────────────────────
static void S3_ComputeLetterbox(U32 winW, U32 winH, S32 *outX, S32 *outY,
                                U32 *outW, U32 *outH) {
    U32 width = (s_presentW != 0) ? s_presentW : s_resX;
    U32 height = (s_presentH != 0) ? s_presentH : s_resY;
    double windowAspect;
    double contentAspect;

    if (winW == 0 || winH == 0 || width == 0 || height == 0) {
        *outX = 0;
        *outY = 0;
        *outW = winW;
        *outH = winH;
        return;
    }

    windowAspect = (double)winW / (double)winH;
    contentAspect = (double)width / (double)height;

    if (windowAspect > contentAspect) {
        /* Window wider than content: pillarbox. */
        U32 w = (U32)((double)winH * contentAspect + 0.5);
        w = (w + 1u) & ~1u;
        if (w > winW) {
            w = winW;
        }
        *outW = w;
        *outH = winH;
        *outX = (S32)((winW - w) / 2);
        *outY = 0;
    } else {
        /* Window taller than content: letterbox. */
        U32 h = (U32)((double)winW / contentAspect + 0.5);
        h = (h + 1u) & ~1u;
        if (h > winH) {
            h = winH;
        }
        *outW = winW;
        *outH = h;
        *outX = 0;
        *outY = (S32)((winH - h) / 2);
    }
}

static void S3_SubmitFrame(void) {
    if (s_swapchainPass != NULL) {
        SDL_EndGPURenderPass(s_swapchainPass);
        s_swapchainPass = NULL;
    }
    if (s_frameCmd != NULL) {
        SDL_SubmitGPUCommandBuffer(s_frameCmd);
        s_frameCmd = NULL;
    }
    s_swapchainTex = NULL;
    s_swapchainW = 0;
    s_swapchainH = 0;
    s_targetPass = NULL;
    s_drawCount = 0;
    s_frameVertCount = 0;
}

static void S3_CancelFrame(void) {
    if (s_swapchainPass != NULL) {
        SDL_EndGPURenderPass(s_swapchainPass);
        s_swapchainPass = NULL;
    }
    if (s_frameCmd != NULL) {
        SDL_CancelGPUCommandBuffer(s_frameCmd);
        s_frameCmd = NULL;
    }
    s_swapchainTex = NULL;
    s_swapchainW = 0;
    s_swapchainH = 0;
    s_targetPass = NULL;
    s_drawCount = 0;
    s_frameVertCount = 0;
}

static void S3_ClearBackbuffer(void) {
    SDL_GPUTexture *swapchainTex = NULL;
    U32 sw = 0;
    U32 sh = 0;
    SDL_GPUColorTargetInfo cti;

    /* Any batch still queued belongs to the offscreen target the composite is
       about to sample, so render it before the window pass opens. */
    S3_FlushDraws();

    if (s_device == NULL || s_swapchainPass != NULL) {
        return;
    }

    /* Clear the offscreen target if nothing drew to it this frame, so the
       composite never samples a stale image. */
    if (s_targetNeedsClear && s_targetColor != NULL) {
        S3_BeginTargetPass();
        if (s_targetPass != NULL) {
            SDL_EndGPURenderPass(s_targetPass);
            s_targetPass = NULL;
        }
    }

    if (s_frameCmd == NULL) {
        s_frameCmd = SDL_AcquireGPUCommandBuffer(s_device);
        if (s_frameCmd == NULL) {
            return;
        }
    }

    if (!SDL_WaitAndAcquireGPUSwapchainTexture(s_frameCmd, s_window,
                                               &swapchainTex, &sw, &sh)) {
        Log_Warn("[SDL3GPU] swapchain acquire failed: %s", SDL_GetError());
        S3_CancelFrame();
        return;
    }
    if (swapchainTex == NULL) {
        /* Not an error: the swapchain is unavailable (e.g. minimised). */
        S3_CancelFrame();
        return;
    }

    S3_ComputeLetterbox(sw, sh, &s_vpX, &s_vpY, &s_vpW, &s_vpH);

    SDL_zero(cti);
    cti.texture = swapchainTex;
    cti.clear_color.r = 0.0f;
    cti.clear_color.g = 0.0f;
    cti.clear_color.b = 0.0f;
    cti.clear_color.a = 1.0f;
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;

    s_swapchainPass = SDL_BeginGPURenderPass(s_frameCmd, &cti, 1, NULL);
    s_swapchainTex = swapchainTex;
    s_swapchainW = sw;
    s_swapchainH = sh;
}

static void S3_DrawPresentQuad(S32 which, S32 linear) {
    SDL_GPUGraphicsPipeline *pipe;
    SDL_GPUTexture *tex;
    SDL_GPUSampler *sampler;
    SDL_GPUViewport vp;
    SDL_Rect scissor;
    SDL_GPUBufferBinding vb;
    SDL_GPUTextureSamplerBinding bind;

    if (s_swapchainPass == NULL) {
        return;
    }

    if (which == 0) {
        if (s_targetColor == NULL) {
            return;
        }
        pipe = s_presentOpaque;
        tex = s_targetColor;
        sampler = s_samplerLinear; /* FBO source is always linear */
    } else {
        if (s_overlayTex == NULL) {
            S3_SubmitFrame();
            return;
        }
        pipe = s_presentBlend;
        tex = s_overlayTex;
        sampler = linear ? s_samplerLinear : s_samplerNearest;
    }
    if (pipe == NULL) {
        if (which != 0) {
            S3_SubmitFrame();
        }
        return;
    }

    SDL_BindGPUGraphicsPipeline(s_swapchainPass, pipe);

    SDL_zero(vp);
    vp.x = (float)s_vpX;
    vp.y = (float)s_vpY;
    vp.w = (float)s_vpW;
    vp.h = (float)s_vpH;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    SDL_SetGPUViewport(s_swapchainPass, &vp);

    SDL_zero(scissor);
    scissor.x = s_vpX;
    scissor.y = s_vpY;
    scissor.w = (int)s_vpW;
    scissor.h = (int)s_vpH;
    SDL_SetGPUScissor(s_swapchainPass, &scissor);

    SDL_zero(vb);
    vb.buffer = s_quadVbo;
    vb.offset = 0;
    SDL_BindGPUVertexBuffers(s_swapchainPass, 0, &vb, 1);

    bind.texture = tex;
    bind.sampler = sampler;
    SDL_BindGPUFragmentSamplers(s_swapchainPass, 0, &bind, 1);

    SDL_DrawGPUPrimitives(s_swapchainPass, 4, 1, 0, 0);

    if (which != 0) {
        S3_SubmitFrame();
    }
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
    S3_TargetSnapshotTerrain,
    S3_TargetRestoreTerrain,
    S3_SetVSync,
    S3_TexUpdatePresent,
    S3_TexUpdateAtlas,
    S3_TexUpdateCLUT,
    S3_TexUpdatePalette,
    S3_TexUpdateBricks,
    S3_DrawTriangles,
    S3_ClearBackbuffer,
    S3_DrawPresentQuad,
};
