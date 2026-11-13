// Frame generation with AMD FSR 3.1 (TOTK_FRAME_GEN, totk.ini frame_gen): between every two frames the game draws,
// one more is made from them and shown halfway in time, so the picture moves twice as often as the game draws. It is
// AMD's frame interpolation of the FidelityFX SDK 1.1.4, from the library vk_fsr3.c loads (vk_ffx.h), and runs on any
// GPU. A generated frame is a guess: what the two frames around it do not show (a place something uncovered) is filled
// in, and the less the game draws per second, the further apart the two frames are.
//
// The library offers a replacement swapchain that makes, paces and presents the generated frames on threads of its own.
// It is not used: the port already presents on a thread of its own that holds each frame until its display time
// (vk_backend.c present_main), and the library's frame generation context also works alone
// (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY; ffx-api/src/ffx_provider_framegeneration.cpp Dispatch needs
// nothing of the swapchain). So this file only makes the picture between the last frame and this one, and
// gfx_present.c shows it before this frame's.
//
// Three steps a frame:
// 1. vk_fg_scene, in the replay, where the game is about to draw its HUD (vk_dlaa.c finds the place): the game's float
//    target is copied, and the library is given that copy with the scene's depth and motion vectors, the ones the
//    upscaler takes. It keeps a copy as the next call's "last frame", estimates by optical flow the motion the
//    vectors do not cover (shadows, particles, what the game animates in a shader), moves both pictures halfway and
//    blends them. The HUD is in neither picture: hearts and the map would otherwise be dragged along with whatever
//    moves behind them, which is the best known flaw of generated frames.
// 2. vk_fg_take, when the present is queued: the frame's two pictures go along with it.
// 3. vk_fg_compose, on the present thread: this frame's HUD goes onto the generated picture (fg_compose.comp).
// The game's float target is linear light with the HUD's white at 1; its last draw stores it sRGB-encoded in the window
// texture, whose bytes the present shows. For the library the pictures are therefore scRGB with 80 nits per unit,
// the transfer function that leaves linear values as they are (ffx_frameinterpolation_common.h ffxscRGBToLinear), and
// vk_fg_compose encodes the generated picture as that last draw would. (The first build of this passed the float
// values to the screen unencoded: every generated frame came out dark and the picture flickered.)
//
// A frame is not generated, and the game's own frame is shown alone, when the scene was not drawn or has no camera
// (menus, loading), when the picture does not continue the last one (the first frame, another size, a frame without
// motion before it), and for the rest of the run once the library refused a call. With HDR output (TOTK_HDR) the
// pictures are the same, and the generated one is made as linear float light for the HDR present (fg_compose_hdr.comp).
//
// The library's context is made for one picture size and made again when that or the depth convention changes or the
// scene grows. Its calls all come from the replay, as the upscaler's do.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fg_compose_hdr_spv.h"
#include "fg_compose_spv.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_ffx11.h"
#include "vk_fg.h"
#include "vk_ffx.h"
// AMD's header declares an enumeration without values (FfxApiConfigureFrameGenerationKey), which C++ compilers let
// pass and C does not. The header stays as AMD ships it: for its length the name stands for an enumeration with one
// value followed by the tag of an empty structure, which takes the header's empty braces.
#define FfxApiConfigureFrameGenerationKey FfxApiConfigureFrameGenerationKey { FFX_API_CONFIGURE_FG_KEY_NONE }; struct ffx_api_fg_no_keys
#include "third_party/ffx_api/ffx_framegeneration.h"
#undef FfxApiConfigureFrameGenerationKey

#define SLOTS 8

struct Pic {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
};

