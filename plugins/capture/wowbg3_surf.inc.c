// ---------------------------------------------------------------- ground surfaces shown
// BG3's ground surfaces (water, grease, fire, ice, acid, poison, blood...) drawn on WoW's ground in
// the composite, with BG3's own look (the chosen route: BG3's G-buffer gets WoW's real
// ground colours, BG3's own surface systems change them, the composite shows that change).
//
// How BG3 draws a surface (harness traces): EoCSurfaceMeshGenerate (async compute) builds
// a screen grid from the linear depth and the decal normal; each surface type then draws twice with
// it: a G-buffer decal (vertex shader SF_VS_DECAL) in the deferred decal pass (scene depth, normal,
// albedo, surface and a fourth target, alpha-blended), and a forward layer (SF_VS_FWD) alpha-blended
// into BG3's pre-lighting HDR target (B10G11R11; the sky is cleared into it first), which BG3's
// lighting composite then adds to the deferred lighting. In world mode BG3's void has no light on
// the ground (WoW's ground in BG3's own frame is black), so BG3's own final pixels cannot be shown;
// and WoW's colour is already lit. So the composite takes BG3's change of the ground in linear light,
// lit like WoW's own pixel (unit light: WoW's lit colour is the albedo):
//   out = WoW + (A1 - A0) + H
//   A0 = the albedo this layer gives WoW's ground (WoW's own colour there, wowbg3_surf_alb.frag),
//   A1 = BG3's albedo after its surface decals (copied right after the decal pass),
//   H  = BG3's pre-lighting target after the forward surface layers (copied right after that pass;
//        it holds only them on WoW's ground: the void's ambient there is about 0.001).
// Per BG3 frame (slot, by the ground injection's serial): Z (the injection's 5th target: WoW's ground
// view depth, 0 where BG3's own geometry stays) and A0; A1 and H in a ring of copy slots of their own:
// BG3 records its decal and forward command buffers in parallel with (often before) the G-buffer's,
// so a copy goes to the slot of the present count it was recorded at and learns its frame when its
// command buffer is submitted (after that frame's G-buffer command buffer: surf_submit_cb). The compositor maps each WoW
// pixel to its world point (WoW's depth, the camera WoW's frame was drawn for) and looks it up in the
// presented BG3 frame where that point is WoW's ground too (wowbg3_compose.comp, surf_delta).
//
// Hooks: ground_inject picks the slot (surf_inject); the end of BG3's G-buffer command buffer runs the
// albedo pass (surf_end_cb); begin_pass / draws / end of pass find the surface draws and copy A1 and H
// (surf_pass_begin, surf_draw, surf_pass_end); void_skip keeps the surface draws; the compositor reads
// the presented frame's slot (surf_for_compose). Compose word "surf" turns all of it on (with
// "ground"); "surfdbg" paints matched surface pixels magenta and unmatched ground red, "surfpix" uses
// BG3's own pixel instead of the reprojection, "sdec=", "sfwd=" weigh the decal and forward parts
// (default 1), "sfloor=" the forward floor (0.004), "stol=" the depth match (0.15 m).
// Without surface draws for SF_IDLE presents the albedo pass and WoW's frame copy stop (sf_active):
// what remains then is the injection's fifth target.
// Included by wowbg3_capture.c after wowbg3_ground.inc.c.
#include "wowbg3_surf_spv.h"

// BG3 frames in flight between their recording and their present. Three was enough at 1280x720 with
// three swapchain images (at most one ahead seen), and too few at 3440x1440 with four: one present in
// four found its slot already taken by a later frame, the compositor then drew no surface at all for
// that frame, and BG3's ground surfaces flickered at a quarter of the frame rate (the beta's "surfaces
// flicker"; his log: `composited 225 (no surface 0, stale 75` per 300 frames, over and over). Six
// leaves room for twice the depth seen; `stale` in the surf line stays at 0 while it is enough, and
// `the deepest frame in flight` below says how close it is.
#define SF_SLOTS 6
#define SF_VS_DECAL 0x36d714a019831c05ull  // surface G-buffer decal (deferred decal pass)
#define SF_VS_FWD 0xe0eca4a6adff9a7aull    // surface forward layer (BG3's pre-lighting HDR target)
#define SF_PC 192                          // albedo pass push constants (wowbg3_surf_alb.frag)
#define SF_SETS 8                          // albedo pass descriptor sets (by injection serial: never updated while a frame in flight uses it)
#define SF_IDLE 60                         // presents without a surface draw after which the albedo pass and WoW's frame copy stop

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } SfImg;
typedef struct {
    SfImg z, a0;                 // R32F, RGBA8
    uint32_t w, hgt;
    uint64_t serial;             // the ground injection (g_gr.serial) this slot belongs to
    uint64_t alb_serial;         // albedo pass recorded for that serial
    GrRec rec;                   // frame N's camera (BG3's per-view block)
    float inv[16];               // inverse of rec.vp (column-major, clip -> eye-relative world)
    uint8_t pc[SF_PC];           // albedo pass push constants, made at the injection
    int have_pc;
    int wc;                      // WoW frame K for the albedo pass: the layer's copy g_sf.wc[wc]
    uint32_t wcam;               // its camera seq
    VkFramebuffer fb;            // albedo pass: BG3's albedo + A0
    VkImageView alb_view;
    uint64_t fb_img;
    uint64_t q_pending;          // timestamps written (bit per pair) and not read back yet
} SfSlot;
typedef struct {
    SfImg a1, h;                 // RGBA8 (BG3's albedo after its decals), R32UI (BG3's B10G11R11 target after the forward layers)
    uint32_t w, hgt;
    uint64_t key;                // the present count its copies were recorded at
    uint64_t dec_serial, fwd_serial;  // the frame (injection serial) whose decal / forward copy it holds, from the submission
} SfCopy;
#define SF_COPIES 4

static struct {
    pthread_mutex_t m;
    int ready, failed;
    DeviceData* d;
    SfSlot s[SF_SLOTS];
    SfCopy c[SF_COPIES];
    VkRenderPass rp;
    VkFormat alb_fmt;
    int32_t alb_lay;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkDescriptorPool pool;
    VkDescriptorSet sets[SF_SETS];  // albedo pass: Z, WoW frame
    SfImg dummy[4];              // 1x1 stand-ins for the compositor when there are no surfaces (Z, A0, A1, H)
    int dummy_init;
    uint64_t alb_img;            // BG3's G-buffer albedo (the G-buffer pass's second colour target)
    // WoW's composited frame copied by each compositor submission (surf_compose_copy), alternately:
    // the albedo pass of a BG3 frame recorded after it reads it (GPU order: that copy, BG3's frame, the
    // next copy into the other one), without holding the reader's slots.
    GBuf wc[2];
    struct { uint32_t ww, wh, cbpr, dbpr, code, cam; float wnear, wzoff; uint32_t glight; } wcm[2];
    int wc_latest;               // the copy recorded last, -1 none
    VkQueryPool qp;              // GPU time: per slot, pairs albedo pass / decal copy / forward copy
    double tick_ns;
    struct { VkCommandBuffer cb; int slot; uint64_t serial; } pend[16];  // G-buffer command buffers owing the albedo pass
    struct { uint64_t cb; int ci, fwd; } copies[32];  // command buffers with a decal / forward copy: their copy slot
    uint32_t copies_next;
    // statistics since the last log
    uint64_t injects, albs, nowow, nocam, decs, fwds, keeps, composed, nosurf, stale, other_frame;
    uint64_t nodec, nofwd;  // the frame had one kind of copy but not the other
    uint64_t deepest;   // the furthest a recorded frame ran ahead of the presented one (SF_SLOTS must beat it)
    uint64_t last_serial;  // the newest frame recorded (for `deepest`)
    uint64_t endwhy[8];  // 6 sf_init failed, 7 BG3's view-projection not invertible
     // owed albedo passes not recorded: 0 no owed pass found at the end, 1 slot moved on, 2 no push constants, 3 surf off or no albedo, 4 view or framebuffer, 5 no WoW copy
    uint64_t gpu_alb, gpu_dec, gpu_fwd, n_alb, n_dec, n_fwd;
    uint64_t px_sp, px_ok, px_bad, px_frames;
    int64_t kage_sum; uint64_t kage_n;
    float dot_f, dot_r, dot_u, eye_max;  // WoW frame's mapped camera vs BG3's own axes (1, 1, 1 when the mapping is right)
    uint64_t dots;
    uint64_t presents;
    uint64_t last_draw;          // present count + 1 when BG3 last recorded a surface draw (0 never)
    uint64_t idle;               // injections without the albedo pass: no surface draw in the last SF_IDLE presents
    // request "surflog<N>": one line per present to OUT_DIR/surflog.txt (research: ghosts, smear)
    FILE* lf;
    int log_left;
    uint32_t last_sp, last_ok, last_bad;
} g_sf = {.m = PTHREAD_MUTEX_INITIALIZER, .dot_f = 2, .dot_r = 2, .dot_u = 2, .wc_latest = -1};

