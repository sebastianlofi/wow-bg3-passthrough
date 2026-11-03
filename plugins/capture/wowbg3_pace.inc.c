// ---------------------------------------------------------------- frame pacing
// A test on the desktop (BG3 fullscreen 3440x1440 at 60): Tav looked lower frame
// rate than the world and the camera staggered. benilla drew on its own 60 Hz clock with the
// newest camera record, Tav blended one BG3 frame behind on BG3's clock, and the compositor took
// the newest WoW frame at each BG3 present: three clocks. Here:
//  - Frame packets: every present tav_write_pose appends the plugin's camera record read at that
//    present (TVC1 trailer, pace_trailer), so benilla can draw Tav and the world from the same BG3
//    state; benilla 0013 draws once per packet and tags its WBF1 frame with the packet and the
//    camera seq (bytes 36..56, see wr_main).
//  - The presented frame's camera: BG3 renders camera-relative and keeps the eye in its view
//    constants (set 0, binding 1, +96). The G-buffer pass's copy (pace_bind) of the frame whose
//    final pass was submitted last before the present (pace_submit), mapped to WoW with world
//    mode's mapping, is matched against the camera records seen (pace_match). When the camera
//    stands still the match is ambiguous and the usual depth (newest record minus matched) is used.
//  - Tav's pose for the packet is read at that G-buffer pass too (pace_pose_snapshot; read at the
//    present, 1% of poses already held the next BG3 tick), and the packet's camera record is the
//    one whose eye the frame has (pace_trailer), so pose and camera are the presented frame's.
//  - Compose takes the WoW frame drawn for that camera, `lag` cameras back when WoW is not that
//    quick (raised after 3 misses in 240 presents, lowered after 600 presents in which the frame
//    for one camera later was always there in time), waiting up to `wait=<ms>` for it (compose
//    words: wait=<ms> default 1 (formerly 3), lag=<n> fixes the lag, newest = the newest
//    WoW frame as before).
//  - The camera seq asked for is held to exactly one record per present by the want clock
//    (pace_flow), because `T - lag` itself is noisy by one record: BG3's camera records do not
//    always arrive one per present, and the match flicking between sure and unsure moves `T` as
//    well. Both made the WoW picture hold for one frame and then jump two records several times a
//    second, which is seen as the world vibrating under the party while it walks. Compose word
//    "noflow" takes the clock out and asks for `T - lag` at each present, as it did before.
//  - `pace:` lines every 300 presents (present intervals, WoW frames per present, lag, depth, waits,
//    frame ages, the want clock); request "1 1 pace<N>" writes the next N presents to OUT_DIR/pace.csv.
// Included by wowbg3_capture.c after everything it uses.

#define PACE_EVERY 300
#define PACE_CBS 32
typedef struct { uint64_t cb; float eye[3], root[3]; int root_ok; uint64_t t; int snap; uint32_t snap_gen; } PaceSample;
// Tav's pose records taken at a G-buffer pass (seqlock: gen_a set before writing, gen_b after).
#define PACE_SNAPS 4
static struct { volatile uint32_t gen_a, gen_b; uint32_t plan; int n; size_t len, cap; uint8_t* buf; } g_snap[PACE_SNAPS];
static uint32_t g_snap_next, g_snap_gen;  // under g_pace.m
static uint64_t g_snap_used, g_snap_missed;  // present thread, for the log
static struct {
    pthread_mutex_t m;
    PaceSample cbs[PACE_CBS];  // G-buffer samples by command buffer, until submitted
    uint32_t next;
    PaceSample pending;        // the last submitted G-buffer pass's sample
    PaceSample framed;         // ... as of the last submitted final pass: the frame presented next
    int pending_ok, framed_ok;
    uint64_t samples;
    // present-time camera record (one read per present, shared with the packet trailer)
    uint64_t cam_present;      // g_req.presents it was read at
    uint8_t cam[96];
    int cam_ok;
    // pacing state
    int lag;                   // cameras behind the presented frame's
    int depth_est;             // newest record minus the presented frame's, usually
    uint32_t depth_hist[4];    // confident matches by depth (0..3), decayed
    uint32_t win_n, win_miss, dwin_n, dwin_early;
    // the want clock: the camera seq asked for, one record per present (pace_flow)
    uint32_t flow;             // 0: not running (no target yet, or the word "noflow")
    uint64_t flow_present;     // g_req.presents it was last advanced at
    uint64_t flow_steps, flow_pull, flow_snap;  // for the log
    uint64_t pickd[3];         // the composited frame's camera against want: older / exact / newer
    // this present
    uint32_t L, M, T, want;
    int conf;
    float err, err2;
    uint64_t waited_ns;
    int pre, post, early;
    PaceSample cur;            // framed sample used this present
    int cur_ok;
    // stats since the last log
    uint64_t n, t_first, t_last, iv[PACE_EVERY];
    uint32_t niv;
    uint64_t dseq[4], dcam[4], lagh[5], deph[4], nconf, waits, wait_ns, misses, used, noframe;
    uint64_t age_n, age_sum, age_max, wage_n;
    double wage_sum, wage_max;
    uint64_t ticks0;
    uint32_t prev_seq, prev_cam;
    // CSV (request "pace<N>")
    FILE* csv;
    int csv_left;
} g_pace = {.m = PTHREAD_MUTEX_INITIALIZER, .lag = 1, .depth_est = 1};

