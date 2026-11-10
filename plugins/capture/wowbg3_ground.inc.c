// ---------------------------------------------------------------- WoW's ground in BG3's G-buffer
// The chosen approach: BG3's own systems draw its ground visuals (enemies' sight overlay, ground
// surfaces, projected decals) and those work from BG3's scene depth and G-buffer, which in world mode
// hold no ground at all (Tav lives in BG3's empty void; WoW draws the world). So every frame, right
// after BG3's G-buffer pass, this draws WoW's ground into that G-buffer with BG3's own camera of that
// frame: depth (reverse-Z, BG3's own projection with its TAA jitter, depth-tested so BG3's own
// geometry still wins where nearer), the view-space normal and the material byte that makes a
// surface receive decals (see wowbg3_ground.frag). Everything BG3 derives from them afterwards (its
// linear depth, the decal normal, EoCSurfaceMeshGenerate, the sight overlay, decals, lighting, TAA)
// then sees WoW's ground.
//
// The ground is the triangle set the world bridge already gives the plugin for BG3's physics
// (wow-bg3/arena-triangles.bin: WoW's terrain mesh as WoW builds it, its WMO and M2 collision models;
// .kinds says which), placed in BG3's world with the session's mapping, so it is WoW's ground at
// BG3's frame time. Not WoW's exported frame depth: the compositor shows the WoW frame drawn for the
// camera `lag` frames back (wowbg3_pace.inc.c; 2 at 30 fps, 3 at 82), so no WoW depth exists yet for
// the camera BG3 is rendering. Synthetic walls (kind 1) and the flat floors WoW draws nothing at
// (kind 4) are left out.
//
// The compositor's "BG3 scene nearer" test must keep seeing BG3's own geometry only, so the depth is
// copied (depth aspect, to a buffer) just before the injection and compose_gpu uses that copy for the
// frame it presents (ground_pre_for_compose).
//
// The injection records into BG3's own command buffer between its G-buffer pass and the next pass, in
// a render pass of its own on BG3's images (LOAD/STORE, the layouts BG3's pass left), using only
// vertex input for its data (per-frame camera as an instance record), so no descriptor set or push
// constant state of BG3's is touched; the graphics pipeline, vertex buffers 0 and 1, viewport and
// scissor BG3 had bound are bound again afterwards.
// Compose word "ground" turns it on. Log: "ground:" lines every 300 presents (frames drawn, triangles,
// GPU time of the injection).
// The injection also writes a fifth target of the layer's own, Z: the ground's
// view depth where WoW's ground is BG3's visible surface (0 elsewhere), per BG3 frame (the slots of
// wowbg3_surf.inc.c), which the surfaces' albedo pass and the compositor read.
// Included by wowbg3_capture.c after the pacing code.
#include "wowbg3_ground_spv.h"

// WoW's triangles and their kinds, in <io> (WOWBG3_IO, see path_init).
static const char* gr_path(int kinds) {
    static __thread char b[2][1100];
    snprintf(b[kinds], sizeof b[0], "%s/arena-triangles.%s", io_dir(), kinds ? "kinds" : "bin");
    return b[kinds];
}
#define GR_TRIS gr_path(0)
#define GR_KINDS gr_path(1)
#define GR_VB 4        // vertex buffers kept (the newest drawn; older ones freed after GR_RETIRE presents)
#define GR_RETIRE 12
#define GR_REC 64      // per-frame camera records (ring)
#define GR_PRE 4       // pre-injection depth copies (ring)
#define GR_FB 16       // framebuffers kept (BG3 keeps one G-buffer, times the GR_PRE slots' Z; resizes make new ones)
#define GR_Q 8         // timestamp pairs (ring)
#define GR_MAP 32      // command buffers that carried an injection, and its depth copy slot

typedef struct { float vp[16], eye[4], right[4], up[4], fwd[4]; } GrRec;  // 128 bytes, one instance
typedef struct { uint64_t img[5]; VkImageView view[5]; VkFramebuffer fb; uint32_t w, h; uint64_t used; } GrFb;  // view[4]: the slot's Z (not ours to destroy)
typedef struct { GBuf b; uint32_t verts; uint64_t retire_at; int live; } GrVb;

static struct {
    pthread_mutex_t m;
    DeviceData* d;
    int ready, failed;
    VkFormat fmt[4];                 // depth, normal, albedo, surface: the formats the pipeline was made for
    int32_t lay[4];                  // their layouts (BG3's pass leaves them so)
    VkRenderPass rp;
    VkPipelineLayout layout;
    VkPipeline pipe;
    GrVb vb[GR_VB];
    int cur;                         // vertex buffer drawn, -1 none
    GBuf rec;                        // GR_REC camera records, host visible
    uint32_t rec_next;
    GBuf pre[GR_PRE];                // depth before the injection, for the compositor
    uint32_t pre_w[GR_PRE], pre_h[GR_PRE], pre_next;
    GrFb fb[GR_FB];
    uint64_t fb_clock;
    VkQueryPool qp;
    double tick_ns;
    uint64_t ts_mask;
    uint32_t q_next;
    int q_pending[GR_Q];
    struct { uint64_t cb; int pre; uint64_t serial; } map[GR_MAP];
    uint32_t map_next;
    int pending_pre, framed_pre;     // the last injected frame submitted / the one of the frame presented next
    int pending_new;                 // an injection was submitted since the last final pass
    uint64_t serial;
    uint64_t pending_serial, framed_serial;  // the same as injection serials (the surfaces' slots)
    // geometry file
    struct timespec mtime;
    off_t size;
    uint32_t tris, tris_kept;
    // statistics since the last log
    uint64_t passes, drawn, nocam, nogeo, nofb, gpu_ns, gpu_n, gpu_max, pre_used, pre_missed, loads;
    uint64_t frames_bare;            // frames presented without an injection, since the last log
    uint64_t cam_elsewhere;          // G-buffer passes that bound nothing, injected with a view block bound elsewhere
    uint64_t presents;
    uint32_t extra_dynamic;          // dynamic states (bit = VkDynamicState < 32) BG3's pipelines have besides viewport and scissor
    uint64_t binds, nobind, badview; // descriptor binds seen in G-buffer passes, without a mapped view block, rejected blocks
    uint64_t sub_same, sub_changed, sub_unread;  // camera read at recording against the same block read at BG3's submit
    double sub_deg;                  // summed view-direction change of the changed ones, degrees
} g_gr = {.m = PTHREAD_MUTEX_INITIALIZER, .cur = -1, .pending_pre = -1, .framed_pre = -1};