// Surfaces drawn lately: the albedo pass and WoW's frame copy run only then (no cost without surfaces;
// a surface's first frames after a pause composite once the next BG3 frame has the albedo pass).
static int sf_active(void) {
    if (g_cc.fxbg) return 1;
    uint64_t l = __atomic_load_n(&g_sf.last_draw, __ATOMIC_RELAXED);
    return l && __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED) + 1 - l < SF_IDLE;
}

// The pass being recorded on this thread (swapchain size, with BG3's scene depth): its attachments
// and the surface draws seen in it.
static __thread VkCommandBuffer t_sf_cb;
static __thread uint32_t t_sf_n;
static __thread uint64_t t_sf_img[8];
static __thread int32_t t_sf_fin[8];
static __thread int t_sf_dec, t_sf_fwd;
static __thread uint64_t t_sf_pipe;  // last pipeline classified on this thread, and its kind
static __thread int t_sf_kind;
// BG3's albedo layout on the G-buffer's command buffer after its pass (barriers update it).
static __thread VkCommandBuffer t_sf_alb_cb;
static __thread int32_t t_sf_alb_lay;

static void sf_img_free(DeviceData* d, SfImg* i) {
    if (i->view && d->DestroyImageView) d->DestroyImageView(d->device, i->view, NULL);
    if (i->img) d->DestroyImage(d->device, i->img, NULL);
    if (i->mem) d->FreeMemory(d->device, i->mem, NULL);
    memset(i, 0, sizeof *i);
}
static int sf_img_make(DeviceData* d, SfImg* i, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (d->CreateImage(d->device, &ci, NULL, &i->img) != VK_SUCCESS) { i->img = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req;
    d->GetImageMemoryRequirements(d->device, i->img, &req);
    int32_t type = memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
    if (type < 0 || d->AllocateMemory(d->device, &mai, NULL, &i->mem) != VK_SUCCESS) { i->mem = VK_NULL_HANDLE; sf_img_free(d, i); return 0; }
    if (d->BindImageMemory(d->device, i->img, i->mem, 0) != VK_SUCCESS) { sf_img_free(d, i); return 0; }
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, i->img, VK_IMAGE_VIEW_TYPE_2D, f,
                                 {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (d->CreateImageView(d->device, &vci, NULL, &i->view) != VK_SUCCESS) { i->view = VK_NULL_HANDLE; sf_img_free(d, i); return 0; }
    return 1;
}
static void sf_barrier(DeviceData* d, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss,
                       VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                              img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    d->CmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

// 4x4 inverse of a column-major matrix (double inside). 1: done.
static int sf_inverse(const float m[16], float out[16]) {
    double a[16], inv[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!(fabs(det) > 1e-30)) return 0;
    for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] / det);
    return 1;
}

// A WoW frame's camera (the plugin's BGC1 record it was drawn from: eye, forward, up in WoW yards,
// vertical fov, aspect) in BG3's world with world mode's mapping, its eye relative to `eyeN`. WoW's
// right is forward x up in WoW's (right-handed) axes; the mapping (x, y, z) -> (y, z, -x) flips
// handedness, so the axes are mapped, never crossed in BG3's. 1: done.
static int sf_cam(const uint8_t rec[96], const float eyeN[3], float eye[3], float fwd[3], float right[3], float up[3], float* tanx, float* tany) {
    if (!g_tav.seq) return 0;
    float e[3], f[3], u[3], fov, asp;
    memcpy(e, rec + 8, 12); memcpy(f, rec + 20, 12); memcpy(u, rec + 32, 12); memcpy(&fov, rec + 44, 4); memcpy(&asp, rec + 48, 4);
    if (!(fov > 0.05f && fov < 3.0f) || !(asp > 0.2f && asp < 8.0f)) return 0;
    double fl = sqrt((double)f[0] * f[0] + (double)f[1] * f[1] + (double)f[2] * f[2]);
    if (!(fl > 1e-6)) return 0;
    double F[3] = {f[0] / fl, f[1] / fl, f[2] / fl};
    double R[3] = {F[1] * u[2] - F[2] * u[1], F[2] * u[0] - F[0] * u[2], F[0] * u[1] - F[1] * u[0]};
    double rl = sqrt(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
    if (!(rl > 1e-6)) return 0;
    for (int k = 0; k < 3; k++) R[k] /= rl;
    double U[3] = {R[1] * F[2] - R[2] * F[1], R[2] * F[0] - R[0] * F[2], R[0] * F[1] - R[1] * F[0]};
    const double yd = 0.9144;
    double eb[3] = {g_tav.centre[0] + (e[1] - g_tav.wow[1]) * yd, g_tav.centre[1] + (e[2] - g_tav.wow[2]) * yd,
                    g_tav.centre[2] - (e[0] - g_tav.wow[0]) * yd};
    for (int k = 0; k < 3; k++) eye[k] = (float)(eb[k] - eyeN[k]);
    fwd[0] = (float)F[1]; fwd[1] = (float)F[2]; fwd[2] = (float)-F[0];
    right[0] = (float)R[1]; right[1] = (float)R[2]; right[2] = (float)-R[0];
    up[0] = (float)U[1]; up[1] = (float)U[2]; up[2] = (float)-U[0];
    *tany = tanf(fov * 0.5f);
    *tanx = *tany * asp;
    return 1;
}
// The camera record with this seq (g_pc ring, filled by the reader thread and the present).
#define WDG_SEQ 0x40000000u
#define WDG_N 32
static struct { pthread_mutex_t m; uint32_t seq[WDG_N]; uint8_t rec[WDG_N][96]; uint32_t next, n; } g_wdg = {PTHREAD_MUTEX_INITIALIZER, {0}, {{0}}, 0, 0};
static int sf_cam_rec(uint32_t seq, uint8_t out[96]) {
    int ok = 0;
    if (seq & WDG_SEQ) {
        pthread_mutex_lock(&g_wdg.m);
        for (uint32_t i = 0; i < WDG_N && !ok; i++) if (g_wdg.seq[i] == seq) { memcpy(out, g_wdg.rec[i], 96); ok = 1; }
        pthread_mutex_unlock(&g_wdg.m);
        return ok;
    }
    pthread_mutex_lock(&g_pc.m);
    for (uint32_t i = 0; i < g_pc.n && !ok; i++) {
        const PaceCam* c = &g_pc.r[(g_pc.head + PC_N - 1 - i) % PC_N];
        if (c->seq == seq) { memcpy(out, c->rec, 96); ok = 1; }
    }
    pthread_mutex_unlock(&g_pc.m);
    return ok;
}

// Pipelines: the albedo pass (g_sf.m held).
static int sf_init(DeviceData* d, VkFormat alb_fmt, int32_t alb_lay) {
    if (g_sf.ready) return g_sf.d == d && g_sf.alb_fmt == alb_fmt && g_sf.alb_lay == alb_lay;
    if (g_sf.failed) return 0;
    g_sf.failed = 1;  // until everything below exists
    g_sf.d = d;
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)d->inst->gipa(d->inst->instance, "vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties pp;
    if (!gpp) return 0;
    gpp(d->physical, &pp);
    if (pp.limits.maxPushConstantsSize < SF_PC) { logf_("surf: push constants limit %u < %d: off", pp.limits.maxPushConstantsSize, SF_PC); return 0; }
    g_sf.alb_fmt = alb_fmt;
    g_sf.alb_lay = alb_lay;
    VkAttachmentDescription at[2] = {
        {0, alb_fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, (VkImageLayout)alb_lay, (VkImageLayout)alb_lay},
        {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL}};
    VkAttachmentReference cref[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {1, VK_IMAGE_LAYOUT_GENERAL}};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 2, cref, NULL, NULL, 0, NULL};
    const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, all, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, all, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0}};
    VkRenderPassCreateInfo rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 2, at, 1, &sp, 2, dep};
    if (d->CreateRenderPass(d->device, &rci, NULL, &g_sf.rp) != VK_SUCCESS) { logf_("surf: render pass not made"); return 0; }
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_sf.dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, SF_PC};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_sf.dsl, 1, &pr};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_sf.layout) != VK_SUCCESS) return 0;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_surf_alb_vert, wowbg3_surf_alb_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_surf_alb_frag, wowbg3_surf_alb_frag};
    if (d->CreateShaderModule(d->device, &mv, NULL, &vs) != VK_SUCCESS || d->CreateShaderModule(d->device, &mf, NULL, &fs) != VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE,
                                                 VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT,
                                               VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE};
    VkPipelineColorBlendAttachmentState cb[2] = {
        {VK_FALSE, 0, 0, 0, 0, 0, 0, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT},
        {VK_FALSE, 0, 0, 0, 0, 0, 0, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT}};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 2, cb, {0, 0, 0, 0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms, NULL,
                                        &cbs, &dy, g_sf.layout, g_sf.rp, 0, VK_NULL_HANDLE, -1};
    VkResult r = d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_sf.pipe);
    if (d->DestroyShaderModule) { d->DestroyShaderModule(d->device, vs, NULL); d->DestroyShaderModule(d->device, fs, NULL); }
    if (r != VK_SUCCESS) { logf_("surf: pipeline not made (%d)", (int)r); return 0; }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SF_SETS}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SF_SETS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, SF_SETS, 2, ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_sf.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l[SF_SETS];
    for (int k = 0; k < SF_SETS; k++) l[k] = g_sf.dsl;
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_sf.pool, SF_SETS, l};
    if (d->AllocateDescriptorSets(d->device, &sai, g_sf.sets) != VK_SUCCESS) return 0;
    if (d->CreateQueryPool && d->CmdResetQueryPool && d->CmdWriteTimestamp && d->GetQueryPoolResults && pp.limits.timestampComputeAndGraphics &&
        pp.limits.timestampPeriod > 0) {
        VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 6 * SF_SLOTS, 0};
        if (d->CreateQueryPool(d->device, &qci, NULL, &g_sf.qp) == VK_SUCCESS) g_sf.tick_ns = pp.limits.timestampPeriod;
        else g_sf.qp = VK_NULL_HANDLE;
    }
    g_sf.ready = 1;
    g_sf.failed = 0;
    logf_("surf: ready (albedo format %d layout %d; timestamps %s)", (int)alb_fmt, (int)alb_lay, g_sf.qp ? "on" : "off");
    return 1;
}

