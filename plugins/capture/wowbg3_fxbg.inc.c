// fx-route prototype (not for release): WoW's picture as the background of BG3's own effects pass.
// Word "fxbg" (with "ground surf"); "fxcal": the same pass draws a grey ramp, to measure BG3's display
// chain from BG3's own output. See AI Context/fx-route-comparison.md.
#include "wowbg3_fxbg_spv.h"
#include "wowbg3_fxfog_spv.h"
#include "wowbg3_bare_spv.h"
#include "wowbg3_post_spv.h"
#include "wowbg3_postcas_spv.h"
#include "wowbg3_ovl_spv.h"
#include "wowbg3_blmask_spv.h"
#include "wowbg3_blcut_spv.h"
#define FB_IMGS 8
static pthread_mutex_t g_fbt_m = PTHREAD_MUTEX_INITIALIZER;  // g_fbg.t and g_bare.t (patch35)
static uint8_t g_fb_dead[FB_IMGS + 4];   // patch36 (fbstale): slot whose image BG3 destroyed, view kept
static uint64_t g_fb_stale[2], g_fb_dead_n;  // draws through a dead slot: feed, wdbare; destroys seen
static void fb_stale_note(int bare, int slot) {
    if (!g_fb_dead[bare ? FB_IMGS + slot : slot]) return;
    uint64_t n = ++g_fb_stale[bare];
    if (n <= 10 || n % 300 == 0) logf_("fbstale: %s drew through the view of an image BG3 destroyed (slot %d), %llu such draws", bare ? "wdbare" : "feed", slot, (unsigned long long)n);
}
static VkImageView wd_mask_view(void);
#define FB_PC 192  // patch61: + evk; + amb (WoW's ambient on BG3's own pixels); + fog (WoW's fog on them)
// wdjit: BG3's TAA jitter (NDC, y down) of the frame recorded at a present, from wd_draw's G-buffer camera.
static struct { uint64_t p; float j[2]; } g_jit[8];
static uint64_t g_jit_used, g_jit_missing, g_jit_dp[5]; static float g_jit_last[2];
#define FB_TAB (17 * 17 * 17 * 3)
#define FB_LIT 8                       // WoW's point lights for BG3's pixels, after the table: a header of 4 floats, 8 a light
#define FB_TAIL (4 + FB_LIT * 8)
static struct {
    int ready, failed;
    VkRenderPass rp; VkPipelineLayout layout; VkDescriptorSetLayout dsl; VkPipeline pipe;
    VkDescriptorPool pool; VkDescriptorSet set[2]; VkBuffer setbuf[2];
    struct { uint64_t img; VkImageView view; VkFramebuffer fb; } t[FB_IMGS];
    uint64_t fed_present, fed_img; uint64_t feeds, refused, nowow; uint64_t fedp[3], fedi[3], fogs;
    VkBuffer tbuf; VkDeviceMemory tmem; float* tmap; struct timespec tseen;
    uint64_t ebuf[2], eoff[2]; int etype[2]; VkBuffer esetbuf[2][2];
    VkSampler asmp;
} g_fbg;
// WoW's ambient on BG3's own pixels (compose word "wdamb=K", K the share of WoW's ambient, 0 or absent: off).
// BG3 lights its characters with its sun and little else: the level of our own has no light probe, so a side turned
// from the sun is near black, where WoW lights every unit with the zone's ambient colour first. benilla writes that
// colour (link/wowsky.txt, patch 0125: the last three numbers, as WoW samples Light.dbc for the time of day), and the
// feed adds what WoW's ambient term is, the material's colour times the ambient, to BG3's lit scene at each of BG3's
// pixels, through the same table that brings WoW's ground into BG3's light (wowbg3_fxbg2.frag).
static struct { pthread_mutex_t m; float rgb[3]; int have; uint64_t reads; float fog[5]; int have_fog; int nlit; float lit[FB_LIT][7]; } g_amb = {.m = PTHREAD_MUTEX_INITIALIZER};
static void amb_present(void) {
    const uint64_t pr = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    if (!g_cc.on || (!(g_cc.wdamb > 0.0f) && !g_cc.wdwfog) || pr % (g_cc.wdwlit ? 6 : 30) != 1) return;
    char path[1100];
    snprintf(path, sizeof path, "%s/wowsky.txt", link_dir());
    float v[14]; int ok = 0, okf = 0;
    FILE* f = fopen(path, "r");
    if (f) {
        ok = fscanf(f, "WSK1 %f %f %f %f %f %f %f %f %f", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7, v + 8) == 9;
        okf = ok && fscanf(f, "%f %f %f %f %f", v + 9, v + 10, v + 11, v + 12, v + 13) == 5;  // benilla patch 0129: fog r g b, start, end
        fclose(f);
    }
    // WoW's point lights near the hero (benilla patch 0130: link/wowlights.txt).
    float lit[FB_LIT][7]; int nlit = 0;
    if (g_cc.wdwlit) {
        snprintf(path, sizeof path, "%s/wowlights.txt", link_dir());
        FILE* lf = fopen(path, "r");
        if (lf) {
            int count = 0;
            if (fscanf(lf, "WLT1 %d", &count) == 1)
                for (int k = 0; k < count && nlit < FB_LIT; k++) {
                    float* v7 = lit[nlit];
                    if (fscanf(lf, "%f %f %f %f %f %f %f", v7, v7 + 1, v7 + 2, v7 + 3, v7 + 4, v7 + 5, v7 + 6) != 7) break;
                    nlit++;
                }
            fclose(lf);
        }
    }
    pthread_mutex_lock(&g_amb.m);
    g_amb.nlit = nlit; memcpy(g_amb.lit, lit, sizeof lit);
    if (ok) { for (int i = 0; i < 3; i++) g_amb.rgb[i] = v[6 + i] < 0 ? 0 : v[6 + i] > 1 ? 1 : v[6 + i]; g_amb.have = 1; }
    if (okf) { memcpy(g_amb.fog, v + 9, sizeof g_amb.fog); g_amb.have_fog = 1; }
    const uint64_t n = ++g_amb.reads;
    pthread_mutex_unlock(&g_amb.m);
    if (n % (g_cc.wdwlit ? 3000 : 600) == 1) logf_("wdamb: WoW's ambient %.3f %.3f %.3f (%s), share %.2f; fog %.3f %.3f %.3f from %.1f to %.1f yd (%s)", g_amb.rgb[0], g_amb.rgb[1], g_amb.rgb[2], ok ? "read" : "kept: no wowsky.txt", g_cc.wdamb,
                         g_amb.fog[0], g_amb.fog[1], g_amb.fog[2], g_amb.fog[3], g_amb.fog[4], !g_cc.wdwfog ? "off" : okf ? "read" : "none in wowsky.txt");
    if (g_cc.wdwlit && n % 3000 == 1) logf_("wdwlit: %d of WoW's point lights near the hero", nlit);
}
// WoW's fog on BG3's own pixels (compose word "wdwfog"). WoW fogs everything it draws by its eye depth, a straight
// blend towards the fog colour between a start and an end distance, in the colours it displays. BG3's characters and
// what it draws on the ground got none of it. The feed finds each of BG3's pixels' depth in BG3's G-buffer (its
// eye-depth image, metres), works out WoW's factor for it and leaves colour and factor beside WoW's own colours (wdsp,
// alpha 2 + factor); wdpostcas, after BG3's display transform, blends as WoW does.
// f[0..3]: start, end, 1 (there is fog), the colour packed 10 bits a channel.
static void wfog_get(float* f) {
    if (!g_cc.wdwfog) return;
    pthread_mutex_lock(&g_amb.m);
    if (g_amb.have_fog && g_amb.fog[4] > g_amb.fog[3]) {
        f[0] = g_amb.fog[3]; f[1] = g_amb.fog[4]; f[2] = 1.0f;
        uint32_t c = 0;
        for (int i = 0; i < 3; i++) { float v = g_amb.fog[i] < 0 ? 0 : g_amb.fog[i] > 1 ? 1 : g_amb.fog[i]; c |= (uint32_t)(v * 1023.0f + 0.5f) << (10 * i); }
        memcpy(f + 3, &c, 4);
    }
    pthread_mutex_unlock(&g_amb.m);
}
// WoW's point lights on BG3's own pixels (compose word "wdwlit"): a torch on a wall, a camp fire, a lamp post light
// WoW's units and left BG3's characters dark beside them. The feed has each of BG3's pixels' place and facing (BG3's
// G-buffer eye depth, and the facing that depth's neighbours give), and adds for it what WoW adds for a unit: its three nearest lights, each its colour
// over 0.7 d + 0.03 d^2 (d in yards) times the facing, into the same sum as the ambient.
// out: count, the ambient share, then a light each: its place in the camera's view (metres: right, up, forward),
// its range (yards), its colour.
static void wlit_put(float* out, int on, const float* eye, const float* fwd, const float* right, const float* up) {
    out[0] = 0.0f; out[1] = g_cc.wdamb;
    { static uint64_t said; if (g_cc.wdwlit && said++ % 1800 == 0) logf_("wdwlit: %d lights; BG3's eye depth %s, ambient %s", g_amb.nlit, g_blv_z ? "found" : "not bound by its lighting pass", on || !g_blv_z ? "on" : "off"); }
    if (!on || !g_cc.wdwlit || !g_tav.seq) return;
    pthread_mutex_lock(&g_amb.m);
    const int n = g_amb.nlit;
    const double yd = 0.9144;
    for (int i = 0; i < n; i++) {
        const float* l = g_amb.lit[i];
        const double w[3] = {g_tav.centre[0] + (l[1] - g_tav.wow[1]) * yd - eye[0], g_tav.centre[1] + (l[2] - g_tav.wow[2]) * yd - eye[1],
                             g_tav.centre[2] - (l[0] - g_tav.wow[0]) * yd - eye[2]};
        float* o8 = out + 4 + i * 8;
        o8[0] = (float)(w[0] * right[0] + w[1] * right[1] + w[2] * right[2]);
        o8[1] = (float)(w[0] * up[0] + w[1] * up[1] + w[2] * up[2]);
        o8[2] = (float)(w[0] * fwd[0] + w[1] * fwd[1] + w[2] * fwd[2]);
        o8[3] = l[3]; o8[4] = l[4]; o8[5] = l[5]; o8[6] = l[6]; o8[7] = 0.0f;
    }
    pthread_mutex_unlock(&g_amb.m);
    { static uint64_t said2; if (g_cc.wdlog && n && said2++ % 600 == 0) logf_("wdwlit: nearest light in view at %.2f %.2f %.2f m (right, up, forward), colour %.2f %.2f %.2f", out[4], out[5], out[6], out[8], out[9], out[10]); }
    out[0] = (float)n;
}
// rgb: WoW's ambient times the dial; 0 when there is none to add.
static int amb_get(float* rgb) {
    if (!(g_cc.wdamb > 0.0f)) return 0;
    pthread_mutex_lock(&g_amb.m);
    const int have = g_amb.have;
    for (int i = 0; i < 3; i++) rgb[i] = g_amb.rgb[i] * g_cc.wdamb;
    pthread_mutex_unlock(&g_amb.m);
    return have;
}
// patch60: BG3's eye state, where its adaptation writes it.
#define WDX_METER 0x7387a4a6b28ed0d9ull
#define WDX_ADAPT 0x1f20b342cda3d311ull
#define WDX_WANT (g_cc.wdevref > 0.0f)  // patch61: the feed's wdevref
static struct {
    GBuf cur, held; int cur_ok, held_ok, made_fail; float held_e;
    uint64_t out_res, out_off, in_res, in_off, n, skipped; uint32_t maxw;
    FILE* csv; uint64_t csv_last;
    struct { uint64_t key; uint32_t set, bind; } seen[64]; int nseen;
} g_wdx;
static __thread VkCommandBuffer t_wdx_cb; static __thread uint32_t t_wdx_mw, t_wdx_mh; static __thread uint64_t t_wdx_key;
static void wdx_dispatch_pre(VkCommandBuffer cb) {
    t_wdx_key = 0;
    if (t_cbind_cb != cb || !t_cpipe) return;
    CPipeInfo* p = g_cpipe ? TABLE_FIND(g_cpipe, CPIPE_CAP, t_cpipe, 0) : NULL;
    if (!p) return;
    t_wdx_key = p->spv;
    if (p->spv != WDX_METER) return;
    uint64_t img = post_image(t_cset, 1, 1);  // the HDR target the meter reads
    ImgInfo* im = img && g_img ? TABLE_FIND(g_img, IMG_CAP, img, 0) : NULL;
    t_wdx_cb = cb; t_wdx_mw = im ? im->w : 0; t_wdx_mh = im ? im->hgt : 0;
    if (im && im->w > g_wdx.maxw) g_wdx.maxw = im->w;
    if (g_cc.wdexplog) {
        static uint64_t nm; uint64_t o2 = 0;
        const DescSlot* s2 = post_slot(t_cset, t_cdyn, t_cndyn, 1, 2, &o2);
        if (nm++ < 4 || nm % 1800 == 0) logf_("wdexplog: meter %llu reads %ux%u, state %llx+%llu (type %d range %llu)", (unsigned long long)nm, t_wdx_mw, t_wdx_mh,
                                              s2 ? (unsigned long long)s2->res : 0ull, (unsigned long long)o2, s2 ? s2->type : -1, s2 ? (unsigned long long)s2->range : 0ull);
    }
}
static void wdx_barrier(DeviceData* d, VkCommandBuffer cb, VkPipelineStageFlags s0, VkAccessFlags a0, VkPipelineStageFlags s1, VkAccessFlags a1) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, a0, a1};
    d->CmdPipelineBarrier(cb, s0, s1, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void wdx_dispatch_after(DeviceData* d, VkCommandBuffer cb) {
    if (t_wdx_key != WDX_ADAPT || t_cbind_cb != cb || !d->CmdCopyBuffer) return;
    if (!(g_cc.wdexplog || g_cc.wdexp > 0.0f || WDX_WANT)) return;
    uint64_t oo = 0, oi = 0;
    const DescSlot* so = post_slot(t_cset, t_cdyn, t_cndyn, 1, 1, &oo);
    const DescSlot* si = post_slot(t_cset, t_cdyn, t_cndyn, 1, 2, &oi);
    if (!so || !so->res) return;
    // Only the main view's eye: its meter ran in this command buffer on the widest HDR target seen.
    if (t_wdx_cb != cb || !t_wdx_mw || t_wdx_mw < g_wdx.maxw) { if (g_wdx.skipped++ < 4) logf_("wdexplog: an adaptation not on the main view skipped (meter %ux%u, widest %u)", t_wdx_mw, t_wdx_mh, g_wdx.maxw); return; }
    if (!g_wdx.cur_ok && !g_wdx.made_fail) {
        if (gbuf_make(d, &g_wdx.cur, 32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) g_wdx.cur_ok = 1;
        else { g_wdx.made_fail = 1; logf_("wdexplog: own state buffer not made"); return; }
        memset(g_wdx.cur.map, 0, 32);
    }
    g_wdx.out_res = so->res; g_wdx.out_off = oo; g_wdx.in_res = si ? si->res : 0; g_wdx.in_off = oi; g_wdx.n++;
    wdx_barrier(d, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    if (g_cc.wdexp > 0.0f) {  // the hold: all three values where BG3's adaptation wrote them
        if (!g_wdx.held_ok) g_wdx.held_ok = gbuf_make(d, &g_wdx.held, 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
        if (g_wdx.held_ok) {
            if (g_wdx.held_e != g_cc.wdexp) {  // a new value only when the word changes
                float e = g_cc.wdexp, v[4] = {e, 1.0f / e, log2f(1.0f / (1.2f * e)), -8.0f};
                memcpy(g_wdx.held.map, v, sizeof v); g_wdx.held_e = e;
                logf_("wdexp: holding BG3's eye at EV %g (%g, %g) after its adaptation", v[2], v[0], v[1]);
            }
            VkBufferCopy r = {0, oo, 12};
            d->CmdCopyBuffer(cb, g_wdx.held.buf, (VkBuffer)so->res, 1, &r);
            wdx_barrier(d, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
    } else g_wdx.held_e = 0.0f;
    VkBufferCopy rc = {oo, 0, 24};  // BG3's newest state into ours (read by the feed of the next frame, in queue order)
    d->CmdCopyBuffer(cb, (VkBuffer)so->res, g_wdx.cur.buf, 1, &rc);
    wdx_barrier(d, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    if (g_cc.wdexplog) {
        float u[4] = {0}; int ok = post_bytes(t_cset, t_cdyn, t_cndyn, 1, 3, u, 16);
        const float* c = (const float*)g_wdx.cur.map;  // a frame or two old on the CPU: a log, not a control
        if (g_wdx.n <= 4 || g_wdx.n % 300 == 0)
            logf_("wdexplog: adaptation %llu out %llx+%llu (type %d) in %llx+%llu; window %g..%g compensation %g speed %g (%s); state %g %g EV %g %g",
                  (unsigned long long)g_wdx.n, (unsigned long long)so->res, (unsigned long long)oo, so->type, (unsigned long long)g_wdx.in_res, (unsigned long long)oi,
                  u[0], u[1], u[2], u[3], ok ? "read" : "not mapped", c[0], c[1], c[2], c[3]);
        if (!g_wdx.csv) { g_wdx.csv = fopen(outp("/wdev.csv"), "a"); if (g_wdx.csv) fprintf(g_wdx.csv, "present,ev,ev_min,ev_max,comp,speed\n"); }
        if (g_wdx.csv && g_req.presents != g_wdx.csv_last) { g_wdx.csv_last = g_req.presents; fprintf(g_wdx.csv, "%llu,%.4f,%g,%g,%g,%g\n", (unsigned long long)g_req.presents, c[2], u[0], u[1], u[2], u[3]); if (g_wdx.n % 60 == 0) fflush(g_wdx.csv); }
    }
}
// Every pipeline that binds the eye state's buffer near its offset (who reads BG3's exposure on the GPU), once per key/set/binding.
static void wdx_readers(VkCommandBuffer cb, int compute) {
    if (!g_wdx.out_res || g_wdx.nseen >= 64) return;
    const uint64_t* sets = compute ? t_cset : t_set;
    if (compute ? (t_cbind_cb != cb || !t_cpipe) : (t_bind_cb != cb || !t_pipe)) return;
    uint64_t key = 0, key2 = 0;
    if (compute) { CPipeInfo* p = g_cpipe ? TABLE_FIND(g_cpipe, CPIPE_CAP, t_cpipe, 0) : NULL; key = p ? p->spv : 0; }
    else {
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        for (uint32_t s = 0; ri && ri->r && s < ri->r->nstages; s++) {
            if (ri->r->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) key = ri->r->st[s].key;
            if (ri->r->st[s].stage == VK_SHADER_STAGE_VERTEX_BIT) key2 = ri->r->st[s].key;
        }
    }
    for (uint32_t s = 0; s < 8; s++) {
        if (!sets[s]) continue;
        SetInfo* si = set_find(sets[s]);
        for (uint32_t b = 0; si && b < DESC_SLOTS; b++) {
            const DescSlot* sl = &si->slot[b];
            if (!sl->res || (sl->res != g_wdx.out_res && sl->res != g_wdx.in_res)) continue;
            uint64_t off = 0;
            post_slot(sets, compute ? t_cdyn : t_dyn, compute ? t_cndyn : t_ndyn, s, b, &off);
            uint64_t rng = sl->range == VK_WHOLE_SIZE ? ~0ull : sl->range;
            int hit_out = sl->res == g_wdx.out_res && g_wdx.out_off >= off && g_wdx.out_off < off + rng;
            int hit_in = sl->res == g_wdx.in_res && g_wdx.in_off >= off && g_wdx.in_off < off + rng;
            int dup = 0;
            for (int k = 0; k < g_wdx.nseen; k++) dup |= g_wdx.seen[k].key == key && g_wdx.seen[k].set == s && g_wdx.seen[k].bind == b;
            if (dup || g_wdx.nseen >= 64) continue;
            g_wdx.seen[g_wdx.nseen].key = key; g_wdx.seen[g_wdx.nseen].set = s; g_wdx.seen[g_wdx.nseen].bind = b; g_wdx.nseen++;
            logf_("wdexplog: reader %s %llx%s%llx set %u binding %u type %d at %llx+%llu range %llu: %s (last out %llx+%llu, in %llx+%llu)", compute ? "cs" : "fs",
                  (unsigned long long)key, compute ? "" : " vs ", (unsigned long long)key2, s, b, sl->type, (unsigned long long)sl->res, (unsigned long long)off,
                  (unsigned long long)sl->range, hit_out ? "covers the last state written" : hit_in ? "covers the last state read" : "same buffer, elsewhere",
                  (unsigned long long)g_wdx.out_res, (unsigned long long)g_wdx.out_off, (unsigned long long)g_wdx.in_res, (unsigned long long)g_wdx.in_off);
        }
    }
}
static int fb_make(DeviceData* d) {
    if (g_fbg.ready) return 1;
    if (g_fbg.failed) return 0;
    g_fbg.failed = 1;
    VkAttachmentDescription at = {0, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &cr, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &at, 1, &sp, 0, NULL};
    if (d->CreateRenderPass(d->device, &rci, NULL, &g_fbg.rp) != VK_SUCCESS) return 0;
    VkDescriptorSetLayoutBinding b[10] = {{8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 10, b};
    if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_fbg.dsl) != VK_SUCCESS) return 0;
    VkSamplerCreateInfo smi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_NEVER, 0, 0, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    if (d->CreateSampler(d->device, &smi, NULL, &g_fbg.asmp) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, FB_PC};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_fbg.dsl, 1, &pr};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_fbg.layout) != VK_SUCCESS) return 0;
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_surf_alb_vert, wowbg3_surf_alb_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_fxbg_frag, wowbg3_fxbg_frag};
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
    // WoW's pixel replaces what BG3 lit there (alpha 1), and on BG3's own pixels WoW's ambient is added to BG3's
    // light (alpha 0): source + destination x (1 - source alpha). Word "fxbgmax": the brighter of the two instead.
    VkPipelineColorBlendAttachmentState cb = {VK_TRUE, VK_BLEND_FACTOR_ONE, g_cc.fxbgmax ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, g_cc.fxbgmax ? VK_BLEND_OP_MAX : VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cb, {0, 0, 0, 0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, NULL, &cbs, &dy, g_fbg.layout, g_fbg.rp, 0, VK_NULL_HANDLE, -1};
    if (d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_fbg.pipe) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 4, ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_fbg.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l[2] = {g_fbg.dsl, g_fbg.dsl};
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_fbg.pool, 2, l};
    if (d->AllocateDescriptorSets(d->device, &sai, g_fbg.set) != VK_SUCCESS) return 0;
    VkBufferCreateInfo tci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (FB_TAB + FB_TAIL) * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (d->CreateBuffer(d->device, &tci, NULL, &g_fbg.tbuf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    d->GetBufferMemoryRequirements(d->device, g_fbg.tbuf, &req);
    int32_t ty = memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)ty};
    if (ty < 0 || d->AllocateMemory(d->device, &mai, NULL, &g_fbg.tmem) != VK_SUCCESS || d->BindBufferMemory(d->device, g_fbg.tbuf, g_fbg.tmem, 0) != VK_SUCCESS) return 0;
    if (d->MapMemory(d->device, g_fbg.tmem, 0, VK_WHOLE_SIZE, 0, (void**)&g_fbg.tmap) != VK_SUCCESS) return 0;
    for (int k = 0; k < 2; k++) {
        VkDescriptorBufferInfo bi = {g_fbg.tbuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_fbg.set[k], 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
        d->UpdateDescriptorSets(d->device, 1, &wr, 0, NULL);
    }
    g_fbg.ready = 1; g_fbg.failed = 0;
    logf_("fxbg: ready");
    return 1;
}

// wdfog: the composite's own atmosphere inputs, read from BG3's bound sets at its draw (recording thread).
#define FOG_FS 0x54fb164bcf9f7fe8ull
#define BARE_FS 0x11129c8f138629e1ull  // BG3's last pass on its lighting buffer (second skin-diffusion pass), which renders into it
#define TAA_FS 0x415d66f480407a17ull  // BG3's TAA resolve (wdpost)
#define CLD_FS 0x2ae2e7a50880c0bbull  // BG3's clouds, composited into its forward target after the sky pass
#define FOG_SETS 8
static struct {
    int ready, failed; VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe;
    VkDescriptorPool pool; VkDescriptorSet set[FOG_SETS]; uint32_t ring; uint64_t refused;
    SfImg st; uint32_t stw, sth; int stfail; VkRenderPass rpst; VkPipeline pipest; VkFramebuffer fbst;
} g_fog;
static __thread struct { DescSlot dep, frox, samp, u30, u4, b1, b2, b3; uint64_t o30, o4; } t_fogd;
static int fog_make(DeviceData* d) {
    if (g_fog.ready) return 1;
    if (g_fog.failed) return 0;
    g_fog.failed = 1;
    VkDescriptorSetLayoutBinding b[13] = {{10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {7, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 13, b};
    if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_fog.dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, FB_PC};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_fog.dsl, 1, &pr};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_fog.layout) != VK_SUCCESS) return 0;
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_surf_alb_vert, wowbg3_surf_alb_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_fxfog_frag, wowbg3_fxfog_frag};
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
    // Added to the composite's output (blend constant 1); the calibration ramp replaces it (constant 0).
    VkPipelineColorBlendAttachmentState cb = {VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cb, {0, 0, 0, 0}};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 3, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, NULL, &cbs, &dy, g_fog.layout, g_fbg.rp, 0, VK_NULL_HANDLE, -1};
    if (d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_fog.pipe) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[6] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * FOG_SETS}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * FOG_SETS},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FOG_SETS}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 5 * FOG_SETS}, {VK_DESCRIPTOR_TYPE_SAMPLER, FOG_SETS},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * FOG_SETS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, FOG_SETS, 6, ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_fog.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l[FOG_SETS];
    for (int i = 0; i < FOG_SETS; i++) l[i] = g_fog.dsl;
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_fog.pool, FOG_SETS, l};
    if (d->AllocateDescriptorSets(d->device, &sai, g_fog.set) != VK_SUCCESS) return 0;
    g_fog.ready = 1; g_fog.failed = 0;
    logf_("wdfog: ready");
    return 1;
}