static struct {
    // ---- the library
    ffxContext ctx;
    uint32_t ow, oh, mw, mh;   // the picture's size; the largest scene
    int inverted, infinite;
    uint64_t id;               // the library's frame number: one more per call
    uint64_t made, frames, shown;
    volatile int32_t failed;
    volatile int32_t live;     // above 0: one of the last 8 frames queued for the present had a generated picture
    // ---- the replay's pictures
    struct Pic before, gen;    // the picture before the HUD, the generated one: RGBA16F, w x h
    uint32_t w, h;
    int fed;                   // this frame's picture went to the library
    int chain;                 // so did the frame's before it: the generated picture is the one between the two
    PlatMutex lock;            // cur: the replay writes it, the thread that queues the present takes it
    VkFgFrame cur;
    int cur_ok;
    // why the frames were not generated (counted in vk_fg_take, logged every 600): index = the cause below
    int cur_why;
    uint64_t why_n[5], takes;
    char line[160];             // the last frames, one letter each (vk_fg_take): G generated, c no camera, x refused, r restart, u no scene
    // ---- the present's pass
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe, pipe_hdr;   // fg_compose.comp (8-bit sRGB bytes) and fg_compose_hdr.comp (linear float)
    VkSampler nearest;
    struct Pic out, out_hdr;   // RGBA8 and RGBA16F, ow x oh
    uint32_t out_w, out_h, out_hdr_w, out_hdr_h;
    VkImageView pic_view[SLOTS], win_view[SLOTS];   // the views each slot's last frame read
} G = {.lock = PLAT_MUTEX_INIT, .cur_why = 4};

int vk_fg_wanted(void) {
    if (!cfg_bool("TOTK_FRAME_GEN") || __atomic_load_n(&G.failed, __ATOMIC_RELAXED)) return 0;
    return vk_ffx() != NULL;
}

// TOTK_FG_DEBUG (test, docs/settings.md), read at each frame: 1 and 2 are fg_compose.comp's views, 3 the library's.
static int debug_view(void) {
    const char *d = cfg_get("TOTK_FG_DEBUG");
    return d ? atoi(d) : 0;
}

static void fail(const char *what, unsigned code) {
    if (!plat_xchg32(&G.failed, 1)) rt_log("fg: %s (%u): frame generation off for this run", what, code);
}

// ---------------------------------------------------------------- images
static int pic_make(struct Pic *p, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *p = (struct Pic){0};
    if (vmaCreateImage(c->vma, &ii, &ai, &p->img, &p->mem, NULL) != VK_SUCCESS) return p->img = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = p->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &p->view) == VK_SUCCESS;
}

static void pic_free(struct Pic *p) {
    const VkCtx *c = vk_ctx();
    if (p->view) vkDestroyImageView(c->device, p->view, NULL);
    if (p->img) vmaDestroyImage(c->vma, p->img, p->mem);
    *p = (struct Pic){0};
}