// wowbg3_surf.inc.c (included after this file)
static VkImageView surf_inject(DeviceData* d, VkCommandBuffer cb, uint64_t serial, const GrRec* rec, uint32_t w, uint32_t h,
                               uint64_t alb_img, VkFormat alb_fmt, int32_t alb_lay);
static void surf_submit_cb(uint64_t cb, uint64_t pending_serial);
static int wd_draw(DeviceData* d, VkCommandBuffer cb, const GrRec* rec, uint32_t w, uint32_t h, VkRenderPass rp);
static void wd_restore(DeviceData* d, VkCommandBuffer cb);
static void wd_mask_copy(DeviceData* d, VkCommandBuffer cb, uint64_t serial, uint32_t w, uint32_t h);
static void wd_mark(DeviceData* d, VkCommandBuffer cb, VkRenderPass rp, VkBuffer vbuf, VkBuffer recb, VkDeviceSize recoff, uint32_t verts);

// The G-buffer pass being recorded on this thread: its command buffer, images, size; the frame's camera.
static __thread VkCommandBuffer t_gr_cb;
static __thread uint64_t t_gr_img[4];
static uint64_t g_gb_alb, g_gb_surf, g_gb_z;  // BG3's G-buffer albedo, surface and eye-depth (metres) images (wdbare; the feed's fog)
static __thread int32_t t_gr_lay[4];
static __thread uint32_t t_gr_w, t_gr_h;
static __thread int t_gr_cam;
static __thread GrRec t_gr_rec;
static __thread uint64_t t_gr_src_res, t_gr_src_off;  // where t_gr_rec was read: BG3's view block (buffer, offset)
// Viewport and scissor BG3 set last on this thread's command buffer (restored after the injection).
static __thread VkCommandBuffer t_vs_cb;
static __thread VkViewport t_vs_vp;
static __thread VkRect2D t_vs_sc;
static __thread int t_vs_have;  // 1 viewport, 2 scissor

static void ground_note_viewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    if (t_vs_cb != cb) { t_vs_cb = cb; t_vs_have = 0; }
    if (first == 0 && n >= 1) { t_vs_vp = v[0]; t_vs_have |= 1; }
}
static void ground_note_scissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    if (t_vs_cb != cb) { t_vs_cb = cb; t_vs_have = 0; }
    if (first == 0 && n >= 1) { t_vs_sc = r[0]; t_vs_have |= 2; }
}
// Pipelines made with dynamic state other than viewport and scissor (state the injection's own
// pipeline would leave undefined for BG3's next draw): noted once, logged by ground_present.
static void ground_note_pipeline(const VkGraphicsPipelineCreateInfo* ci) {
    const VkPipelineDynamicStateCreateInfo* ds = ci->pDynamicState;
    for (uint32_t k = 0; ds && k < ds->dynamicStateCount; k++) {
        VkDynamicState s = ds->pDynamicStates[k];
        if (s != VK_DYNAMIC_STATE_VIEWPORT && s != VK_DYNAMIC_STATE_SCISSOR)
            __atomic_or_fetch(&g_gr.extra_dynamic, (uint32_t)s < 32 ? 1u << s : 0x80000000u, __ATOMIC_RELAXED);
    }
}