static void fb_retire(SfImg* i);  // wowbg3_fxbg.inc.c: freed once the GPU is past it
static void fb_retire_fb(VkFramebuffer fb, VkImageView view);
// The compositor's 1x1 stand-ins for frames without surfaces (g_sf.m held). 1: there.
static int sf_dummies(DeviceData* d) {
    if (g_sf.dummy[3].view) return 1;
    for (int k = 0; k < 4; k++) sf_img_free(d, &g_sf.dummy[k]);
    const VkFormat f[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT};
    for (int k = 0; k < 4; k++)
        if (!sf_img_make(d, &g_sf.dummy[k], f[k], 1, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            for (int j = 0; j < 4; j++) sf_img_free(d, &g_sf.dummy[j]);
            return 0;
        }
    g_sf.dummy_init = 0;
    return 1;
}

// The slot's images at this size (g_sf.m held; a resize leaks the old ones to in-flight work).
static int sf_slot_images(DeviceData* d, SfSlot* s, uint32_t w, uint32_t h) {
    if (s->z.img && s->w == w && s->hgt == h) return 1;
    if (s->z.img) {  // a resize (BG3's scene changed size: upscaling on or off, a resolution): retired, freed once the GPU is past them
        fb_retire(&s->z); fb_retire(&s->a0);
        fb_retire_fb(s->fb, s->alb_view);
        s->fb = VK_NULL_HANDLE; s->alb_view = VK_NULL_HANDLE; s->fb_img = 0;
    }
    const VkImageUsageFlags st = VK_IMAGE_USAGE_STORAGE_BIT;
    if (!sf_img_make(d, &s->z, VK_FORMAT_R32_SFLOAT, w, h, st | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        !sf_img_make(d, &s->a0, VK_FORMAT_R8G8B8A8_UNORM, w, h, st | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        sf_img_free(d, &s->z); sf_img_free(d, &s->a0);
        logf_("surf: images %ux%u not made", w, h);
        return 0;
    }
    s->w = w; s->hgt = h;
    s->alb_serial = 0;
    return 1;
}
// A copy slot's images at this size (g_sf.m held).
static int sf_copy_images(DeviceData* d, SfCopy* c, uint32_t w, uint32_t h) {
    if (c->a1.img && c->w == w && c->hgt == h) return 1;
    // A new size (BG3's scene changed size: a resolution, upscaling on or off): the old images may still be in command
    // buffers BG3 has not finished, so they are retired, not freed here.
    fb_retire(&c->a1); fb_retire(&c->h);
    const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!sf_img_make(d, &c->a1, VK_FORMAT_R8G8B8A8_UNORM, w, h, u) || !sf_img_make(d, &c->h, VK_FORMAT_R32_UINT, w, h, u)) {
        sf_img_free(d, &c->a1); sf_img_free(d, &c->h);
        logf_("surf: copy images %ux%u not made", w, h);
        return 0;
    }
    c->w = w; c->hgt = h; c->key = ~0ull; c->dec_serial = c->fwd_serial = 0;
    return 1;
}

// ground_inject (BG3's recording thread, after its G-buffer pass): this frame's slot, its Z view for
// the injection's fifth target, and what the albedo pass will need (WoW frame K, cameras). The
// albedo pass itself is recorded at the end of `cb` (surf_end_cb).
static VkImageView surf_inject(DeviceData* d, VkCommandBuffer cb, uint64_t serial, const GrRec* rec, uint32_t w, uint32_t h,
                               uint64_t alb_img, VkFormat alb_fmt, int32_t alb_lay) {
    pthread_mutex_lock(&g_sf.m);
    VkImageView zv = VK_NULL_HANDLE;
    if (g_sf.d && g_sf.d != d) { pthread_mutex_unlock(&g_sf.m); return VK_NULL_HANDLE; }
    g_sf.d = d;
    SfSlot* s = &g_sf.s[serial % SF_SLOTS];
    if (!sf_slot_images(d, s, w, h)) { pthread_mutex_unlock(&g_sf.m); return VK_NULL_HANDLE; }
    s->serial = serial;
    if (serial > g_sf.last_serial) g_sf.last_serial = serial;
    s->rec = *rec;
    s->have_pc = 0;
    zv = s->z.view;
    g_sf.injects++;
    int want = g_cc.surf && sf_init(d, alb_fmt, alb_lay);
    if (g_cc.surf && !want) g_sf.endwhy[6]++;
    if (want && !sf_active()) { want = 0; g_sf.idle++; }
    if (g_cc.wdepth) want = 0;  // gate a: the placeholder stays neutral
    if (want) { want = sf_inverse(rec->vp, s->inv); if (!want) g_sf.endwhy[7]++; }
    if (want) g_sf.alb_img = alb_img;
    pthread_mutex_unlock(&g_sf.m);
    t_sf_alb_cb = cb;  // BG3's albedo as its G-buffer pass left it (barriers after it update this)
    t_sf_alb_lay = alb_lay;
    if (!want) return zv;
    // WoW frame K: the layer's copy of the frame composited last (surf_compose_copy).
    pthread_mutex_lock(&g_sf.m);
    int wc = g_sf.wc_latest;
    __typeof__(g_sf.wcm[0]) w0;
    if (wc >= 0) w0 = g_sf.wcm[wc];
    pthread_mutex_unlock(&g_sf.m);
    if (wc < 0 || !w0.cam) { __atomic_add_fetch(&g_sf.nowow, 1, __ATOMIC_RELAXED); return zv; }
    uint8_t crec[96];
    float eye[3], fwd[3], right[3], up[3], tx, ty;
    if (!sf_cam_rec(w0.cam, crec) || !sf_cam(crec, rec->eye, eye, fwd, right, up, &tx, &ty)) {
        __atomic_add_fetch(&g_sf.nocam, 1, __ATOMIC_RELAXED);
        return zv;
    }
    // Push constants (wowbg3_surf_alb.frag): inv, fwdN, eyeK (w tanx), fwdK (w tany), rightK, upK,
    // ww, wh, cbpr, dbpr, code, flags (1: decode code 2; 2: WoW's ground light given, in the
    // last word as bits), wnear, wzoff, 1/W, 1/H, gamma, ground light.
    uint8_t pc[SF_PC];
    float* f = (float*)pc;
    pthread_mutex_lock(&g_sf.m);
    memcpy(f, s->inv, 64);
    f[16] = rec->fwd[0]; f[17] = rec->fwd[1]; f[18] = rec->fwd[2]; f[19] = 0;
    f[20] = eye[0]; f[21] = eye[1]; f[22] = eye[2]; f[23] = tx;
    f[24] = fwd[0]; f[25] = fwd[1]; f[26] = fwd[2]; f[27] = ty;
    f[28] = right[0]; f[29] = right[1]; f[30] = right[2]; f[31] = 0;
    f[32] = up[0]; f[33] = up[1]; f[34] = up[2]; f[35] = 0;
    uint32_t* u = (uint32_t*)(f + 36);
    u[0] = w0.ww; u[1] = w0.wh; u[2] = w0.cbpr; u[3] = w0.dbpr; u[4] = w0.code; u[5] = (w0.code == 2 && !g_cc.wowraw) ? 1u : 0u;
    f[42] = w0.wnear; f[43] = (w0.wzoff > -100.0f && w0.wzoff < 100.0f) ? w0.wzoff : 0.0f;
    f[44] = 1.0f / (float)w; f[45] = 1.0f / (float)h;
    f[46] = g_cc.wgamma; f[47] = 0;
    if (g_cc.slight && w0.glight) { u[5] |= 2u; memcpy(&f[47], &w0.glight, 4); }
    if (s->serial == serial) {
        memcpy(s->pc, pc, SF_PC);
        s->have_pc = 1;
        s->wc = wc; s->wcam = w0.cam;
        // the end of this command buffer owes the albedo pass
        int k = 0;
        while (k < 16 && g_sf.pend[k].cb && g_sf.pend[k].cb != cb) k++;
        if (k == 16) { k = (int)(serial % 16); if (g_sf.pend[k].cb) g_sf.endwhy[0]++; }
        g_sf.pend[k].cb = cb; g_sf.pend[k].slot = (int)(serial % SF_SLOTS); g_sf.pend[k].serial = serial;
    }
    pthread_mutex_unlock(&g_sf.m);
    return zv;
}

// BG3 ends a command buffer (before the driver's End): the albedo pass when it is the G-buffer's.
static uint64_t g_sf_ends;  // command buffer ends seen (surf_end_cb), for the log line
static void surf_end_cb(DeviceData* d, VkCommandBuffer cb) {
    __atomic_add_fetch(&g_sf_ends, 1, __ATOMIC_RELAXED);
    if (!g_sf.ready) return;
    pthread_mutex_lock(&g_sf.m);
    int k = 0;
    while (k < 16 && g_sf.pend[k].cb != cb) k++;
    if (k == 16) { pthread_mutex_unlock(&g_sf.m); return; }  // (most command buffers owe nothing)
    int si = g_sf.pend[k].slot;
    uint64_t serial = g_sf.pend[k].serial;
    g_sf.pend[k].cb = VK_NULL_HANDLE;
    SfSlot* s = &g_sf.s[si];
    if (s->serial != serial || !s->have_pc || !g_cc.surf || !g_sf.alb_img) {
        g_sf.endwhy[s->serial != serial ? 1 : !s->have_pc ? 2 : 3]++;
        pthread_mutex_unlock(&g_sf.m);
        return;
    }
    // BG3's albedo image view and the framebuffer over it and A0
    if (!s->fb || s->fb_img != g_sf.alb_img) {
        // A new albedo image (BG3 keeps one G-buffer; a resize makes another): the old framebuffer may
        // still be in flight, so it is left (BG3 destroying its image frees the current one).
        if (s->fb) logf_("surf: BG3's albedo image changed (slot %d)", si);
        s->fb = VK_NULL_HANDLE; s->alb_view = VK_NULL_HANDLE;
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)g_sf.alb_img, VK_IMAGE_VIEW_TYPE_2D, g_sf.alb_fmt,
                                     {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (d->CreateImageView(d->device, &vci, NULL, &s->alb_view) != VK_SUCCESS) { s->alb_view = VK_NULL_HANDLE; g_sf.endwhy[4]++; pthread_mutex_unlock(&g_sf.m); return; }
        VkImageView v[2] = {s->alb_view, s->a0.view};
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_sf.rp, 2, v, s->w, s->hgt, 1};
        if (d->CreateFramebuffer(d->device, &fci, NULL, &s->fb) != VK_SUCCESS) { s->fb = VK_NULL_HANDLE; g_sf.endwhy[4]++; pthread_mutex_unlock(&g_sf.m); return; }
        s->fb_img = g_sf.alb_img;
    }
    VkDescriptorSet set = g_sf.sets[serial % SF_SETS];
    VkDescriptorImageInfo zi = {VK_NULL_HANDLE, s->z.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo wb = {g_sf.wc[s->wc].buf, 0, VK_WHOLE_SIZE};
    if (!wb.buffer) { g_sf.endwhy[5]++; pthread_mutex_unlock(&g_sf.m); return; }
    VkWriteDescriptorSet wr[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &zi, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &wb, NULL}};
    d->UpdateDescriptorSets(d->device, 2, wr, 0, NULL);
    VkRenderPass rp = g_sf.rp;
    VkFramebuffer fb = s->fb;
    VkPipeline pipe = g_sf.pipe;
    VkPipelineLayout lay = g_sf.layout;
    uint8_t pc[SF_PC];
    memcpy(pc, s->pc, SF_PC);
    uint32_t w = s->w, h = s->hgt;
    VkQueryPool qp = g_sf.qp;
    int timed = qp && !(s->q_pending & 1);
    if (timed) s->q_pending |= 1;
    s->alb_serial = serial;
    VkImage alb = (VkImage)g_sf.alb_img;
    int32_t want_lay = g_sf.alb_lay;
    pthread_mutex_unlock(&g_sf.m);
    // BG3's albedo in the layout the albedo pass was made for (as BG3's G-buffer pass leaves it)
    int32_t cur = t_sf_alb_cb == cb ? t_sf_alb_lay : want_lay;
    if (cur != want_lay && cur >= 0)
        sf_barrier(d, cb, alb, (VkImageLayout)cur, (VkImageLayout)want_lay, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (timed) {
        d->CmdResetQueryPool(cb, qp, 6 * si, 2);
        d->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 6 * si);
    }
    // Z was written as a colour attachment by the injection earlier in this command buffer.
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          0, 1, &mb, 0, NULL, 0, NULL);
    VkClearValue cv[2];
    memset(cv, 0, sizeof cv);
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {w, h}}, 2, cv};
    d->CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = {0, 0, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {w, h}};
    d->CmdSetViewport(cb, 0, 1, &vp);
    d->CmdSetScissor(cb, 0, 1, &sc);
    d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &set, 0, NULL);
    d->CmdPushConstants(cb, lay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, SF_PC, pc);
    d->CmdDraw(cb, 3, 1, 0, 0);
    d->CmdEndRenderPass(cb);
    if (cur != want_lay && cur >= 0)
        sf_barrier(d, cb, alb, (VkImageLayout)want_lay, (VkImageLayout)cur, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (timed) d->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 6 * si + 1);
    __atomic_add_fetch(&g_sf.albs, 1, __ATOMIC_RELAXED);
}
static void surf_kept(void) { g_sf.keeps++; }
// A barrier BG3 records (g_trace_lock not needed): BG3's albedo's layout on the G-buffer's command buffer.
static void surf_note_barrier(VkCommandBuffer cb, uint64_t img, int32_t layout) {
    if (t_sf_alb_cb == cb && img && img == g_sf.alb_img) t_sf_alb_lay = layout;
}
// BG3 destroys an image (its work on it is done): our views and framebuffers over its albedo go.
static void surf_forget_image(DeviceData* d, VkImage img) {
    if (!img) return;
    pthread_mutex_lock(&g_sf.m);
    for (int k = 0; k < SF_SLOTS; k++) {
        SfSlot* s = &g_sf.s[k];
        if (s->fb_img != (uint64_t)img) continue;
        if (s->fb && d->DestroyFramebuffer) d->DestroyFramebuffer(d->device, s->fb, NULL);
        if (s->alb_view && d->DestroyImageView) d->DestroyImageView(d->device, s->alb_view, NULL);
        s->fb = VK_NULL_HANDLE; s->alb_view = VK_NULL_HANDLE; s->fb_img = 0;
    }
    if (g_sf.alb_img == (uint64_t)img) g_sf.alb_img = 0;
    pthread_mutex_unlock(&g_sf.m);
}
// ground_submit (g_gr.m and g_trace_lock held): a command buffer with a surface copy is submitted
// after its frame's G-buffer command buffer, so its copy slot holds that frame's decals / forward layers.
static void surf_submit_cb(uint64_t cb, uint64_t pending_serial) {
    if (!g_sf.ready) return;
    pthread_mutex_lock(&g_sf.m);
    for (uint32_t k = 0; k < 32; k++)
        if (g_sf.copies[k].cb == cb) {
            SfCopy* c = &g_sf.c[g_sf.copies[k].ci];
            uint64_t* at = g_sf.copies[k].fwd ? &c->fwd_serial : &c->dec_serial;
            if (*at == pending_serial) g_sf.other_frame++;  // two copies of one kind for one frame (the later one counts)
            *at = pending_serial;
            g_sf.copies[k].cb = 0;
        }
    pthread_mutex_unlock(&g_sf.m);
}