static void pics_free(void *arg) {
    struct Pic *p = arg;
    pic_free(&p[0]), pic_free(&p[1]);
    free(p);
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static void memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The two pictures of the replay at w x h, in GENERAL. The pair they replace was read by the presents of the frames
// before this one, so it goes when the GPU has finished this submission, which comes after them. 0: no memory.
static int pics_for(VkExec *ex, uint32_t w, uint32_t h) {
    if (G.before.img && G.w == w && G.h == h) return 1;
    const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct Pic n[2], *old = malloc(2 * sizeof *old);
    int ok = pic_make(&n[0], w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u);
    ok = ok && pic_make(&n[1], w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u);
    if (!ok || !old) {
        pic_free(&n[0]);
        if (ok || n[1].img) pic_free(&n[1]);
        free(old);
        return 0;
    }
    old[0] = G.before, old[1] = G.gen;
    G.before = n[0], G.gen = n[1], G.w = w, G.h = h;
    if (old[0].img) vk_completion_add(ex->fence, pics_free, old);
    else free(old);
    image_barrier(ex->cb, G.before.img, VK_IMAGE_LAYOUT_UNDEFINED);
    image_barrier(ex->cb, G.gen.img, VK_IMAGE_LAYOUT_UNDEFINED);
    ex->recorded += 2;
    G.chain = G.fed = 0;   // the library's last picture is another size's
    return 1;
}

// ---------------------------------------------------------------- the library
// Which library generates: 1 FSR 4 frame generation through the FSR SDK loader (vk_ffx.h vk_ffx_fg_pick), 0 FSR 3.1
// frame generation of amd_fidelityfx_vk.dll (vk_fsr3.c vk_ffx), -1 not decided yet (at the first context).
static int ml = -1;
static uint32_t fx_configure(void *desc) {
    return ml > 0 ? vk_ffx_fg_configure(&G.ctx, desc) : vk_ffx()->configure(&G.ctx, (ffxConfigureDescHeader *)desc);
}
static uint32_t fx_dispatch(void *desc) {
    return ml > 0 ? vk_ffx_fg_dispatch(&G.ctx, desc) : vk_ffx()->dispatch(&G.ctx, (ffxDispatchDescHeader *)desc);
}

static void ctx_destroy(void) {
    if (!G.ctx) return;
    vkDeviceWaitIdle(vk_ctx()->device);   // its images may still be read by the frames before this one
    if (ml > 0) vk_ffx_fg_destroy(&G.ctx);
    else vk_ffx()->destroy(&G.ctx, NULL);
    G.ctx = NULL;
}

static int ctx_for(const VkFgScene *s, uint32_t ow, uint32_t oh, int infinite) {
    if (G.ctx && ow == G.ow && oh == G.oh && s->w <= G.mw && s->h <= G.mh && s->inverted == G.inverted &&
        infinite == G.infinite)
        return 1;
    int same = G.ctx && ow == G.ow && oh == G.oh;
    ctx_destroy();
    if (ml < 0) {
        char nm[64];
        ml = vk_ffx_fg_pick(nm, sizeof nm);
        rt_log("fg: generating with %s", ml ? nm : "FSR Frame Generation 3.1 (amd_fidelityfx_vk.dll)");
    }
    const VkFfx *fx = vk_ffx();
    uint32_t mw = same && G.mw > s->w ? G.mw : s->w, mh = same && G.mh > s->h ? G.mh : s->h;
    struct ffxCreateBackendVKDesc backend;
    vk_ffx_backend(&backend);
    struct ffxCreateContextDescFrameGeneration cd = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, .pNext = &backend.header},
        .flags = (s->inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) |
                 (infinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0),
        .displaySize = {ow, oh}, .maxRenderSize = {mw, mh},
        .backBufferFormat = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT};
    ffxReturnCode_t r;
    if (ml) {
        cd.header.pNext = NULL;   // vk_ffx_fg_create chains the loader's backend and versions
        r = vk_ffx_fg_create(&G.ctx, &cd.header);
    } else {
        r = fx->create(&G.ctx, &cd.header, NULL);
    }
    if (r != FFX_API_RETURN_OK || !G.ctx) {
        G.ctx = NULL;
        return fail("the library made no frame generation context", (unsigned)r), 0;
    }
    struct ffxConfigureDescGlobalDebug1 dbg = {.header = {.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1},
                                               .fpMessage = vk_ffx_message,
                                               .debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS};
    fx_configure(&dbg.header);
    G.ow = ow, G.oh = oh, G.mw = mw, G.mh = mh, G.inverted = s->inverted, G.infinite = infinite;
    G.chain = 0;   // a new context has no last picture
    rt_log("fg: context %llu for %ux%u pictures and scenes up to %ux%u%s%s", (unsigned long long)++G.made, ow, oh, mw, mh,
           s->inverted ? ", inverted depth" : "", infinite ? ", no far plane" : "");
    return 1;
}

