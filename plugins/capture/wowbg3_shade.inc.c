// WOW-BG3 capture layer: WoW's ground shade on the figures BG3 draws.
//
// WoW shades a unit as a whole: it reads the baked shadow of the ground under it (trees, buildings, roofs), and its
// sunlight ramps down in a fraction of a second when it steps into shade. benilla does the same for its copies of
// the party's figures and writes their levels to link/wowshade.txt (patch 0126: "WSH1 <count>", then
// "<x y z, WoW yards> <0 lit .. 1 shadowed>" a figure). BG3 draws those figures itself, in its own light, where
// nothing of WoW's world stands between them and the sun.
// So what WoW calls shade is put where BG3's own lighting looks for it: in its sun shadow maps. After each of
// BG3's world shadow passes that drew a kept character, a disc facing the light is drawn into that map between
// each shaded figure and the sun, with the pass's own view (set 0 binding 2, the layout of the camera's block: an
// orthographic reverse-Z light view). Its radius follows the figure's level, so the shade closes over the figure
// with WoW's ramp. BG3's lighting then shadows the figure as it shadows anything: sunlight off, sky and bounce on,
// its own filtering at the edge. WoW's pixels are not BG3's to light, so the disc shows on nothing else.
// Compose word "noshade": off.
#include "wowbg3_shade_spv.h"

#define SHD_MAX 8
#define SHD_FB 8
static struct {
    pthread_mutex_t m;
    int n;
    float at[SHD_MAX][3], level[SHD_MAX];  // BG3 world metres (feet), 0..1
    uint64_t read_at;
    int ready, failed; VkFormat fmt; VkRenderPass rp; int32_t rp_lay;
    VkPipelineLayout layout; VkPipeline pipe;
    struct { uint64_t img; VkImageView view; VkFramebuffer fb; uint32_t w, h; } t[SHD_FB];
    uint64_t passes, drawn, discs, refused, nolayout, views;
} g_shd = {.m = PTHREAD_MUTEX_INITIALIZER};
static __thread struct { VkCommandBuffer cb; uint64_t img; int32_t fin; uint32_t w, h; int have; float vp[16], eye[3], fwd[3]; } t_shd;