// The S/T image (RGBA16F at the target's size: BG3's in-scatter, its transmittance) and the fog pass that writes it.
static int fog_st(DeviceData* d, VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (g_fog.st.img) return g_fog.stw == w && g_fog.sth == h;
    if (g_fog.stfail) return 0;
    g_fog.stfail = 1;
    if (!sf_img_make(d, &g_fog.st, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    g_fog.stw = w; g_fog.sth = h;
    sf_barrier(d, cb, g_fog.st.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue one = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d->CmdClearColorImage(cb, g_fog.st.img, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &rg);
    sf_barrier(d, cb, g_fog.st.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    g_fog.stfail = 0;
    logf_("wdfog: S/T image %ux%u", w, h);
    return 1;
}
static int fog_st_pass(DeviceData* d) {
    if (g_fog.pipest) return 1;
    if (!g_fog.st.img || !g_fog.ready) return 0;
    VkAttachmentDescription at = {0, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &cr, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &at, 1, &sp, 0, NULL};
    if (d->CreateRenderPass(d->device, &rci, NULL, &g_fog.rpst) != VK_SUCCESS) return 0;
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_fog.rpst, 1, &g_fog.st.view, g_fog.stw, g_fog.sth, 1};
    if (d->CreateFramebuffer(d->device, &fci, NULL, &g_fog.fbst) != VK_SUCCESS) return 0;
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_surf_alb_vert, wowbg3_surf_alb_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_fxfog_frag, wowbg3_fxfog_frag};
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
    VkPipelineColorBlendAttachmentState cb = {VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cb, {0, 0, 0, 0}};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 3, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, NULL, &cbs, &dy, g_fog.layout, g_fog.rpst, 0, VK_NULL_HANDLE, -1};
    if (d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_fog.pipest) != VK_SUCCESS) { g_fog.pipest = VK_NULL_HANDLE; return 0; }
    logf_("wdfog: S/T pass ready");
    return 1;
}