static __thread VkCommandBuffer t_pace_gcb;  // the G-buffer pass is being recorded into this
static __thread int t_pace_done;

// role_pass found BG3's G-buffer pass on `cb` (g_trace_lock held).
static void pace_gbuffer(VkCommandBuffer cb) {
    t_pace_gcb = cb;
    t_pace_done = 0;
}

// Set 0 binding 1 (BG3's view constants) as bound on this thread now, NULL when not mapped.
static const uint8_t* pace_view_constants(uint32_t need) {
    if (!t_set[0]) return NULL;
    SetInfo* si = set_find(t_set[0]);
    if (!si) return NULL;
    DescSlot* sl = &si->slot[1];
    if (!sl->res) return NULL;
    uint64_t off = sl->off, left = 0;
    if (sl->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || sl->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
        uint32_t at = 0;
        for (uint32_t b = 0; b < 1; b++)
            if (si->slot[b].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || si->slot[b].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) at++;
        if (at < t_ndyn[0] && at < 8) off += t_dyn[0][at];
    }
    const uint8_t* p = buf_cpu(sl->res, off, &left);
    return p && left >= need ? p : NULL;
}

// A graphics descriptor bind on `cb` (after the layer's own bookkeeping): the first one with the
// view constants in the G-buffer pass gives this frame's eye, with Tav's root read at the same time.
static void pace_bind(VkCommandBuffer cb) {
    if (cb != t_pace_gcb || t_pace_done || !g_cc.on) return;
    const uint8_t* p = pace_view_constants(112);
    if (!p) return;
    PaceSample s = {(uint64_t)(uintptr_t)cb, {0, 0, 0}, {0, 0, 0}, 0, now_ns(), -1, 0};
    memcpy(s.eye, p + 96, 12);
    if (!isfinite(s.eye[0]) || !isfinite(s.eye[1]) || !isfinite(s.eye[2])) return;
    t_pace_done = 1;
    float wt[10];
    if (g_tav.n > 0 && tav_rd(g_tav.m[0].wt, wt, sizeof wt)) { memcpy(s.root, wt + 4, 12); s.root_ok = 1; }
    // Tav's pose as this frame has it (the present comes a little later, when BG3's next tick may
    // already have moved her).
    int n = g_tav.n;
    uint32_t plan = g_tav.seq;
    if (g_tav.mesh_ok && n > 0 && n <= TAV_MAX) {
        size_t need = (size_t)n * (56 + TAV_MAX_BONES * 48);
        pthread_mutex_lock(&g_pace.m);
        int k = (int)g_snap_next;
        g_snap_next = (g_snap_next + 1) % PACE_SNAPS;
        uint32_t gen = ++g_snap_gen;
        pthread_mutex_unlock(&g_pace.m);
        if (g_snap[k].cap < need) {
            uint8_t* nb = malloc(need);
            if (nb) { free(g_snap[k].buf); g_snap[k].buf = nb; g_snap[k].cap = need; }
        }
        if (g_snap[k].cap >= need) {
            __atomic_store_n(&g_snap[k].gen_a, gen, __ATOMIC_RELEASE);
            g_snap[k].len = tav_pose_records(g_snap[k].buf);
            g_snap[k].plan = plan;
            g_snap[k].n = n;
            __atomic_store_n(&g_snap[k].gen_b, gen, __ATOMIC_RELEASE);
            s.snap = k;
            s.snap_gen = gen;
        }
    }
    pthread_mutex_lock(&g_pace.m);
    uint32_t k = 0;
    while (k < PACE_CBS && g_pace.cbs[k].cb != s.cb) k++;
    if (k == PACE_CBS) { k = g_pace.next; g_pace.next = (g_pace.next + 1) % PACE_CBS; }
    g_pace.cbs[k] = s;
    g_pace.samples++;
    pthread_mutex_unlock(&g_pace.m);
}

