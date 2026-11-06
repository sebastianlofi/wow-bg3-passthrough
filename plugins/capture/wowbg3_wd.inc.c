// Gate a prototype (not for release): WoW's depth per pixel in BG3's depth, neutral placeholder. Word "wdepth".
#include "wowbg3_wd_spv.h"
static struct {
    int ready, failed; VkRenderPass made_for;
    VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; VkDescriptorPool pool; VkDescriptorSet set[2]; VkBuffer setbuf[2];
    uint64_t draws, nowow, nocam;
} g_wd;
static int wd_make(DeviceData* d, VkRenderPass rp) {
    if (g_wd.ready && g_wd.made_for == rp) return 1;
    if (g_wd.failed > 3) return 0;
    g_wd.failed++;
    if (!g_wd.dsl) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
        if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_wd.dsl) != VK_SUCCESS) return 0;
        VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 244};
        VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_wd.dsl, 1, &pr};
        if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_wd.layout) != VK_SUCCESS) return 0;
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 1, &ps};
        if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_wd.pool) != VK_SUCCESS) return 0;
        VkDescriptorSetLayout l[2] = {g_wd.dsl, g_wd.dsl};
        VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_wd.pool, 2, l};
        if (d->AllocateDescriptorSets(d->device, &sai, g_wd.set) != VK_SUCCESS) return 0;
    }
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_surf_alb_vert, wowbg3_surf_alb_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_wd_frag, wowbg3_wd_frag};
    if (d->CreateShaderModule(d->device, &mv, NULL, &vs) != VK_SUCCESS || d->CreateShaderModule(d->device, &mf, NULL, &fs) != VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE};
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, VK_TRUE, VK_COMPARE_OP_GREATER, VK_FALSE, VK_FALSE, {0}, {0}, 0.0f, 1.0f};
    VkPipelineColorBlendAttachmentState cb[4];
    for (int k = 0; k < 4; k++) cb[k] = (VkPipelineColorBlendAttachmentState){VK_FALSE, 0, 0, 0, 0, 0, 0, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    cb[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    cb[3].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 4, cb, {0, 0, 0, 0}};
    VkDynamicState dyn[9] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 9, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, &dss, &cbs, &dy, g_wd.layout, rp, 0, VK_NULL_HANDLE, -1};
    VkResult r = d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_wd.pipe);
    if (r != VK_SUCCESS) { logf_("wdepth: pipeline not made (%d)", (int)r); return 0; }
    g_wd.made_for = rp; g_wd.ready = 1; g_wd.failed = 0;
    logf_("wdepth: ready");
    return 1;
}
// Inside the ground's render pass, in place of its triangles. 1: drawn.
static struct { pthread_mutex_t m; float eye[3], fwd[3], up[3], vp[16]; double vpe[4]; uint64_t t, n; } g_lkr = {PTHREAD_MUTEX_INITIALIZER, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0}, {0, 0, 0, 0}, 0, 0};
// The eye a view-projection implies: the kernel of its x, y and w rows (vp column-major), homogeneous.
static void wd_vp_eye(const float* vp, double out[4]) {
    double A[3][4];
    const int rows[3] = {0, 1, 3};
    for (int i = 0; i < 3; i++) for (int c = 0; c < 4; c++) A[i][c] = vp[c * 4 + rows[i]];
    for (int k = 0; k < 4; k++) {
        int cols[3], n = 0; for (int c = 0; c < 4; c++) if (c != k) cols[n++] = c;
        double m = A[0][cols[0]] * (A[1][cols[1]] * A[2][cols[2]] - A[1][cols[2]] * A[2][cols[1]])
                 - A[0][cols[1]] * (A[1][cols[0]] * A[2][cols[2]] - A[1][cols[2]] * A[2][cols[0]])
                 + A[0][cols[2]] * (A[1][cols[0]] * A[2][cols[1]] - A[1][cols[1]] * A[2][cols[0]]);
        out[k] = (k % 2 ? -m : m);
    }
}
static int wd_draw(DeviceData* d, VkCommandBuffer cb, const GrRec* rec, uint32_t w, uint32_t h, VkRenderPass rp) {
    if (g_cc.wdjit) {  // where BG3's jittered projection puts the centre ray: its jitter, NDC (camera-relative vp, element (r, c) at vp[c * 4 + r])
        double cl[4];
        for (int r = 0; r < 4; r++) cl[r] = (rec->vp[0 * 4 + r] * rec->fwd[0] + rec->vp[1 * 4 + r] * rec->fwd[1] + rec->vp[2 * 4 + r] * rec->fwd[2]) * 10.0 + rec->vp[12 + r];
        const uint64_t pr = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
        if (cl[3] > 1e-6) { int s = (int)(pr % 8); g_jit[s].p = pr + 1; g_jit[s].j[0] = (float)(cl[0] / cl[3]); g_jit[s].j[1] = (float)(cl[1] / cl[3]); }
    }
    pthread_mutex_lock(&g_lkr.m); memcpy(g_lkr.eye, rec->eye, 12); memcpy(g_lkr.fwd, rec->fwd, 12); memcpy(g_lkr.up, rec->up, 12); memcpy(g_lkr.vp, rec->vp, 64); wd_vp_eye(rec->vp, g_lkr.vpe); g_lkr.t = now_ns(); g_lkr.n++; pthread_mutex_unlock(&g_lkr.m);
    if (!wd_make(d, rp)) return 0;
    pthread_mutex_lock(&g_sf.m);
    int k = g_sf.wc_latest;
    __typeof__(g_sf.wcm[0]) w0;
    VkBuffer wb = VK_NULL_HANDLE;
    if (k >= 0) { w0 = g_sf.wcm[k]; wb = g_sf.wc[k].buf; }
    pthread_mutex_unlock(&g_sf.m);
    if (g_cc.wdlock && g_lk.have) { k = 0; w0 = g_lk.m; wb = g_lk.b.buf; }
    if (!wb || !w0.cam) { g_wd.nowow++; return 0; }
    uint8_t crec[96];
    float eye[3], fwd[3], right[3], up[3], tx, ty;
    if (g_cc.wdlock && g_lk.have) {
        memset(eye, 0, sizeof eye); memcpy(fwd, rec->fwd, 12); memcpy(right, rec->right, 12); memcpy(up, rec->up, 12); tx = g_lk.tx; ty = g_lk.ty;
    } else if (!sf_cam_rec(w0.cam, crec) || !sf_cam(crec, rec->eye, eye, fwd, right, up, &tx, &ty)) { g_wd.nocam++; return 0; }
    float f[48];
    if (!sf_inverse(rec->vp, f)) return 0;
    memcpy(f + 16, rec->fwd, 12); f[19] = 0; memcpy(f + 20, rec->right, 12); f[23] = 0; memcpy(f + 24, rec->up, 12); f[27] = 0;
    // The mat4's columns are vp's rows of four: the z and w rows across them.
    for (int c = 0; c < 4; c++) { f[28 + c] = rec->vp[c * 4 + 2]; f[32 + c] = rec->vp[c * 4 + 3]; }
    float pc2[64];
    memcpy(pc2, f, 36 * sizeof(float));
    memcpy(pc2 + 36, eye, 12); pc2[39] = tx; memcpy(pc2 + 40, fwd, 12); pc2[43] = ty; memcpy(pc2 + 44, right, 12); pc2[47] = 0; memcpy(pc2 + 48, up, 12); pc2[51] = 0;
    uint32_t* u = (uint32_t*)(pc2 + 52);
    u[0] = w0.ww; u[1] = w0.wh; u[2] = w0.cbpr; u[3] = w0.dbpr;
    pc2[56] = w0.wnear; pc2[57] = (w0.wzoff > -100.0f && w0.wzoff < 100.0f) ? w0.wzoff : 0.0f; pc2[58] = 1.0f / (float)w; pc2[59] = 1.0f / (float)h; pc2[60] = g_cc.wdocc;
    if (g_wd.setbuf[k] != wb) {
        VkDescriptorBufferInfo bi = {wb, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_wd.set[k], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
        d->UpdateDescriptorSets(d->device, 1, &wr, 0, NULL);
        g_wd.setbuf[k] = wb;
    }
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_wd.pipe);
    d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_wd.layout, 0, 1, &g_wd.set[k], 0, NULL);
    d->CmdPushConstants(cb, g_wd.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 244, pc2);
    d->CmdDraw(cb, 3, 1, 0, 0);
    if (++g_wd.draws % 600 == 1) logf_("wdepth: %llu draws, %llu without a WoW frame, %llu without its camera", (unsigned long long)g_wd.draws, (unsigned long long)g_wd.nowow, (unsigned long long)g_wd.nocam);
    return 1;
}
// BG3's descriptor sets back (this pass bound its own), as fb_feed does.
static void wd_restore(DeviceData* d, VkCommandBuffer cb) {
    if (t_bind_cb != cb || !t_pipe) return;
    RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
    Recipe* rr = ri ? ri->r : NULL;
    PlInfo* pl = (rr && g_pl && rr->layout) ? TABLE_FIND(g_pl, PL_CAP, rr->layout, 0) : NULL;
    for (uint32_t k2 = 0; pl && k2 < pl->nsets && k2 < 8; k2++)
        if (t_set[k2]) { VkDescriptorSet ds = (VkDescriptorSet)t_set[k2]; d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)rr->layout, k2, 1, &ds, t_ndyn[k2], t_dyn[k2]); }
}