// wdbare: the lighting buffer's image and layout, and BG3's own views of its G-buffer albedo and surface (recording thread).
static __thread struct { uint64_t img; int32_t lay; uint64_t alb, surf, z; int32_t lalb, lsurf, lz; } t_bare;
static struct {
    int ready, failed; VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; VkSampler smp;
    VkDescriptorPool pool; VkDescriptorSet set[FOG_SETS]; uint32_t ring;
    struct { uint64_t img; VkImageView view; VkFramebuffer fb; } t[4];
    uint64_t passes, refused;
} g_bare;
// BG3 destroys one of its images (layer_DestroyImage, before the driver's destroy): its view and framebuffer in
// either table go too, so a new image at a recycled handle gets its own. BG3 has finished every command that used
// it (Vulkan's rule for destroying an image), and our draws are in BG3's command buffers.
static void post_forget_image(DeviceData* d, uint64_t img);  // patch46 (after g_pst and g_blm)
static void fb_forget_image(DeviceData* d, VkImage img) {
    if (!img || !d) return;
    static uint64_t forgot;
    pthread_mutex_lock(&g_fbt_m);
    for (int i = 0; i < FB_IMGS + 4; i++) {
        uint64_t* im = i < FB_IMGS ? &g_fbg.t[i].img : &g_bare.t[i - FB_IMGS].img;
        VkImageView* v = i < FB_IMGS ? &g_fbg.t[i].view : &g_bare.t[i - FB_IMGS].view;
        VkFramebuffer* f = i < FB_IMGS ? &g_fbg.t[i].fb : &g_bare.t[i - FB_IMGS].fb;
        if (*im != (uint64_t)img) continue;
        if (g_cc.fbstale) {
            if (!g_fb_dead[i]) { g_fb_dead[i] = 1; uint64_t n = ++g_fb_dead_n; if (n <= 20 || n % 100 == 0) logf_("fbstale: BG3 destroyed image %llx while it is cached (%s slot %d): view kept, %llu so far", (unsigned long long)img, i < FB_IMGS ? "feed" : "wdbare", i < FB_IMGS ? i : i - FB_IMGS, (unsigned long long)n); }
            continue;
        }
        if (*f && d->DestroyFramebuffer) d->DestroyFramebuffer(d->device, *f, NULL);
        if (*v && d->DestroyImageView) d->DestroyImageView(d->device, *v, NULL);
        *im = 0; *v = VK_NULL_HANDLE; *f = VK_NULL_HANDLE;
        forgot++;
        if (forgot <= 20 || forgot % 100 == 0)
            logf_("%s: BG3 destroyed its image %llx: its view and framebuffer forgotten (%llu so far)", i < FB_IMGS ? "fxbg" : "wdbare", (unsigned long long)img, (unsigned long long)forgot);
    }
    if (!g_cc.fbstale) post_forget_image(d, (uint64_t)img);
    pthread_mutex_unlock(&g_fbt_m);
}
static void bare_draw(VkCommandBuffer cb) {
    // BG3's sets at its draw: the views of its albedo and surface images.
    t_bare.alb = t_bare.surf = t_bare.z = 0;
    for (uint32_t s = 0; s < 4; s++) {
        SetInfo* si = t_set[s] ? set_find(t_set[s]) : NULL;
        for (uint32_t b = 0; si && b < DESC_SLOTS; b++) {
            if (si->slot[b].type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || !si->slot[b].res) continue;
            uint64_t im = view_image(si->slot[b].res);
            if (im && im == g_gb_alb) { t_bare.alb = si->slot[b].res; t_bare.lalb = si->slot[b].layout; }
            if (im && im == g_gb_z) { t_bare.z = si->slot[b].res; t_bare.lz = si->slot[b].layout; }
            if (im && im == g_gb_surf) { t_bare.surf = si->slot[b].res; t_bare.lsurf = si->slot[b].layout; }
        }
    }
    t_bare.img = 0;
    for (uint32_t k = 0; k < t_pass_n && k < 8; k++) {
        ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, t_pass_imgs[k], 0);
        if (i && i->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32) { t_bare.img = t_pass_imgs[k]; t_bare.lay = t_pass_fins[k]; }
    }
    static int said;
    if (!t_bare.img || !t_bare.alb || !t_bare.surf) { if (!said++) logf_("wdbare: pass found without its parts: target %llx albedo %llx surface %llx (G-buffer %llx %llx)", (unsigned long long)t_bare.img, (unsigned long long)t_bare.alb, (unsigned long long)t_bare.surf, (unsigned long long)g_gb_alb, (unsigned long long)g_gb_surf); return; }
    g_blv_alb = t_bare.alb; g_blv_lalb = t_bare.lalb; g_blv_surf = t_bare.surf; g_blv_lsurf = t_bare.lsurf;  // patch28
    g_blv_z = t_bare.z; g_blv_lz = t_bare.lz;
    { static int saidz; if (g_cc.wdwfog && !t_bare.z && !saidz++) logf_("wdwfog: BG3's lighting pass binds no eye-depth image (G-buffer %llx): no fog on BG3's pixels", (unsigned long long)g_gb_z); }
    t_bare_cb = cb;
}
static int bare_make(DeviceData* d) {
    if (g_bare.ready) return 1;
    if (g_bare.failed) return 0;
    g_bare.failed = 1;
    VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, b};
    if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_bare.dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_bare.dsl, 0, NULL};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_bare.layout) != VK_SUCCESS) return 0;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    if (d->CreateSampler(d->device, &sci, NULL, &g_bare.smp) != VK_SUCCESS) return 0;
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_surf_alb_vert, wowbg3_surf_alb_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_bare_frag, wowbg3_bare_frag};
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
    VkPipelineColorBlendAttachmentState cbs1 = {VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cbs1, {0, 0, 0, 0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, NULL, &cbs, &dy, g_bare.layout, g_fbg.rp, 0, VK_NULL_HANDLE, -1};
    if (d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_bare.pipe) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FOG_SETS}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * FOG_SETS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, FOG_SETS, 2, ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_bare.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l[FOG_SETS];
    for (int i = 0; i < FOG_SETS; i++) l[i] = g_bare.dsl;
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_bare.pool, FOG_SETS, l};
    if (d->AllocateDescriptorSets(d->device, &sai, g_bare.set) != VK_SUCCESS) return 0;
    g_bare.ready = 1; g_bare.failed = 0;
    logf_("wdbare: ready");
    return 1;
}
static void fb_bare(DeviceData* d, VkCommandBuffer cb) {
    if (!g_cc.wdbare || !g_fbg.ready || !bare_make(d)) return;
    VkImageView mv = wd_mask_view();
    ImgInfo* ii = TABLE_FIND(g_img, IMG_CAP, t_bare.img, 0);
    if (!mv || !ii || t_bare.lay < 0) { g_bare.refused++; return; }
    int slot = -1;
    pthread_mutex_lock(&g_fbt_m);
    for (int i = 0; i < 4 && slot < 0; i++) if (g_bare.t[i].img == t_bare.img) slot = i;
    if (slot < 0) {
        for (int i = 0; i < 4 && slot < 0; i++) if (!g_bare.t[i].img) slot = i;
        if (slot < 0) { g_bare.refused++; pthread_mutex_unlock(&g_fbt_m); return; }
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)t_bare.img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
            {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (d->CreateImageView(d->device, &vci, NULL, &g_bare.t[slot].view) != VK_SUCCESS) { pthread_mutex_unlock(&g_fbt_m); return; }
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_fbg.rp, 1, &g_bare.t[slot].view, ii->w, ii->hgt, 1};
        if (d->CreateFramebuffer(d->device, &fci, NULL, &g_bare.t[slot].fb) != VK_SUCCESS) { d->DestroyImageView(d->device, g_bare.t[slot].view, NULL); g_bare.t[slot].view = VK_NULL_HANDLE; pthread_mutex_unlock(&g_fbt_m); return; }
        g_bare.t[slot].img = t_bare.img; g_fb_dead[FB_IMGS + slot] = 0;
        logf_("wdbare: lighting buffer %llx %ux%u layout %d", (unsigned long long)t_bare.img, ii->w, ii->hgt, (int)t_bare.lay);
    }
    VkFramebuffer bare_fb = g_bare.t[slot].fb;
    fb_stale_note(1, slot);
    pthread_mutex_unlock(&g_fbt_m);
    VkDescriptorSet set = g_bare.set[g_bare.ring++ % FOG_SETS];
    VkDescriptorImageInfo i0 = {VK_NULL_HANDLE, mv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo i1 = {g_bare.smp, (VkImageView)t_bare.alb, (VkImageLayout)t_bare.lalb}, i2 = {g_bare.smp, (VkImageView)t_bare.surf, (VkImageLayout)t_bare.lsurf};
    VkWriteDescriptorSet wr[3] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i0, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i1, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i2, NULL, NULL}};
    d->UpdateDescriptorSets(d->device, 3, wr, 0, NULL);
    VkImageLayout lay = (VkImageLayout)t_bare.lay;
    sf_barrier(d, cb, (VkImage)t_bare.img, lay, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, g_fbg.rp, bare_fb, {{0, 0}, {ii->w, ii->hgt}}, 0, NULL};
    d->CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_bare.pipe);
    d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_bare.layout, 0, 1, &set, 0, NULL);
    VkViewport vp = {0, 0, (float)ii->w, (float)ii->hgt, 0, 1};
    VkRect2D sc = {{0, 0}, {ii->w, ii->hgt}};
    d->CmdSetViewport(cb, 0, 1, &vp);
    d->CmdSetScissor(cb, 0, 1, &sc);
    d->CmdDraw(cb, 3, 1, 0, 0);
    d->CmdEndRenderPass(cb);
    // BG3's state as it was (as fb_draw does): pipeline, sets, viewport and scissor.
    if (t_bind_cb == cb && t_pipe) {
        d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)t_pipe);
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        Recipe* rr = ri ? ri->r : NULL;
        PlInfo* pl = (rr && g_pl && rr->layout) ? TABLE_FIND(g_pl, PL_CAP, rr->layout, 0) : NULL;
        for (uint32_t k2 = 0; pl && k2 < pl->nsets && k2 < 8; k2++)
            if (t_set[k2]) { VkDescriptorSet ds = (VkDescriptorSet)t_set[k2]; d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)rr->layout, k2, 1, &ds, t_ndyn[k2], t_dyn[k2]); }
    }
    if (t_vs_cb == cb) {
        if (t_vs_have & 1) d->CmdSetViewport(cb, 0, 1, &t_vs_vp);
        if (t_vs_have & 2) d->CmdSetScissor(cb, 0, 1, &t_vs_sc);
    }
    sf_barrier(d, cb, (VkImage)t_bare.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, lay, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (g_bare.passes++ % 600 == 0) logf_("wdbare: %llu passes, %llu refused", (unsigned long long)g_bare.passes, (unsigned long long)g_bare.refused);
}