// Command buffers submitted in order (g_trace_lock held, from role_submit): a G-buffer sample
// becomes pending; a final pass (it samples the HUD) makes the pending one the frame's.
static void pace_submit(uint32_t n, const VkCommandBuffer* cbs) {
    pthread_mutex_lock(&g_pace.m);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t h = (uint64_t)(uintptr_t)cbs[i];
        for (uint32_t k = 0; k < PACE_CBS; k++)
            if (g_pace.cbs[k].cb == h) { g_pace.pending = g_pace.cbs[k]; g_pace.pending_ok = 1; g_pace.cbs[k].cb = 0; break; }
        CbRole* r = TABLE_FIND(g_cbrole, CBROLE_CAP, h, 0);
        if (r && r->init && r->fin_ui && g_pace.pending_ok) { g_pace.framed = g_pace.pending; g_pace.framed_ok = 1; }
    }
    pthread_mutex_unlock(&g_pace.m);
}

// The plugin's camera record, read once per present (the packet trailer and pacing share it).
static int pace_present_cam(uint8_t out[96]) {
    if (g_pace.cam_present != g_req.presents) {
        g_pace.cam_present = g_req.presents;
        g_pace.cam_ok = read_framed(bg3cam_path(), g_pace.cam, sizeof g_pace.cam, 4, 64);
        if (g_pace.cam_ok) pace_cam_note(g_pace.cam, now_ns());
    }
    if (g_pace.cam_ok) memcpy(out, g_pace.cam, 96);
    else memset(out, 0, 96);
    return g_pace.cam_ok;
}

// The pose records of the frame being presented, taken at its G-buffer pass (tav_write_pose, at
// the present, g_lock held): their size, 0 when there is none for this plan (then they are read
// now). A snapshot rewritten while copying (four frames recorded since) is not used.
static size_t pace_pose_snapshot(uint8_t* out, size_t cap) {
    pthread_mutex_lock(&g_pace.m);
    PaceSample f = g_pace.framed;
    int ok = g_pace.framed_ok;
    pthread_mutex_unlock(&g_pace.m);
    if (!ok || f.snap < 0 || f.snap >= PACE_SNAPS) { g_snap_missed++; return 0; }
    int k = f.snap;
    if (__atomic_load_n(&g_snap[k].gen_b, __ATOMIC_ACQUIRE) != f.snap_gen || __atomic_load_n(&g_snap[k].gen_a, __ATOMIC_ACQUIRE) != f.snap_gen ||
        g_snap[k].plan != g_tav.seq || g_snap[k].n != g_tav.n || g_snap[k].len > cap) { g_snap_missed++; return 0; }
    size_t len = g_snap[k].len;
    memcpy(out, g_snap[k].buf, len);
    if (__atomic_load_n(&g_snap[k].gen_a, __ATOMIC_ACQUIRE) != f.snap_gen) { g_snap_missed++; return 0; }
    g_snap_used++;
    return len;
}

static int pace_match(const float eb[3], uint32_t* seq, float* err, float* err2);