static void sf_scene_copy(VkCommandBuffer cb, uint64_t img, int32_t lay);
// begin_pass, at swapchain size (g_trace_lock held): remember the pass's attachments. The hook runs
// before BG3 records its own vkCmdBeginRenderPass, so a copy made here lands outside the pass.
static void fb_feed(DeviceData* d, VkCommandBuffer cb, uint64_t img, VkImageLayout lay);
static __thread VkCommandBuffer t_fbg_cb;
static __thread uint64_t t_fbg_img;
static __thread int32_t t_fbg_lay;
static void fb_fog(DeviceData* d, VkCommandBuffer cb);
static void fb_clr(DeviceData* d, VkCommandBuffer cb);
static __thread VkCommandBuffer t_pass_cb, t_fog_cb, t_cld_cb;  // wdfog: the full-size pass being recorded; the composite pass seen
static __thread uint64_t t_pass_imgs[8]; static __thread int32_t t_pass_fins[8];
static void fb_bare(DeviceData* d, VkCommandBuffer cb);
static void fb_post(DeviceData* d, VkCommandBuffer cb);
static void fb_post_fin(DeviceData* d, VkCommandBuffer cb);  // wdpost's classes at BG3's exposure pass
static __thread VkCommandBuffer t_pfin_cb; static __thread uint64_t t_pfin_img; static __thread int32_t t_pfin_lay;
static __thread VkCommandBuffer t_post_cb;
static void fb_bmask(DeviceData* d, VkCommandBuffer cb);
static void fb_bcut(DeviceData* d, VkCommandBuffer cb);
static void late_pass_end(DeviceData* d, VkCommandBuffer cb);
static void fb_ovl(DeviceData* d, VkCommandBuffer cb);
static __thread VkCommandBuffer t_ovl_cb;
static __thread VkCommandBuffer t_bare_cb;
static __thread uint32_t t_pass_n; static __thread uint64_t t_pass_img0, t_fog_img, t_cld_img; static __thread int32_t t_pass_fin0, t_fog_lay, t_cld_lay;
static void surf_pass_begin(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const int32_t* ini, const uint8_t* load, const int32_t* fin) {
    t_pass_cb = cb; t_pass_n = n; t_pass_img0 = n ? imgs[0] : 0; t_pass_fin0 = n ? fin[0] : -1;
    for (uint32_t k = 0; k < 8; k++) { t_pass_imgs[k] = k < n ? imgs[k] : 0; t_pass_fins[k] = k < n ? fin[k] : -1; }
    t_sf_cb = VK_NULL_HANDLE;
    if (!g_cc.surf || !g_sf.ready || g_screen) return;  // a BG3 screen (screen_check): BG3's own picture
    int depth = 0;
    for (uint32_t k = 0; k < n; k++) depth |= imgs[k] == g_role.depth;
    // Word "surfscene": take the surfaces' look from BG3's **lit** scene instead of its
    // pre-lighting forward layer. Measured: the pre-lighting layer (H) is empty on WoW's ground in
    // the void -- 40x it with no floor and nothing appears -- so all that reached WoW's picture was
    // the decals' flat albedo change, while BG3's own picture shows the surface bright and lumpy.
    // BG3's effects pass loads the lit scene, so copying that attachment here (before the pass, so
    // before BG3's own effects are drawn into it) gives the surfaces lit as BG3 lights them, and
    // nothing else: the void's ground is black, characters are excluded by the ground depth, and
    // BG3's effects come later and are replayed by benilla instead. It goes into the same slot the
    // forward layer used, so the compositor, its flag and the "sfwd=" / "sfloor=" weights are
    // unchanged.
    t_fbg_cb = VK_NULL_HANDLE;
    if (depth && g_cc.fxbg) {
        // BG3's forward target: cleared here (its sky), then its surfaces and effects blend over it, and the
        // lighting composite (54fb164b) puts that over the lit scene. So WoW goes in right after the clear.
        int colors = 0, at = -1;
        for (uint32_t k = 0; k < n; k++) {
            if (imgs[k] == g_role.depth || !imgs[k]) continue;
            colors++;
            ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, imgs[k], 0);
            if (i && i->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && i->w == SCENE_W && i->hgt == SCENE_H && load[k] == VK_ATTACHMENT_LOAD_OP_CLEAR) at = (int)k;
        }
        if (colors == 1 && at >= 0) { t_fbg_cb = cb; t_fbg_img = imgs[at]; t_fbg_lay = fin[at]; }
    }
    if (depth && g_cc.surfscene && !g_cc.surfpost) {
        int colors = 0, at = -1;
        for (uint32_t k = 0; k < n; k++) {
            if (imgs[k] == g_role.depth || !imgs[k]) continue;
            colors++;
            ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, imgs[k], 0);
            if (i && i->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && i->w == SCENE_W && i->hgt == SCENE_H
                && load[k] == VK_ATTACHMENT_LOAD_OP_LOAD && ini[k] > 0)
                at = (int)k;
        }
        if (colors == 1 && at >= 0) sf_scene_copy(cb, imgs[at], ini[at]);
    }
    if (!depth) return;
    t_sf_cb = cb;
    t_sf_n = n > 8 ? 8 : n;
    for (uint32_t k = 0; k < t_sf_n; k++) { t_sf_img[k] = imgs[k]; t_sf_fin[k] = fin[k]; }
    t_sf_dec = t_sf_fwd = 0;
}
// An indexed draw: 1 (decal) or 2 (forward) when it is a surface draw of BG3's (in such a pass).
static int surf_draw(VkCommandBuffer cb) {
    if (t_sf_cb != cb || t_bind_cb != cb || !t_pipe) return 0;
    if (t_sf_pipe != t_pipe) {
        t_sf_pipe = t_pipe;
        t_sf_kind = 0;
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        Recipe* r = ri ? ri->r : NULL;
        for (uint32_t s = 0; r && s < r->nstages; s++)
            if (r->st[s].stage == VK_SHADER_STAGE_VERTEX_BIT)
                t_sf_kind = r->st[s].key == SF_VS_DECAL ? 1 : r->st[s].key == SF_VS_FWD ? 2 : 0;
    }
    if (t_sf_kind == 1) t_sf_dec = 1;
    if (t_sf_kind == 2) t_sf_fwd = 1;
    if (t_sf_kind) __atomic_store_n(&g_sf.last_draw, __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    return t_sf_kind;
}
// One copy of BG3's image (left in `lay` by its pass) into the slot's image, outside any pass.
static void sf_copy(DeviceData* d, VkCommandBuffer cb, VkImage src, VkImageLayout lay, SfImg* dst, uint32_t w, uint32_t h) {
    sf_barrier(d, cb, src, lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    sf_barrier(d, cb, dst->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    d->CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    sf_barrier(d, cb, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    sf_barrier(d, cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lay, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}
// BG3 ended a pass (its EndRenderPass already recorded): after the surface decals, A1; after the
// surface forward layers, H. Copies record no binding, so BG3's state is untouched.
static void surf_pass_end(DeviceData* d, VkCommandBuffer cb) {
    if (t_fbg_cb == cb) { t_fbg_cb = VK_NULL_HANDLE; fb_feed(d, cb, t_fbg_img, (VkImageLayout)t_fbg_lay); }
    if (t_fog_cb == cb) { t_fog_cb = VK_NULL_HANDLE; fb_fog(d, cb); }
    if (t_cld_cb == cb) { t_cld_cb = VK_NULL_HANDLE; fb_clr(d, cb); }
    if (t_bare_cb == cb) { t_bare_cb = VK_NULL_HANDLE; fb_bare(d, cb); }
    if (t_pfin_cb == cb) { t_pfin_cb = VK_NULL_HANDLE; fb_post_fin(d, cb); }
    if (t_post_cb == cb) { t_post_cb = VK_NULL_HANDLE; fb_post(d, cb); }
    if (t_blm_cb == cb) { t_blm_cb = VK_NULL_HANDLE; fb_bmask(d, cb); }
    if (t_blc_cb == cb) { t_blc_cb = VK_NULL_HANDLE; fb_bcut(d, cb); }
    late_pass_end(d, cb);
    if (t_ovl_cb == cb) { t_ovl_cb = VK_NULL_HANDLE; fb_ovl(d, cb); }
    if (t_pass_cb == cb) t_pass_cb = VK_NULL_HANDLE;  // a draw outside a full-size pass is in none of them
    if (t_sf_cb != cb) return;
    t_sf_cb = VK_NULL_HANDLE;
    if (g_cc.surfscene && !g_cc.surfpost) t_sf_fwd = 0;  // the lit-scene copy at the pass's start holds that slot
    // Word "surfpost" (research): take the copy at the END of BG3's effects pass instead
    // of before it, so it carries BG3's effects as well as its surfaces. Tells whether a thing that
    // is missing from WoW's picture (Grease's 3D burst blob) is drawn in that pass at all.
    if (g_cc.surfpost && !t_sf_fwd) {
        pthread_mutex_lock(&g_trace_lock);
        int colors = 0, at = -1;
        for (uint32_t k = 0; k < t_sf_n; k++) {
            if (t_sf_img[k] == g_role.depth || !t_sf_img[k]) continue;
            colors++;
            ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, t_sf_img[k], 0);
            if (i && i->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && i->w == SCENE_W && i->hgt == SCENE_H) at = (int)k;
        }
        pthread_mutex_unlock(&g_trace_lock);
        if (colors == 1 && at >= 0) t_sf_fwd = 1;
    }
    if (!t_sf_dec && !t_sf_fwd) return;
    if (!g_cc.surf || !g_sf.ready || !d->CmdCopyImage) return;
    // the attachment to copy: BG3's albedo (decals), the swapchain-size B10G11R11 target (forward)
    VkImage src = VK_NULL_HANDLE;
    int32_t lay = -1;
    VkFormat want = t_sf_fwd ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : g_sf.alb_fmt;
    pthread_mutex_lock(&g_trace_lock);
    for (uint32_t k = 0; k < t_sf_n && !src; k++) {
        if (t_sf_fwd) {
            ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, t_sf_img[k], 0);
            if (i && i->fmt == want && i->w == SCENE_W && i->hgt == SCENE_H) { src = (VkImage)t_sf_img[k]; lay = t_sf_fin[k]; }
        } else if (t_sf_img[k] == g_sf.alb_img) {
            src = (VkImage)t_sf_img[k]; lay = t_sf_fin[k];
        }
    }
    pthread_mutex_unlock(&g_trace_lock);
    if (!src || lay <= 0) return;
    // The copy is the source image's own size: the scene's size remembered from the last G-buffer pass can be another
    // while BG3 rebuilds its targets at a new size, and a copy past the image's edge lost the GPU (a crash in BG3 when
    // FSR 2.2 was switched off, 2026-11-12).
    uint32_t w = 0, h = 0;
    pthread_mutex_lock(&g_trace_lock);
    { ImgInfo* si = TABLE_FIND(g_img, IMG_CAP, (uint64_t)src, 0); if (si) { w = si->w; h = si->hgt; } }
    pthread_mutex_unlock(&g_trace_lock);
    pthread_mutex_lock(&g_sf.m);
    // the copy slot of the present count this frame is recorded at (its frame: surf_submit_cb)
    const uint64_t key = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    const int ci = (int)(key % SF_COPIES);
    SfCopy* c = &g_sf.c[ci];
    if (!g_sf.d || !w || !sf_copy_images(g_sf.d, c, w, h)) { pthread_mutex_unlock(&g_sf.m); return; }
    if (c->key != key) { c->key = key; c->dec_serial = c->fwd_serial = 0; }
    SfImg* dst = t_sf_fwd ? &c->h : &c->a1;
    int pair = t_sf_fwd ? 2 : 1;
    SfSlot* s = &g_sf.s[ci % SF_SLOTS];  // timestamps only
    VkQueryPool qp = g_sf.qp;
    int si = ci % SF_SLOTS;
    int timed = qp && !(s->q_pending & (1u << pair));
    if (timed) s->q_pending |= 1u << pair;
    g_sf.copies[g_sf.copies_next % 32].cb = (uint64_t)(uintptr_t)cb;
    g_sf.copies[g_sf.copies_next % 32].ci = ci;
    g_sf.copies[g_sf.copies_next % 32].fwd = t_sf_fwd;
    g_sf.copies_next++;
    pthread_mutex_unlock(&g_sf.m);
    if (timed) {
        d->CmdResetQueryPool(cb, qp, 6 * si + 2 * pair, 2);
        d->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 6 * si + 2 * pair);
    }
    sf_copy(d, cb, src, (VkImageLayout)lay, dst, w, h);
    if (timed) d->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 6 * si + 2 * pair + 1);
    __atomic_add_fetch(t_sf_fwd ? &g_sf.fwds : &g_sf.decs, 1, __ATOMIC_RELAXED);
}

// Word "surfscene": one copy of BG3's lit scene (the effects pass's colour attachment, before the
// pass) into the copy slot the forward layer uses. Mirrors surf_pass_end's forward half.
static void sf_scene_copy(VkCommandBuffer cb, uint64_t img, int32_t lay) {
    DeviceData* d = device_for(cb);
    if (!d || !d->CmdCopyImage || !g_sf.ready) return;
    pthread_mutex_lock(&g_sf.m);
    const uint64_t key = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    const int ci = (int)(key % SF_COPIES);
    SfCopy* c = &g_sf.c[ci];
    uint32_t w = 0, h = 0;  // the source image's own size (surf_pass_end)
    { ImgInfo* si = g_img ? TABLE_FIND(g_img, IMG_CAP, img, 0) : NULL; if (si) { w = si->w; h = si->hgt; } }
    if (!g_sf.d || !w || !sf_copy_images(g_sf.d, c, w, h)) { pthread_mutex_unlock(&g_sf.m); return; }
    if (c->key != key) { c->key = key; c->dec_serial = c->fwd_serial = 0; }
    g_sf.copies[g_sf.copies_next % 32].cb = (uint64_t)(uintptr_t)cb;
    g_sf.copies[g_sf.copies_next % 32].ci = ci;
    g_sf.copies[g_sf.copies_next % 32].fwd = 1;
    g_sf.copies_next++;
    pthread_mutex_unlock(&g_sf.m);
    sf_copy(d, cb, (VkImage)img, (VkImageLayout)lay, &c->h, w, h);
    __atomic_add_fetch(&g_sf.fwds, 1, __ATOMIC_RELAXED);
}

// What the compositor needs for the frame it presents (g_lock and g_trace_lock held): the slot's
// images (or the 1x1 stand-ins) and the camera block (wowbg3_compose.comp, Cam). Returns the flags
// (0: no surfaces this frame). `cam_m` is the BGC1 record seq WoW's composited frame was drawn for.
static uint32_t surf_for_compose(DeviceData* d, uint64_t framed_serial, uint32_t cam_m, uint32_t glight_m, VkImageView views[4], uint8_t camblk[176]) {
    memset(camblk, 0, 176);
    pthread_mutex_lock(&g_sf.m);
    int have = sf_dummies(d);
    for (int k = 0; k < 4; k++) views[k] = g_sf.dummy[k].view;
    if (!have || !g_cc.surf || !g_sf.ready || !framed_serial || g_sf.d != d) { pthread_mutex_unlock(&g_sf.m); return have ? 0 : 0xffffffffu; }
    SfSlot* s = &g_sf.s[framed_serial % SF_SLOTS];
    uint32_t fl = 0;
    if (s->serial == framed_serial && s->alb_serial == framed_serial) {
        int dec = -1, fwd = -1;
        for (int k = 0; k < SF_COPIES; k++) {
            if (g_sf.c[k].a1.img && g_sf.c[k].dec_serial == framed_serial) dec = k;
            if (g_sf.c[k].h.img && g_sf.c[k].fwd_serial == framed_serial) fwd = k;
        }
        if (dec < 0 && fwd >= 0) g_sf.nodec++;
        if (fwd < 0 && dec >= 0) g_sf.nofwd++;
        if (dec >= 0 || fwd >= 0) {
            fl = 1u | (dec >= 0 ? 2u : 0u) | (fwd >= 0 ? 4u : 0u);
            views[0] = s->z.view; views[1] = s->a0.view;
            if (dec >= 0) views[2] = g_sf.c[dec].a1.view;
            if (fwd >= 0) views[3] = g_sf.c[fwd].h.view;
        } else {
            g_sf.nosurf++;
        }
    } else {
        g_sf.stale++;
    }
    if (g_sf.last_serial > framed_serial && g_sf.last_serial - framed_serial > g_sf.deepest) g_sf.deepest = g_sf.last_serial - framed_serial;
    GrRec rec = s->rec;
    if (g_sf.lf && g_sf.log_left > 0) {
        fprintf(g_sf.lf, "%llu,%llu,%llu,%u,%u,%u,%u,%u,%u\n", (unsigned long long)g_req.presents, (unsigned long long)now_ns(),
                (unsigned long long)framed_serial, fl, cam_m, s->wcam, g_sf.last_sp, g_sf.last_ok, g_sf.last_bad);
        if (--g_sf.log_left == 0) { fclose(g_sf.lf); g_sf.lf = NULL; }
    }
    pthread_mutex_unlock(&g_sf.m);
    if (!fl && !g_cc.fxbg) return 0;
    uint8_t crec[96];
    float eye[3], fwd[3], right[3], up[3], tx, ty;
    if (!g_cc.surfpix && (!cam_m || !sf_cam_rec(cam_m, crec) || !sf_cam(crec, rec.eye, eye, fwd, right, up, &tx, &ty))) return 0;
    if (g_cc.surfpix) fl |= 8u;
    if (g_cc.surfdbg) fl |= 16u;
    float* f = (float*)camblk;
    memcpy(f, rec.vp, 64);
    f[16] = rec.fwd[0]; f[17] = rec.fwd[1]; f[18] = rec.fwd[2]; f[19] = 0;
    if (!g_cc.surfpix) {
        f[20] = eye[0]; f[21] = eye[1]; f[22] = eye[2]; f[23] = tx;
        f[24] = fwd[0]; f[25] = fwd[1]; f[26] = fwd[2]; f[27] = ty;
        f[28] = right[0]; f[29] = right[1]; f[30] = right[2]; f[31] = 0;
        f[32] = up[0]; f[33] = up[1]; f[34] = up[2]; f[35] = 0;
    }
    f[36] = g_cc.sdec; f[37] = g_cc.sfwd; f[38] = g_cc.sfloor; f[39] = g_cc.stol;
    uint32_t* u = (uint32_t*)(f + 40);
    // WoW's light on the ground (benilla 0052): BG3's decal colours lit by it.
    if (g_cc.slight && glight_m) { fl |= 32u; u[1] = glight_m; }
    if (g_cc.fxbg) fl |= 64u;  // the camera block for the effects' lookup
    u[0] = fl;
    g_sf.composed++;
    if (!g_cc.surfpix) {  // the mapping check: WoW's camera (a few BG3 frames older) has BG3's own axes
        float df = fwd[0] * rec.fwd[0] + fwd[1] * rec.fwd[1] + fwd[2] * rec.fwd[2];
        float dr = right[0] * rec.right[0] + right[1] * rec.right[1] + right[2] * rec.right[2];
        float du = up[0] * rec.up[0] + up[1] * rec.up[1] + up[2] * rec.up[2];
        float el = sqrtf(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
        if (df < g_sf.dot_f) g_sf.dot_f = df;
        if (dr < g_sf.dot_r) g_sf.dot_r = dr;
        if (du < g_sf.dot_u) g_sf.dot_u = du;
        if (el > g_sf.eye_max) g_sf.eye_max = el;
        g_sf.dots++;
    }
    // how many BG3 frames WoW's frame K (albedo) and M (composite) lag behind N
    if (cam_m && s->wcam) { g_sf.kage_sum += (int32_t)(cam_m - s->wcam); g_sf.kage_n++; }
    return fl;
}
// The compositor's command buffer, after its work (g_lock held): WoW's frame it composited, copied
// for the albedo pass of the next BG3 frame recorded (colour and depth rows, as the reader keeps them).
static void surf_compose_copy(DeviceData* d, VkCommandBuffer cmd, const WowSlot* w) {
    if (!g_cc.surf || !g_sf.ready || !w->b.buf || !w->cam || !d->CmdCopyBuffer) return;
    if (!sf_active()) {  // no surfaces lately: no copy (an old one is not used when they come back)
        pthread_mutex_lock(&g_sf.m);
        g_sf.wc_latest = -1;
        pthread_mutex_unlock(&g_sf.m);
        return;
    }
    VkDeviceSize n = (VkDeviceSize)w->cbpr * w->wh + (VkDeviceSize)w->dbpr * w->wh;
    pthread_mutex_lock(&g_sf.m);
    int k = g_sf.wc_latest < 0 ? 0 : 1 - g_sf.wc_latest;
    if (!gbuf_make(d, &g_sf.wc[k], n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0)) {
        pthread_mutex_unlock(&g_sf.m);
        return;
    }
    VkBuffer dst = g_sf.wc[k].buf;
    g_sf.wcm[k].ww = w->ww; g_sf.wcm[k].wh = w->wh; g_sf.wcm[k].cbpr = w->cbpr; g_sf.wcm[k].dbpr = w->dbpr;
    g_sf.wcm[k].code = w->code; g_sf.wcm[k].cam = w->cam; g_sf.wcm[k].wnear = w->wnear; g_sf.wcm[k].wzoff = w->wzoff;
    g_sf.wcm[k].glight = w->glight;
    g_sf.wc_latest = k;
    pthread_mutex_unlock(&g_sf.m);
    // its reads by an earlier albedo pass first; host writes (the reader) are visible at submit
    VkMemoryBarrier b0 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b0, 0, NULL, 0, NULL);
    VkBufferCopy rg = {0, 0, n};
    d->CmdCopyBuffer(cmd, w->b.buf, dst, 1, &rg);
    VkMemoryBarrier b1 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &b1, 0, NULL, 0, NULL);
}