// The second draw: WoW's collision ground, tested against WoW's depth just written. Where it passes, the ground is what WoW shows, so
// only those pixels get the "receives decals" byte and the ground's own normal. WoW's models and leaves stay out of BG3's decals.
#include "wowbg3_wdt_spv.h"
static VkPipeline g_wd_tri; static VkRenderPass g_wd_tri_for;
static VkPipeline wd_tri(DeviceData* d, VkRenderPass rp) {
    if (g_wd_tri && g_wd_tri_for == rp) return g_wd_tri;
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_wdt_vert, wowbg3_wdt_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_ground_frag, wowbg3_ground_frag};
    if (d->CreateShaderModule(d->device, &mv, NULL, &vs) != VK_SUCCESS || d->CreateShaderModule(d->device, &mf, NULL, &fs) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkVertexInputBindingDescription vb[2] = {{0, 24, VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(GrRec), VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription va[10] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
    for (uint32_t k = 0; k < 8; k++) va[2 + k] = (VkVertexInputAttributeDescription){2 + k, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * k};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 2, vb, 10, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE};
    // Reverse-Z: passes where the ground, moved a little toward the eye, is at or nearer than WoW's depth. No depth write.
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, VK_FALSE, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_FALSE, VK_FALSE, {0}, {0}, 0.0f, 1.0f};
    VkPipelineColorBlendAttachmentState cb[4];
    memset(cb, 0, sizeof cb);
    cb[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;  // the ground's normal
    cb[2].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;  // the material byte
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 4, cb, {0, 0, 0, 0}};
    VkDynamicState dyn[9] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 9, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, &dss, &cbs, &dy, g_gr.layout, rp, 0, VK_NULL_HANDLE, -1};
    if (d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_wd_tri) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_wd_tri_for = rp;
    logf_("wdepth: ground mark ready");
    return g_wd_tri;
}
static void wd_mark(DeviceData* d, VkCommandBuffer cb, VkRenderPass rp, VkBuffer vbuf, VkBuffer recb, VkDeviceSize recoff, uint32_t verts) {
    VkPipeline p = wd_tri(d, rp);
    if (!p) return;
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkBuffer bufs[2] = {vbuf, recb};
    VkDeviceSize offs[2] = {0, recoff};
    d->CmdBindVertexBuffers(cb, 0, 2, bufs, offs);
    d->CmdDraw(cb, verts, 1, 0, 0);
}