// ---- wdbloom (patch28): bare WoW pixels out of BG3's bloom. wfed: what the feed wrote (rgb, a 1); nob: 1 at pure WoW pixels.
static struct {
    SfImg wfed, nob, wdsp; uint32_t w, h; int fail;  // wdsp: WoW's own displayed colour per fed pixel (the feed), for wdpostcas
    SfImg cmp0, cmp1, late;  // BG3's composite before and after its effects pass; late: what that pass drew over a pure pixel (wowbg3_post.frag)
    int ready, failed; VkDescriptorSetLayout mdsl, cdsl; VkPipelineLayout ml, cl; VkPipeline mp, cp; VkSampler smp;
    VkDescriptorPool pool; VkDescriptorSet mset[FOG_SETS], cset[FOG_SETS]; uint32_t mring, cring;
    struct { uint64_t img; VkImageView view; VkFramebuffer fb; uint32_t w, h; } t[8];
    uint64_t masks, cuts, refused;
    uint64_t cmp_img, cmp_at;  // the composite's image, and the present at which its effects pass was last copied before and after
} g_blm;
static int bl_imgs(DeviceData* d, VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (g_blm.wfed.img) return g_blm.w == w && g_blm.h == h;
    if (g_blm.fail) return 0;
    g_blm.fail = 1;
    if (!sf_img_make(d, &g_blm.wfed, VK_FORMAT_R32G32B32A32_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    if (!sf_img_make(d, &g_blm.nob, VK_FORMAT_R32_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    if (!sf_img_make(d, &g_blm.wdsp, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    if (!sf_img_make(d, &g_blm.cmp0, VK_FORMAT_B10G11R11_UFLOAT_PACK32, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    if (!sf_img_make(d, &g_blm.cmp1, VK_FORMAT_B10G11R11_UFLOAT_PACK32, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    if (!sf_img_make(d, &g_blm.late, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    g_blm.w = w; g_blm.h = h;
    VkClearColorValue z = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImage im[6] = {g_blm.wfed.img, g_blm.nob.img, g_blm.wdsp.img, g_blm.cmp0.img, g_blm.cmp1.img, g_blm.late.img};
    for (int i = 0; i < 6; i++) {
        sf_barrier(d, cb, im[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        d->CmdClearColorImage(cb, im[i], VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
        sf_barrier(d, cb, im[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    g_blm.fail = 0;
    logf_("wdbloom: images %ux%u", w, h);
    return 1;
}
static VkPipeline bl_pipe(DeviceData* d, VkPipelineLayout layout, const uint32_t* code, size_t size, VkColorComponentFlags wmask) {
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_surf_alb_vert, wowbg3_surf_alb_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    if (d->CreateShaderModule(d->device, &mv, NULL, &vs) != VK_SUCCESS || d->CreateShaderModule(d->device, &mf, NULL, &fs) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE};
    VkPipelineColorBlendAttachmentState cbs1 = {VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, wmask};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cbs1, {0, 0, 0, 0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, NULL, &cbs, &dy, layout, g_fbg.rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    if (d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}
static int bl_make(DeviceData* d) {
    if (g_blm.ready) return 1;
    if (g_blm.failed) return 0;
    g_blm.failed = 1;
    VkDescriptorSetLayoutBinding mb[6] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}, {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo mci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 6, mb};
    VkDescriptorSetLayoutBinding cb1 = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo cci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &cb1};
    if (d->CreateDescriptorSetLayout(d->device, &mci, NULL, &g_blm.mdsl) != VK_SUCCESS || d->CreateDescriptorSetLayout(d->device, &cci, NULL, &g_blm.cdsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo mli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_blm.mdsl, 1, &pr};
    VkPipelineLayoutCreateInfo cli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_blm.cdsl, 1, &pr};
    if (d->CreatePipelineLayout(d->device, &mli, NULL, &g_blm.ml) != VK_SUCCESS || d->CreatePipelineLayout(d->device, &cli, NULL, &g_blm.cl) != VK_SUCCESS) return 0;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    if (d->CreateSampler(d->device, &sci, NULL, &g_blm.smp) != VK_SUCCESS) return 0;
    g_blm.mp = bl_pipe(d, g_blm.ml, wowbg3_blmask_frag, sizeof wowbg3_blmask_frag, 0);  // writes nob only
    g_blm.cp = bl_pipe(d, g_blm.cl, wowbg3_blcut_frag, sizeof wowbg3_blcut_frag, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT);
    if (!g_blm.mp || !g_blm.cp) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * FOG_SETS}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * FOG_SETS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2 * FOG_SETS, 2, ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_blm.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l[FOG_SETS];
    for (int i = 0; i < FOG_SETS; i++) l[i] = g_blm.mdsl;
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_blm.pool, FOG_SETS, l};
    if (d->AllocateDescriptorSets(d->device, &sai, g_blm.mset) != VK_SUCCESS) return 0;
    for (int i = 0; i < FOG_SETS; i++) l[i] = g_blm.cdsl;
    if (d->AllocateDescriptorSets(d->device, &sai, g_blm.cset) != VK_SUCCESS) return 0;
    g_blm.ready = 1; g_blm.failed = 0;
    logf_("wdbloom: ready");
    return 1;
}
// A framebuffer of ours on one of BG3's B10G11R11 targets (the composite's, the prefilter's).
static int bl_target_l(DeviceData* d, uint64_t img, int* slot);
static int bl_target(DeviceData* d, uint64_t img, int* slot) {
    pthread_mutex_lock(&g_fbt_m); int r = bl_target_l(d, img, slot); pthread_mutex_unlock(&g_fbt_m); return r;
}
static int bl_target_l(DeviceData* d, uint64_t img, int* slot) {
    ImgInfo* ii = TABLE_FIND(g_img, IMG_CAP, img, 0);
    if (!ii || ii->fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return 0;
    *slot = -1;
    for (int i = 0; i < 8 && *slot < 0; i++) if (g_blm.t[i].img == img) *slot = i;
    if (*slot >= 0) return 1;
    for (int i = 0; i < 8 && *slot < 0; i++) if (!g_blm.t[i].img) *slot = i;
    if (*slot < 0) return 0;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (d->CreateImageView(d->device, &vci, NULL, &g_blm.t[*slot].view) != VK_SUCCESS) return 0;
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_fbg.rp, 1, &g_blm.t[*slot].view, ii->w, ii->hgt, 1};
    if (d->CreateFramebuffer(d->device, &fci, NULL, &g_blm.t[*slot].fb) != VK_SUCCESS) return 0;
    g_blm.t[*slot].img = img; g_blm.t[*slot].w = ii->w; g_blm.t[*slot].h = ii->hgt;
    logf_("wdbloom: target %llx %ux%u", (unsigned long long)img, ii->w, ii->hgt);
    return 1;
}
static void bl_run(DeviceData* d, VkCommandBuffer cb, int slot, VkImageLayout lay, VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet set, const uint32_t* pc) {
    uint32_t w = g_blm.t[slot].w, h = g_blm.t[slot].h;
    VkImage img = (VkImage)g_blm.t[slot].img;
    sf_barrier(d, cb, img, lay, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, g_fbg.rp, g_blm.t[slot].fb, {{0, 0}, {w, h}}, 0, NULL};
    d->CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    d->CmdSetViewport(cb, 0, 1, &vp);
    d->CmdSetScissor(cb, 0, 1, &sc);
    d->CmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    d->CmdDraw(cb, 3, 1, 0, 0);
    d->CmdEndRenderPass(cb);
    // BG3's state as it was (as fb_bare does): pipeline, sets, viewport and scissor.
    if (t_bind_cb == cb && t_pipe) {
        d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)t_pipe);
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        Recipe* rr = ri ? ri->r : NULL;
        PlInfo* pli = (rr && g_pl && rr->layout) ? TABLE_FIND(g_pl, PL_CAP, rr->layout, 0) : NULL;
        for (uint32_t k2 = 0; pli && k2 < pli->nsets && k2 < 8; k2++)
            if (t_set[k2]) { VkDescriptorSet ds = (VkDescriptorSet)t_set[k2]; d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)rr->layout, k2, 1, &ds, t_ndyn[k2], t_dyn[k2]); }
    }
    if (t_vs_cb == cb) {
        if (t_vs_have & 1) d->CmdSetViewport(cb, 0, 1, &t_vs_vp);
        if (t_vs_have & 2) d->CmdSetScissor(cb, 0, 1, &t_vs_sc);
    }
    sf_barrier(d, cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, lay, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}
// BG3's effects pass: it loads the composite's image with the depth and draws what comes after the composite (the aura
// of a character aiming, an ability's area, a spell's wisps and glow). The image is copied before and after the pass, in
// the pass's own command buffer, so the two copies are in order whatever BG3's threads record first; wdpost compares
// them (wowbg3_post.frag).
static __thread VkCommandBuffer t_late_cb; static __thread uint64_t t_late_img; static __thread int32_t t_late_fin;
static void late_pass_begin(DeviceData* d, VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const uint8_t* load, const int32_t* ini, const int32_t* fin) {
    t_late_cb = VK_NULL_HANDLE;
    if (!d || !g_cc.wdpost || !g_blm.ready || !g_blm.cmp0.img || !g_blm.cmp_img || !d->CmdCopyImage || !g_role.depth) return;
    int depth = 0, at = -1;
    for (uint32_t k = 0; k < n; k++) {
        if (imgs[k] == g_role.depth) depth = 1;
        else if (imgs[k] == g_blm.cmp_img) at = (int)k;
        else return;
    }
    if (!depth || at < 0 || load[at] != VK_ATTACHMENT_LOAD_OP_LOAD || ini[at] < 0 || fin[at] < 0) return;
    ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, imgs[at], 0);
    if (!i || i->fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || i->w != g_blm.w || i->hgt != g_blm.h) return;
    sf_copy(d, cb, (VkImage)imgs[at], (VkImageLayout)ini[at], &g_blm.cmp0, g_blm.w, g_blm.h);  // called with the trace lock held: no other lock here
    t_late_cb = cb; t_late_img = imgs[at]; t_late_fin = fin[at];
}
static void late_pass_end(DeviceData* d, VkCommandBuffer cb) {
    if (t_late_cb != cb) return;
    t_late_cb = VK_NULL_HANDLE;
    if (!d) return;
    pthread_mutex_lock(&g_fbt_m);
    if (g_blm.cmp1.img && t_late_img == g_blm.cmp_img) {
        sf_copy(d, cb, (VkImage)t_late_img, (VkImageLayout)t_late_fin, &g_blm.cmp1, g_blm.w, g_blm.h);
        __atomic_store_n(&g_blm.cmp_at, __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_fbt_m);
}
// After BG3's composite: which pixels are pure WoW (nob).
static void fb_bmask(DeviceData* d, VkCommandBuffer cb) {
    if (!(g_cc.wdbloom || g_cc.wdpost) || !g_fbg.ready || !g_blm.wfed.img || !bl_make(d)) return;  // patch46: wdpost reads it too
    VkImageView mv = wd_mask_view();
    int slot;
    if (!mv || !t_blm_b1 || t_blm_lay < 0 || t_blm_b1lay < 0 || (!g_cc.fxcal && (!g_blv_alb || !g_blv_surf)) || !bl_target(d, t_blm_img, &slot)) { g_blm.refused++; return; }
    VkDescriptorSet set = g_blm.mset[g_blm.mring++ % FOG_SETS];
    VkDescriptorImageInfo i0 = {VK_NULL_HANDLE, mv, VK_IMAGE_LAYOUT_GENERAL}, i1 = {VK_NULL_HANDLE, g_blm.wfed.view, VK_IMAGE_LAYOUT_GENERAL}, i2 = {VK_NULL_HANDLE, g_blm.nob.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo i3 = {g_blm.smp, (VkImageView)t_blm_b1, (VkImageLayout)t_blm_b1lay};
    VkDescriptorImageInfo i4 = {g_blm.smp, (VkImageView)(g_blv_alb ? g_blv_alb : t_blm_b1), (VkImageLayout)(g_blv_alb ? g_blv_lalb : t_blm_b1lay)};
    VkDescriptorImageInfo i5 = {g_blm.smp, (VkImageView)(g_blv_surf ? g_blv_surf : t_blm_b1), (VkImageLayout)(g_blv_surf ? g_blv_lsurf : t_blm_b1lay)};
    VkWriteDescriptorSet wr[6] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i0, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i1, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i2, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i3, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i4, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 5, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i5, NULL, NULL}};
    d->UpdateDescriptorSets(d->device, 6, wr, 0, NULL);
    VkImage im[2] = {g_blm.wfed.img, g_blm.nob.img};  // the feed's writes before; the last cut's reads before
    for (int i = 0; i < 2; i++)
        sf_barrier(d, cb, im[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    uint32_t pc[4] = {g_cc.fxcal ? 1u : 0u, 0, 0, 0};
    bl_run(d, cb, slot, (VkImageLayout)t_blm_lay, g_blm.mp, g_blm.ml, set, pc);
    g_blm.cmp_img = t_blm_img;  // the composite's image: BG3's effects pass draws into it next (late_pass_begin)
    if (g_blm.masks++ % 600 == 0) logf_("wdbloom: %llu masks, %llu cuts, %llu refused", (unsigned long long)g_blm.masks, (unsigned long long)g_blm.cuts, (unsigned long long)g_blm.refused);
}
// After BG3's bloom prefilter: 0 where its source pixels hold pure WoW.
static void fb_bcut(DeviceData* d, VkCommandBuffer cb) {
    if (!g_cc.wdbloom || !g_blm.ready || !g_blm.nob.img) return;
    int slot;
    if (t_blc_lay < 0 || !bl_target(d, t_blc_img, &slot)) { g_blm.refused++; return; }
    VkDescriptorSet set = g_blm.cset[g_blm.cring++ % FOG_SETS];
    VkDescriptorImageInfo i0 = {VK_NULL_HANDLE, g_blm.nob.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i0, NULL, NULL};
    d->UpdateDescriptorSets(d->device, 1, &wr, 0, NULL);
    sf_barrier(d, cb, g_blm.nob.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    uint32_t pc[4] = {g_blm.w / (g_blm.t[slot].w ? g_blm.t[slot].w : 1), g_blm.h / (g_blm.t[slot].h ? g_blm.t[slot].h : 1), g_blm.w, g_blm.h};
    if (pc[0] < 1) pc[0] = 1;
    if (pc[1] < 1) pc[1] = 1;
    bl_run(d, cb, slot, (VkImageLayout)t_blc_lay, g_blm.cp, g_blm.cl, set, pc);
    g_blm.cuts++;
}

// wdpost: WoW's bare pixels after BG3's TAA (patch 31).
static struct {
    struct { uint64_t img; SfImg s; uint32_t w, h; int init; uint64_t at; } sn[2];  // per fed image (the feed runs on each full-size target BG3 clears with depth)
    uint64_t fwd_img;  // the image the composite reads as its forward target
    uint64_t alb, surf, fwd; int32_t lalb, lsurf, lfwd;  // BG3's own views of its albedo, surface and forward target, newest
    int ready, failed; VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; VkSampler smp;
    VkDescriptorPool pool; VkDescriptorSet set[FOG_SETS]; uint32_t ring;
    struct { uint64_t img; VkImageView view; VkFramebuffer fb; } t[4];
    uint64_t passes, refused, snaps, nocomp, noalb;
} g_pst;
static __thread struct { uint64_t out, in; int32_t lout, lin; } t_pst;
// BG3's overlay passes between its TAA and its display transform, there only while something is aimed or highlighted:
// each reads the scene and writes it again with its overlay (an ability's area on the ground, an outline).
#define OVL_FS_A 0xf9ac3c52a928d4f4ull
#define OVL_FS_B 0x4571b5640092d8cbull
static __thread uint64_t t_ovl_in, t_ovl_out; static __thread int32_t t_ovl_lin, t_ovl_lout;
// patch46: no snapshot of the forward target any more (wdpost reads wdbloom's mask, made at the composite).
// At BG3's draws (fog_draw): the views wdpost needs, and the TAA pass to follow.
static void post_watch(VkCommandBuffer cb, Recipe* r) {
    uint64_t key = 0;
    for (uint32_t s = 0; r && s < r->nstages; s++) if (r->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) key = r->st[s].key;
    if (key == BARE_FS) {
        for (uint32_t s = 0; s < 4; s++) {
            SetInfo* si = t_set[s] ? set_find(t_set[s]) : NULL;
            for (uint32_t b = 0; si && b < DESC_SLOTS; b++) {
                if (si->slot[b].type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || !si->slot[b].res) continue;
                uint64_t im = view_image(si->slot[b].res);
                if (im && im == g_gb_alb) { g_pst.alb = si->slot[b].res; g_pst.lalb = si->slot[b].layout; }
                if (im && im == g_gb_surf) { g_pst.surf = si->slot[b].res; g_pst.lsurf = si->slot[b].layout; }
            }
        }
    } else if (key == FOG_FS) {
        SetInfo* s1 = set_find(t_set[1]);
        if (s1 && s1->slot[1].res) { g_pst.fwd = s1->slot[1].res; g_pst.lfwd = s1->slot[1].layout; g_pst.fwd_img = view_image(s1->slot[1].res); }
    } else if (key == OVL_FS_A || key == OVL_FS_B) {
        // One of BG3's overlay passes: its scene input (the bound image of the scene's kind and size) and its target.
        t_ovl_cb = VK_NULL_HANDLE;
        for (uint32_t s = 0; s < 4 && t_ovl_cb != cb; s++) {
            SetInfo* si = t_set[s] ? set_find(t_set[s]) : NULL;
            for (uint32_t b = 0; si && b < DESC_SLOTS; b++) {
                if ((si->slot[b].type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && si->slot[b].type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) || !si->slot[b].res) continue;
                uint64_t im = view_image(si->slot[b].res);
                ImgInfo* ii = im ? TABLE_FIND(g_img, IMG_CAP, im, 0) : NULL;
                if (!ii || ii->fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || ii->w != g_blm.w || ii->hgt != g_blm.h || im == t_pass_img0) continue;
                t_ovl_in = si->slot[b].res; t_ovl_lin = si->slot[b].layout; t_ovl_out = t_pass_img0; t_ovl_lout = t_pass_fin0; t_ovl_cb = cb;
                break;
            }
        }
    } else if (key == TAA_FS) {
        SetInfo* s1 = set_find(t_set[1]);
        t_pst.out = 0;
        for (uint32_t k = 0; k < t_pass_n && k < 8; k++) {
            ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, t_pass_imgs[k], 0);
            if (i && i->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32) { t_pst.out = t_pass_imgs[k]; t_pst.lout = t_pass_fins[k]; }
        }
        if (!s1 || !s1->slot[2].res || !t_pst.out) { static int said; if (!said++) logf_("wdpost: TAA drawn without its parts: set %llx input %llx target %llx", (unsigned long long)t_set[1], s1 ? (unsigned long long)s1->slot[2].res : 0ull, (unsigned long long)t_pst.out); return; }
        t_pst.in = s1->slot[2].res; t_pst.lin = s1->slot[2].layout;
        t_post_cb = cb;
    }
}
static int post_make(DeviceData* d) {
    if (g_pst.ready) return 1;
    if (g_pst.failed) return 0;
    g_pst.failed = 1;
    VkDescriptorSetLayoutBinding b[7] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    for (uint32_t k = 1; k < 6; k++) b[k] = (VkDescriptorSetLayoutBinding){k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    b[6] = (VkDescriptorSetLayoutBinding){6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};  // late
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 7, b};
    if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_pst.dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};  // patch45: wdpostdbg
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_pst.dsl, 1, &pcr};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_pst.layout) != VK_SUCCESS) return 0;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    if (d->CreateSampler(d->device, &sci, NULL, &g_pst.smp) != VK_SUCCESS) return 0;
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_surf_alb_vert, wowbg3_surf_alb_vert};
    VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_post_frag, wowbg3_post_frag};
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
    VkPipelineColorBlendAttachmentState cbs1 = {VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cbs1, {0, 0, 0, 0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, NULL, &cbs, &dy, g_pst.layout, g_fbg.rp, 0, VK_NULL_HANDLE, -1};
    if (d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &pci, NULL, &g_pst.pipe) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * FOG_SETS}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * FOG_SETS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, FOG_SETS, 2, ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_pst.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l[FOG_SETS];
    for (int i = 0; i < FOG_SETS; i++) l[i] = g_pst.dsl;
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_pst.pool, FOG_SETS, l};
    if (d->AllocateDescriptorSets(d->device, &sai, g_pst.set) != VK_SUCCESS) return 0;
    g_pst.ready = 1; g_pst.failed = 0;
    logf_("wdpost: ready");
    return 1;
}
static void fb_post_l(DeviceData* d, VkCommandBuffer cb);
static void fb_post(DeviceData* d, VkCommandBuffer cb) {
    if (!g_cc.wdpost || !g_fbg.ready || !post_make(d)) return;
    pthread_mutex_lock(&g_fbt_m); fb_post_l(d, cb); pthread_mutex_unlock(&g_fbt_m);
}
static void fb_post_l(DeviceData* d, VkCommandBuffer cb) {
    ImgInfo* ii = TABLE_FIND(g_img, IMG_CAP, t_pst.out, 0);
    // patch46: the pure-WoW mask wdbloom's pass made at this frame's composite (same size as the TAA output)
    if (!g_blm.ready || !g_blm.nob.img || !ii || t_pst.lout < 0 || !t_pst.in || ii->w != g_blm.w || ii->hgt != g_blm.h) {
        if (g_pst.refused++ % 600 == 0) logf_("wdpost: refused %llu (mask %d target %d input %d, mask %ux%u)", (unsigned long long)g_pst.refused,
            g_blm.nob.img != VK_NULL_HANDLE, ii != NULL, t_pst.in != 0, g_blm.w, g_blm.h);
        return;
    }
    int slot = -1;
    for (int i = 0; i < 4 && slot < 0; i++) if (g_pst.t[i].img == t_pst.out) slot = i;
    if (slot < 0) {
        for (int i = 0; i < 4 && slot < 0; i++) if (!g_pst.t[i].img) slot = i;
        if (slot < 0) { g_pst.refused++; return; }
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)t_pst.out, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
            {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (d->CreateImageView(d->device, &vci, NULL, &g_pst.t[slot].view) != VK_SUCCESS) return;
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_fbg.rp, 1, &g_pst.t[slot].view, ii->w, ii->hgt, 1};
        if (d->CreateFramebuffer(d->device, &fci, NULL, &g_pst.t[slot].fb) != VK_SUCCESS) return;
        g_pst.t[slot].img = t_pst.out;
        logf_("wdpost: TAA output %llx %ux%u layout %d, input view %llx layout %d, forward view %llx (image %llx) layout %d, albedo %d, surface %d", (unsigned long long)t_pst.out, ii->w, ii->hgt, (int)t_pst.lout,
              (unsigned long long)t_pst.in, (int)t_pst.lin, (unsigned long long)g_pst.fwd, (unsigned long long)g_pst.fwd_img, (int)g_pst.lfwd, (int)g_pst.lalb, (int)g_pst.lsurf);
    }
    VkDescriptorSet set = g_pst.set[g_pst.ring++ % FOG_SETS];
    VkDescriptorImageInfo i0 = {VK_NULL_HANDLE, g_blm.nob.view, VK_IMAGE_LAYOUT_GENERAL};  // patch46: binding 0 is the mask, 5 TAA's input; 1-4 unused
    VkDescriptorImageInfo im[5];
    for (int j = 0; j < 5; j++) im[j] = (VkDescriptorImageInfo){g_pst.smp, (VkImageView)t_pst.in, (VkImageLayout)t_pst.lin};
    im[0] = (VkDescriptorImageInfo){g_pst.smp, g_blm.wfed.view, VK_IMAGE_LAYOUT_GENERAL};  // binding 1: what the feed wrote
    if (g_pst.fwd) im[1] = (VkDescriptorImageInfo){g_pst.smp, (VkImageView)g_pst.fwd, (VkImageLayout)g_pst.lfwd};  // binding 2: BG3's forward target after its effects pass
    sf_barrier(d, cb, g_blm.wfed.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    sf_barrier(d, cb, g_blm.nob.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    // Bindings 3 and 4: BG3's composite before and after its effects pass, when both are this frame's.
    const uint64_t pres = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    const int cmp_ok = g_blm.cmp_img && pres - __atomic_load_n(&g_blm.cmp_at, __ATOMIC_RELAXED) <= 1;
    im[2] = (VkDescriptorImageInfo){g_pst.smp, g_blm.cmp0.view, VK_IMAGE_LAYOUT_GENERAL};
    im[3] = (VkDescriptorImageInfo){g_pst.smp, g_blm.cmp1.view, VK_IMAGE_LAYOUT_GENERAL};
    { static uint64_t said; if (said++ % 1800 == 0) logf_("wdpost: BG3's effects pass on the composite's image %llx copied %llu presents ago: what it drew over WoW's ground is %s", (unsigned long long)g_blm.cmp_img,
          (unsigned long long)(pres - g_blm.cmp_at), cmp_ok ? "kept" : "not known"); }
    VkDescriptorImageInfo i6 = {VK_NULL_HANDLE, g_blm.late.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[7] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i0, NULL, NULL}};
    for (uint32_t k = 0; k < 5; k++) wr[1 + k] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1 + k, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &im[k], NULL, NULL};
    wr[6] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i6, NULL, NULL};
    d->UpdateDescriptorSets(d->device, 7, wr, 0, NULL);
    VkImageLayout lay = (VkImageLayout)t_pst.lout;
    sf_barrier(d, cb, (VkImage)t_pst.out, lay, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, g_fbg.rp, g_pst.t[slot].fb, {{0, 0}, {ii->w, ii->hgt}}, 0, NULL};
    d->CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pst.pipe);
    d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pst.layout, 0, 1, &set, 0, NULL);
    { uint32_t dbg = (g_cc.wdpostdbg ? 1u : 0u) | (g_cc.wdtable || !g_pst.fwd ? 2u : 0u) | (cmp_ok ? 4u : 0u); d->CmdPushConstants(cb, g_pst.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &dbg); }
    VkViewport vp = {0, 0, (float)ii->w, (float)ii->hgt, 0, 1};
    VkRect2D sc = {{0, 0}, {ii->w, ii->hgt}};
    d->CmdSetViewport(cb, 0, 1, &vp);
    d->CmdSetScissor(cb, 0, 1, &sc);
    d->CmdDraw(cb, 3, 1, 0, 0);
    d->CmdEndRenderPass(cb);
    if (t_bind_cb == cb && t_pipe) {
        d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)t_pipe);
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        Recipe* rr = ri ? ri->r : NULL;
        PlInfo* pl = (rr && g_pl && rr->layout) ? TABLE_FIND(g_pl, PL_CAP, rr->layout, 0) : NULL;
        for (uint32_t k2 = 0; pl && k2 < pl->nsets && k2 < 8; k2++)
            if (t_set[k2]) { VkDescriptorSet ds = (VkDescriptorSet)t_set[k2]; d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)rr->layout, k2, 1, &ds, t_ndyn[k2], t_dyn[k2]); }
    }
    if (t_vs_cb == cb) {
        if (t_vs_have & 1) d->CmdSetViewport(cb, 0, 1, &t_vs_vp);
        if (t_vs_have & 2) d->CmdSetScissor(cb, 0, 1, &t_vs_sc);
    }
    sf_barrier(d, cb, (VkImage)t_pst.out, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, lay, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (g_pst.passes++ % 600 == 0) logf_("wdpost: %llu passes, %llu refused (on wdbloom's mask)", (unsigned long long)g_pst.passes, (unsigned long long)g_pst.refused);
}