// The picture between the library's last one and G.before, into G.gen. 0: the library refused.
static int generate(VkCommandBuffer cb, const VkFgScene *s, uint32_t ow, uint32_t oh, int reset, float dt_ms) {
    if (!ctx_for(s, ow, oh, s->far_m >= 1e8f)) return 0;
    uint64_t id = ++G.id;
    int view = debug_view() == 3;   // the library's debug view
    uint32_t flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY |
                     (view ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0);
    // nothing of a swapchain, and no picture with the HUD: the HUD goes back on in vk_fg_compose
    struct ffxConfigureDescFrameGeneration cf = {.header = {.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION},
                                                 .frameGenerationEnabled = true, .flags = flags,
                                                 .generationRect = {0, 0, (int32_t)ow, (int32_t)oh}, .frameID = id};
    ffxReturnCode_t r = fx_configure(&cf.header);
    if (r != FFX_API_RETURN_OK) return fail("the library refused the frame's settings", (unsigned)r), 0;
    // FSR 4 frame generation takes the prepare description of the SDK 2.4 (PrepareV2): the same fields with the camera's
    // place and axes after them
    struct { struct ffxDispatchDescFrameGenerationPrepare p; float pos[3], up[3], right[3], fwd[3]; } p2;
    memset(&p2, 0, sizeof p2);
    struct ffxDispatchDescFrameGenerationPrepare p;
    memset(&p, 0, sizeof p);
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    p.frameID = id;
    p.flags = flags;
    p.commandList = (void *)cb;
    p.renderSize.width = s->w, p.renderSize.height = s->h;
    // as vk_fsr3.c gives them to the upscaler: the picture moved by the jitter, x right and y down; the vectors are
    // last frame's window position minus this frame's in scene pixels, at a scale of one
    p.jitterOffset.x = s->jx, p.jitterOffset.y = s->jy;
    p.motionVectorScale.x = p.motionVectorScale.y = 1.0f;
    p.frameTimeDelta = dt_ms;
    p.cameraNear = s->near_m, p.cameraFar = s->far_m, p.cameraFovAngleVertical = s->fov_y;
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = vk_ffx_resource(s->depth, VK_FORMAT_D32_SFLOAT, s->w, s->h, 0);
    p.motionVectors = vk_ffx_resource(s->mv, VK_FORMAT_R16G16_SFLOAT, s->w, s->h, 0);
    if (ml) {
        p.header.type = FFX_API_EFFECT_ID_FRAMEGENERATION | 0x0cu;   // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 (SDK 2.4)
        p.unused_reset = reset != 0;   // "reset" in PrepareV2
        p2.p = p;
        if (s->have_cam) {
            memcpy(p2.pos, s->cam_pos, sizeof p2.pos), memcpy(p2.up, s->cam_up, sizeof p2.up);
            memcpy(p2.right, s->cam_right, sizeof p2.right), memcpy(p2.fwd, s->cam_fwd, sizeof p2.fwd);
        }
        r = fx_dispatch(&p2.p.header);
    } else {
        r = fx_dispatch(&p.header);
    }
    if (r != FFX_API_RETURN_OK) return fail("the library refused the scene's depth and motion", (unsigned)r), 0;
    struct ffxDispatchDescFrameGeneration d;
    memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    d.commandList = (void *)cb;
    d.presentColor = vk_ffx_resource(G.before.img, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, 0);
    d.outputs[0] = vk_ffx_resource(G.gen.img, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, 1);
    d.numGeneratedFrames = 1;
    d.reset = reset != 0;
    d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;   // linear light
    // 1.0 is the picture's white; with HDR the highlights reach the display's peak, in units of 80 nits
    d.minMaxLuminance[0] = 0.0f, d.minMaxLuminance[1] = vk_hdr_wanted() ? fmaxf((float)cfg_double("TOTK_HDR_PEAK_NITS"), 80.0f) : 80.0f;
    d.generationRect = cf.generationRect;
    d.frameID = id;
    r = fx_dispatch(&d.header);
    if (r != FFX_API_RETURN_OK) return fail("the library refused the frame", (unsigned)r), 0;
    if (!G.frames++)
        rt_log("fg: generating %ux%u pictures from %ux%u scenes (near %g m, far %g m, vertical field of view %.1f "
               "degrees)", ow, oh, s->w, s->h, s->near_m, s->far_m, s->fov_y * 57.29578f);
    return 1;
}

// ---------------------------------------------------------------- the replay
void vk_fg_scene(VkExec *ex, const VkFgScene *s) {
    const VkExecTarget *t = s->target;
    uint32_t w = t->width, h = t->height;
    VkCommandBuffer cb = ex->cb;
    if (!pics_for(ex, w, h)) return fail("no memory for the pictures", 0);
    // The picture so far, exactly: RGBA16F holds every value of the game's R11G11B10F target.
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, G.before.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    memory_barrier(cb);
    ex->recorded += 2;
    int made = 0, reset = s->reset || !G.chain;
    int why = 0;   // FgWhy: 0 generated, 1 no motion vectors (no camera), 2 the library refused, 3 a restart (a cut, or the
                   // frame before gave the library nothing), 4 no scene in the frame at all (set by vk_fg_take)
    if (!s->have_mv) why = 1;
    if (s->have_mv) {
        static uint64_t last_ns;
        uint64_t now = plat_now_ns();
        double dt = last_ns ? (double)(now - last_ns) / 1e6 : 0.0;
        last_ns = now;
        rec_drain();   // the library records into the command buffer itself: the recorder must have made every call so far
        made = generate(cb, s, w, h, reset, dt > 0.0 && dt < 100.0 ? (float)dt : 16.67f);
        // generate() may have made the context again, which leaves it without a last picture
        reset = reset || !G.chain;
        memory_barrier(cb);
        ex->recorded++;
        why = !made ? 2 : reset ? 3 : 0;
    }
    G.fed = made;
    plat_mutex_lock(&G.lock);
    G.cur_ok = made && !reset;
    G.cur_why = why;
    G.cur = (VkFgFrame){.target = t->image, .before_hud = G.before.view, .generated = G.gen.view, .w = w, .h = h};
    plat_mutex_unlock(&G.lock);
}