// BG3's per-view block (set 0 binding 2, 224 bytes) as bound on this thread now: a camera record when
// it is a perspective reverse-Z one (as rebase_view in benilla checks it).
static int ground_view_parse(const float* f, GrRec* out) {
    // floats 50, 51 and 55 are not the camera's (55 is an int)
    for (int i = 0; i < 55; i++) if (i != 50 && i != 51 && !isfinite(f[i])) { g_gr.badview++; return 0; }
    if (f[16 + 2 * 4 + 3] != 1.0f || f[16 + 3 * 4 + 3] != 0.0f || !(f[16] > 0.0f) || !(f[16 + 5] > 0.0f) || !(f[48] > 0.0f)) {
        g_gr.badview++;
        return 0;
    }
    memcpy(out->vp, f + 32, 64);
    out->eye[0] = f[52]; out->eye[1] = f[53]; out->eye[2] = f[54]; out->eye[3] = 1.0f;
    // V: float i * 4 + j is axis j's component i (rows right, up, forward).
    for (int i = 0; i < 3; i++) { out->right[i] = f[i * 4 + 0]; out->up[i] = f[i * 4 + 1]; out->fwd[i] = f[i * 4 + 2]; }
    out->right[3] = out->up[3] = out->fwd[3] = 0.0f;
    return 1;
}
static int ground_view_record(GrRec* out) {
    const float* f = (const float*)fxc_slot_cpu(0, 2, 224);
    if (!f) { g_gr.nobind++; return 0; }
    if (!ground_view_parse(f, out)) return 0;
    // The block's buffer and offset as bound (fxc_slot_cpu's lookup), to read it again at BG3's submit.
    SetInfo* si = set_find(t_set[0]);
    DescSlot* sl = si ? &si->slot[2] : NULL;
    t_gr_src_res = sl ? sl->res : 0;
    t_gr_src_off = sl ? sl->off : 0;
    if (sl && (sl->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || sl->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) {
        uint32_t at = 0;
        for (uint32_t b = 0; b < 2; b++)
            if (si->slot[b].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || si->slot[b].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) at++;
        if (at < t_ndyn[0] && at < 8) t_gr_src_off += t_dyn[0][at];
    }
    return 1;
}

// With nothing of BG3's in view (Tav out of frame, a stand-in not drawn yet) its G-buffer pass makes no
// descriptor bind, so the pass itself names no camera; BG3's decal and forward passes of the frame still
// bind the view block, on other threads, often before. So every view block bound anywhere is remembered,
// as its buffer and offset (BG3 may free a buffer; buf_cpu then finds nothing), newest last. Shadow views
// are blocks too: a block is the camera's when its eye is one of the plugin's camera records.
#define GR_VIEWS 24
static struct { uint64_t res, off, clock; } g_gr_views[GR_VIEWS];
static uint32_t g_gr_views_next;
static uint64_t g_gr_views_clock;
static pthread_mutex_t g_gr_views_m = PTHREAD_MUTEX_INITIALIZER;
static __thread uint64_t t_gr_view_res, t_gr_view_off;

// A graphics descriptor bind on this thread, in any pass.
static void ground_view_seen(void) {
    if (!g_cc.ground || !t_set[0]) return;
    SetInfo* si = set_find(t_set[0]);
    if (!si) return;
    DescSlot* sl = &si->slot[2];
    if (!sl->res) return;
    uint64_t off = sl->off;
    if (sl->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || sl->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
        uint32_t at = 0;
        for (uint32_t b = 0; b < 2; b++)
            if (si->slot[b].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || si->slot[b].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) at++;
        if (at < t_ndyn[0] && at < 8) off += t_dyn[0][at];
    }
    if (sl->res == t_gr_view_res && off == t_gr_view_off) return;
    t_gr_view_res = sl->res;
    t_gr_view_off = off;
    pthread_mutex_lock(&g_gr_views_m);
    int k = -1;
    for (int i = 0; i < GR_VIEWS && k < 0; i++) if (g_gr_views[i].res == sl->res && g_gr_views[i].off == off) k = i;
    if (k < 0) { k = (int)g_gr_views_next; g_gr_views_next = (g_gr_views_next + 1) % GR_VIEWS; }
    g_gr_views[k].res = sl->res; g_gr_views[k].off = off; g_gr_views[k].clock = ++g_gr_views_clock;
    pthread_mutex_unlock(&g_gr_views_m);
}

// The camera of a G-buffer pass that bound nothing: the newest view block whose eye is a camera record's.
// It is this frame's when another pass of the frame has bound it already, else the frame before's.
static int ground_view_elsewhere(GrRec* out) {
    __typeof__(g_gr_views[0]) v[GR_VIEWS];
    pthread_mutex_lock(&g_gr_views_m);
    memcpy(v, g_gr_views, sizeof v);
    pthread_mutex_unlock(&g_gr_views_m);
    for (int n = 0; n < GR_VIEWS; n++) {
        int k = -1;
        for (int i = 0; i < GR_VIEWS; i++) if (v[i].res && (k < 0 || v[i].clock > v[k].clock)) k = i;
        if (k < 0) return 0;
        uint64_t left = 0, kres = v[k].res, koff = v[k].off;
        const float* f = (const float*)buf_cpu(kres, koff, &left);
        v[k].res = 0;
        if (!f || left < 224 || !ground_view_parse(f, out)) continue;
        uint32_t seq = 0;
        float err = -1, err2 = -1;
        if (pace_match(out->eye, &seq, &err, &err2) && err < 0.05f) { t_gr_src_res = kres; t_gr_src_off = koff; return 1; }
    }
    return 0;
}

// The camera as read while BG3 records the frame, against the same view block read again when BG3 submits that
// command buffer (by then BG3 has written the frame's constants: Vulkan wants host writes done before the submit).
// Different means BG3 fills the block after recording, and the injection drew with an older frame's camera.
#define GR_SUB 64
static struct { uint64_t cb, res, off; float vp[16], fwd[3]; } g_gr_sub[GR_SUB];
static uint32_t g_gr_sub_next;
static void ground_sub_note(VkCommandBuffer cb) {  // g_gr.m held
    uint32_t k = g_gr_sub_next++ % GR_SUB;
    g_gr_sub[k].cb = (uint64_t)(uintptr_t)cb; g_gr_sub[k].res = t_gr_src_res; g_gr_sub[k].off = t_gr_src_off;
    memcpy(g_gr_sub[k].vp, t_gr_rec.vp, 64); memcpy(g_gr_sub[k].fwd, t_gr_rec.fwd, 12);
}
static void ground_cam_check(VkCommandBuffer cb) {
    pthread_mutex_lock(&g_gr.m);
    for (uint32_t i = 0; i < GR_SUB; i++) {
        if (g_gr_sub[i].cb != (uint64_t)(uintptr_t)cb) continue;
        __typeof__(g_gr_sub[0]) e = g_gr_sub[i];
        g_gr_sub[i].cb = 0;
        uint64_t left = 0;
        const float* f = e.res ? (const float*)buf_cpu(e.res, e.off, &left) : NULL;
        GrRec r;
        if (!f || left < 224 || !ground_view_parse(f, &r)) { g_gr.sub_unread++; continue; }
        if (!memcmp(r.vp, e.vp, 64)) { g_gr.sub_same++; continue; }
        g_gr.sub_changed++;
        double dot = e.fwd[0] * r.fwd[0] + e.fwd[1] * r.fwd[1] + e.fwd[2] * r.fwd[2];
        g_gr.sub_deg += acos(dot > 1.0 ? 1.0 : dot < -1.0 ? -1.0 : dot) * 57.29577951308232;
    }
    pthread_mutex_unlock(&g_gr.m);
}

// begin_pass found a pass at swapchain size (g_trace_lock held): BG3's G-buffer pass is the scene
// depth with five colour targets cleared, the first R16G16 (normal, albedo, surface, two more).
static void ground_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const uint8_t* load, const int32_t* fin, uint32_t w, uint32_t h) {
    if (t_gr_cb == cb) t_gr_cb = VK_NULL_HANDLE;
    if (!g_cc.ground || !g_role.depth || (g_screen && !g_screen_dialog)) return;
    uint64_t col[8]; int32_t cfin[8]; uint32_t nc = 0; int depth = 0, cleared = 1; int32_t dfin = -1;
    for (uint32_t k = 0; k < n; k++) {
        if (imgs[k] == g_role.depth) { depth = 1; dfin = fin[k]; continue; }
        if (nc < 8) { cfin[nc] = fin[k]; col[nc++] = imgs[k]; }
        if (load[k] != VK_ATTACHMENT_LOAD_OP_CLEAR) cleared = 0;
    }
    if (!depth || nc != 5 || !cleared) return;
    // The layouts the pass leaves its attachments in (the injection's own pass keeps them).
    if (dfin != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || cfin[0] != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        cfin[1] != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || cfin[2] != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        static int said;
        if (!said++) logf_("ground: BG3's G-buffer pass leaves layouts %d %d %d %d: not injected", dfin, cfin[0], cfin[1], cfin[2]);
        return;
    }
    ImgInfo* i0 = TABLE_FIND(g_img, IMG_CAP, col[0], 0);
    ImgInfo* i1 = TABLE_FIND(g_img, IMG_CAP, col[1], 0);
    ImgInfo* i2 = TABLE_FIND(g_img, IMG_CAP, col[2], 0);
    if (!i0 || !i1 || !i2 || i0->fmt != VK_FORMAT_R16G16_SFLOAT || i0->samples > 1) return;
    t_gr_cb = cb;
    t_gr_img[0] = g_role.depth; t_gr_img[1] = col[0]; t_gr_img[2] = col[1]; t_gr_img[3] = col[2];
    g_gb_alb = col[1]; g_gb_surf = col[2]; g_gb_z = col[3];
    t_gr_lay[0] = dfin; t_gr_lay[1] = cfin[0]; t_gr_lay[2] = cfin[1]; t_gr_lay[3] = cfin[2];
    t_gr_w = w; t_gr_h = h;
    t_gr_cam = t_bind_cb == cb ? ground_view_record(&t_gr_rec) : 0;
}
// A descriptor bind in the G-buffer pass: this frame's camera (the newest one bound in the pass).
static void ground_bind(VkCommandBuffer cb) {
    if (t_gr_cb != cb) return;
    g_gr.binds++;
    GrRec r;
    uint64_t res0 = t_gr_src_res, off0 = t_gr_src_off;
    if (ground_view_record(&r)) { t_gr_rec = r; t_gr_cam = 1; }
    else { t_gr_src_res = res0; t_gr_src_off = off0; }
}