static SfImg g_wd_mask; static uint32_t g_wd_mw, g_wd_mh; static int g_wd_minit;
static VkImageView wd_mask_view(void) { return g_wd_mask.view; }
static void wd_mask_copy(DeviceData* d, VkCommandBuffer cb, uint64_t serial, uint32_t w, uint32_t h) {
    pthread_mutex_lock(&g_sf.m);
    VkImage z = g_sf.s[serial % SF_SLOTS].z.img;
    if (!g_wd_mask.img || g_wd_mw != w || g_wd_mh != h) {
        if (g_wd_mask.img) sf_img_free(d, &g_wd_mask);
        if (!sf_img_make(d, &g_wd_mask, VK_FORMAT_R32_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { pthread_mutex_unlock(&g_sf.m); return; }
        g_wd_mw = w; g_wd_mh = h; g_wd_minit = 0;
    }
    VkImage m = g_wd_mask.img; int init = g_wd_minit; g_wd_minit = 1;
    pthread_mutex_unlock(&g_sf.m);
    if (!init) sf_barrier(d, cb, m, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    else sf_barrier(d, cb, m, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    sf_barrier(d, cb, z, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageCopy rg = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    d->CmdCopyImage(cb, z, VK_IMAGE_LAYOUT_GENERAL, m, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    sf_barrier(d, cb, m, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    sf_barrier(d, cb, z, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

#define LK_N 4
static struct { VkCommandPool pool; VkCommandBuffer cb[LK_N]; VkFence f[LK_N]; uint32_t n; uint64_t frames, hits, misses, none, wait_ns, wait_max; } g_lks;
static void wd_lock_submit(DeviceData* d, VkQueue queue, uint32_t family) {
    if (g_lk_sent_ || !g_cc.wdepth || !g_wr.d) return;
    // this frame's camera: the G-buffer sample recorded since the last present, newest
    static uint64_t last_present; static uint64_t last_t;
    const uint64_t pres = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    PaceSample best; int found = 0;
    pthread_mutex_lock(&g_pace.m);
    for (uint32_t i = 0; i < PACE_CBS; i++) if (g_pace.cbs[i].cb && g_pace.cbs[i].t > last_t && (!found || g_pace.cbs[i].t > best.t)) { best = g_pace.cbs[i]; found = 1; }
    if (found) { g_pace.framed = best; g_pace.framed_ok = 1; last_t = best.t; }
    pthread_mutex_unlock(&g_pace.m);
    if (!found) { if (last_present != pres) { g_lks.none++; last_present = pres; } return; }
    last_present = pres;
    g_lk_sent_ = 1;
    if (!(g_tav.mesh_ok || !g_tav.n)) return;
    if (g_cc.wdgcam) {
        static uint64_t prev_lock, built, stale, said; static uint32_t wseq;
        float re[3], rf[3], ru[3], vp[16]; uint64_t rt;
        pthread_mutex_lock(&g_lkr.m); memcpy(re, g_lkr.eye, 12); memcpy(rf, g_lkr.fwd, 12); memcpy(ru, g_lkr.up, 12); memcpy(vp, g_lkr.vp, 64); rt = g_lkr.t; pthread_mutex_unlock(&g_lkr.m);
        uint8_t base[96]; int bok = 0;
        pthread_mutex_lock(&g_pc.m); if (g_pc.n) { memcpy(base, g_pc.r[(g_pc.head + PC_N - 1) % PC_N].rec, 96); bok = 1; } pthread_mutex_unlock(&g_pc.m);
        const double rx = sqrt((double)vp[0] * vp[0] + (double)vp[4] * vp[4] + (double)vp[8] * vp[8]);
        const double ry = sqrt((double)vp[1] * vp[1] + (double)vp[5] * vp[5] + (double)vp[9] * vp[9]);
        const double rw = sqrt((double)vp[3] * vp[3] + (double)vp[7] * vp[7] + (double)vp[11] * vp[11]);
        if (rt > prev_lock && bok && g_tav.seq && rx > 0 && ry > 0 && rw > 0) {
            const double yd = 0.9144, tany = rw / ry, tanx = rw / rx;
            float v[11] = {(float)(g_tav.wow[0] - (re[2] - g_tav.centre[2]) / yd), (float)(g_tav.wow[1] + (re[0] - g_tav.centre[0]) / yd),
                           (float)(g_tav.wow[2] + (re[1] - g_tav.centre[1]) / yd), -rf[2], rf[0], rf[1], -ru[2], ru[0], ru[1],
                           (float)(2.0 * atan(tany)), (float)(tanx / tany)};
            uint8_t* c = g_wdg_cam_;
            memcpy(c, base, 96);
            wseq = (wseq + 1) & ~WDG_SEQ; if (!wseq) wseq = 1;
            const uint32_t s = WDG_SEQ | wseq;
            memcpy(c + 4, &s, 4); memcpy(c + 8, v, sizeof v); memcpy(c + 64, &s, 4);
            pthread_mutex_lock(&g_wdg.m); g_wdg.seq[g_wdg.next] = s; memcpy(g_wdg.rec[g_wdg.next], c, 96); g_wdg.next = (g_wdg.next + 1) % WDG_N; pthread_mutex_unlock(&g_wdg.m);
            g_wdg_pending_ = 1; built++;
            if (said < 3 || built % 600 == 0) {  // round trip: the record read back as sf_cam reads it must be the block's camera
                said++;
                float e[3], fw[3], ri[3], up[3], tx, ty;
                if (sf_cam(c, re, e, fw, ri, up, &tx, &ty)) {
                    double de = sqrt((double)e[0] * e[0] + (double)e[1] * e[1] + (double)e[2] * e[2]);
                    double df = fw[0] * rf[0] + fw[1] * rf[1] + fw[2] * rf[2], du = up[0] * ru[0] + up[1] * ru[1] + up[2] * ru[2];
                    float bfov; memcpy(&bfov, base + 44, 4); float basp; memcpy(&basp, base + 48, 4);
                    logf_("wdgcam: %llu built, %llu stale; round trip eye %.5f m, fwd dot %.7f, up dot %.7f; fov %.5f (plugin's %.5f), aspect %.5f (plugin's %.5f)",
                          (unsigned long long)built, (unsigned long long)stale, de, df, du, v[9], bfov, v[10], basp);
                }
            }
        } else stale++;
        prev_lock = now_ns();
    }
    const uint64_t tp0 = now_ns();
    tav_write_pose();
    const uint64_t tp1 = now_ns();
    const uint32_t want = g_lk_seq_;
    // the frame drawn from it
    const uint64_t t0 = now_ns();
    int got = -1;
    g_wr_hurry = 1;
    for (;;) {
        pthread_mutex_lock(&g_wr.m);
        int newest = -1;
        for (int j = 0; j < WR_N; j++) {
            WowSlot* w = &g_wr.s[j];
            if ((w->state != 2 && w->state != 3) || w->gen != g_wr.gen) continue;
            if (w->cam == want) { got = j; break; }
            if (newest < 0 || (int32_t)(w->seq - g_wr.s[newest].seq) > 0) newest = j;
        }
        if (got < 0 && now_ns() - t0 > 40000000ull) {
            static int said;
            if (said++ < 40) {
                char b[160]; int o = 0;
                for (int j = 0; j < WR_N; j++) o += snprintf(b + o, sizeof b - o, " %u", g_wr.s[j].cam);
                uint32_t nc = 0; pthread_mutex_lock(&g_pc.m); if (g_pc.n) nc = g_pc.r[(g_pc.head + PC_N - 1) % PC_N].seq; pthread_mutex_unlock(&g_pc.m);
                logf_("wdlock miss: want %u, newest camera record %u, packet frame %u, slots' cameras%s", want, nc, g_tav.frame, b);
            }
            got = newest; g_lks.misses++; pthread_mutex_unlock(&g_wr.m); break; }
        if (got >= 0) { g_lks.hits++; pthread_mutex_unlock(&g_wr.m); break; }
        if (g_wr.cv_ok) {  // patch 42: woken when the reader hands a frame over (at most 1 ms per look)
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_wr.cv, &g_wr.m, &ts);
            pthread_mutex_unlock(&g_wr.m);
        } else { pthread_mutex_unlock(&g_wr.m); usleep(150); }
    }
    g_wr_hurry = 0;
    const uint64_t waited = now_ns() - t0;
    if (g_cc.wdtime) {
        static FILE* tf; static uint64_t n;
        if (!tf) { tf = fopen(outp("/lktime.csv"), "w"); if (tf) fprintf(tf, "pose0_ns,pose1_ns,frame,want,got_cam,got_seq,waited_ns,hit,present\n"); }
        if (tf) { fprintf(tf, "%llu,%llu,%u,%u,%u,%u,%llu,%d,%llu\n", (unsigned long long)tp0, (unsigned long long)tp1, g_tav.frame, want,
                          got >= 0 ? g_wr.s[got].cam : 0, got >= 0 ? g_wr.s[got].seq : 0, (unsigned long long)waited, got >= 0 && g_wr.s[got].cam == want,
                          (unsigned long long)pres); if (++n % 60 == 0) fflush(tf); }
    }
    {
        static FILE* tf; static uint64_t lines;
        if (!tf) { tf = fopen(outp("/wdtrail.csv"), "w"); if (tf) fprintf(tf, "present,want,newest,match,err,err2,sure,slotcam,hit,eye_m,ang_deg,gb_eye_m,gb_dt_ms,wait_ms,turn_deg,vpw,vp_x,vp_y,vp_z,b1_x,b1_y,b1_z,b2_x,b2_y,b2_z\n"); }
        if (tf) {
            float re[3], rf[3]; double vpe[4]; uint64_t rt;
            pthread_mutex_lock(&g_lkr.m); memcpy(re, g_lkr.eye, 12); memcpy(rf, g_lkr.fwd, 12); memcpy(vpe, g_lkr.vpe, sizeof vpe); rt = g_lkr.t; pthread_mutex_unlock(&g_lkr.m);
            uint32_t m = 0; float err = -1, err2 = -1;
            int mok = pace_match(best.eye, &m, &err, &err2);
            int sure = mok && err < 0.05f && err2 > err + 0.02f && err2 < 1e29f;
            uint32_t nc = 0; pthread_mutex_lock(&g_pc.m); if (g_pc.n) nc = g_pc.r[(g_pc.head + PC_N - 1) % PC_N].seq; pthread_mutex_unlock(&g_pc.m);
            uint32_t sc = got >= 0 ? g_wr.s[got].cam : 0;
            double em = -1, ad = -1;
            uint8_t crec[96]; float e[3], fw[3], ri[3], up[3], tx, ty;
            if (sc && sf_cam_rec(sc, crec) && sf_cam(crec, re, e, fw, ri, up, &tx, &ty)) {
                em = sqrt((double)e[0] * e[0] + (double)e[1] * e[1] + (double)e[2] * e[2]);
                double dt = fw[0] * rf[0] + fw[1] * rf[1] + fw[2] * rf[2]; if (dt > 1) dt = 1; if (dt < -1) dt = -1;
                ad = acos(dt) * 57.29577951308232;
            }
            static float pf[3]; static int pf_ok;
            double turn = -1;
            if (pf_ok) { double dt = pf[0] * rf[0] + pf[1] * rf[1] + pf[2] * rf[2]; if (dt > 1) dt = 1; if (dt < -1) dt = -1; turn = acos(dt) * 57.29577951308232; }
            memcpy(pf, rf, 12); pf_ok = 1;
            double ge = sqrt((double)(best.eye[0] - re[0]) * (best.eye[0] - re[0]) + (double)(best.eye[1] - re[1]) * (best.eye[1] - re[1]) + (double)(best.eye[2] - re[2]) * (best.eye[2] - re[2]));
            fprintf(tf, "%llu,%u,%u,%u,%.4f,%.4f,%d,%u,%d,%.4f,%.4f,%.4f,%.2f,%.2f,%.4f,%.6g,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n", (unsigned long long)pres, want, nc, m, err, err2, sure, sc,
                    sc == want, em, ad, ge, ((double)rt - (double)best.t) / 1e6, waited / 1e6, turn, vpe[3], vpe[3] ? vpe[0] / vpe[3] : 0, vpe[3] ? vpe[1] / vpe[3] : 0, vpe[3] ? vpe[2] / vpe[3] : 0,
                    best.eye[0], best.eye[1], best.eye[2], re[0], re[1], re[2]);
            if (++lines % 60 == 0) fflush(tf);
        }
    }
    g_lks.wait_ns += waited; if (waited > g_lks.wait_max) g_lks.wait_max = waited;
    if (++g_lks.frames % 300 == 0) {
        logf_("wdlock: %llu frames, %llu on the very camera, %llu late, %llu presents with no camera yet; wait mean %.2f ms max %.2f ms", (unsigned long long)g_lks.frames, (unsigned long long)g_lks.hits, (unsigned long long)g_lks.misses, (unsigned long long)g_lks.none, g_lks.wait_ns / 1e6 / 300, g_lks.wait_max / 1e6);
        g_lks.wait_ns = 0; g_lks.wait_max = 0;
    }
    if (got < 0) return;
    if (!g_lks.pool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
        if (d->CreateCommandPool(d->device, &pci, NULL, &g_lks.pool) != VK_SUCCESS) return;
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_lks.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, LK_N};
        if (d->AllocateCommandBuffers(d->device, &ai, g_lks.cb) != VK_SUCCESS) return;
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        for (int k = 0; k < LK_N; k++) {
            if (d->setLoaderData && d->setLoaderData(d->device, g_lks.cb[k]) != VK_SUCCESS) return;
            if (d->CreateFence(d->device, &fci, NULL, &g_lks.f[k]) != VK_SUCCESS) return;
        }
    }
    WowSlot* w = &g_wr.s[got];
    VkDeviceSize n = (VkDeviceSize)w->cbpr * w->wh + (VkDeviceSize)w->dbpr * w->wh;
    if (!gbuf_make(d, &g_lk.b, n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0)) return;
    const uint32_t k = g_lks.n++ % LK_N;
    d->WaitForFences(d->device, 1, &g_lks.f[k], VK_TRUE, 100000000ull);
    d->ResetFences(d->device, 1, &g_lks.f[k]);
    VkCommandBuffer cb = g_lks.cb[k];
    d->ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    d->BeginCommandBuffer(cb, &bi);
    VkMemoryBarrier b0 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b0, 0, NULL, 0, NULL);
    VkBufferCopy rg = {0, 0, n};
    d->CmdCopyBuffer(cb, w->b.buf, g_lk.b.buf, 1, &rg);
    VkMemoryBarrier b1 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b1, 0, NULL, 0, NULL);
    d->EndCommandBuffer(cb);
    VkCommandBufferSubmitInfo cs = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cs, 0, NULL};
    if (d->QueueSubmit2(queue, 1, &si, g_lks.f[k]) != VK_SUCCESS) return;
    g_lk.m.ww = w->ww; g_lk.m.wh = w->wh; g_lk.m.cbpr = w->cbpr; g_lk.m.dbpr = w->dbpr; g_lk.m.code = w->code; g_lk.m.cam = w->cam ? w->cam : 1; g_lk.m.wnear = w->wnear; g_lk.m.wzoff = w->wzoff; g_lk.m.glight = w->glight;
    uint8_t crec[96]; float zero[3] = {0, 0, 0}, e[3], fw[3], ri[3], up[3], tx, ty;
    if (w->cam && sf_cam_rec(w->cam, crec) && sf_cam(crec, zero, e, fw, ri, up, &tx, &ty)) { g_lk.tx = tx; g_lk.ty = ty; g_lk.have = 1; }
}

#include "wowbg3_wdeye_spv.h"
static struct { GBuf host; uint64_t res, off, n, runs; int ready, failed; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe;
                VkDescriptorPool pool; VkDescriptorSet set; VkCommandPool cpool; VkCommandBuffer cb; VkFence f; uint64_t set_res, set_off; } g_eye;
static void wd_eye_after(DeviceData* d, VkCommandBuffer cb) {
    (void)d;
    if (t_cbind_cb != cb || !t_cpipe || !t_cset[1]) return;
    CPipeInfo* p = g_cpipe ? TABLE_FIND(g_cpipe, CPIPE_CAP, t_cpipe, 0) : NULL;
    if (!p || p->spv != 0x7387a4a6b28ed0d9ull) return;
    SetInfo* si = set_find(t_cset[1]);
    if (!si || !si->slot[2].res) return;
    __atomic_store_n(&g_eye.off, si->slot[2].off, __ATOMIC_RELAXED);
    __atomic_store_n(&g_eye.res, si->slot[2].res, __ATOMIC_RELEASE);
    g_eye.runs++;
}
static int wd_eye_make(DeviceData* d, uint32_t family) {
    if (g_eye.ready) return 1;
    if (g_eye.failed) return 0;
    g_eye.failed = 1;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_eye.dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_eye.dsl, 0, NULL};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_eye.pl) != VK_SUCCESS) return 0;
    VkShaderModule m; VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_wdeye_comp, wowbg3_wdeye_comp};
    if (d->CreateShaderModule(d->device, &mci, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL}, g_eye.pl, VK_NULL_HANDLE, -1};
    VkResult r = d->CreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, NULL, &g_eye.pipe);
    d->DestroyShaderModule(d->device, m, NULL);
    if (r != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_eye.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_eye.pool, 1, &g_eye.dsl};
    if (d->AllocateDescriptorSets(d->device, &sai, &g_eye.set) != VK_SUCCESS) return 0;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    if (d->CreateCommandPool(d->device, &pci, NULL, &g_eye.cpool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_eye.cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (d->AllocateCommandBuffers(d->device, &ai, &g_eye.cb) != VK_SUCCESS) return 0;
    if (d->setLoaderData && d->setLoaderData(d->device, g_eye.cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    if (d->CreateFence(d->device, &fci, NULL, &g_eye.f) != VK_SUCCESS) return 0;
    if (!gbuf_make(d, &g_eye.host, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1)) return 0;
    g_eye.ready = 1; g_eye.failed = 0;
    logf_("wdeyelog: readback ready");
    return 1;
}
static void wd_eye_readback(DeviceData* d, VkQueue queue, uint32_t family) {
    uint64_t res = __atomic_load_n(&g_eye.res, __ATOMIC_ACQUIRE), off = __atomic_load_n(&g_eye.off, __ATOMIC_RELAXED);
    if (!res || !wd_eye_make(d, family)) return;
    if (d->GetFenceStatus(d->device, g_eye.f) != VK_SUCCESS) return;  // the last copy still running: next time
    if (g_eye.n++ % 120 == 0) {
        float f[8]; memcpy(f, g_eye.host.map, sizeof f);
        logf_("wdeyelog: eye state (buffer %llx+%llu, %llu eye runs): %g %g %g %g %g %g %g [%g]", (unsigned long long)res, (unsigned long long)off,
              (unsigned long long)g_eye.runs, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
    if (g_eye.set_res != res || g_eye.set_off != off) {
        VkDescriptorBufferInfo bi[2] = {{(VkBuffer)res, off, 28}, {g_eye.host.buf, 0, 32}};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_eye.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[0], NULL},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_eye.set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[1], NULL}};
        d->UpdateDescriptorSets(d->device, 2, w, 0, NULL);
        g_eye.set_res = res; g_eye.set_off = off;
    }
    d->ResetFences(d->device, 1, &g_eye.f);
    d->ResetCommandBuffer(g_eye.cb, 0);
    VkCommandBufferBeginInfo bgi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    d->BeginCommandBuffer(g_eye.cb, &bgi);
    VkMemoryBarrier b0 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    d->CmdPipelineBarrier(g_eye.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b0, 0, NULL, 0, NULL);
    d->CmdBindPipeline(g_eye.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_eye.pipe);
    d->CmdBindDescriptorSets(g_eye.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_eye.pl, 0, 1, &g_eye.set, 0, NULL);
    d->CmdDispatch(g_eye.cb, 1, 1, 1);
    VkMemoryBarrier b1 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    d->CmdPipelineBarrier(g_eye.cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b1, 0, NULL, 0, NULL);
    d->EndCommandBuffer(g_eye.cb);
    VkCommandBufferSubmitInfo cs = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, g_eye.cb, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cs, 0, NULL};
    d->QueueSubmit2(queue, 1, &si, g_eye.f);
}

__attribute__((visibility("hidden"))) void wd_eye_readback_(DeviceData* d, VkQueue queue, uint32_t family) { wd_eye_readback(d, queue, family); }