int vk_fg_take(VkFgFrame *f) {
    plat_mutex_lock(&G.lock);
    int ok = G.cur_ok;
    *f = G.cur;
    G.why_n[G.cur_why]++;
    G.line[G.takes % (sizeof G.line - 1)] = "Gcxru"[G.cur_why];
    G.cur_ok = 0, G.cur_why = 4;   // 4 until the next frame's scene says otherwise
    G.chain = G.fed, G.fed = 0;   // a frame that gave the library nothing breaks the chain
    uint64_t n[5], takes = ++G.takes;
    memcpy(n, G.why_n, sizeof n);
    if (takes % 600 == 0) memset(G.why_n, 0, sizeof G.why_n);
    plat_mutex_unlock(&G.lock);
    // Blinking: generated and not generated frames alternating, 10 or more changes within the last 40 frames
    static uint64_t last_blink;
    unsigned flips = 0;
    for (unsigned i = 1; i < 40; i++) {
        char a = G.line[(takes + sizeof G.line - 1 - i) % (sizeof G.line - 1)], b = G.line[(takes + sizeof G.line - i) % (sizeof G.line - 1)];
        flips += (a == 'G') != (b == 'G');
    }
    int blink = flips >= 10 && takes > 160 && takes - last_blink > 300;
    if (blink) last_blink = takes;
    if (blink || (takes % 600 == 0 && n[0] < 600)) {   // only when frames were lost: the 600 frames just past
        char tl[sizeof G.line];   // the last frames in order, oldest first: G generated, c no camera, x refused, r restart, u no scene
        unsigned L = sizeof G.line - 1, at = (unsigned)(takes % L);
        for (unsigned i = 0; i < L; i++) tl[i] = G.line[(at + i) % L] ? G.line[(at + i) % L] : '.';
        tl[L] = 0;
        rt_log("fg: the last %u frames, oldest first (G generated, u no scene, c no camera, x refused, r restart): %s", L, tl);
        rt_log("fg: of the last 600 frames %llu had a generated picture; without: %llu no camera, %llu refused by the "
               "library, %llu a restart (cut, or the frame before had none), %llu no scene found",
               (unsigned long long)n[0], (unsigned long long)n[1], (unsigned long long)n[2], (unsigned long long)n[3],
               (unsigned long long)n[4]);
    }
    // a single frame without one (a camera cut) does not move the frame limit (frame_rate.c frame_step_cap_ns)
    int32_t live = __atomic_load_n(&G.live, __ATOMIC_RELAXED);
    plat_xchg32(&G.live, ok ? 8 : live > 0 ? live - 1 : 0);
    return ok;
}

int gfx_frame_gen_live(void) { return __atomic_load_n(&G.live, __ATOMIC_RELAXED) > 0; }

// frame_gen = 1: the frame limit counts the frames shown, so the game draws half of it (frame_rate.c); x2: the limit
// stays the game's and twice as many frames are shown.
int gfx_frame_gen_halves(void) { return gfx_frame_gen_live() && !strcmp(cfg_value("TOTK_FRAME_GEN"), "1"); }

// ---------------------------------------------------------------- the present
static int init(const VkCtx *c) {
    G.tried = 1;
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 5; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                              .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 5, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fg_compose_spv,
                                   .pCode = fg_compose_spv};
    VkShaderModuleCreateInfo mh = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fg_compose_hdr_spv,
                                   .pCode = fg_compose_hdr_spv};
    VkShaderModule m = VK_NULL_HANDLE, mhdr = VK_NULL_HANDLE;
    int ok = G.push && vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    ok = ok && vkCreatePipelineLayout(c->device, &li, NULL, &G.layout) == VK_SUCCESS &&
         vkCreateSampler(c->device, &si, NULL, &G.nearest) == VK_SUCCESS &&
         vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS &&
         vkCreateShaderModule(c->device, &mh, NULL, &mhdr) == VK_SUCCESS;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = G.layout};
    ok = ok && vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &G.pipe) == VK_SUCCESS;
    ci.stage.module = mhdr;
    ok = ok && vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &G.pipe_hdr) == VK_SUCCESS;
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    if (mhdr) vkDestroyShaderModule(c->device, mhdr, NULL);
    if (!ok) return fail("the pass that puts the HUD on generated frames could not be made", 0), 0;
    return G.ok = 1;
}