// The packet trailer appended to tavpose.bin (128 bytes, after the mesh records; readers that
// stop after the records ignore it): "TVC1", frame u32 (the header's), present time f64
// (CLOCK_MONOTONIC s), present index u64, the BGC1 record read at this present (96 bytes, zeros
// when torn or missing), its seq u32 (0 none), "TVC1".
#define PACE_TRAILER 128
static void pace_trailer(uint8_t* p, uint32_t frame) {
    uint8_t cam[96];
    int ok = pace_present_cam(cam);
    uint32_t seq = 0;
    if (ok) memcpy(&seq, cam + 4, 4);
    // The record whose eye the presented frame has, when the match is sure (the newest otherwise).
    pthread_mutex_lock(&g_pace.m);
    PaceSample f = g_pace.framed;
    int fok = g_pace.framed_ok;
    pthread_mutex_unlock(&g_pace.m);
    uint32_t m = 0;
    float err = -1, err2 = -1;
    if (fok && pace_match(f.eye, &m, &err, &err2) && err < 0.05f && err2 > err + 0.02f && err2 < 1e29f && m != seq) {
        pthread_mutex_lock(&g_pc.m);
        for (uint32_t i = 0; i < g_pc.n; i++) {
            const PaceCam* c = &g_pc.r[(g_pc.head + PC_N - 1 - i) % PC_N];
            if (c->seq == m) { memcpy(cam, c->rec, 96); seq = m; ok = 1; break; }
        }
        pthread_mutex_unlock(&g_pc.m);
    }
    double t = now_ns() * 1e-9;
    uint64_t idx = g_req.presents;
    memcpy(p, "TVC1", 4);
    memcpy(p + 4, &frame, 4);
    memcpy(p + 8, &t, 8);
    memcpy(p + 16, &idx, 8);
    memcpy(p + 24, cam, 96);
    memcpy(p + 120, &seq, 4);
    memcpy(p + 124, "TVC1", 4);
}

// The camera record whose eye is the frame's (BG3 metres -> WoW yards with world mode's mapping,
// WorldMode.lua's toWow: the plugin's lead basis is the same Centre and WowOrigin). 1: found, with
// the distance to it and to the nearest record with another eye.
static int pace_match(const float eb[3], uint32_t* seq, float* err, float* err2) {
    if (!g_tav.seq) return 0;
    const float yd = 0.9144f;
    float w[3] = {g_tav.wow[0] - (eb[2] - g_tav.centre[2]) / yd, g_tav.wow[1] + (eb[0] - g_tav.centre[0]) / yd,
                  g_tav.wow[2] + (eb[1] - g_tav.centre[1]) / yd};
    int found = 0;
    float best = 1e30f, second = 1e30f, beye[3] = {0, 0, 0};
    pthread_mutex_lock(&g_pc.m);
    uint32_t n = g_pc.n < 64 ? g_pc.n : 64;
    for (uint32_t i = 0; i < n; i++) {  // newest first: ties keep the newest record
        const PaceCam* c = &g_pc.r[(g_pc.head + PC_N - 1 - i) % PC_N];
        float dx = c->eye[0] - w[0], dy = c->eye[1] - w[1], dz = c->eye[2] - w[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < best) {
            float ex = c->eye[0] - beye[0], ey = c->eye[1] - beye[1], ez = c->eye[2] - beye[2];
            if (found && ex * ex + ey * ey + ez * ez > 1e-8f) second = best;
            best = d; *seq = c->seq; memcpy(beye, c->eye, 12); found = 1;
        } else if (d < second) {
            float ex = c->eye[0] - beye[0], ey = c->eye[1] - beye[1], ez = c->eye[2] - beye[2];
            if (ex * ex + ey * ey + ez * ez > 1e-8f) second = d;
        }
    }
    pthread_mutex_unlock(&g_pc.m);
    *err = best;
    *err2 = second;
    return found;
}