// The pass on this thread: one of the world's sun shadow maps (depth only, square, 2048 or more: void_depth_only).
static void shade_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const int32_t* fin, uint32_t w, uint32_t h) {
    t_shd.cb = VK_NULL_HANDLE;
    if (!g_cc.on || g_cc.noshade) return;
    int colors = 0; uint64_t dimg = 0; int32_t dfin = -1; uint32_t dw = 0, dh = 0;
    for (uint32_t k = 0; k < n; k++) {
        if (!imgs[k]) continue;
        ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, imgs[k], 0);
        if (!i) continue;
        if (is_depth_format(i->fmt)) { dimg = imgs[k]; dfin = fin[k]; dw = i->w; dh = i->hgt; } else colors++;
    }
    (void)w; (void)h;
    if (colors || !dimg || dimg == g_role.depth || dw != dh || dw < 2048) return;
    { static int said; if (said < 3) { said++; logf_("shade: a world shadow pass, image %llx %ux%u, layout after it %d", (unsigned long long)dimg, dw, dh, (int)dfin); } }
    if (dfin < 0) { __atomic_add_fetch(&g_shd.nolayout, 1, __ATOMIC_RELAXED); return; }
    t_shd.cb = cb; t_shd.img = dimg; t_shd.fin = dfin; t_shd.w = dw; t_shd.h = dh; t_shd.have = 0;
}
// A kept character's draw in that pass: the pass's light view, from the block BG3 bound for it.
static void shade_view(VkCommandBuffer cb) {
    if (t_shd.cb != cb || t_shd.have) return;
    const float* f = (const float*)fxc_slot_cpu(0, 2, 224);
    if (!f) return;
    for (int i = 0; i < 55; i++) if (i != 50 && i != 51 && !isfinite(f[i])) return;
    if (f[16 + 2 * 4 + 3] != 0.0f || f[16 + 3 * 4 + 3] != 1.0f) return;  // not an orthographic view
    memcpy(t_shd.vp, f + 32, 64);
    t_shd.eye[0] = f[52]; t_shd.eye[1] = f[53]; t_shd.eye[2] = f[54];
    for (int i = 0; i < 3; i++) t_shd.fwd[i] = f[i * 4 + 2];
    t_shd.have = 1;
    __atomic_add_fetch(&g_shd.views, 1, __ATOMIC_RELAXED);
}
static int shade_make(DeviceData* d, VkFormat fmt, int32_t lay) {
    if (g_shd.ready && g_shd.fmt == fmt && g_shd.rp_lay == lay) return 1;
    if (g_shd.ready || g_shd.failed) return 0;  // one format and layout: BG3's shadow maps are all alike
    g_shd.failed = 1;
    VkAttachmentDescription at = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, (VkImageLayout)lay, (VkImageLayout)lay};
    VkAttachmentReference dr = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 0, NULL, NULL, &dr, 0, NULL};
    VkRenderPassCreateInfo rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &at, 1, &sp, 0, NULL};
    if (d->CreateRenderPass(d->device, &rci, NULL, &g_shd.rp) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 112};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pr};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_shd.layout) != VK_SUCCESS) return 0;
    VkShaderModule vs;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_shade_vert, wowbg3_shade_vert};
    if (d->CreateShaderModule(d->device, &mv, NULL, &vs) != VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE};
    // BG3's light views are reverse-Z (the nearer to the light, the larger): the disc wins where nothing nearer is drawn.
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, VK_TRUE, VK_COMPARE_OP_GREATER_OR_EQUAL,
        VK_FALSE, VK_FALSE, {0}, {0}, 0.0f, 1.0f};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 1, &st, &vi, &ia, NULL, &vps, &rs, &ms, &dss, NULL, &dy, g_shd.layout, g_shd.rp, 0, VK_NULL_HANDLE, -1};
    VkResult r = d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_shd.pipe);
    d->DestroyShaderModule(d->device, vs, NULL);
    if (r != VK_SUCCESS) { logf_("shade: pipeline not made (%d)", (int)r); return 0; }
    g_shd.fmt = fmt; g_shd.rp_lay = lay; g_shd.ready = 1; g_shd.failed = 0;
    logf_("shade: ready (shadow maps of format %d, layout %d after BG3's pass)", (int)fmt, (int)lay);
    return 1;
}
// BG3 destroyed an image: its view and framebuffer here go with it.
static void shade_forget_image(DeviceData* d, VkImage img) {
    pthread_mutex_lock(&g_shd.m);
    for (int i = 0; i < SHD_FB; i++) {
        if (g_shd.t[i].img != (uint64_t)img) continue;
        if (g_shd.t[i].fb) d->DestroyFramebuffer(d->device, g_shd.t[i].fb, NULL);
        if (g_shd.t[i].view) d->DestroyImageView(d->device, g_shd.t[i].view, NULL);
        memset(&g_shd.t[i], 0, sizeof g_shd.t[i]);
    }
    pthread_mutex_unlock(&g_shd.m);
}
// After BG3's shadow pass (its render pass has ended): the discs into the same map.
static void shade_end(DeviceData* d, VkCommandBuffer cb) {
    if (t_shd.cb != cb) return;
    t_shd.cb = VK_NULL_HANDLE;
    if (!d || !t_shd.have) return;
    float at[SHD_MAX][3], level[SHD_MAX]; int n;
    pthread_mutex_lock(&g_shd.m);
    n = g_shd.n; memcpy(at, g_shd.at, sizeof at); memcpy(level, g_shd.level, sizeof level);
    g_shd.passes++;
    pthread_mutex_unlock(&g_shd.m);
    int any = 0;
    for (int k = 0; k < n; k++) if (level[k] > 0.02f) any = 1;
    if (!any) return;
    ImgInfo* ii = TABLE_FIND(g_img, IMG_CAP, t_shd.img, 0);
    if (!ii || !shade_make(d, ii->fmt, t_shd.fin)) { g_shd.refused++; return; }
    pthread_mutex_lock(&g_shd.m);
    int slot = -1;
    for (int i = 0; i < SHD_FB && slot < 0; i++) if (g_shd.t[i].img == t_shd.img) slot = i;
    if (slot < 0) {
        for (int i = 0; i < SHD_FB && slot < 0; i++) if (!g_shd.t[i].img) slot = i;
        if (slot >= 0) {
            VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)t_shd.img, VK_IMAGE_VIEW_TYPE_2D, ii->fmt,
                {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
            VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_shd.rp, 1, &g_shd.t[slot].view, t_shd.w, t_shd.h, 1};
            if (d->CreateImageView(d->device, &vci, NULL, &g_shd.t[slot].view) != VK_SUCCESS ||
                d->CreateFramebuffer(d->device, &fci, NULL, &g_shd.t[slot].fb) != VK_SUCCESS) {
                if (g_shd.t[slot].view) d->DestroyImageView(d->device, g_shd.t[slot].view, NULL);
                memset(&g_shd.t[slot], 0, sizeof g_shd.t[slot]);
                slot = -1;
            } else { g_shd.t[slot].img = t_shd.img; g_shd.t[slot].w = t_shd.w; g_shd.t[slot].h = t_shd.h; }
        }
    }
    VkFramebuffer fb = slot >= 0 ? g_shd.t[slot].fb : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_shd.m);
    if (!fb) { g_shd.refused++; return; }
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, g_shd.rp, fb, {{0, 0}, {t_shd.w, t_shd.h}}, 0, NULL};
    d->CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_shd.pipe);
    VkViewport vp = {0, 0, (float)t_shd.w, (float)t_shd.h, 0, 1};
    VkRect2D sc = {{0, 0}, {t_shd.w, t_shd.h}};
    d->CmdSetViewport(cb, 0, 1, &vp);
    d->CmdSetScissor(cb, 0, 1, &sc);
    uint64_t discs = 0;
    for (int k = 0; k < n; k++) {
        if (!(level[k] > 0.02f)) continue;
        // Between the figure and the light: the middle of the body, two metres towards the sun, wide enough for the
        // whole figure when the ramp is through.
        float pc[28];
        memcpy(pc, t_shd.vp, 64);
        pc[16] = t_shd.eye[0]; pc[17] = t_shd.eye[1]; pc[18] = t_shd.eye[2]; pc[19] = 1.5f * level[k];
        pc[20] = at[k][0] - 2.0f * t_shd.fwd[0]; pc[21] = at[k][1] + 1.0f - 2.0f * t_shd.fwd[1]; pc[22] = at[k][2] - 2.0f * t_shd.fwd[2]; pc[23] = 0.0f;
        pc[24] = t_shd.fwd[0]; pc[25] = t_shd.fwd[1]; pc[26] = t_shd.fwd[2]; pc[27] = 0.0f;
        d->CmdPushConstants(cb, g_shd.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 112, pc);
        d->CmdDraw(cb, 48, 1, 0, 0);
        discs++;
    }
    d->CmdEndRenderPass(cb);
    __atomic_add_fetch(&g_shd.drawn, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_shd.discs, discs, __ATOMIC_RELAXED);
}
// Every sixth present: benilla's levels, into BG3's world (the packet's anchor: g_tav.centre is where g_tav.wow stands).
static void shade_present(void) {
    const uint64_t pr = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    if (!g_cc.on || g_cc.noshade || pr % 6 != 0) return;
    char path[1100];
    snprintf(path, sizeof path, "%s/wowshade.txt", link_dir());
    float at[SHD_MAX][3], level[SHD_MAX]; int n = 0;
    FILE* f = fopen(path, "r");
    if (f) {
        int count = 0;
        if (fscanf(f, "WSH1 %d", &count) == 1) {
            const double yd = 0.9144;
            for (int k = 0; k < count && n < SHD_MAX; k++) {
                float x, y, z, s;
                if (fscanf(f, "%f %f %f %f", &x, &y, &z, &s) != 4) break;
                at[n][0] = (float)(g_tav.centre[0] + (y - g_tav.wow[1]) * yd);
                at[n][1] = (float)(g_tav.centre[1] + (z - g_tav.wow[2]) * yd);
                at[n][2] = (float)(g_tav.centre[2] - (x - g_tav.wow[0]) * yd);
                level[n] = s < 0 ? 0 : s > 1 ? 1 : s;
                n++;
            }
        }
        fclose(f);
    }
    pthread_mutex_lock(&g_shd.m);
    g_shd.n = g_tav.seq ? n : 0;
    memcpy(g_shd.at, at, sizeof at); memcpy(g_shd.level, level, sizeof level);
    const uint64_t passes = g_shd.passes;
    pthread_mutex_unlock(&g_shd.m);
    if (pr % 600 == 0 && n) {
        float most = 0; for (int k = 0; k < n; k++) if (level[k] > most) most = level[k];
        logf_("shade: %d figures (deepest level %.2f, first at %.1f %.1f %.1f), %llu shadow passes with a kept character (%llu views read, %llu passes without a known layout), %llu with discs (%llu discs), %llu refused",
              n, most, at[0][0], at[0][1], at[0][2], (unsigned long long)passes, (unsigned long long)g_shd.views, (unsigned long long)g_shd.nolayout, (unsigned long long)g_shd.drawn,
              (unsigned long long)g_shd.discs, (unsigned long long)g_shd.refused);
    }
}