// wdpost, the overlay passes: after one of them, what it changed at a pure pixel joins what BG3's effects pass drew there
// (late: BG3's share of the pixel a channel), so wdpostcas shows it over WoW's own colour.
static struct { int ready, failed; VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; VkDescriptorPool pool; VkDescriptorSet set[FOG_SETS]; uint32_t ring;
                struct { uint64_t img; VkImageView view; } t[8]; uint64_t runs, refused; } g_ovl;
static int ovl_make(DeviceData* d) {
    if (g_ovl.ready) return 1;
    if (g_ovl.failed) return 0;
    g_ovl.failed = 1;
    VkDescriptorSetLayoutBinding b[4] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, b};
    if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_ovl.dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_ovl.dsl, 0, NULL};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_ovl.layout) != VK_SUCCESS) return 0;
    VkShaderModule m; VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_ovl_comp, wowbg3_ovl_comp};
    if (d->CreateShaderModule(d->device, &mci, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL}, g_ovl.layout, VK_NULL_HANDLE, -1};
    VkResult r = d->CreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, NULL, &g_ovl.pipe);
    d->DestroyShaderModule(d->device, m, NULL);
    if (r != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * FOG_SETS}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * FOG_SETS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, FOG_SETS, 2, ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_ovl.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l[FOG_SETS];
    for (int i = 0; i < FOG_SETS; i++) l[i] = g_ovl.dsl;
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_ovl.pool, FOG_SETS, l};
    if (d->AllocateDescriptorSets(d->device, &sai, g_ovl.set) != VK_SUCCESS) return 0;
    g_ovl.ready = 1; g_ovl.failed = 0;
    return 1;
}
static void fb_ovl(DeviceData* d, VkCommandBuffer cb) {
    if (!g_cc.wdpost || !g_cc.wdpostcas || !g_blm.ready || !g_blm.late.img || !g_pst.ready || !t_ovl_in || !t_ovl_out || t_ovl_lout < 0 || !ovl_make(d)) return;
    pthread_mutex_lock(&g_fbt_m);
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++) if (g_ovl.t[i].img == t_ovl_out) slot = i;
    for (int i = 0; i < 8 && slot < 0; i++) if (!g_ovl.t[i].img) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)t_ovl_out, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
            {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (d->CreateImageView(d->device, &vci, NULL, &g_ovl.t[i].view) == VK_SUCCESS) { g_ovl.t[i].img = t_ovl_out; slot = i; }
        break;
    }
    if (slot < 0) { g_ovl.refused++; pthread_mutex_unlock(&g_fbt_m); return; }
    VkDescriptorSet set = g_ovl.set[g_ovl.ring++ % FOG_SETS];
    VkDescriptorImageInfo i0 = {VK_NULL_HANDLE, g_blm.nob.view, VK_IMAGE_LAYOUT_GENERAL}, i3 = {VK_NULL_HANDLE, g_blm.late.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo i1 = {g_pst.smp, (VkImageView)t_ovl_in, (VkImageLayout)t_ovl_lin}, i2 = {g_pst.smp, g_ovl.t[slot].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[4] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i0, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i1, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i2, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i3, NULL, NULL}};
    d->UpdateDescriptorSets(d->device, 4, wr, 0, NULL);
    const VkImageLayout lay = (VkImageLayout)t_ovl_lout;
    sf_barrier(d, cb, (VkImage)t_ovl_out, lay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_ovl.pipe);
    d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_ovl.layout, 0, 1, &set, 0, NULL);
    d->CmdDispatch(cb, (g_blm.w + 7) / 8, (g_blm.h + 7) / 8, 1);
    sf_barrier(d, cb, (VkImage)t_ovl_out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, lay, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    // BG3's compute state as it was, where it had one in this command buffer.
    if (t_cbind_cb == cb && t_cpipe) {
        d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)t_cpipe);
        for (uint32_t s = 0; s < 8 && t_clayout; s++)
            if (t_cset[s]) { VkDescriptorSet ds = (VkDescriptorSet)t_cset[s]; d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)t_clayout, s, 1, &ds, t_cndyn[s], t_cdyn[s]); }
    }
    if (g_ovl.runs++ % 1800 == 0) logf_("wdpost: %llu overlay passes of BG3's followed, %llu refused", (unsigned long long)g_ovl.runs, (unsigned long long)g_ovl.refused);
    pthread_mutex_unlock(&g_fbt_m);
}
// patch46: BG3 destroyed img (g_fbt_m held): wdpost's view of it and wdbloom's.
static void post_forget_image(DeviceData* d, uint64_t img) {
    for (int i = 0; i < 4; i++) if (g_pst.t[i].img == img) {
        if (g_pst.t[i].fb && d->DestroyFramebuffer) d->DestroyFramebuffer(d->device, g_pst.t[i].fb, NULL);
        if (g_pst.t[i].view && d->DestroyImageView) d->DestroyImageView(d->device, g_pst.t[i].view, NULL);
        g_pst.t[i].img = 0; g_pst.t[i].view = VK_NULL_HANDLE; g_pst.t[i].fb = VK_NULL_HANDLE;
        logf_("wdpost: BG3 destroyed its image %llx: our view of it forgotten", (unsigned long long)img);
    }
    for (int i = 0; i < 8; i++) if (g_ovl.t[i].img == img) {
        if (g_ovl.t[i].view && d->DestroyImageView) d->DestroyImageView(d->device, g_ovl.t[i].view, NULL);
        g_ovl.t[i].img = 0; g_ovl.t[i].view = VK_NULL_HANDLE;
    }
    for (int i = 0; i < 8; i++) if (g_blm.t[i].img == img) {
        if (g_blm.t[i].fb && d->DestroyFramebuffer) d->DestroyFramebuffer(d->device, g_blm.t[i].fb, NULL);
        if (g_blm.t[i].view && d->DestroyImageView) d->DestroyImageView(d->device, g_blm.t[i].view, NULL);
        g_blm.t[i].img = 0; g_blm.t[i].view = VK_NULL_HANDLE; g_blm.t[i].fb = VK_NULL_HANDLE;
        logf_("wdbloom: BG3 destroyed its image %llx: our view of it forgotten", (unsigned long long)img);
    }
}
// wdpostcas (patch 32): after BG3's CAS (compute e2ffaef8), its input over its output at wdpost's pixels.
#define CAS_CS 0xe2ffaef855eba087ull
static struct { int ready, failed; VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; VkDescriptorPool pool; VkDescriptorSet set[FOG_SETS]; uint32_t ring;
                uint64_t runs, refused; } g_pcas;