static int ground_init(DeviceData* d, const VkFormat fmt[4], const int32_t lay[4]) {
    if (g_gr.ready) {
        if (g_gr.d != d) return 0;
        if (!memcmp(g_gr.fmt, fmt, sizeof g_gr.fmt) && !memcmp(g_gr.lay, lay, sizeof g_gr.lay)) return 1;
        // Other formats or layouts (never seen): a new render pass and pipeline; the old ones stay.
        g_gr.ready = 0;
        for (int k = 0; k < GR_FB; k++) memset(&g_gr.fb[k], 0, sizeof g_gr.fb[k]);  // leaked: tied to the old pass
    }
    if (g_gr.failed > 3) return 0;
    if (!d->CreateRenderPass || !d->CreateGraphicsPipelines || !d->CreatePipelineLayout || !d->CreateShaderModule ||
        !d->CreateImageView || !d->CreateFramebuffer || !d->CmdSetViewport || !d->CmdSetScissor || !d->CmdBeginRenderPass) {
        g_gr.failed = 99;
        logf_("ground: device functions missing; off");
        return 0;
    }
    g_gr.failed++;
    g_gr.d = d;
    memcpy(g_gr.fmt, fmt, sizeof g_gr.fmt);
    memcpy(g_gr.lay, lay, sizeof g_gr.lay);
    VkAttachmentDescription at[5];
    for (int k = 0; k < 4; k++)
        at[k] = (VkAttachmentDescription){0, fmt[k], VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                                          VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, (VkImageLayout)lay[k], (VkImageLayout)lay[k]};
    // Z (the layer's own): cleared to 0 each frame, left in GENERAL for the surfaces' readers.
    at[4] = (VkAttachmentDescription){0, VK_FORMAT_R32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                      VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference cref[4] = {{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                     {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {4, VK_IMAGE_LAYOUT_GENERAL}};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 4, cref, NULL, &dref, 0, NULL};
    const VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags ac = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, st | VK_PIPELINE_STAGE_TRANSFER_BIT, st, ac | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, ac, 0},
        {0, VK_SUBPASS_EXTERNAL, st, st | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
         ac, ac | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0}};
    VkRenderPassCreateInfo rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 5, at, 1, &sp, 2, dep};
    if (d->CreateRenderPass(d->device, &rci, NULL, &g_gr.rp) != VK_SUCCESS) { logf_("ground: render pass not made"); return 0; }
    if (!g_gr.layout) {
        VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
        if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_gr.layout) != VK_SUCCESS) return 0;
    }
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_ground_vert, wowbg3_ground_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_ground_frag, wowbg3_ground_frag};
    if (d->CreateShaderModule(d->device, &mv, NULL, &vs) != VK_SUCCESS || d->CreateShaderModule(d->device, &mf, NULL, &fs) != VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkVertexInputBindingDescription vb[2] = {{0, 24, VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(GrRec), VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription va[10] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}};
    for (uint32_t k = 0; k < 8; k++) va[2 + k] = (VkVertexInputAttributeDescription){2 + k, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * k};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 2, vb, 10, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE,
                                                 VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT,
                                               VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE};
    // Reverse-Z: nearer is greater. BG3's own geometry (stand-ins) keeps its depth where it is nearer.
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, VK_TRUE,
                                                 VK_COMPARE_OP_GREATER, VK_FALSE, VK_FALSE, {0}, {0}, 0.0f, 1.0f};
    VkPipelineColorBlendAttachmentState cb[4];
    for (int k = 0; k < 4; k++)
        cb[k] = (VkPipelineColorBlendAttachmentState){VK_FALSE, 0, 0, 0, 0, 0, 0,
                                                      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    cb[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    cb[3].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 4, cb, {0, 0, 0, 0}};
    // Viewport and scissor (set here), and every other core dynamic state BG3's own pipelines use
    // (stencil reference, seen): dynamic in this pipeline too, so binding it leaves BG3's
    // values in the command buffer as they were. None of them matters to this draw (no stencil test,
    // depth bias, blending, depth bounds or lines).
    VkDynamicState dyn[9] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 9, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms, &dss,
                                        &cbs, &dy, g_gr.layout, g_gr.rp, 0, VK_NULL_HANDLE, -1};
    // The driver's entry, not the layer's own (the recipe and pipeline tables are BG3's).
    VkResult r = d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_gr.pipe);
    if (d->DestroyShaderModule) { d->DestroyShaderModule(d->device, vs, NULL); d->DestroyShaderModule(d->device, fs, NULL); }
    if (r != VK_SUCCESS) { logf_("ground: pipeline not made (%d)", (int)r); return 0; }
    if (!g_gr.rec.buf && !gbuf_make(d, &g_gr.rec, sizeof(GrRec) * GR_REC, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1)) return 0;
    if (!g_gr.qp && d->CreateQueryPool && d->CmdResetQueryPool && d->CmdWriteTimestamp && d->GetQueryPoolResults) {
        PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)d->inst->gipa(d->inst->instance, "vkGetPhysicalDeviceProperties");
        VkPhysicalDeviceProperties pp;
        if (gpp) {
            gpp(d->physical, &pp);
            VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * GR_Q, 0};
            if (pp.limits.timestampComputeAndGraphics && pp.limits.timestampPeriod > 0 && d->CreateQueryPool(d->device, &qci, NULL, &g_gr.qp) == VK_SUCCESS) {
                g_gr.tick_ns = pp.limits.timestampPeriod;
                g_gr.ts_mask = ~0ull;
            }
        }
    }
    g_gr.ready = 1;
    g_gr.failed = 0;
    logf_("ground: ready (depth format %d layout %d, normal %d, albedo %d, surface %d; timestamps %s)", (int)fmt[0], (int)lay[0], (int)fmt[1],
          (int)fmt[2], (int)fmt[3], g_gr.qp ? "on" : "off");
    return 1;
}