// Compose word "noflow": the want clock off. compose_config keeps only the file's mtime, so the
// word is read here and again whenever that mtime moves (no stat of our own).
static int pace_flow_on(void) {
    static struct timespec seen;
    static int on = 1, read_once = 0;
    if (!read_once || seen.tv_sec != g_cc.seen.tv_sec || seen.tv_nsec != g_cc.seen.tv_nsec) {
        seen = g_cc.seen;
        read_once = 1;
        char line[256] = {0};
        FILE* f = fopen(outp("/compose"), "r");
        if (f) {
            if (!fgets(line, sizeof line, f)) line[0] = 0;
            fclose(f);
        }
        on = !has_word(line, "noflow");
    }
    return on;
}

// The clock is set to the target when it is this far off: a real jump (a load, a teleport, benilla
// restarting and counting from 1 again), not the noise of one record.
#define PACE_FLOW_SNAP 4

// The camera seq to ask the WoW frames for: `target` (= T - lag) stepped once per present.
// A play session of the 0.1.0 beta logged "camera step (0/1/2/3+) 10/280/10/0" window after
// window, the 0s and the 2s always in equal numbers, while frames genuinely missing were 0 or 1 in
// 300 presents. So the WoW picture held for a frame and then moved two camera records 3 to 6 times
// a second because the target itself moved that way, not because WoW was late. The clock keeps its
// own rate of one record per present and ignores a target within one record of it, so that noise
// is gone; a lasting difference (the camera stream really running ahead or behind, or `lag`
// changing) is pulled in one record per present, and a jump is taken at once.
static uint32_t pace_flow(uint32_t target) {
    if (!target || !pace_flow_on()) { g_pace.flow = 0; return target; }
    uint64_t pres = g_req.presents;
    if (!g_pace.flow) { g_pace.flow = target; g_pace.flow_present = pres; return target; }
    uint64_t dp = pres > g_pace.flow_present ? pres - g_pace.flow_present : 0;
    if (dp > 8) dp = 8;  // compose skipped for a while: the snap below takes over if it was long
    g_pace.flow_present = pres;
    uint32_t next = g_pace.flow + (uint32_t)dp;
    int32_t off = (int32_t)(target - next);
    if (off > PACE_FLOW_SNAP || off < -PACE_FLOW_SNAP) { next = target; g_pace.flow_snap++; }
    else if (off >= 2) { next++; g_pace.flow_pull++; }
    else if (off <= -2) { next--; g_pace.flow_pull++; }
    if (dp) g_pace.flow_steps++;
    g_pace.flow = next;
    return next;
}