// The image the compose pass writes: RGBA8 for the SDR present, RGBA16F (linear light) for the HDR one.
static int out_for(const VkCtx *c, uint32_t w, uint32_t h, int hdr) {
    struct Pic *o = hdr ? &G.out_hdr : &G.out;
    uint32_t *ow = hdr ? &G.out_hdr_w : &G.out_w, *oh = hdr ? &G.out_hdr_h : &G.out_h;
    if (o->img && *ow == w && *oh == h) return 1;
    // another picture size: earlier presents may still read the old image (as vk_fsr.c ensure_images)
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    pic_free(o);
    if (!pic_make(o, w, h, hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        pic_free(o);
        return 0;
    }
    *ow = w, *oh = h;
    return 1;
}

// The window texture's bytes as they are: its sRGB formats are viewed as their UNORM twins (the back end's colour
// images take views of another format, vk_texture.c).
static VkFormat bytes_format(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f;
}

static VkImageView view_of(const VkCtx *c, VkImageView *slot, VkImage img, VkFormat fmt) {
    if (*slot) vkDestroyImageView(c->device, *slot, NULL);   // the present waited for this slot's fence
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, slot) != VK_SUCCESS) *slot = VK_NULL_HANDLE;
    return *slot;
}

// TOTK_FG_SHOTS=w<lo>-<hi> [TOTK_FG_SHOTS_DIR=dir] (a debugging aid): every generated picture shown while the world
// clock (frame_step_world_us) is in [lo, hi] seconds is saved as <dir>/g<compose>_<seconds>.png, HUD included, as the
// player sees it; the series shots (TOTK_SHOTS) only see the game's own frames. SDR output only (the HDR picture is
// float). The picture is copied into the slot's host buffer with the compose and written when the slot comes round
// again, after the present waited for its fence.
struct FgShot {
    VkBuffer b;
    VmaAllocation a;
    uint8_t *p;
    uint32_t w, h, pending;
    uint64_t n;
    double sec;
};
static struct {
    int parsed, on;
    double lo, hi;
    const char *dir;
    struct FgShot s[SLOTS];
    uint64_t n;
} FS;

static void fg_shot_write(unsigned slot) {
    struct FgShot *b = &FS.s[slot];
    if (!b->pending) return;
    char path[512];
    snprintf(path, sizeof path, "%s/g%06llu_%.3f.png", FS.dir, (unsigned long long)b->n, b->sec);
    rt_write_png(path, b->p, b->w, b->h);
    b->pending = 0;
}

static void fg_shot_take(const VkCtx *c, VkCommandBuffer cb, unsigned slot, VkImage img, uint32_t w, uint32_t h) {
    if (!FS.parsed) {
        FS.parsed = 1;
        const char *e = cfg_get("TOTK_FG_SHOTS");
        FS.on = e && e[0] == 'w' && sscanf(e + 1, "%lf-%lf", &FS.lo, &FS.hi) == 2;
        FS.dir = cfg_get("TOTK_FG_SHOTS_DIR") ? cfg_get("TOTK_FG_SHOTS_DIR") : ".";
        if (FS.on) {
            plat_file_mkdir_p(FS.dir);
            rt_log("fg: generated pictures from %.2f to %.2f s go to %s", FS.lo, FS.hi, FS.dir);
        }
    }
    FS.n++;
    if (!FS.on) return;
    int64_t wus = frame_step_world_us();
    double sec = wus < 0 ? -1.0 : (double)wus / 1e6;
    if (sec < FS.lo || sec > FS.hi) return;
    struct FgShot *b = &FS.s[slot];
    if (b->b && (b->w != w || b->h != h)) {
        vmaDestroyBuffer(c->vma, b->b, b->a);
        b->b = VK_NULL_HANDLE;
    }
    if (!b->b) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &b->b, &b->a, &info) != VK_SUCCESS) {
            b->b = VK_NULL_HANDLE;
            return;
        }
        b->p = info.pMappedData, b->w = w, b->h = h;
    }
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, b->b, 1, &r);
    memory_barrier(cb);
    b->pending = 1, b->n = FS.n, b->sec = sec;
}