// The framebuffer over BG3's four images and the slot's Z (g_gr.m held).
static VkFramebuffer ground_fb(DeviceData* d, const uint64_t img[4], VkImageView zview, uint32_t w, uint32_t h) {
    GrFb* lru = &g_gr.fb[0];
    for (int k = 0; k < GR_FB; k++) {
        GrFb* f = &g_gr.fb[k];
        if (f->fb && f->w == w && f->h == h && !memcmp(f->img, img, 4 * sizeof f->img[0]) && f->view[4] == zview) { f->used = ++g_gr.fb_clock; return f->fb; }
        if (!f->fb || f->used < lru->used) lru = f;
    }
    if (lru->fb) return VK_NULL_HANDLE;  // all in use: should not happen (ground_forget_image frees them)
    GrFb f = {{img[0], img[1], img[2], img[3], (uint64_t)zview}, {0}, VK_NULL_HANDLE, w, h, ++g_gr.fb_clock};
    for (int k = 0; k < 4; k++) {
        VkImageAspectFlags a = VK_IMAGE_ASPECT_COLOR_BIT;
        if (k == 0) {
            a = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (g_gr.fmt[0] == VK_FORMAT_D32_SFLOAT_S8_UINT || g_gr.fmt[0] == VK_FORMAT_D24_UNORM_S8_UINT || g_gr.fmt[0] == VK_FORMAT_D16_UNORM_S8_UINT)
                a |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)img[k], VK_IMAGE_VIEW_TYPE_2D, g_gr.fmt[k],
                                     {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                     {a, 0, 1, 0, 1}};
        if (d->CreateImageView(d->device, &vci, NULL, &f.view[k]) != VK_SUCCESS) {
            for (int j = 0; j < k; j++) if (d->DestroyImageView) d->DestroyImageView(d->device, f.view[j], NULL);
            return VK_NULL_HANDLE;
        }
    }
    f.view[4] = zview;
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_gr.rp, 5, f.view, w, h, 1};
    if (d->CreateFramebuffer(d->device, &fci, NULL, &f.fb) != VK_SUCCESS) {
        for (int j = 0; j < 4; j++) if (d->DestroyImageView) d->DestroyImageView(d->device, f.view[j], NULL);
        return VK_NULL_HANDLE;
    }
    *lru = f;
    return f.fb;
}
// BG3 destroys an image (it waited for its own work, ours included): drop our views of it.
static void ground_forget_image(DeviceData* d, VkImage img) {
    pthread_mutex_lock(&g_gr.m);
    for (int k = 0; k < GR_FB; k++) {
        GrFb* f = &g_gr.fb[k];
        if (!f->fb) continue;
        int hit = 0;
        for (int j = 0; j < 4; j++) hit |= f->img[j] == (uint64_t)img;
        if (!hit) continue;
        if (d->DestroyFramebuffer) d->DestroyFramebuffer(d->device, f->fb, NULL);
        for (int j = 0; j < 4; j++) if (d->DestroyImageView) d->DestroyImageView(d->device, f->view[j], NULL);
        memset(f, 0, sizeof *f);
    }
    pthread_mutex_unlock(&g_gr.m);
}