// Before compose (g_lock held, not g_trace_lock): which camera the presented frame has, which WoW
// frame to take, and a bounded wait for it.
static void pace_before_compose(void) {
    memset(&g_pt, 0, sizeof g_pt);
    uint8_t cam[96];
    g_pace.L = pace_present_cam(cam) ? *(const uint32_t*)(cam + 4) : 0;
    pthread_mutex_lock(&g_pace.m);
    g_pace.cur = g_pace.framed;
    g_pace.cur_ok = g_pace.framed_ok;
    pthread_mutex_unlock(&g_pace.m);
    g_pace.M = 0; g_pace.conf = 0; g_pace.err = g_pace.err2 = -1;
    if (g_pace.cur_ok && pace_match(g_pace.cur.eye, &g_pace.M, &g_pace.err, &g_pace.err2))
        g_pace.conf = g_pace.err < 0.05f && g_pace.err2 > g_pace.err + 0.02f && g_pace.err2 < 1e29f;
    if (g_pace.conf && g_pace.L) {
        int32_t dep = (int32_t)(g_pace.L - g_pace.M);
        if (dep >= 0 && dep < 4) {
            // decayed counts: the usual depth follows a lasting change within a few seconds
            for (int k = 0; k < 4; k++) g_pace.depth_hist[k] -= g_pace.depth_hist[k] >> 7;
            g_pace.depth_hist[dep] += 256;
            int bestk = 0;
            for (int k = 1; k < 4; k++) if (g_pace.depth_hist[k] > g_pace.depth_hist[bestk]) bestk = k;
            g_pace.depth_est = bestk;
        }
    }
    g_pace.T = g_pace.conf ? g_pace.M : (g_pace.L ? g_pace.L - (uint32_t)g_pace.depth_est : 0);
    if (g_cc.lag >= 0) g_pace.lag = g_cc.lag;
    g_pace.want = pace_flow(g_pace.T ? g_pace.T - (uint32_t)g_pace.lag : 0);
    g_pt.newest_only = g_cc.newest;
    g_pt.want = g_pace.want;
    g_pace.waited_ns = 0;
    g_pace.pre = g_pace.post = g_pace.early = -1;
    if (!g_pace.want || g_cc.newest || !g_wr.started) return;
    pthread_mutex_lock(&g_wr.m);
    // only when WoW's frames carry their camera (benilla 0013)
    int given = g_wr.latest >= 0 && g_wr.s[g_wr.latest].state != 0 && g_wr.s[g_wr.latest].cam;
    if (given) {
        g_pace.pre = wr_have_cam(g_pace.want);
        g_pace.early = wr_have_cam(g_pace.want + 1);
        if (!g_pace.pre && g_cc.wait_us > 0 && g_wr.cv_ok) {
            uint64_t t0 = now_ns();
            uint64_t until = t0 + (uint64_t)g_cc.wait_us * 1000ull;
            struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
            while (!wr_have_cam(g_pace.want))
                if (pthread_cond_timedwait(&g_wr.cv, &g_wr.m, &ts) != 0) break;
            g_pace.waited_ns = now_ns() - t0;
        }
        g_pace.post = wr_have_cam(g_pace.want);
    }
    pthread_mutex_unlock(&g_wr.m);
    if (!given || g_cc.lag >= 0) return;
    // the lag follows WoW: 3 misses in 240 presents raise it, 600 presents with the frame for one
    // camera later always there in time lower it
    g_pace.win_n++;
    if (!g_pace.post) g_pace.win_miss++;
    if (g_pace.win_miss >= 3) {
        if (g_pace.lag < 3) { g_pace.lag++; logf_("pace: lag %d (WoW's frame missed 3 times in %u presents)", g_pace.lag, g_pace.win_n); }
        g_pace.win_n = g_pace.win_miss = 0;
        g_pace.dwin_n = g_pace.dwin_early = 0;
    } else if (g_pace.win_n >= 240) {
        g_pace.win_n = g_pace.win_miss = 0;
    }
    if (g_pace.lag > 0) {
        g_pace.dwin_n++;
        if (g_pace.early) g_pace.dwin_early++;
        if (g_pace.dwin_n >= 600) {
            if (g_pace.dwin_early >= g_pace.dwin_n - 1) { g_pace.lag--; logf_("pace: lag %d (WoW's frames were early for 600 presents)", g_pace.lag); }
            g_pace.dwin_n = g_pace.dwin_early = 0;
        }
    }
}

static void pace_request(int n) {
    if (g_pace.csv) fclose(g_pace.csv);
    g_pace.csv = fopen(outp("/pace.csv"), "w");
    g_pace.csv_left = n > 0 && n <= 36000 ? n : 600;
    if (g_pace.csv)
        fprintf(g_pace.csv, "present,t_ns,L,M,conf,err,err2,eye_x,eye_y,eye_z,rootg_x,rootg_y,rootg_z,rootp_x,rootp_y,rootp_z,"
                            "sample_age_us,want,lag,wow_seq,wow_packet,wow_cam,ready_age_us,publish_age_us,waited_us,pre,post\n");
    logf_("pace: writing %d presents to pace.csv", g_pace.csv_left);
}