VkImage vk_fg_compose(VkCommandBuffer cb, unsigned slot, const VkFgFrame *f, VkImage pic, VkFormat pic_fmt, VkImage win,
                      VkFormat win_fmt, int hdr) {
    const VkCtx *c = vk_ctx();
    if (__atomic_load_n(&G.failed, __ATOMIC_RELAXED) || (!G.tried && !init(c)) || !G.ok) return VK_NULL_HANDLE;
    if (pic != f->target || !out_for(c, f->w, f->h, hdr)) return VK_NULL_HANDLE;
    struct Pic *out = hdr ? &G.out_hdr : &G.out;
    slot %= SLOTS;
    VkImageView pv = view_of(c, &G.pic_view[slot], pic, pic_fmt), wv = view_of(c, &G.win_view[slot], win, bytes_format(win_fmt));
    if (!pv || !wv) return VK_NULL_HANDLE;
    // what the replay wrote becomes visible; the last present's read of the output is over
    memory_barrier(cb);
    image_barrier(cb, out->img, VK_IMAGE_LAYOUT_UNDEFINED);
    int debug = debug_view();   // fg_compose.comp's views
    VkDescriptorImageInfo ii[5] = {{G.nearest, pv, VK_IMAGE_LAYOUT_GENERAL},
                                   {G.nearest, f->before_hud, VK_IMAGE_LAYOUT_GENERAL},
                                   {G.nearest, f->generated, VK_IMAGE_LAYOUT_GENERAL},
                                   {G.nearest, wv, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, out->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet ws[5];
    for (uint32_t i = 0; i < 5; i++)
        ws[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i,
                                       .descriptorCount = 1, .pImageInfo = &ii[i],
                                       .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    int32_t pc[3] = {(int32_t)f->w, (int32_t)f->h, debug == 1 || debug == 2 ? debug : 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hdr ? G.pipe_hdr : G.pipe);
    rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 5, ws);
    vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (f->w + 7) / 8, (f->h + 7) / 8, 1);
    memory_barrier(cb);
    fg_shot_write(slot);   // this slot's last copy: the present waited for its fence (view_of above relies on the same)
    if (!hdr) fg_shot_take(c, cb, slot, out->img, f->w, f->h);
    if (!G.shown++)
        rt_log("fg: generated frames are shown (%ux%u, the HUD from the game's frame; float target format %d, window "
               "texture format %d%s)", f->w, f->h, (int)pic_fmt, (int)win_fmt, hdr ? "; HDR output, linear float" : "");
    return out->img;
}

void vk_fg_release(void) {
    const VkCtx *c = vk_ctx();
    if (!c) return;
    ctx_destroy();
    pic_free(&G.before), pic_free(&G.gen), pic_free(&G.out), pic_free(&G.out_hdr);
    for (int i = 0; i < SLOTS; i++) {
        if (G.pic_view[i]) vkDestroyImageView(c->device, G.pic_view[i], NULL);
        if (G.win_view[i]) vkDestroyImageView(c->device, G.win_view[i], NULL);
        G.pic_view[i] = G.win_view[i] = VK_NULL_HANDLE;
    }
    if (G.pipe) vkDestroyPipeline(c->device, G.pipe, NULL);
    if (G.pipe_hdr) vkDestroyPipeline(c->device, G.pipe_hdr, NULL);
    if (G.nearest) vkDestroySampler(c->device, G.nearest, NULL);
    if (G.layout) vkDestroyPipelineLayout(c->device, G.layout, NULL);
    if (G.dsl) vkDestroyDescriptorSetLayout(c->device, G.dsl, NULL);
    G.pipe = G.pipe_hdr = VK_NULL_HANDLE, G.nearest = VK_NULL_HANDLE, G.layout = VK_NULL_HANDLE, G.dsl = VK_NULL_HANDLE;
    G.tried = G.ok = 0, G.w = G.h = G.out_w = G.out_h = G.out_hdr_w = G.out_hdr_h = 0, G.chain = G.fed = G.cur_ok = 0;
}