// After BG3's G-buffer pass ended on `cb` (its EndRenderPass already recorded): WoW's ground into it.
static void ground_inject(DeviceData* d, VkCommandBuffer cb) {
    t_gr_cb = VK_NULL_HANDLE;
    if (!g_cc.on || !g_cc.ground || !d) return;
    __atomic_add_fetch(&g_gr.passes, 1, __ATOMIC_RELAXED);
    if (!t_gr_cam) {
        if (!ground_view_elsewhere(&t_gr_rec)) { __atomic_add_fetch(&g_gr.nocam, 1, __ATOMIC_RELAXED); return; }
        t_gr_cam = 1;
        __atomic_add_fetch(&g_gr.cam_elsewhere, 1, __ATOMIC_RELAXED);
    }
    VkFormat fmt[4];
    int32_t lay[4];
    pthread_mutex_lock(&g_trace_lock);
    for (int k = 0; k < 4; k++) {
        ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, t_gr_img[k], 0);
        fmt[k] = i ? i->fmt : VK_FORMAT_UNDEFINED;
    }
    pthread_mutex_unlock(&g_trace_lock);
    memcpy(lay, t_gr_lay, sizeof lay);
    if (!fmt[0] || !fmt[1] || !fmt[2] || !fmt[3]) return;
    pthread_mutex_lock(&g_gr.m);
    if (!ground_init(d, fmt, lay)) { pthread_mutex_unlock(&g_gr.m); return; }
    const int vi = g_gr.cur;
    if (vi < 0 || !g_gr.vb[vi].verts) { pthread_mutex_unlock(&g_gr.m); __atomic_add_fetch(&g_gr.nogeo, 1, __ATOMIC_RELAXED); return; }
    VkBuffer vbuf = g_gr.vb[vi].b.buf;
    const uint32_t verts = g_gr.vb[vi].verts;
    const uint64_t serial = ++g_gr.serial;  // this BG3 frame (the surfaces' slot)
    pthread_mutex_unlock(&g_gr.m);
    // The slot's Z (the fifth target) and the surfaces' albedo pass, owed at the end of `cb`.
    VkImageView zview = surf_inject(d, cb, serial, &t_gr_rec, t_gr_w, t_gr_h, t_gr_img[2], fmt[2], lay[2]);
    if (!zview) { __atomic_add_fetch(&g_gr.nofb, 1, __ATOMIC_RELAXED); return; }
    pthread_mutex_lock(&g_gr.m);
    VkFramebuffer fb = ground_fb(d, t_gr_img, zview, t_gr_w, t_gr_h);
    if (!fb) { pthread_mutex_unlock(&g_gr.m); __atomic_add_fetch(&g_gr.nofb, 1, __ATOMIC_RELAXED); return; }
    const uint32_t ri = g_gr.rec_next++ % GR_REC;
    memcpy((uint8_t*)g_gr.rec.map + (size_t)ri * sizeof(GrRec), &t_gr_rec, sizeof(GrRec));
    ground_sub_note(cb);
    if (g_cc.wdtime) {  // the camera each injection drew with: eye and view direction of BG3's view block, per BG3 frame
        static FILE* cf; static uint64_t cn;
        if (!cf) { cf = fopen(outp("/grcam.csv"), "w"); if (cf) fprintf(cf, "serial,present,t_ns,eye_x,eye_y,eye_z,fwd_x,fwd_y,fwd_z\n"); }
        if (cf) { fprintf(cf, "%llu,%llu,%llu,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f\n", (unsigned long long)serial,
                          (unsigned long long)__atomic_load_n(&g_req.presents, __ATOMIC_RELAXED), (unsigned long long)now_ns(),
                          t_gr_rec.eye[0], t_gr_rec.eye[1], t_gr_rec.eye[2], t_gr_rec.fwd[0], t_gr_rec.fwd[1], t_gr_rec.fwd[2]);
                  if (++cn % 60 == 0) fflush(cf); }
    }
    // The depth as BG3 drew it, for the compositor (only when it composites on the GPU).
    int pre = -1;
    const int db = depth_bytes(fmt[0]);
    if (db == 4) {
        const uint32_t pk = g_gr.pre_next % GR_PRE;
        if (gbuf_make(d, &g_gr.pre[pk], (VkDeviceSize)t_gr_w * t_gr_h * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0)) {
            pre = (int)pk;
            g_gr.pre_w[pk] = t_gr_w; g_gr.pre_h[pk] = t_gr_h;
            g_gr.pre_next++;
        }
    }
    const uint32_t qi = g_gr.q_next++ % GR_Q;
    const int timed = g_gr.qp && !g_gr.q_pending[qi];
    if (timed) g_gr.q_pending[qi] = 1;
    // Which command buffer carried which depth copy (the compositor finds its frame's by submission order).
    g_gr.map[g_gr.map_next % GR_MAP].cb = (uint64_t)(uintptr_t)cb;
    g_gr.map[g_gr.map_next % GR_MAP].pre = pre;
    g_gr.map[g_gr.map_next % GR_MAP].serial = serial;
    g_gr.map_next++;
    VkRenderPass rpass = g_gr.rp;
    VkPipeline pipe = g_gr.pipe;
    VkBuffer recb = g_gr.rec.buf;
    VkBuffer preb = pre >= 0 ? g_gr.pre[pre].buf : VK_NULL_HANDLE;
    VkQueryPool qp = g_gr.qp;
    pthread_mutex_unlock(&g_gr.m);

    if (timed) {
        d->CmdResetQueryPool(cb, qp, 2 * qi, 2);
        d->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 2 * qi);
    }
    if (preb) {
        VkImageAspectFlags da = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (fmt[0] == VK_FORMAT_D32_SFLOAT_S8_UINT) da |= VK_IMAGE_ASPECT_STENCIL_BIT;
        barrier_aspect(d, cb, (VkImage)t_gr_img[0], da, (VkImageLayout)lay[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy rd = {0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {t_gr_w, t_gr_h, 1}};
        d->CmdCopyImageToBuffer(cb, (VkImage)t_gr_img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, preb, 1, &rd);
        barrier_aspect(d, cb, (VkImage)t_gr_img[0], da, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkImageLayout)lay[0]);
    }
    VkClearValue clear[5];
    memset(clear, 0, sizeof clear);  // only Z (attachment 4) is cleared
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rpass, fb, {{0, 0}, {t_gr_w, t_gr_h}}, 5, clear};
    d->CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)t_gr_w, (float)t_gr_h, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {t_gr_w, t_gr_h}};
    int wdrawn = 0;
    if (g_cc.wdepth) {
        d->CmdSetViewport(cb, 0, 1, &vp);
        d->CmdSetScissor(cb, 0, 1, &sc);
        wdrawn = wd_draw(d, cb, &t_gr_rec, t_gr_w, t_gr_h, rpass);
        if (wdrawn) wd_mark(d, cb, rpass, vbuf, recb, (VkDeviceSize)ri * sizeof(GrRec), verts);
    }
    if (!wdrawn) {
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    d->CmdSetViewport(cb, 0, 1, &vp);
    d->CmdSetScissor(cb, 0, 1, &sc);
    VkBuffer bufs[2] = {vbuf, recb};
    VkDeviceSize offs[2] = {0, (VkDeviceSize)ri * sizeof(GrRec)};
    d->CmdBindVertexBuffers(cb, 0, 2, bufs, offs);
    d->CmdDraw(cb, verts, 1, 0, 0);
    }
    d->CmdEndRenderPass(cb);
    if (wdrawn) { wd_restore(d, cb); wd_mask_copy(d, cb, serial, t_gr_w, t_gr_h); }
    if (timed) d->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2 * qi + 1);
    // BG3's state as it was: its graphics pipeline, vertex buffers 0 and 1, viewport and scissor.
    if (t_bind_cb == cb) {
        if (t_pipe) d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)t_pipe);
        for (uint32_t k = 0; k < 2; k++)
            if (t_vb[k]) { VkBuffer b = (VkBuffer)t_vb[k]; VkDeviceSize o = t_vbo[k]; d->CmdBindVertexBuffers(cb, k, 1, &b, &o); }
    }
    if (t_vs_cb == cb) {
        if (t_vs_have & 1) d->CmdSetViewport(cb, 0, 1, &t_vs_vp);
        if (t_vs_have & 2) d->CmdSetScissor(cb, 0, 1, &t_vs_sc);
    }
    __atomic_add_fetch(&g_gr.drawn, 1, __ATOMIC_RELAXED);
}