static int pcas_make(DeviceData* d) {
    if (g_pcas.ready) return 1;
    if (g_pcas.failed) return 0;
    g_pcas.failed = 1;
    VkDescriptorSetLayoutBinding b[7] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    for (uint32_t k = 1; k < 6; k++) b[k] = (VkDescriptorSetLayoutBinding){k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    b[6] = (VkDescriptorSetLayoutBinding){6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 7, b};
    if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_pcas.dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};  // wdpcdbg
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_pcas.dsl, 1, &pcr};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_pcas.layout) != VK_SUCCESS) return 0;
    VkShaderModule m; VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_postcas_comp, wowbg3_postcas_comp};
    if (d->CreateShaderModule(d->device, &mci, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL}, g_pcas.layout, VK_NULL_HANDLE, -1};
    VkResult r = d->CreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, NULL, &g_pcas.pipe);
    d->DestroyShaderModule(d->device, m, NULL);
    if (r != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * FOG_SETS}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * FOG_SETS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, FOG_SETS, 2, ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_pcas.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l[FOG_SETS];
    for (int i = 0; i < FOG_SETS; i++) l[i] = g_pcas.dsl;
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_pcas.pool, FOG_SETS, l};
    if (d->AllocateDescriptorSets(d->device, &sai, g_pcas.set) != VK_SUCCESS) return 0;
    g_pcas.ready = 1; g_pcas.failed = 0;
    logf_("wdpostcas: ready");
    return 1;
}
static void post_cas_l(DeviceData* d, VkCommandBuffer cb);
static void post_cas(DeviceData* d, VkCommandBuffer cb) {
    if (t_cbind_cb != cb || !t_cpipe || !t_cset[1] || !g_cpipe) return;
    CPipeInfo* p = TABLE_FIND(g_cpipe, CPIPE_CAP, t_cpipe, 0);
    if (!p || p->spv != CAS_CS) return;
    if (!g_cc.wdpost || g_cc.wdpostdbg || !g_fbg.ready || !pcas_make(d)) return;
    pthread_mutex_lock(&g_fbt_m); post_cas_l(d, cb); pthread_mutex_unlock(&g_fbt_m);
}
static void post_cas_l(DeviceData* d, VkCommandBuffer cb) {
    SetInfo* si = set_find(t_cset[1]);
    if (!si || !si->slot[1].res || !si->slot[2].res || !g_blm.ready || !g_blm.nob.img || !t_clayout) {
        if (g_pcas.refused++ % 600 == 0) logf_("wdpostcas: refused %llu (set %d input %d output %d mask %d layout %d)", (unsigned long long)g_pcas.refused,
            si != NULL, si && si->slot[1].res, si && si->slot[2].res, g_blm.nob.img != VK_NULL_HANDLE, t_clayout != 0);
        return;
    }
    uint64_t oimg = view_image(si->slot[2].res);
    ImgInfo* ii = oimg ? TABLE_FIND(g_img, IMG_CAP, oimg, 0) : NULL;
    if (!ii || ii->w != g_blm.w || ii->hgt != g_blm.h) { g_pcas.refused++; return; }
    if (g_pcas.runs == 0) logf_("wdpostcas: CAS input view %llx layout %d, output view %llx (image %llx f%d %ux%u) layout %d", (unsigned long long)si->slot[1].res, (int)si->slot[1].layout,
        (unsigned long long)si->slot[2].res, (unsigned long long)oimg, (int)ii->fmt, ii->w, ii->hgt, (int)si->slot[2].layout);
    VkDescriptorSet set = g_pcas.set[g_pcas.ring++ % FOG_SETS];
    VkDescriptorImageInfo i0 = {VK_NULL_HANDLE, g_blm.nob.view, VK_IMAGE_LAYOUT_GENERAL};  // patch46: the mask; 1-4 unused, 5 CAS's input
    VkDescriptorImageInfo im[5];
    for (int j = 0; j < 5; j++) im[j] = (VkDescriptorImageInfo){g_pst.smp, (VkImageView)si->slot[1].res, (VkImageLayout)si->slot[1].layout};
    im[0] = (VkDescriptorImageInfo){g_pst.smp, g_blm.wdsp.view, VK_IMAGE_LAYOUT_GENERAL};  // binding 1: WoW's own colour per pixel
    im[2] = (VkDescriptorImageInfo){g_pst.smp, g_blm.late.view, VK_IMAGE_LAYOUT_GENERAL};  // binding 3: what BG3 drew over a pure pixel after its composite
    VkDescriptorImageInfo i6 = {VK_NULL_HANDLE, (VkImageView)si->slot[2].res, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[7] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i0, NULL, NULL}};
    for (uint32_t j = 0; j < 5; j++) wr[1 + j] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1 + j, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &im[j], NULL, NULL};
    wr[6] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i6, NULL, NULL};
    d->UpdateDescriptorSets(d->device, 7, wr, 0, NULL);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pcas.pipe);
    d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pcas.layout, 0, 1, &set, 0, NULL);
    { uint32_t dbg = g_cc.wdpcdbg ? 1u : 0u; d->CmdPushConstants(cb, g_pcas.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &dbg); }
    d->CmdDispatch(cb, (ii->w + 7) / 8, (ii->hgt + 7) / 8, 1);
    VkMemoryBarrier mb2 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb2, 0, NULL, 0, NULL);
    // BG3's compute state as it was.
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)t_cpipe);
    for (uint32_t s = 0; s < 8; s++)
        if (t_cset[s]) { VkDescriptorSet ds = (VkDescriptorSet)t_cset[s]; d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)t_clayout, s, 1, &ds, t_cndyn[s], t_cdyn[s]); }
    if (g_pcas.runs++ % 600 == 0) logf_("wdpostcas: %llu runs, %llu refused", (unsigned long long)g_pcas.runs, (unsigned long long)g_pcas.refused);
}
// Dynamic offset of binding b of set s as BG3 bound it (dynamic offsets go in binding order).
static uint64_t fog_dyn(const SetInfo* si, uint32_t s, uint32_t b) {
    uint8_t ty = si->slot[b].type;
    if (ty != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && ty != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) return 0;
    uint32_t idx = 0;
    for (uint32_t k = 0; k < b; k++)
        if (si->slot[k].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || si->slot[k].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) idx++;
    return (s < 8 && idx < t_ndyn[s] && idx < 8) ? t_dyn[s][idx] : 0;
}
static void fog_draw(VkCommandBuffer cb) {
    if ((!g_cc.wdfog && !g_cc.wdbare && !g_cc.wdbloom && !g_cc.wdpost) || t_bind_cb != cb || !t_pipe || t_pass_cb != cb || !g_rec) return;
    RecInfo* ri = TABLE_FIND(g_rec, REC_CAP, t_pipe, 0);
    Recipe* r = ri ? ri->r : NULL;
    int is = 0;
    for (uint32_t s = 0; r && s < r->nstages; s++) if (r->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) is = r->st[s].key == FOG_FS ? 1 : r->st[s].key == CLD_FS ? 2 : is;
    if (g_cc.wdpost) post_watch(cb, r);
    if (g_cc.wdbare) for (uint32_t s = 0; r && s < r->nstages; s++) if (r->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT && r->st[s].key == BARE_FS) { bare_draw(cb); return; }
    if ((g_cc.wdbloom || g_cc.wdpost) && is == 1) {  // patch28: after the composite, the pure WoW pixels (patch46: also for wdpost)
        SetInfo* sb = set_find(t_set[1]);
        if (sb && sb->slot[1].res) { t_blm_b1 = sb->slot[1].res; t_blm_b1lay = sb->slot[1].layout; t_blm_img = t_pass_img0; t_blm_lay = t_pass_fin0; t_blm_cb = cb; }
    }
    if (!g_cc.wdfog) return;
    if (is == 2) { t_cld_cb = cb; t_cld_img = t_pass_img0; t_cld_lay = t_pass_fin0; return; }  // clear WoW's pixels again after BG3's clouds
    if (!is) return;
    SetInfo* s1 = set_find(t_set[1]);
    SetInfo* s0 = set_find(t_set[0]);
    static int said;
    if (!s1 || !s0) { if (!said++) logf_("wdfog: composite drawn with sets %llx %llx not known", (unsigned long long)t_set[0], (unsigned long long)t_set[1]); return; }
    t_fogd.b1 = s1->slot[1]; t_fogd.b2 = s1->slot[2]; t_fogd.b3 = s1->slot[3];
    t_fogd.dep = s1->slot[0]; t_fogd.frox = s1->slot[21]; t_fogd.samp = s1->slot[23]; t_fogd.u30 = s1->slot[30]; t_fogd.u4 = s0->slot[4];
    t_fogd.o30 = t_fogd.u30.off + fog_dyn(s1, 1, 30); t_fogd.o4 = t_fogd.u4.off + fog_dyn(s0, 0, 4);
    static uint64_t n;
    if (n++ % 120 == 0)
        logf_("wdfog: composite %llu (present %llu, fog passes %llu): depth view %llx (type %d layout %d) froxels %llx (type %d layout %d) sampler %llx (type %d) u30 %llx+%llu range %llu (type %d) u4 %llx+%llu (type %d); target %llx layout %d",
              (unsigned long long)n, (unsigned long long)__atomic_load_n(&g_req.presents, __ATOMIC_RELAXED), (unsigned long long)g_fbg.fogs, (unsigned long long)t_fogd.dep.res, t_fogd.dep.type, t_fogd.dep.layout, (unsigned long long)t_fogd.frox.res, t_fogd.frox.type, t_fogd.frox.layout,
              (unsigned long long)t_fogd.samp.samp, t_fogd.samp.type, (unsigned long long)t_fogd.u30.res, (unsigned long long)t_fogd.o30, (unsigned long long)t_fogd.u30.range, t_fogd.u30.type,
              (unsigned long long)t_fogd.u4.res, (unsigned long long)t_fogd.o4, t_fogd.u4.type, (unsigned long long)t_pass_img0, (int)t_pass_fin0);
    if ((n - 1) % 120 == 0) {
        uint64_t left = 0, l4 = 0;
        const float* v = (const float*)buf_cpu(t_fogd.u30.res, t_fogd.o30, &left);
        const uint32_t* q = (const uint32_t*)buf_cpu(t_fogd.u4.res, t_fogd.o4, &l4);
        if (v && left >= 528)
            logf_("wdfog: u30 f0-8 %g %g %g %g %g %g %g %g | 15,16 %g %g %g %g | 17 %g %g %g | 18 %g | 19 rows %g %g %g %g / %g %g %g %g / %g %g %g %g | 21-33 %g %g %g %g %g %g %g %g %g %g %g %g %g | u4 %u %u %u %u %u %u %u",
                  v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[84], v[85], v[86], v[87], v[88], v[89], v[90], v[91],
                  v[92], v[93], v[94], v[95], v[96], v[97], v[98], v[99], v[100], v[101], v[102], v[103],
                  v[120], v[121], v[122], v[123], v[124], v[125], v[126], v[127], v[128], v[129], v[130], v[131], v[132],
                  q && l4 >= 48 ? q[0] : 0, q && l4 >= 48 ? q[1] : 0, q && l4 >= 48 ? q[8] : 0, q && l4 >= 48 ? q[9] : 0, q && l4 >= 48 ? q[10] : 0, q && l4 >= 48 ? q[11] : 0, q && l4 >= 48 ? q[12] : 0);
        else logf_("wdfog: u30 not readable on the CPU (left %llu)", (unsigned long long)left);
    }
    t_fog_cb = cb; t_fog_img = t_pass_img0; t_fog_lay = t_pass_fin0;
}
// BG3's HDR image, left in `lay`: WoW's picture into it, back in `lay`. Outside any pass. BG3's dynamic state is
// rebound by BG3 at its next pass (it binds per pass; the ground injection sets the same rule).
static void fb_draw(DeviceData* d, VkCommandBuffer cb, uint64_t img, VkImageLayout lay, int mode) {
    const int fog = mode == 1;  // 0 the feed (with wdfog: the clear), 1 the fog pass after the composite, 2 the clear after BG3's clouds
    const uint64_t present = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    if ((g_cc.wdepth && !g_cc.wdfeed) || !g_cc.fxbg || !g_sf.ready || !fb_make(d)) return;
    if (fog && !fog_make(d)) return;
    // The measured table, read again when its file changes (log2 x for a displayed colour; capture/fxbg-table.bin).
    struct stat tst;
    if (present % 30 == 0 && stat(outp("/fxbg-table.bin"), &tst) == 0 && tst.st_mtim.tv_sec != g_fbg.tseen.tv_sec) {
        FILE* tf = fopen(outp("/fxbg-table.bin"), "rb");
        if (tf) { size_t got = fread(g_fbg.tmap, sizeof(float), FB_TAB, tf); fclose(tf); g_fbg.tseen = tst.st_mtim; logf_("fxbg: table read, %zu values", got); }
    }
    pthread_mutex_lock(&g_sf.m);
    if (g_fbg.fedp[mode] == present + 1 && g_fbg.fedi[mode] == img) { pthread_mutex_unlock(&g_sf.m); return; }
    g_fbg.fedp[mode] = present + 1; g_fbg.fedi[mode] = img;
    int k = g_sf.wc_latest;
    __typeof__(g_sf.wcm[0]) w0;
    VkBuffer wb = VK_NULL_HANDLE;
    if (k >= 0) { w0 = g_sf.wcm[k]; wb = g_sf.wc[k].buf; }
    pthread_mutex_unlock(&g_sf.m);
    if (g_cc.wdlock && g_lk.have) { k = 0; w0 = g_lk.m; wb = g_lk.b.buf; }
    if (!wb) { g_fbg.nowow++; return; }
    PaceCam cn;
    int have = 0;
    pthread_mutex_lock(&g_pc.m);
    if (g_pc.n) { cn = g_pc.r[(g_pc.head + PC_N - 1) % PC_N]; have = 1; }
    pthread_mutex_unlock(&g_pc.m);
    uint8_t crec[96];
    float zero[3] = {0, 0, 0}, eN[3], fN[3], rN[3], uN[3], txN, tyN, eK[3], fK[3], rK[3], uK[3], txK, tyK;
    if (!have || !sf_cam(cn.rec, zero, eN, fN, rN, uN, &txN, &tyN)) { g_fbg.refused++; return; }
    if (g_cc.wdlock && g_lk.have) {  // the frame drawn from this very camera: no warp
        memset(eK, 0, sizeof eK); memcpy(fK, fN, 12); memcpy(rK, rN, 12); memcpy(uK, uN, 12); txK = txN; tyK = tyN;
    } else if (!sf_cam_rec(w0.cam, crec) || !sf_cam(crec, eN, eK, fK, rK, uK, &txK, &tyK)) { g_fbg.refused++; return; }
    ImgInfo* ii = TABLE_FIND(g_img, IMG_CAP, img, 0);
    if (!ii || ii->fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || lay < 0) return;
    if (!fog_st(d, cb, ii->w, ii->hgt)) { g_fbg.refused++; return; }
    if (!bl_imgs(d, cb, ii->w, ii->hgt)) { g_fbg.refused++; return; }  // patch28: wfed
    int slot = -1;
    pthread_mutex_lock(&g_fbt_m);
    for (int i = 0; i < FB_IMGS && slot < 0; i++) if (g_fbg.t[i].img == img) slot = i;
    if (slot < 0) {
        for (int i = 0; i < FB_IMGS && slot < 0; i++) if (!g_fbg.t[i].img) slot = i;
        if (slot < 0) { slot = (int)(present % FB_IMGS); }
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
            {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (d->CreateImageView(d->device, &vci, NULL, &g_fbg.t[slot].view) != VK_SUCCESS) { g_fbg.t[slot].img = 0; pthread_mutex_unlock(&g_fbt_m); return; }
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_fbg.rp, 1, &g_fbg.t[slot].view, ii->w, ii->hgt, 1};
        if (d->CreateFramebuffer(d->device, &fci, NULL, &g_fbg.t[slot].fb) != VK_SUCCESS) { g_fbg.t[slot].img = 0; pthread_mutex_unlock(&g_fbt_m); return; }
        g_fbg.t[slot].img = img; g_fb_dead[slot] = 0;
        logf_("fxbg: target %llx %ux%u (layout %d)", (unsigned long long)img, ii->w, ii->hgt, (int)lay);
    }
    VkFramebuffer slot_fb = g_fbg.t[slot].fb;
    fb_stale_note(0, slot);
    pthread_mutex_unlock(&g_fbt_m);
    if (!g_fbg.ebuf[0] || !g_fbg.ebuf[1]) { g_fbg.refused++; return; }
    const int wdx_fix = mode == 0 && g_cc.wdevref > 0.0f && !g_cc.fxcal && g_wdx.cur_ok;  // patch61: binding 1 = BG3's newest eye state
    const uint64_t eb[2] = {wdx_fix ? (uint64_t)g_wdx.cur.buf : g_fbg.ebuf[0], g_fbg.ebuf[1]};
    for (int e = 0; e < 2; e++) if (g_fbg.esetbuf[k][e] != (VkBuffer)eb[e]) {
        VkDescriptorBufferInfo bi = {(VkBuffer)eb[e], 0, 16};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_fbg.set[k], (uint32_t)e + 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &bi, NULL};
        d->UpdateDescriptorSets(d->device, 1, &wr, 0, NULL);
        g_fbg.esetbuf[k][e] = (VkBuffer)eb[e];
    }
    {
        static VkImageView seen[2];
        VkImageView mv = wd_mask_view();
        if (!mv) { g_fbg.refused++; return; }
        if (seen[k] != mv) {
            VkDescriptorImageInfo ii2 = {VK_NULL_HANDLE, mv, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_fbg.set[k], 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii2, NULL, NULL};
            d->UpdateDescriptorSets(d->device, 1, &wr, 0, NULL);
            seen[k] = mv;
        }
        static VkImageView seen6[2];
        if (seen6[k] != g_blm.wfed.view) {
            VkDescriptorImageInfo i6 = {VK_NULL_HANDLE, g_blm.wfed.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr6 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_fbg.set[k], 6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i6, NULL, NULL};
            d->UpdateDescriptorSets(d->device, 1, &wr6, 0, NULL);
            seen6[k] = g_blm.wfed.view;
        }
        // Binding 8: BG3's G-buffer albedo (its own view, as its lighting pass bound it), for WoW's ambient on BG3's
        // pixels; until that pass has been seen, an image of our own, and no ambient.
        // Written at every feed: BG3 rebuilds its G-buffer during a session and a new view can get an old one's handle,
        // so "the same handle as last time" does not mean the set still points at a live view (a play session lost
        // the ambient on its characters that way).
        const VkImageView av = g_blv_alb ? (VkImageView)g_blv_alb : g_blm.wdsp.view;
        {
            VkDescriptorImageInfo i8 = {g_fbg.asmp, av, g_blv_alb ? (VkImageLayout)g_blv_lalb : VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr8 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_fbg.set[k], 8, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i8, NULL, NULL};
            d->UpdateDescriptorSets(d->device, 1, &wr8, 0, NULL);
        }
        const VkImageView zv = g_blv_z ? (VkImageView)g_blv_z : g_blm.wdsp.view;  // binding 9: BG3's G-buffer eye depth, for WoW's fog on BG3's pixels
        {
            VkDescriptorImageInfo i9 = {g_fbg.asmp, zv, g_blv_z ? (VkImageLayout)g_blv_lz : VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr9 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_fbg.set[k], 9, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i9, NULL, NULL};
            d->UpdateDescriptorSets(d->device, 1, &wr9, 0, NULL);
        }
        static VkImageView seen7[2];
        if (seen7[k] != g_blm.wdsp.view) {
            VkDescriptorImageInfo i7 = {VK_NULL_HANDLE, g_blm.wdsp.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr7 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_fbg.set[k], 7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i7, NULL, NULL};
            d->UpdateDescriptorSets(d->device, 1, &wr7, 0, NULL);
            seen7[k] = g_blm.wdsp.view;
        }
        static VkImageView seen5[2];
        if (seen5[k] != g_fog.st.view) {
            VkDescriptorImageInfo i5 = {VK_NULL_HANDLE, g_fog.st.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_fbg.set[k], 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i5, NULL, NULL};
            d->UpdateDescriptorSets(d->device, 1, &wr, 0, NULL);
            seen5[k] = g_fog.st.view;
        }
    }
    if (g_fbg.setbuf[k] != wb) {
        VkDescriptorBufferInfo bi = {wb, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_fbg.set[k], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
        d->UpdateDescriptorSets(d->device, 1, &wr, 0, NULL);
        g_fbg.setbuf[k] = wb;
    }
    if (g_cc.wdlog && d->CmdCopyBuffer) {
        static GBuf rd; static uint64_t n;
        if (gbuf_make(d, &rd, 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) {
            const float* m = (const float*)rd.map;
            if (++n % 30 == 0) logf_("wdlog: %llu e0 %g %g %g %g e1 %g %g %g %g", (unsigned long long)present, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
            VkBufferCopy r0 = {g_fbg.eoff[0], 0, 16}, r1 = {g_fbg.eoff[1], 16, 16};
            d->CmdCopyBuffer(cb, (VkBuffer)g_fbg.ebuf[0], rd.buf, 1, &r0);
            d->CmdCopyBuffer(cb, (VkBuffer)g_fbg.ebuf[1], rd.buf, 1, &r1);
        }
    }
    // patch60: the wdexp hold moved to right after BG3's adaptation (wdx_dispatch_after), all three values.
    VkDescriptorSet fset = VK_NULL_HANDLE;
    if (fog) {
        const DescSlot *dp = &t_fogd.dep, *fx = &t_fogd.frox, *sm = &t_fogd.samp, *u30 = &t_fogd.u30, *u4 = &t_fogd.u4;
        int ubo30 = u30->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || u30->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        int ubo4 = u4->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || u4->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        if (dp->type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || fx->type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sm->type != VK_DESCRIPTOR_TYPE_SAMPLER || !ubo30 || !ubo4
            || !dp->res || !fx->res || !sm->samp || !u30->res || !u4->res || ii->fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
            if (!g_fog.refused++) logf_("wdfog: refused: descriptor types %d %d %d %d %d, target format %d", dp->type, fx->type, sm->type, u30->type, u4->type, (int)ii->fmt);
            return;
        }
        fset = g_fog.set[g_fog.ring++ % FOG_SETS];
        VkDescriptorBufferInfo b0 = {wb, 0, VK_WHOLE_SIZE}, b1 = {(VkBuffer)g_fbg.ebuf[0], 0, 16}, b2 = {(VkBuffer)g_fbg.ebuf[1], 0, 16}, b3 = {g_fbg.tbuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo b8 = {(VkBuffer)u30->res, t_fogd.o30, u30->range}, b9 = {(VkBuffer)u4->res, t_fogd.o4, u4->range};
        VkDescriptorImageInfo i4 = {VK_NULL_HANDLE, wd_mask_view(), VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i5 = {VK_NULL_HANDLE, (VkImageView)dp->res, (VkImageLayout)dp->layout}, i6 = {VK_NULL_HANDLE, (VkImageView)fx->res, (VkImageLayout)fx->layout};
        VkDescriptorImageInfo i7 = {(VkSampler)sm->samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkDescriptorImageInfo i10 = {VK_NULL_HANDLE, (VkImageView)t_fogd.b1.res, (VkImageLayout)t_fogd.b1.layout}, i11 = {VK_NULL_HANDLE, (VkImageView)t_fogd.b2.res, (VkImageLayout)t_fogd.b2.layout},
                              i12 = {VK_NULL_HANDLE, (VkImageView)t_fogd.b3.res, (VkImageLayout)t_fogd.b3.layout};
        if (t_fogd.b1.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t_fogd.b2.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t_fogd.b3.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            if (!g_fog.refused++) logf_("wdfog: refused: b1-b3 types %d %d %d", t_fogd.b1.type, t_fogd.b2.type, t_fogd.b3.type);
            return;
        }
        if (!i4.imageView) { g_fbg.refused++; return; }
        VkWriteDescriptorSet wr[13] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 10, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &i10, NULL, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 11, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &i11, NULL, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 12, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &i12, NULL, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &b0, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &b1, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &b2, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &b3, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &i4, NULL, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &i5, NULL, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 6, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &i6, NULL, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 7, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &i7, NULL, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 8, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &b8, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, fset, 9, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &b9, NULL}};
        d->UpdateDescriptorSets(d->device, 13, wr, 0, NULL);
    }
    float pc[48];
    memset(pc, 0, sizeof pc);
    if (!fog && g_blv_alb && amb_get(pc + 40)) pc[43] = 1.0f;  // WoW's ambient, times the dial (amb_present)
    if (!fog && g_blv_z) wfog_get(pc + 44);
    if (!fog && g_fbg.tmap) wlit_put(g_fbg.tmap + FB_TAB, g_blv_z && pc[43] > 0.5f, eN, fN, rN, uN);  // WoW's fog: start and end (yards of eye depth), 1 when there is one, and its colour packed
    memcpy(pc + 0, fN, 12); pc[3] = txN; memcpy(pc + 4, rN, 12); pc[7] = tyN; memcpy(pc + 8, uN, 12); pc[11] = 1.0f / (float)ii->w;
    memcpy(pc + 12, eK, 12); pc[15] = txK; memcpy(pc + 16, fK, 12); pc[19] = tyK; memcpy(pc + 20, rK, 12); pc[23] = 1.0f / (float)ii->hgt; memcpy(pc + 24, uK, 12);
    uint32_t* u = (uint32_t*)(pc + 28);
    u[0] = w0.ww; u[1] = w0.wh; u[2] = w0.cbpr; u[3] = w0.dbpr; u[4] = w0.code; u[5] = ((w0.code == 2 && !g_cc.wowraw) ? 1u : 0u) | (g_cc.fxcal ? 2u | (uint32_t)(present / 10) << 8 : 0u);
    if (g_cc.wdfog && !fog) u[5] |= g_cc.wdfogst ? 64u : 4u;
    if (g_cc.wdfog && fog && g_cc.wdfogst) u[5] |= 64u;
    if (fog && g_cc.wdfogdbg) u[5] |= (uint32_t)g_cc.wdfogdbg << 4;  // 16: W off, 32: S out, 48/64/80: b2/b3/b1  // the feed only clears BG3's forward target at WoW's pixels
    if (wdx_fix) { pc[39] = 1.2f * exp2f(g_cc.wdevref); u[5] |= 8u; }  // patch61
    pc[34] = w0.wnear; pc[35] = (w0.wzoff > -100.0f && w0.wzoff < 100.0f) ? w0.wzoff : 0.0f; pc[36] = g_cc.wgamma;
    if (g_cc.wdjit && !fog) {
        int b = -1;  // the newest G-buffer camera recorded: this frame's (its G-buffer pass comes before the forward pass)
        for (int j = 0; j < 8; j++) if (g_jit[j].p && (b < 0 || g_jit[j].p > g_jit[b].p)) b = j;
        if (b >= 0) { pc[37] = g_jit[b].j[0]; pc[38] = g_jit[b].j[1]; u[5] |= 128u; g_jit_used++; g_jit_last[0] = pc[37]; g_jit_last[1] = pc[38];
                      int dp = (int)((int64_t)present - (int64_t)g_jit[b].p) + 2; if (dp < 0) dp = 0; if (dp > 4) dp = 4; g_jit_dp[dp]++; } else g_jit_missing++;
        if ((g_jit_used + g_jit_missing) % 600 == 1) logf_("wdjit: %llu feeds with the newest jitter, %llu without; feed present minus jitter present -2..2+: %llu %llu %llu %llu %llu; last %.6f %.6f (NDC), %.3f %.3f px",
            (unsigned long long)g_jit_used, (unsigned long long)g_jit_missing, (unsigned long long)g_jit_dp[0], (unsigned long long)g_jit_dp[1], (unsigned long long)g_jit_dp[2], (unsigned long long)g_jit_dp[3], (unsigned long long)g_jit_dp[4],
            g_jit_last[0], g_jit_last[1], g_jit_last[0] * 0.5f * (float)ii->w, g_jit_last[1] * 0.5f * (float)ii->hgt);
    }
    sf_barrier(d, cb, (VkImage)img, lay, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    sf_barrier(d, cb, g_blm.wfed.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);  // patch28: after the last mask read it
    const int tost = fog && g_cc.wdfogst;
    if (tost && !fog_st_pass(d)) { g_fbg.refused++; return; }
    if (tost) sf_barrier(d, cb, g_fog.st.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, tost ? g_fog.rpst : g_fbg.rp, tost ? g_fog.fbst : slot_fb, {{0, 0}, {ii->w, ii->hgt}}, 0, NULL};
    d->CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    uint32_t eoffs[2] = {wdx_fix ? 0u : (uint32_t)g_fbg.eoff[0], (uint32_t)g_fbg.eoff[1]};
    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fog ? (tost ? g_fog.pipest : g_fog.pipe) : g_fbg.pipe);
    d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fog ? g_fog.layout : g_fbg.layout, 0, 1, fog ? &fset : &g_fbg.set[k], 2, eoffs);
    if (fog) { float k = (g_cc.fxcal || g_cc.wdfogdbg >= 2) ? 0.0f : 1.0f; float bc[4] = {k, k, k, 1.0f}; d->CmdSetBlendConstants(cb, bc); }
    VkViewport vp = {0, 0, (float)ii->w, (float)ii->hgt, 0, 1};
    VkRect2D sc = {{0, 0}, {ii->w, ii->hgt}};
    d->CmdSetViewport(cb, 0, 1, &vp);
    d->CmdSetScissor(cb, 0, 1, &sc);
    d->CmdPushConstants(cb, fog ? g_fog.layout : g_fbg.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, FB_PC, pc);
    d->CmdDraw(cb, 3, 1, 0, 0);
    d->CmdEndRenderPass(cb);
    if (tost) sf_barrier(d, cb, g_fog.st.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    // BG3's state as it was, as ground_inject does, and its descriptor sets (this pass binds its own).
    if (t_bind_cb == cb && t_pipe) {
        d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)t_pipe);
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        Recipe* rr = ri ? ri->r : NULL;
        PlInfo* pl = (rr && g_pl && rr->layout) ? TABLE_FIND(g_pl, PL_CAP, rr->layout, 0) : NULL;
        for (uint32_t k2 = 0; pl && k2 < pl->nsets && k2 < 8; k2++)
            if (t_set[k2]) { VkDescriptorSet ds = (VkDescriptorSet)t_set[k2]; d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)rr->layout, k2, 1, &ds, t_ndyn[k2], t_dyn[k2]); }
    }
    if (t_vs_cb == cb) {
        if (t_vs_have & 1) d->CmdSetViewport(cb, 0, 1, &t_vs_vp);
        if (t_vs_have & 2) d->CmdSetScissor(cb, 0, 1, &t_vs_sc);
    }
    sf_barrier(d, cb, (VkImage)img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, lay, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (fog) { if (g_fbg.fogs++ % 600 == 0) logf_("wdfog: %llu fog passes", (unsigned long long)g_fbg.fogs); return; }
    g_fbg.feeds++;
    if (g_fbg.feeds % 600 == 1) logf_("fxbg: %llu feeds, %llu refused, %llu without a WoW frame", (unsigned long long)g_fbg.feeds, (unsigned long long)g_fbg.refused, (unsigned long long)g_fbg.nowow);
}