static uint64_t pace_pct(uint64_t* v, uint32_t n, double q) {
    if (!n) return 0;
    uint32_t i = (uint32_t)(q * (n - 1) + 0.5);
    return v[i];
}
static int pace_cmp(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

// After compose (g_lock held): statistics and the CSV.
static void pace_after_compose(uint64_t t_in) {
    if (g_pace.n && g_pace.niv < PACE_EVERY) g_pace.iv[g_pace.niv++] = t_in - g_pace.t_last;
    if (!g_pace.n) { g_pace.t_first = t_in; g_pace.ticks0 = g_pc.ticks; }
    g_pace.t_last = t_in;
    g_pace.n++;
    if (g_pace.conf) {
        g_pace.nconf++;
        int32_t dep = (int32_t)(g_pace.L - g_pace.M);
        g_pace.deph[dep < 0 ? 0 : dep > 3 ? 3 : dep]++;
    }
    if (g_pace.waited_ns) { g_pace.waits++; g_pace.wait_ns += g_pace.waited_ns; }
    if (g_pace.post == 0) g_pace.misses++;
    if (g_pt.used) {
        g_pace.used++;
        int32_t ds = (int32_t)(g_pt.seq - g_pace.prev_seq);
        g_pace.dseq[ds <= 0 ? 0 : ds > 3 ? 3 : ds]++;
        if (g_pt.cam && g_pace.prev_cam) {
            int32_t dc = (int32_t)(g_pt.cam - g_pace.prev_cam);
            g_pace.dcam[dc <= 0 ? 0 : dc > 3 ? 3 : dc]++;
        }
        if (g_pt.cam && g_pace.T) {
            int32_t lg = (int32_t)(g_pace.T - g_pt.cam);
            g_pace.lagh[lg < 0 ? 0 : lg > 3 ? 4 : lg + 1]++;
        }
        // whether the frame that was composited is the one that was asked for: "older" is WoW late
        // (or the clock ahead), "newer" the asked-for frame already gone from the reader's slots
        if (g_pt.cam && g_pace.want) {
            int32_t pd = (int32_t)(g_pt.cam - g_pace.want);
            g_pace.pickd[pd < 0 ? 0 : pd == 0 ? 1 : 2]++;
        }
        g_pace.prev_seq = g_pt.seq;
        g_pace.prev_cam = g_pt.cam;
        uint64_t age = t_in > g_pt.ready_ns ? t_in - g_pt.ready_ns : 0;
        g_pace.age_n++; g_pace.age_sum += age; if (age > g_pace.age_max) g_pace.age_max = age;
        if (g_pt.wtime > 0) {
            double wa = t_in * 1e-9 - g_pt.wtime;
            g_pace.wage_n++; g_pace.wage_sum += wa; if (wa > g_pace.wage_max) g_pace.wage_max = wa;
        }
    } else {
        g_pace.noframe++;
    }
    if (g_pace.csv && g_pace.csv_left > 0) {
        const PaceSample* s = &g_pace.cur;
        fprintf(g_pace.csv, "%llu,%llu,%u,%u,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%lld,%u,%d,%u,%u,%u,%lld,%lld,%llu,%d,%d\n",
                (unsigned long long)g_req.presents, (unsigned long long)t_in, g_pace.L, g_pace.M, g_pace.conf, g_pace.err, g_pace.err2,
                s->eye[0], s->eye[1], s->eye[2], s->root[0], s->root[1], s->root[2], g_tav_root[0], g_tav_root[1], g_tav_root[2],
                g_pace.cur_ok ? (long long)(t_in - s->t) / 1000 : -1, g_pace.want, g_pace.lag, g_pt.seq, g_pt.packet, g_pt.cam,
                g_pt.used ? (long long)(t_in - g_pt.ready_ns) / 1000 : -1,
                g_pt.wtime > 0 ? (long long)((t_in * 1e-9 - g_pt.wtime) * 1e6) : -1, (unsigned long long)(g_pace.waited_ns / 1000),
                g_pace.pre, g_pace.post);
        if (--g_pace.csv_left == 0) { fclose(g_pace.csv); g_pace.csv = NULL; logf_("pace: pace.csv done"); }
    }
    if (g_pace.n < PACE_EVERY) return;
    qsort(g_pace.iv, g_pace.niv, sizeof g_pace.iv[0], pace_cmp);
    uint64_t sum = 0, slow = 0;
    for (uint32_t i = 0; i < g_pace.niv; i++) { sum += g_pace.iv[i]; if (g_pace.iv[i] > 20000000ull) slow++; }
    double span = (g_pace.t_last - g_pace.t_first) * 1e-9;
    logf_("pace: %.1f presents/s, interval ms mean %.2f p50 %.2f p95 %.2f max %.2f (%llu over 20); ticks/present %.2f; "
          "WoW frames per present (seq step 0/1/2/3+) %llu/%llu/%llu/%llu, camera step %llu/%llu/%llu/%llu, none %llu; "
          "lag want %d, actual (<0/0/1/2/3+) %llu/%llu/%llu/%llu/%llu; depth (0/1/2/3) %llu/%llu/%llu/%llu of %llu matched (G-buffer samples %llu); "
          "waits %llu (%.2f ms avg), misses %llu; frame age at present ms: read %.2f (max %.2f), published %.2f (max %.2f); "
          "want clock %s (steps %llu, pulled %llu, resynced %llu), composited vs wanted (older/exact/newer) %llu/%llu/%llu; "
          "Tav pose from the G-buffer pass %llu, at the present %llu",
          span > 0 ? (g_pace.n - 1) / span : 0.0, g_pace.niv ? sum / 1e6 / g_pace.niv : 0.0, pace_pct(g_pace.iv, g_pace.niv, 0.5) / 1e6,
          pace_pct(g_pace.iv, g_pace.niv, 0.95) / 1e6, g_pace.niv ? g_pace.iv[g_pace.niv - 1] / 1e6 : 0.0, (unsigned long long)slow,
          (double)(g_pc.ticks - g_pace.ticks0) / (double)g_pace.n,
          (unsigned long long)g_pace.dseq[0], (unsigned long long)g_pace.dseq[1], (unsigned long long)g_pace.dseq[2], (unsigned long long)g_pace.dseq[3],
          (unsigned long long)g_pace.dcam[0], (unsigned long long)g_pace.dcam[1], (unsigned long long)g_pace.dcam[2], (unsigned long long)g_pace.dcam[3],
          (unsigned long long)g_pace.noframe, g_pace.lag,
          (unsigned long long)g_pace.lagh[0], (unsigned long long)g_pace.lagh[1], (unsigned long long)g_pace.lagh[2], (unsigned long long)g_pace.lagh[3],
          (unsigned long long)g_pace.lagh[4],
          (unsigned long long)g_pace.deph[0], (unsigned long long)g_pace.deph[1], (unsigned long long)g_pace.deph[2], (unsigned long long)g_pace.deph[3],
          (unsigned long long)g_pace.nconf, (unsigned long long)g_pace.samples,
          (unsigned long long)g_pace.waits, g_pace.waits ? g_pace.wait_ns / 1e6 / g_pace.waits : 0.0, (unsigned long long)g_pace.misses,
          g_pace.age_n ? g_pace.age_sum / 1e6 / g_pace.age_n : 0.0, g_pace.age_max / 1e6,
          g_pace.wage_n ? g_pace.wage_sum * 1e3 / g_pace.wage_n : -1.0, g_pace.wage_n ? g_pace.wage_max * 1e3 : -1.0,
          pace_flow_on() ? "on" : "off",
          (unsigned long long)g_pace.flow_steps, (unsigned long long)g_pace.flow_pull, (unsigned long long)g_pace.flow_snap,
          (unsigned long long)g_pace.pickd[0], (unsigned long long)g_pace.pickd[1], (unsigned long long)g_pace.pickd[2],
          (unsigned long long)g_snap_used, (unsigned long long)g_snap_missed);
    g_snap_used = g_snap_missed = 0;
    g_pace.n = 0; g_pace.niv = 0;
    memset(g_pace.dseq, 0, sizeof g_pace.dseq); memset(g_pace.dcam, 0, sizeof g_pace.dcam); memset(g_pace.lagh, 0, sizeof g_pace.lagh);
    memset(g_pace.deph, 0, sizeof g_pace.deph);
    memset(g_pace.pickd, 0, sizeof g_pace.pickd);
    g_pace.flow_steps = g_pace.flow_pull = g_pace.flow_snap = 0;
    g_pace.nconf = g_pace.waits = g_pace.wait_ns = g_pace.misses = g_pace.used = g_pace.noframe = 0;
    g_pace.age_n = g_pace.age_sum = g_pace.age_max = g_pace.wage_n = 0;
    g_pace.wage_sum = g_pace.wage_max = 0;
    g_pace.samples = 0;
}