static void ground_end(DeviceData* d, VkCommandBuffer cb) {
    if (t_gr_cb == cb) ground_inject(d, cb);
}

// Submission order (from role_submit, g_trace_lock held): an injected frame becomes pending; the
// final pass that follows makes it the frame presented next. A final pass with no injection since the
// last one is a frame without WoW's ground in it, and nothing of an older frame may stand in for it:
// no depth copy (the compositor reads BG3's depth image, which has no ground to exclude) and no
// surface slot. Its A1 and H say nothing about the ground (no ground there, H holds BG3's sky), and
// pairing them with an older frame's A0 and Z whitened all of WoW's ground for as long as it lasted.
static void ground_submit(uint32_t n, const VkCommandBuffer* cbs) {
    if (!g_cc.ground) return;
    for (uint32_t i = 0; i < n; i++) ground_cam_check(cbs[i]);
    pthread_mutex_lock(&g_gr.m);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t h = (uint64_t)(uintptr_t)cbs[i];
        uint64_t best = 0;
        for (uint32_t k = 0; k < GR_MAP; k++)
            if (g_gr.map[k].cb == h) {
                if (g_gr.map[k].serial > best) { best = g_gr.map[k].serial; g_gr.pending_pre = g_gr.map[k].pre; g_gr.pending_serial = best; }
                g_gr.map[k].cb = 0;  // BG3 re-records this handle: a later frame on it may have no injection
                g_gr.pending_new = 1;
            }
        surf_submit_cb(h, g_gr.pending_serial);
        CbRole* r = TABLE_FIND(g_cbrole, CBROLE_CAP, h, 0);
        if (r && r->init && r->fin_ui) {
            g_gr.framed_pre = g_gr.pending_new ? g_gr.pending_pre : -1;
            g_gr.framed_serial = g_gr.pending_new ? g_gr.pending_serial : 0;
            if (!g_gr.pending_new) g_gr.frames_bare++;
            g_gr.pending_new = 0;
        }
    }
    pthread_mutex_unlock(&g_gr.m);
}

// compose_gpu: the injection serial (surfaces' slot) of the frame being presented, 0 none.
static uint64_t ground_framed_serial(void) {
    if (!g_cc.ground) return 0;
    pthread_mutex_lock(&g_gr.m);
    uint64_t s = g_gr.framed_serial;
    pthread_mutex_unlock(&g_gr.m);
    return s;
}

// compose_gpu (g_lock and g_trace_lock held): the depth copy of the frame being presented, taken
// before the injection, or NULL (compose then copies BG3's depth image as before).
static GBuf* ground_pre_for_compose(uint32_t w, uint32_t h, VkFormat dfmt) {
    if (!g_cc.ground || depth_bytes(dfmt) != 4) return NULL;
    pthread_mutex_lock(&g_gr.m);
    int k = g_gr.framed_pre;
    GBuf* b = (k >= 0 && k < GR_PRE && g_gr.pre[k].buf && g_gr.pre_w[k] == w && g_gr.pre_h[k] == h) ? &g_gr.pre[k] : NULL;
    if (b) g_gr.pre_used++; else g_gr.pre_missed++;
    pthread_mutex_unlock(&g_gr.m);
    return b;
}