static void fb_feed(DeviceData* d, VkCommandBuffer cb, uint64_t img, VkImageLayout lay) { fb_draw(d, cb, img, lay, 0); }
static void fb_fog(DeviceData* d, VkCommandBuffer cb) { fb_draw(d, cb, t_fog_img, (VkImageLayout)t_fog_lay, 1); }
static void fb_clr(DeviceData* d, VkCommandBuffer cb) { if (g_cc.wdfog) fb_draw(d, cb, t_cld_img, (VkImageLayout)t_cld_lay, 2); }
// BG3's exposure this frame: the two buffers its exposure pass reads (g_fxl_lock held). The feed reads them on the GPU,
// so WoW's picture keeps its own brightness while BG3's eye adapts for its effects as it does in BG3.
static void fb_exposure(const DescSlot* a, uint64_t o0, const DescSlot* b, uint64_t o1) {
    static int said;
    if (!said) { said = 1; logf_("fxbg: exposure buffers type %d %d offsets %llu %llu", (int)a->type, (int)b->type, (unsigned long long)o0, (unsigned long long)o1); }
    g_fbg.ebuf[0] = a->res; g_fbg.eoff[0] = o0; g_fbg.ebuf[1] = b->res; g_fbg.eoff[1] = o1;
}
static void fb_copy_hook(DeviceData* d, VkCommandBuffer cb, uint64_t s, uint64_t t, VkImageLayout sl) { (void)d; (void)cb; (void)s; (void)t; (void)sl; }