// The compositor's surface pixel counts of a finished submission.
static void surf_counts(uint32_t sp, uint32_t ok, uint32_t bad) {
    g_sf.px_sp += sp; g_sf.px_ok += ok; g_sf.px_bad += bad; g_sf.px_frames++;
    g_sf.last_sp = sp; g_sf.last_ok = ok; g_sf.last_bad = bad;
}
static void surf_log_request(int n) {
    pthread_mutex_lock(&g_sf.m);
    if (g_sf.lf) fclose(g_sf.lf);
    g_sf.lf = fopen(outp("/surflog.txt"), "w");
    g_sf.log_left = n > 0 && n <= 36000 ? n : 600;
    if (g_sf.lf) fprintf(g_sf.lf, "present,t_ns,serial,flags,cam_m,cam_k,changed_px,matched_px,unmatched_px (counts: the submission 3 presents back)\n");
    pthread_mutex_unlock(&g_sf.m);
    logf_("surf: writing %d presents to surflog.txt", n);
}
// Before the compositor's dispatch (its command buffer): the slot images and stand-ins enter GENERAL once.
static void surf_compose_prepare(DeviceData* d, VkCommandBuffer cmd) {
    pthread_mutex_lock(&g_sf.m);
    if (!g_sf.dummy_init && g_sf.dummy[3].img) {
        for (int k = 0; k < 4; k++)
            sf_barrier(d, cmd, g_sf.dummy[k].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        g_sf.dummy_init = 1;
    }
    pthread_mutex_unlock(&g_sf.m);
}
// At each present (g_lock held): GPU times and the log.
static void surf_present(void) {
    if (!g_sf.ready) return;
    g_sf.presents++;
    DeviceData* d = g_sf.d;
    pthread_mutex_lock(&g_sf.m);
    if (g_sf.qp) {
        for (int si = 0; si < SF_SLOTS; si++) {
            SfSlot* s = &g_sf.s[si];
            for (int p = 0; p < 3; p++) {
                if (!(s->q_pending & (1u << p))) continue;
                uint64_t ts[2];
                VkResult r = d->GetQueryPoolResults(d->device, g_sf.qp, 6 * si + 2 * p, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT);
                if (r == VK_NOT_READY) continue;
                s->q_pending &= ~(1u << p);
                if (r != VK_SUCCESS || ts[1] < ts[0]) continue;
                uint64_t ns = (uint64_t)((double)(ts[1] - ts[0]) * g_sf.tick_ns);
                if (p == 0) { g_sf.gpu_alb += ns; g_sf.n_alb++; }
                if (p == 1) { g_sf.gpu_dec += ns; g_sf.n_dec++; }
                if (p == 2) { g_sf.gpu_fwd += ns; g_sf.n_fwd++; }
            }
        }
    }
    if (g_sf.presents % 300 == 0) {
        logf_("surf: %llu injections (%llu idle: no surface drawn lately), %llu albedo passes (%.3f ms GPU), %llu decal copies (%.3f ms), %llu forward copies (%.3f ms); "
              "no WoW frame %llu, no camera %llu; surface draws kept in the void %llu; composited %llu (no surface %llu, no decal copy %llu, no forward copy %llu, stale %llu of %d slots, deepest frame in flight %llu, a second copy of a kind in one frame %llu), "
              "WoW frame of the albedo %.1f cameras before the composited one; pixels per frame: changed %.0f, ground matched %.0f, not matched %.0f; "
              "WoW camera vs BG3's axes (min dot) forward %.4f right %.4f up %.4f, eye apart up to %.2f m (%llu frames); "
              "albedo passes owed but not recorded: slot moved on %llu, no push constants %llu, off %llu, framebuffer %llu, no WoW copy %llu; "
              "owed and not seen ending %llu; injections without the pass: setup %llu, camera not invertible %llu; command buffer ends seen %llu",
              (unsigned long long)g_sf.injects, (unsigned long long)g_sf.idle, (unsigned long long)g_sf.albs, g_sf.n_alb ? g_sf.gpu_alb / 1e6 / g_sf.n_alb : 0.0,
              (unsigned long long)g_sf.decs, g_sf.n_dec ? g_sf.gpu_dec / 1e6 / g_sf.n_dec : 0.0, (unsigned long long)g_sf.fwds,
              g_sf.n_fwd ? g_sf.gpu_fwd / 1e6 / g_sf.n_fwd : 0.0, (unsigned long long)g_sf.nowow, (unsigned long long)g_sf.nocam,
              (unsigned long long)g_sf.keeps, (unsigned long long)g_sf.composed, (unsigned long long)g_sf.nosurf, (unsigned long long)g_sf.nodec,
              (unsigned long long)g_sf.nofwd, (unsigned long long)g_sf.stale, SF_SLOTS,
              (unsigned long long)g_sf.deepest,
              (unsigned long long)g_sf.other_frame,
              g_sf.kage_n ? (double)g_sf.kage_sum / g_sf.kage_n : 0.0, g_sf.px_frames ? (double)g_sf.px_sp / g_sf.px_frames : 0.0,
              g_sf.px_frames ? (double)g_sf.px_ok / g_sf.px_frames : 0.0, g_sf.px_frames ? (double)g_sf.px_bad / g_sf.px_frames : 0.0,
              g_sf.dots ? g_sf.dot_f : 0.0, g_sf.dots ? g_sf.dot_r : 0.0, g_sf.dots ? g_sf.dot_u : 0.0, g_sf.eye_max, (unsigned long long)g_sf.dots,
              (unsigned long long)g_sf.endwhy[1], (unsigned long long)g_sf.endwhy[2], (unsigned long long)g_sf.endwhy[3],
              (unsigned long long)g_sf.endwhy[4], (unsigned long long)g_sf.endwhy[5], (unsigned long long)g_sf.endwhy[0],
              (unsigned long long)g_sf.endwhy[6], (unsigned long long)g_sf.endwhy[7], (unsigned long long)__atomic_exchange_n(&g_sf_ends, 0, __ATOMIC_RELAXED));
        memset(g_sf.endwhy, 0, sizeof g_sf.endwhy);
        g_sf.injects = g_sf.idle = g_sf.albs = g_sf.nowow = g_sf.nocam = g_sf.decs = g_sf.fwds = g_sf.keeps = g_sf.composed = g_sf.nosurf = g_sf.stale = 0;
        g_sf.nodec = g_sf.nofwd = 0;
        g_sf.other_frame = 0;
        g_sf.gpu_alb = g_sf.gpu_dec = g_sf.gpu_fwd = g_sf.n_alb = g_sf.n_dec = g_sf.n_fwd = 0;
        g_sf.px_sp = g_sf.px_ok = g_sf.px_bad = g_sf.px_frames = 0;
        g_sf.kage_sum = 0; g_sf.kage_n = 0;
        g_sf.dot_f = g_sf.dot_r = g_sf.dot_u = 2; g_sf.eye_max = 0; g_sf.dots = 0;
    }
    pthread_mutex_unlock(&g_sf.m);
}