// At each present (g_lock held): the triangle file, GPU times, the log.
static void ground_present(void) {
    if (!g_cc.on || !g_cc.ground) return;
    g_gr.presents++;
    DeviceData* d = g_gr.d;
    if (!d) {  // the first injection makes the device known; until then use the compositor's
        d = g_gc.dev;
        if (!d) return;
        g_gr.d = d;
    }
    pthread_mutex_lock(&g_gr.m);
    // Timestamps of injections at least GR_Q / 2 presents old.
    if (g_gr.qp) {
        for (uint32_t k = 0; k < GR_Q; k++) {
            if (!g_gr.q_pending[k]) continue;
            uint64_t ts[2];
            VkResult r = d->GetQueryPoolResults(d->device, g_gr.qp, 2 * k, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT);
            if (r == VK_NOT_READY) continue;
            g_gr.q_pending[k] = 0;
            if (r != VK_SUCCESS) continue;
            uint64_t ns = (uint64_t)((double)((ts[1] - ts[0]) & g_gr.ts_mask) * g_gr.tick_ns);
            g_gr.gpu_ns += ns; g_gr.gpu_n++;
            if (ns > g_gr.gpu_max) g_gr.gpu_max = ns;
        }
    }
    // Retired vertex buffers.
    for (int k = 0; k < GR_VB; k++)
        if (g_gr.vb[k].live && k != g_gr.cur && g_gr.vb[k].retire_at && g_gr.presents >= g_gr.vb[k].retire_at) {
            gbuf_free(d, &g_gr.vb[k].b);
            memset(&g_gr.vb[k], 0, sizeof g_gr.vb[k]);
        }
    pthread_mutex_unlock(&g_gr.m);
    // The triangles, when the bridge wrote new ones (checked every 10 presents).
    if (g_gr.presents % 10 == 1) {
        struct stat st, sk;
        if (stat(GR_TRIS, &st) == 0 && stat(GR_KINDS, &sk) == 0 &&
            (st.st_mtim.tv_sec != g_gr.mtime.tv_sec || st.st_mtim.tv_nsec != g_gr.mtime.tv_nsec || st.st_size != g_gr.size)) {
            uint8_t* tri = NULL;
            uint8_t* kind = NULL;
            size_t nt = (size_t)st.st_size / 36;
            int ok = 0;
            if (nt && nt < 2000000 && (size_t)sk.st_size == nt) {
                tri = malloc(nt * 36);
                kind = malloc(nt);
                FILE* f = tri && kind ? fopen(GR_TRIS, "rb") : NULL;
                FILE* g = f ? fopen(GR_KINDS, "rb") : NULL;
                ok = f && g && fread(tri, 36, nt, f) == nt && fread(kind, 1, nt, g) == nt;
                if (f) fclose(f);
                if (g) fclose(g);
            }
            if (ok) {
                // Kept: 0 terrain, 2 WMO, 3 M2 (what WoW draws); left out: 1 synthetic walls, 4 flat
                // floors WoW draws nothing at. Per vertex: position, the face's normal (terrain up).
                size_t keep = 0;
                for (size_t t = 0; t < nt; t++) keep += kind[t] == 0 || kind[t] == 2 || kind[t] == 3;
                float* v = keep ? malloc(keep * 3 * 24) : NULL;
                size_t o = 0;
                for (size_t t = 0; v && t < nt; t++) {
                    if (!(kind[t] == 0 || kind[t] == 2 || kind[t] == 3)) continue;
                    float p[9];
                    memcpy(p, tri + t * 36, 36);
                    float ux = p[3] - p[0], uy = p[4] - p[1], uz = p[5] - p[2], wx = p[6] - p[0], wy = p[7] - p[1], wz = p[8] - p[2];
                    float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
                    float l = sqrtf(nx * nx + ny * ny + nz * nz);
                    if (!(l > 1e-12f)) { nx = 0; ny = 1; nz = 0; l = 1; }
                    nx /= l; ny /= l; nz /= l;
                    if (kind[t] == 0 && ny < 0) { nx = -nx; ny = -ny; nz = -nz; }
                    for (int c = 0; c < 3; c++) {
                        v[o++] = p[c * 3]; v[o++] = p[c * 3 + 1]; v[o++] = p[c * 3 + 2];
                        v[o++] = nx; v[o++] = ny; v[o++] = nz;
                    }
                }
                pthread_mutex_lock(&g_gr.m);
                int slot = -1;
                for (int k = 0; k < GR_VB && slot < 0; k++) if (!g_gr.vb[k].live) slot = k;
                if (slot >= 0 && v && gbuf_make(d, &g_gr.vb[slot].b, keep * 3 * 24, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1)) {
                    memcpy(g_gr.vb[slot].b.map, v, keep * 3 * 24);
                    g_gr.vb[slot].verts = (uint32_t)(keep * 3);
                    g_gr.vb[slot].live = 1;
                    g_gr.vb[slot].retire_at = 0;
                    if (g_gr.cur >= 0) g_gr.vb[g_gr.cur].retire_at = g_gr.presents + GR_RETIRE;
                    g_gr.cur = slot;
                    g_gr.mtime = st.st_mtim;
                    g_gr.size = st.st_size;
                    g_gr.tris = (uint32_t)nt;
                    g_gr.tris_kept = (uint32_t)keep;
                    g_gr.loads++;
                }
                pthread_mutex_unlock(&g_gr.m);
                free(v);
            }
            free(tri);
            free(kind);
        }
    }
    if (g_gr.presents % 300 == 0) {
        pthread_mutex_lock(&g_gr.m);
        const uint32_t extra = g_gr.extra_dynamic & ~0x1fcu;  // beyond those the injection keeps dynamic (2..8)
        logf_("ground: %llu G-buffer passes, %llu drawn (%u of %u WoW triangles; %llu loads), %llu without BG3's camera, %llu with one bound elsewhere (%llu binds, %llu unmapped,"
              " %llu rejected), %llu without triangles, %llu without a framebuffer; GPU %.3f ms each (max %.3f, %llu timed); compositor took BG3's"
              " own depth %llu times, missed %llu; %llu frames presented without an injection; BG3 dynamic states %#x%s;"
              " camera at submit: %llu as recorded, %llu changed (view turned %.3f deg on average), %llu not read",
              (unsigned long long)g_gr.passes, (unsigned long long)g_gr.drawn, g_gr.tris_kept, g_gr.tris, (unsigned long long)g_gr.loads,
              (unsigned long long)g_gr.nocam, (unsigned long long)g_gr.cam_elsewhere, (unsigned long long)g_gr.binds, (unsigned long long)g_gr.nobind, (unsigned long long)g_gr.badview,
              (unsigned long long)g_gr.nogeo, (unsigned long long)g_gr.nofb,
              g_gr.gpu_n ? g_gr.gpu_ns / 1e6 / g_gr.gpu_n : 0.0, g_gr.gpu_max / 1e6, (unsigned long long)g_gr.gpu_n,
              (unsigned long long)g_gr.pre_used, (unsigned long long)g_gr.pre_missed, (unsigned long long)g_gr.frames_bare, g_gr.extra_dynamic,
              extra ? " (NOTE: some the injection's pipeline sets statically)" : "",
              (unsigned long long)g_gr.sub_same, (unsigned long long)g_gr.sub_changed, g_gr.sub_changed ? g_gr.sub_deg / g_gr.sub_changed : 0.0,
              (unsigned long long)g_gr.sub_unread);
        g_gr.passes = g_gr.drawn = g_gr.nocam = g_gr.nogeo = g_gr.nofb = g_gr.gpu_ns = g_gr.gpu_n = g_gr.gpu_max = g_gr.pre_used = g_gr.pre_missed = g_gr.frames_bare = g_gr.cam_elsewhere = 0;
        g_gr.loads = g_gr.binds = g_gr.nobind = g_gr.badview = 0;
        g_gr.sub_same = g_gr.sub_changed = g_gr.sub_unread = 0; g_gr.sub_deg = 0;
        pthread_mutex_unlock(&g_gr.m);
    }
}
