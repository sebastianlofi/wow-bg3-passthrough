// ---------------------------------------------------------------- compositor on the GPU
// The CPU compositor (compose_frame) read BG3's final picture, scene depth and HUD back to host
// memory and waited for them at every present, merged per pixel on the CPU and uploaded the
// result: BG3's render thread waited for its own GPU work every frame (14 ms a frame at 720p with
// another game loading the GPU) and the merge costs about 14 ms of CPU a frame at
// 3440x1440 (synthetic benchmark). Here the same per-pixel rule runs in a compute shader
// (wowbg3_compose.comp, SPIR-V in wowbg3_compose_spv.h) inside one command buffer submitted
// before the present, without any CPU wait:
//   copy swapchain image, scene depth and HUD to device-local buffers, dispatch, copy back.
// WoW's frame (WBF1 slot files) is read by a reader thread straight into host-visible buffers
// (WR_N of them, each free, being filled, ready or read by the GPU), so the present only picks
// the newest whole one. WoW's frame may differ in size from BG3's swapchain (KWin clamping
// benilla's window): it is sampled scaled instead of skipped.
// The CPU path stays for the compose words "fx" (legacy pixel effects) and "cpu" (comparison),
// and for frames whose HUD is still guessed (no final pass seen yet).
// The shader also lays BG3's ground surfaces onto WoW's ground (bindings 5..9,
// wowbg3_surf.inc.c; compose word "surf"); the CPU path does not.
// Included by wowbg3_capture.c (shares its tables and helpers).
#include <fcntl.h>
#include "wowbg3_compose_spv.h"

static void fxj_retire_buffer(uint64_t buffer);  // wowbg3_fxcap.inc.c: effects readbacks still reading a buffer

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// ---- frame-time accounting (perf_present logs it every PERF_EVERY presents)
#define PERF_EVERY 300
static struct {
    uint64_t n, hook, hook_max, glock, tav, fxc, compose, compose_max;  // present thread, under g_lock
    uint64_t next, next_max, submit, tlock;  // atomics: the driver's present, BG3's recording and submitting threads
} g_perf;
static void perf_add(uint64_t* at, uint64_t v) { __atomic_fetch_add(at, v, __ATOMIC_RELAXED); }
// g_trace_lock, counting the time BG3's threads wait for it (the layer's present path holds it).
static void trace_lock_timed(void) {
    if (pthread_mutex_trylock(&g_trace_lock) == 0) return;
    uint64_t t = now_ns();
    pthread_mutex_lock(&g_trace_lock);
    perf_add(&g_perf.tlock, now_ns() - t);
}

// Memory type with `want`, preferring one without `avoid`.
static int32_t memory_type_pref(DeviceData* d, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    VkPhysicalDeviceMemoryProperties props;
    d->inst->GetPhysicalDeviceMemoryProperties(d->physical, &props);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = props.memoryTypes[i].propertyFlags;
            if ((bits & (1u << i)) && (f & want) == want && (pass || !(f & avoid))) return (int32_t)i;
        }
    return -1;
}

typedef struct { VkBuffer buf; VkDeviceMemory mem; void* map; VkDeviceSize size; } GBuf;
static void gbuf_free(DeviceData* d, GBuf* b) {
    if (b->buf) d->DestroyBuffer(d->device, b->buf, NULL);
    if (b->mem) d->FreeMemory(d->device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}
// A buffer of at least `size` bytes: device-local (host 0), or host-visible and mapped: system
// memory, host cached preferred (host 1), or video memory the CPU writes through the BAR (host 2,
// falls back to 1).
static int gbuf_make(DeviceData* d, GBuf* b, VkDeviceSize size, VkBufferUsageFlags usage, int host) {
    if (b->buf && b->size >= size) return 1;
    gbuf_free(d, b);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (d->CreateBuffer(d->device, &bci, NULL, &b->buf) != VK_SUCCESS) { b->buf = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req;
    d->GetBufferMemoryRequirements(d->device, b->buf, &req);
    int32_t type = host == 2 ? memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : -1;
    if (type >= 0) {
        // video memory through the BAR
    } else if (host) {
        type = memory_type_pref(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = memory_type_pref(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } else {
        type = memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (type < 0) { gbuf_free(d, b); return 0; }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
    if (d->AllocateMemory(d->device, &mai, NULL, &b->mem) != VK_SUCCESS) {
        b->mem = VK_NULL_HANDLE;
        gbuf_free(d, b);
        return host == 2 ? gbuf_make(d, b, size, usage, 1) : 0;  // BAR full: system memory
    }
    if (d->BindBufferMemory(d->device, b->buf, b->mem, 0) != VK_SUCCESS) { gbuf_free(d, b); return 0; }
    if (host && d->MapMemory(d->device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) { b->map = NULL; gbuf_free(d, b); return 0; }
    b->size = size;
    return 1;
}

// ---- WoW's frame reader (a thread of its own; the present never touches the files' pixels)
// With frame pacing (wowbg3_pace.inc.c) every WoW frame is read, oldest unread
// first, and several stay ready: compose picks the one drawn for the camera of the BG3 frame being
// presented, which is not always the newest. WBF1 bytes 36..56 (benilla 0013): the frame packet
// (BG3 present, tavpose.bin's frame) and the camera record seq it was drawn from, flags, and
// benilla's publish time (CLOCK_MONOTONIC s). An older benilla leaves them 0: newest wins then.
#define WR_N 6
typedef struct {
    GBuf b;
    int bar;                   // b was made for the "wowbar" word (video memory through the BAR)
    int state;                 // 0 free, 1 being filled, 2 ready, 3 read by the GPU
    uint64_t use_serial;       // the compose submission that read it last
    uint32_t seq, ww, wh, code, cbpr, dbpr;
    float wnear, wzoff;
    uint32_t packet, cam, wflags;  // WBF1 36, 40, 44 (0: not given)
    double wtime;              // WBF1 48: benilla's publish time (CLOCK_MONOTONIC s), 0 not given
    uint32_t glight;           // WBF1 60 (benilla 0052): WoW's light on level sunlit ground, RGB gamma bytes, byte 3 = 1
    uint64_t ready_ns;         // when this reader finished it (CLOCK_MONOTONIC ns)
    uint32_t gen;              // benilla run (g_wr.gen when read): seqs compare within one run only
} WowSlot;
static struct {
    pthread_mutex_t m;
    pthread_cond_t cv;         // signalled when a frame is ready (CLOCK_MONOTONIC timed waits)
    int cv_ok;
    pthread_t th;
    int started;
    volatile int stop;
    DeviceData* d;
    WowSlot s[WR_N];
    int latest;                // newest ready slot (by seq), -1 none
    int cur;                   // the slot composited last (state 3), -1 none
    uint32_t last_seq, prev_seq;  // the last two frames read
    uint32_t gen;              // bumped when benilla restarts (its seq goes back)
    uint64_t frames, torn, full, ns, fails;  // since the last compose log
    uint64_t left;  // leftover frame files skipped (wr_header; the reader's own, racy count)
} g_wr = {.m = PTHREAD_MUTEX_INITIALIZER, .latest = -1, .cur = -1};

// The plugin's camera records seen (bg3cam.bin, one per BG3 tick), sampled by the reader thread
// every millisecond and at each present: the pacing code matches the presented frame's eye
// against them (wowbg3_pace.inc.c).
#define PC_N 256
typedef struct { uint32_t seq; float eye[3]; double t; uint64_t seen_ns; uint8_t rec[96]; } PaceCam;
static struct {
    pthread_mutex_t m;
    PaceCam r[PC_N];
    uint32_t n, head;          // head: next write index
    uint32_t last;
    uint64_t ticks;            // records seen, for the log
} g_pc = {.m = PTHREAD_MUTEX_INITIALIZER};
static void pace_cam_note(const uint8_t cam[96], uint64_t now) {
    uint32_t seq;
    memcpy(&seq, cam + 4, 4);
    pthread_mutex_lock(&g_pc.m);
    if (seq && seq != g_pc.last) {
        PaceCam* c = &g_pc.r[g_pc.head];
        c->seq = seq;
        memcpy(c->eye, cam + 8, 12);
        memcpy(&c->t, cam + 56, 8);
        memcpy(c->rec, cam, 96);
        c->seen_ns = now;
        g_pc.head = (g_pc.head + 1) % PC_N;
        if (g_pc.n < PC_N) g_pc.n++;
        g_pc.last = seq;
        g_pc.ticks++;
    }
    pthread_mutex_unlock(&g_pc.m);
}

static int wr_pread(int fd, void* out, size_t n, off_t at) {
    uint8_t* p = out;
    while (n) {
        ssize_t r = pread(fd, p, n, at);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return 0; }
        p += r; n -= (size_t)r; at += r;
    }
    return 1;
}
// A whole header (WBF1, seq == seq2 != 0) of slot file `k`: its seq, else 0 (fd stays open).
// A frame file benilla has not rewritten for WR_LEFT_NS is a leftover, not a live frame: after a death
// and a load the WoW side restarts and counts its frames from 1 again, while its two files still held
// the last frames of the run before; with the read seqs forgotten at the load (wr_forget) they counted as
// unread and newer, and the picture stayed on one of them (a playtest: 24 to 42 s old and growing,
// seq step 0). The age is benilla's publish time (WBF1 48, CLOCK_MONOTONIC s), else the file's mtime.
#define WR_LEFT_NS 1000000000ull
static uint32_t wr_header(int k, int* fd, uint8_t hdr[64]) {
    *fd = open(wow_frame(k), O_RDONLY | O_CLOEXEC);
    if (*fd < 0) return 0;
    uint32_t a, b;
    if (!wr_pread(*fd, hdr, 64, 0) || memcmp(hdr, "WBF1", 4)) return 0;
    memcpy(&a, hdr + 4, 4);
    memcpy(&b, hdr + 56, 4);
    if (!a || a != b) return 0;
    double wtime;
    memcpy(&wtime, hdr + 48, 8);
    if (wtime > 0) {
        double age = (double)now_ns() / 1e9 - wtime;
        if (age > (double)WR_LEFT_NS / 1e9) { g_wr.left++; return 0; }
    } else {
        struct stat st;
        struct timespec t;
        if (fstat(*fd, &st) == 0 && clock_gettime(CLOCK_REALTIME, &t) == 0) {
            int64_t age = ((int64_t)t.tv_sec - st.st_mtim.tv_sec) * 1000000000ll + (t.tv_nsec - st.st_mtim.tv_nsec);
            if (age > (int64_t)WR_LEFT_NS) { g_wr.left++; return 0; }
        }
    }
    return a;
}
// A slot to fill (g_wr.m held): a free one, else the oldest ready one that compose is not using.
static int wr_take_slot(void) {
    int slot = -1;
    for (int j = 0; j < WR_N && slot < 0; j++) if (g_wr.s[j].state == 0) slot = j;
    if (slot < 0) {
        for (int j = 0; j < WR_N; j++)
            if (g_wr.s[j].state == 2 && (slot < 0 || g_wr.s[j].gen != g_wr.gen ||
                                         (g_wr.s[slot].gen == g_wr.gen && (int32_t)(g_wr.s[j].seq - g_wr.s[slot].seq) < 0))) slot = j;
        if (slot >= 0) { g_wr.full++; if (g_wr.latest == slot) g_wr.latest = -1; }
    }
    if (slot >= 0) g_wr.s[slot].state = 1;
    return slot;
}
// WoW's light for BG3's gameplay light: <link>/wowlight.txt holds
// "<luma> <r> <g> <b> <n>", WoW's light on level sunlit ground from the frame header (bytes 60..64, gamma
// 0..255, benilla 0052) and a count. Rewritten when the luma moves by 4 or 2 s passed. The mod's
// WorldLight.lua turns it into BG3 light round the party: day lit, night dim, as on a normal level.
static void wowlight_note(uint32_t gl) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t last_ns, n;
    static int last_l = -100, logged_l = -100;
    const uint8_t* b = (const uint8_t*)&gl;
    if (b[3] != 1 || pthread_mutex_trylock(&m) != 0) return;
    int l = (299 * b[0] + 587 * b[1] + 114 * b[2] + 500) / 1000;
    uint64_t now = now_ns();
    if (abs(l - last_l) >= 4 || now - last_ns >= 2000000000ull) {
        last_l = l; last_ns = now; n++;
        char path[1100], tmp[1120];
        snprintf(path, sizeof path, "%s/wowlight.txt", link_dir());
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE* f = fopen(tmp, "w");
        if (f) {
            fprintf(f, "%d %u %u %u %llu\n", l, b[0], b[1], b[2], (unsigned long long)n);
            if (fclose(f) == 0) rename(tmp, path);
        }
        if (abs(l - logged_l) >= 16) { logged_l = l; logf_("wowlight: WoW's ground light %u %u %u (luma %d) to %s", b[0], b[1], b[2], l, path); }
    }
    pthread_mutex_unlock(&m);
}
// Patch 41: WR_PAR threads read a frame's payload together (the reader thread takes the first part).
#define WR_PAR 4
static struct { pthread_mutex_t m; pthread_cond_t go, done; int started, fd, left, ok; uint64_t gen; uint8_t* dst; size_t n; off_t at; } g_wrp =
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, -1, 0, 1, 0, NULL, 0, 0};
static void wrp_part(int i, int fd, uint8_t* dst, size_t n, off_t at, int* ok) {
    size_t a = n / WR_PAR * (size_t)i, b = i == WR_PAR - 1 ? n : n / WR_PAR * (size_t)(i + 1);
    a &= ~(size_t)4095; if (i != WR_PAR - 1) b &= ~(size_t)4095;
    if (b > a && !wr_pread(fd, dst + a, b - a, at + (off_t)a)) *ok = 0;
}
static void* wrp_main(void* arg) {
    const int i = (int)(intptr_t)arg;
    uint64_t seen = 0;
    pthread_mutex_lock(&g_wrp.m);
    for (;;) {
        while (g_wrp.gen == seen) pthread_cond_wait(&g_wrp.go, &g_wrp.m);
        seen = g_wrp.gen;
        int fd = g_wrp.fd; uint8_t* dst = g_wrp.dst; size_t n = g_wrp.n; off_t at = g_wrp.at;
        pthread_mutex_unlock(&g_wrp.m);
        int ok = 1;
        wrp_part(i, fd, dst, n, at, &ok);
        pthread_mutex_lock(&g_wrp.m);
        if (!ok) g_wrp.ok = 0;
        if (--g_wrp.left == 0) pthread_cond_signal(&g_wrp.done);
    }
    return NULL;
}
// The payload read in WR_PAR parts at once; one thread's read when the helpers are not running.
static int wr_pread_par(int fd, void* out, size_t n, off_t at) {
    if (!g_wrp.started) {
        g_wrp.started = -1;
        int made = 0;
        for (int i = 1; i < WR_PAR; i++) { pthread_t t; if (pthread_create(&t, NULL, wrp_main, (void*)(intptr_t)i) == 0) { pthread_detach(t); made++; } }
        if (made == WR_PAR - 1) g_wrp.started = 1;
        logf_("reader: %s", g_wrp.started == 1 ? "frames read on 4 threads (patch 41)" : "helper threads failed; frames read on one thread");
    }
    if (g_wrp.started != 1) return wr_pread(fd, out, n, at);
    pthread_mutex_lock(&g_wrp.m);
    g_wrp.fd = fd; g_wrp.dst = out; g_wrp.n = n; g_wrp.at = at; g_wrp.ok = 1; g_wrp.left = WR_PAR - 1; g_wrp.gen++;
    pthread_cond_broadcast(&g_wrp.go);
    pthread_mutex_unlock(&g_wrp.m);
    int ok = 1;
    wrp_part(0, fd, out, n, at, &ok);
    pthread_mutex_lock(&g_wrp.m);
    while (g_wrp.left > 0) pthread_cond_wait(&g_wrp.done, &g_wrp.m);
    ok = ok && g_wrp.ok;
    pthread_mutex_unlock(&g_wrp.m);
    return ok;
}
// Patch 42: set while BG3 waits for a WoW frame in lockstep (wowbg3_wd.inc.c): look for it every 100 us.
static volatile int g_wr_hurry;
static void* wr_main(void* arg) {
    (void)arg;
    while (!g_wr.stop) {
        if (!g_cc.on || !g_wr.d) { usleep(20000); continue; }
        {
            uint8_t cam[96];
            if (read_framed(bg3cam_path(), cam, sizeof cam, 4, 64)) pace_cam_note(cam, now_ns());
        }
        uint8_t h[2][64];
        int fd[2];
        uint32_t s[2] = {wr_header(0, &fd[0], h[0]), wr_header(1, &fd[1], h[1])};
        // The older unread whole slot first, so no frame is skipped while the reader keeps up (a
        // restarted benilla counts from 1 again: anything but the last two frames read is unread).
        int k = -1;
        for (int j = 0; j < 2; j++)
            if (s[j] && s[j] != g_wr.last_seq && s[j] != g_wr.prev_seq && (k < 0 || (int32_t)(s[j] - s[k]) < 0)) k = j;
        uint32_t ww = 0, wh = 0, code = 0, cbpr = 0, dbpr = 0, packet = 0, cam = 0, wflags = 0;
        float wnear = 0, wzoff = 0;
        double wtime = 0;
        uint32_t glight = 0;
        if (k >= 0) {
            memcpy(&glight, h[k] + 60, 4);
            wowlight_note(glight);
            memcpy(&ww, h[k] + 8, 4); memcpy(&wh, h[k] + 12, 4); memcpy(&code, h[k] + 16, 4);
            memcpy(&cbpr, h[k] + 20, 4); memcpy(&dbpr, h[k] + 24, 4); memcpy(&wnear, h[k] + 28, 4); memcpy(&wzoff, h[k] + 32, 4);
            memcpy(&packet, h[k] + 36, 4); memcpy(&cam, h[k] + 40, 4); memcpy(&wflags, h[k] + 44, 4); memcpy(&wtime, h[k] + 48, 8);
            uint32_t bpt = code == 5 ? 8 : 4;
            if (!ww || !wh || ww > 8192 || wh > 8192 || !code || code > 5 || cbpr < ww * bpt || dbpr < ww * 4 || (cbpr & 3) || (dbpr & 3)) k = -1;
        }
        int slot = -1;
        if (k >= 0) {
            pthread_mutex_lock(&g_wr.m);
            slot = wr_take_slot();
            pthread_mutex_unlock(&g_wr.m);
        }
        if (slot < 0) {
            for (int j = 0; j < 2; j++) if (fd[j] >= 0) close(fd[j]);
            usleep(k >= 0 ? 1000 : (g_wr_hurry ? 100 : 500));
            continue;
        }
        WowSlot* w = &g_wr.s[slot];
        size_t payload = (size_t)cbpr * wh + (size_t)dbpr * wh;
        // Compose word "wowbar": WoW's frame in video memory, written by this thread through the
        // BAR (the GPU then reads it at video memory speed instead of over PCIe in the shader).
        int bar = g_cc.wowbar;
        if (w->b.buf && w->bar != bar) gbuf_free(g_wr.d, &w->b);
        w->bar = bar;
        uint64_t t0 = now_ns();
        int ok = gbuf_make(g_wr.d, &w->b, payload, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, bar ? 2 : 1) &&
                 wr_pread_par(fd[k], w->b.map, payload, 64);
        uint8_t after[64];
        // Torn when benilla started rewriting this slot meanwhile (it writes seq2 = 0 first).
        int torn = ok && (!wr_pread(fd[k], after, 64, 0) || memcmp(after + 4, h[k] + 4, 4) || memcmp(after + 56, h[k] + 56, 4));
        uint64_t t1 = now_ns();
        for (int j = 0; j < 2; j++) if (fd[j] >= 0) close(fd[j]);
        if (g_cc.wdtime) {
            static FILE* tf; static uint64_t n;
            if (!tf) { tf = fopen(outp("/wrtime.csv"), "w"); if (tf) fprintf(tf, "read0_ns,ready_ns,seq,packet,cam,publish_ns,torn,ok\n"); }
            if (tf) { fprintf(tf, "%llu,%llu,%u,%u,%u,%llu,%d,%d\n", (unsigned long long)t0, (unsigned long long)t1, s[k], packet, cam,
                              (unsigned long long)(wtime * 1e9), torn, ok); if (++n % 60 == 0) fflush(tf); }
        }
        pthread_mutex_lock(&g_wr.m);
        // benilla restarted (its seq went back): frames of the old run are never newer. A torn frame counts
        // too: the first frame of a new run read torn hid the restart, and the picture stayed on the old
        // run's last frame until the new run's count passed it (a playtest: seq 70003, over 13 minutes).
        if ((ok || torn) && g_wr.last_seq && (int32_t)(s[k] - g_wr.last_seq) < 0) {
            g_wr.gen++;
            for (int j = 0; j < WR_N; j++) if (g_wr.s[j].state == 2) g_wr.s[j].state = 0;
            g_wr.latest = -1;
        }
        if (!ok || torn) {
            w->state = 0;
            if (torn) g_wr.torn++; else g_wr.fails++;
            // a torn frame is gone for good (benilla moved on): do not try it again
            if (torn) { g_wr.prev_seq = g_wr.last_seq; g_wr.last_seq = s[k]; }
        } else {
            w->gen = g_wr.gen;
            w->seq = s[k]; w->ww = ww; w->wh = wh; w->code = code; w->cbpr = cbpr; w->dbpr = dbpr; w->wnear = wnear; w->wzoff = wzoff;
            w->packet = packet; w->cam = cam; w->wflags = wflags; w->wtime = wtime; w->ready_ns = t1;
            w->glight = (glight >> 24) == 1u ? glight : 0u;
            w->state = 2;
            if (g_wr.latest < 0 || g_wr.s[g_wr.latest].state == 0 || (int32_t)(s[k] - g_wr.s[g_wr.latest].seq) > 0)
                g_wr.latest = slot;
            g_wr.prev_seq = g_wr.last_seq;
            g_wr.last_seq = s[k];
            g_wr.frames++;
            g_wr.ns += t1 - t0;
            if (g_wr.cv_ok) pthread_cond_broadcast(&g_wr.cv);
        }
        pthread_mutex_unlock(&g_wr.m);
        // Patch 42: after the hand-over (it reads video memory, 0.7 ms); only this thread refills the slot.
        if (ok && !torn) {   // Flash recorder: WoW's picture jumping in brightness between two frames goes in the log, with that
            // frame's colour rows on disk and BG3's next frames as a capture (at most 8 in a session).
            static float flash_last = -1;
            static int flash_n;
            uint64_t sum = 0, count = 0;
            for (uint32_t sy = wh / 40 + 1; sy < wh; sy += wh / 20 + 1)
                for (uint32_t sx = ww / 80 + 1; sx < ww; sx += ww / 40 + 1) {
                    uint32_t px;  // one read: the frame may be in video memory, slow to read
                    memcpy(&px, (const uint8_t*)w->b.map + (size_t)sy * cbpr + (size_t)sx * 4, 4);
                    sum += (px & 255) + ((px >> 8) & 255) + ((px >> 16) & 255);
                    count += 3;
                }
            float mean = count ? (float)sum / count : 0;
            if (flash_last >= 0 && !g_cc.camp && !g_cc.camp_hold && fabsf(mean - flash_last) > 30) {
                logf_("compose: WoW frame %u jumps in brightness, %.0f to %.0f", s[k], flash_last, mean);
                if (0 && flash_n < 8) {  // fx-route: the 6-frame capture would take the recorder
                    char tag[32], path[256];
                    flash_n++;
                    snprintf(tag, sizeof tag, "wflash%d-%u", flash_n, s[k]);
                    snprintf(path, sizeof path, "/%s.bgra-%ux%u-%u", tag, ww, wh, cbpr);
                    FILE* f = fopen(outp(path), "wb");
                    if (f) { fwrite(w->b.map, 1, (size_t)cbpr * wh, f); fclose(f); }
                    f = fopen(outp("/request"), "w");
                    if (f) { fprintf(f, "6 1 %s\n", tag); fclose(f); }
                }
            }
            flash_last = mean;
        }
        if (!ok) usleep(20000);
    }
    return NULL;
}

// Pacing's choice for this present (wowbg3_pace.inc.c, pace_before_compose): the camera record
// the WoW frame should have been drawn from (0: just take the newest frame), and what compose
// actually used (for the pace log).
static struct {
    uint32_t want;             // camera seq wanted (0: newest)
    int newest_only;           // compose word "newest", or no camera seqs in WoW's frames
    // filled by compose_gpu
    int used;                  // a WoW frame was composited
    uint32_t seq, packet, cam;
    double wtime;
    uint64_t ready_ns;
} g_pt;

#define WR_STALE_NS 1000000000ull
// BG3 starts a load (bg3_state_check, g_lock held): every WoW frame read so far belongs to the world
// before it, never to be shown again; the next run's frames start a new generation.
static void wr_forget(void) {
    pthread_mutex_lock(&g_wr.m);
    g_wr.gen++;
    for (int j = 0; j < WR_N; j++) if (g_wr.s[j].state == 2) g_wr.s[j].state = 0;
    g_wr.cur = -1;  // a slot a submission still reads stays in use until it completes
    g_wr.latest = -1;
    g_wr.last_seq = g_wr.prev_seq = 0;
    pthread_mutex_unlock(&g_wr.m);
}
// The WoW frame to composite (g_wr.m held): the newest, or with pacing the one drawn for the
// wanted camera (the newest at or before it; the oldest after it when none is at or before).
static int wr_pick(void) {
    int best = -1, newest = -1, have_cam = 1;
    uint64_t last_read = 0;
    for (int j = 0; j < WR_N; j++) {
        WowSlot* w = &g_wr.s[j];
        if ((w->state == 2 || (w->state == 3 && j == g_wr.cur)) && w->gen == g_wr.gen && w->ready_ns > last_read) last_read = w->ready_ns;
    }
    for (int j = 0; j < WR_N; j++) {
        WowSlot* w = &g_wr.s[j];
        if (!(w->state == 2 || (w->state == 3 && j == g_wr.cur)) || w->gen != g_wr.gen) continue;
        // Long before the last frame read: not the live run, whatever its seq says.
        if (last_read - w->ready_ns > WR_STALE_NS) continue;
        if (newest < 0 || (int32_t)(w->seq - g_wr.s[newest].seq) > 0) newest = j;
        if (!w->cam) have_cam = 0;
    }
    if (newest < 0) return g_wr.cur >= 0 && g_wr.s[g_wr.cur].state == 3 ? g_wr.cur : -1;
    if (g_pt.newest_only || !g_pt.want || !have_cam) return newest;
    const uint32_t want = g_pt.want;
    const uint64_t fresh = g_wr.s[newest].ready_ns;
    for (int j = 0; j < WR_N; j++) {
        WowSlot* w = &g_wr.s[j];
        if (!(w->state == 2 || (w->state == 3 && j == g_wr.cur)) || w->gen != g_wr.gen) continue;
        // A frame read more than WR_STALE_NS before the newest is never chosen (a playtest report:
        // after a death and a load the picture stayed on the frame read just before, 150 s and
        // counting, as it was drawn "at or before" the camera wanted and the new run's were after).
        if ((int64_t)(fresh - w->ready_ns) > (int64_t)WR_STALE_NS) continue;
        int32_t d = (int32_t)(w->cam - want);
        if (best < 0) { best = j; continue; }
        int32_t bd = (int32_t)(g_wr.s[best].cam - want);
        // at or before the wanted camera beats after it; nearer beats farther; then the newer frame
        if ((d <= 0) != (bd <= 0)) { if (d <= 0) best = j; continue; }
        if (d <= 0 ? d > bd : d < bd) { best = j; continue; }
        if (d == bd && (int32_t)(w->seq - g_wr.s[best].seq) > 0) best = j;
    }
    return best;
}
// Whether a frame drawn for camera `cam` or later is ready (g_wr.m held).
__attribute__((unused)) static int wr_have_cam(uint32_t cam) {
    for (int j = 0; j < WR_N; j++) {
        WowSlot* w = &g_wr.s[j];
        if ((w->state == 2 || w->state == 3) && w->gen == g_wr.gen && w->cam && (int32_t)(w->cam - cam) >= 0) return 1;
    }
    return 0;
}

// ---- the compositor's GPU objects (one device and queue family, like the CPU path's)
#define GC_SLOTS 3
static struct {
    int ready, failed;
    DeviceData* dev;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd[GC_SLOTS];
    VkFence fence[GC_SLOTS];
    VkSemaphore done[GC_SLOTS];
    uint64_t serial[GC_SLOTS], next_serial, completed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkDescriptorPool pool_ds;
    VkDescriptorSet set[GC_SLOTS];
    GBuf out, dep, ui, cnt[GC_SLOTS], cam[GC_SLOTS];  // cam: the surfaces' camera block (wowbg3_compose.comp, Cam)
    GBuf dark;                // the one-pixel backdrop frame of BG3's own screens (gc_dark_frame)
    VkQueryPool qp;           // GPU timestamps at the start and end of each slot's work (0: none)
    double tick_ns;
    uint64_t ts_mask, gpu_ns, gpu_max, gpu_n;
    uint32_t k;
    int cur;                  // WoW slot composited last, -1 none
    uint32_t cur_ww, cur_wh;
    uint64_t wow_px, px;      // WoW pixels counted by the shader (read back when a slot comes round)
} g_gc = {.cur = -1};

typedef struct {
    uint32_t W, H, ww, wh, cbpr, dbpr, code, flags;
    float P22, P32, P23, P33, wnear, wzoff, far, wgamma;  // wgamma: the sRGB decode (flag 64)
} GcPush;

static int gc_init(DeviceData* d, uint32_t family) {
    if (g_gc.ready) return g_gc.dev == d && g_gc.family == family;
    if (g_gc.failed) return 0;
    g_gc.failed = 1;  // until everything below exists
    if (!d->CreateDescriptorPool || !d->AllocateDescriptorSets || !d->GetFenceStatus || !d->CreateComputePipelines ||
        !d->CreateShaderModule || !d->CreateDescriptorSetLayout || !d->CreatePipelineLayout || !d->CmdDispatch || !d->CmdCopyBufferToImage) {
        logf_("compose: GPU path unavailable (device functions missing)");
        return 0;
    }
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    if (d->CreateCommandPool(d->device, &pci, NULL, &g_gc.pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_gc.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, GC_SLOTS};
    if (d->AllocateCommandBuffers(d->device, &ai, g_gc.cmd) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    for (int k = 0; k < GC_SLOTS; k++) {
        if (d->setLoaderData && d->setLoaderData(d->device, g_gc.cmd[k]) != VK_SUCCESS) return 0;
        if (d->CreateFence(d->device, &fci, NULL, &g_gc.fence[k]) != VK_SUCCESS) return 0;
        if (d->CreateSemaphore(d->device, &sci, NULL, &g_gc.done[k]) != VK_SUCCESS) return 0;
        if (!gbuf_make(d, &g_gc.cnt[k], 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1)) return 0;
        if (!gbuf_make(d, &g_gc.cam[k], 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1)) return 0;
        memset(g_gc.cnt[k].map, 0, 16);
        memset(g_gc.cam[k].map, 0, 256);
    }
    // 0..4 and 9 storage buffers, 5..8 the surfaces' storage images
    VkDescriptorSetLayoutBinding b[10];
    for (uint32_t i = 0; i < 10; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, i >= 5 && i <= 8 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                              VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 10, b};
    if (d->CreateDescriptorSetLayout(d->device, &dci, NULL, &g_gc.dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GcPush)};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_gc.dsl, 1, &pr};
    if (d->CreatePipelineLayout(d->device, &lci, NULL, &g_gc.layout) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_compose_spv, wowbg3_compose_spv};
    VkShaderModule mod;
    if (d->CreateShaderModule(d->device, &mci, NULL, &mod) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL},
        g_gc.layout, VK_NULL_HANDLE, -1};
    VkResult r = d->CreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, NULL, &g_gc.pipe);
    if (d->DestroyShaderModule) d->DestroyShaderModule(d->device, mod, NULL);
    if (r != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 * GC_SLOTS}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * GC_SLOTS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, GC_SLOTS, 2, ps};
    if (d->CreateDescriptorPool(d->device, &dpi, NULL, &g_gc.pool_ds) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout l[GC_SLOTS];
    for (int k = 0; k < GC_SLOTS; k++) l[k] = g_gc.dsl;
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_gc.pool_ds, GC_SLOTS, l};
    if (d->AllocateDescriptorSets(d->device, &sai, g_gc.set) != VK_SUCCESS) return 0;
    // GPU time of the compositor's own work, for the log (optional).
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)d->inst->gipa(d->inst->instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties gqf =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)d->inst->gipa(d->inst->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (gpp && gqf && d->CreateQueryPool && d->CmdResetQueryPool && d->CmdWriteTimestamp && d->GetQueryPoolResults) {
        VkPhysicalDeviceProperties pp;
        gpp(d->physical, &pp);
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16;
        gqf(d->physical, &nq, qf);
        uint32_t bits = family < nq ? qf[family].timestampValidBits : 0;
        VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * GC_SLOTS, 0};
        if (bits && pp.limits.timestampPeriod > 0 && d->CreateQueryPool(d->device, &qci, NULL, &g_gc.qp) == VK_SUCCESS) {
            g_gc.tick_ns = pp.limits.timestampPeriod;
            g_gc.ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
        } else {
            g_gc.qp = VK_NULL_HANDLE;
        }
    }
    g_gc.dev = d;
    g_gc.family = family;
    g_gc.ready = 1;
    g_gc.failed = 0;
    logf_("compose: GPU path ready");
    return 1;
}

// Submissions known complete (fences signal in submission order on the one queue).
static void gc_completed(void) {
    for (int k = 0; k < GC_SLOTS; k++)
        if (g_gc.serial[k] > g_gc.completed && g_gc.dev->GetFenceStatus(g_gc.dev->device, g_gc.fence[k]) == VK_SUCCESS)
            g_gc.completed = g_gc.serial[k];
}
static void gc_wait_all(void) {
    if (!g_gc.ready) return;
    g_gc.dev->WaitForFences(g_gc.dev->device, GC_SLOTS, g_gc.fence, VK_TRUE, 2000000000ull);
    gc_completed();
}
// BG3 destroys an image: when it is one the compositor reads, let our copies finish first.
static void gc_retire_image(VkImage img) {
    if (!g_gc.ready || !img || ((uint64_t)img != g_role.depth && (uint64_t)img != g_role.ui)) return;
    pthread_mutex_lock(&g_lock);
    gc_wait_all();
    pthread_mutex_unlock(&g_lock);
}

static GBuf* ground_pre_for_compose(uint32_t w, uint32_t h, VkFormat dfmt);  // wowbg3_ground.inc.c
static uint64_t ground_framed_serial(void);                                  // wowbg3_ground.inc.c
// wowbg3_surf.inc.c
static uint32_t surf_for_compose(DeviceData* d, uint64_t framed_serial, uint32_t cam_m, uint32_t glight_m, VkImageView views[4], uint8_t camblk[176]);
static void surf_counts(uint32_t sp, uint32_t ok, uint32_t bad);
static void surf_compose_prepare(DeviceData* d, VkCommandBuffer cmd);
static void surf_compose_copy(DeviceData* d, VkCommandBuffer cmd, const WowSlot* w);

// The backdrop for BG3's own 3D screens (compose word "screenvoid", see g_screen): a WoW frame of
// one pixel in one dark colour whose depth says "nothing here", so the shader keeps BG3's picture
// where BG3's scene depth has something and paints the backdrop over every other pixel. The buffer
// is made once and holds the same two words for the rest of the session.
#define GC_DARK_BGRA 0x00120e0eu  // blue 18, green 14, red 14: nearly black, a hint of blue
static int gc_dark_frame(DeviceData* d, WowSlot* w) {
    if (!g_gc.dark.map) {
        // 64 bytes for 2 words: the shader reads word 0 (the colour row) and word 1 (the depth row)
        // only, and the rest leaves no byte of the binding it could read past.
        if (!gbuf_make(d, &g_gc.dark, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1) || !g_gc.dark.map) return 0;
        uint32_t px[16] = {GC_DARK_BGRA, 0};  // the colour row, then the depth row (0: no WoW surface)
        memcpy(g_gc.dark.map, px, sizeof px);
    }
    w->b = g_gc.dark;
    w->ww = 1; w->wh = 1;
    w->cbpr = 4; w->dbpr = 4;
    w->code = 3;        // BGRA bytes as they are (no decode)
    w->wnear = 1.0f;    // never used: the depth row keeps this pixel behind everything
    return 1;
}

#define GSKIP(r) { g_cc.skipped++; g_cc.why[(r) & 7]++; \
    if (g_cc.skipped < 200 || g_cc.skipped % 300 == 1) logf_("compose: skipping (reason %d; %llu skipped, %llu frames; depth %llx l%d ui %llx l%d ext %ux%u)", (r), \
        (unsigned long long)g_cc.skipped, (unsigned long long)g_cc.frames, (unsigned long long)g_role.depth, g_role.dlayout, \
        (unsigned long long)g_role.ui, g_role.ulayout, g_role.ew, g_role.eh); \
    return 0; }

// The GPU compositor. 1: the present must wait on *sem; 0: frame left untouched; -1: use the CPU
// path. Called at present with g_lock and g_trace_lock held, like compose_frame.
static int compose_gpu(QueueData* q, SwapchainData* c, uint32_t index, const VkPresentInfoKHR* info, VkSemaphore* sem) {
    DeviceData* d = q->dev;
    if (c->format != VK_FORMAT_B8G8R8A8_UNORM && c->format != VK_FORMAT_B8G8R8A8_SRGB) return -1;
    if (g_cc.cpu || g_cc.fx || g_gc.failed) return -1;
    VkImage depth = (VkImage)g_role.depth;
    VkImage ui = g_cc.ui && g_role.ulayout >= 0 ? (VkImage)g_role.ui : VK_NULL_HANDLE;
    if (ui && !g_role.fin_seen) return -1;  // the HUD is still guessed: the CPU path judges it
    // A BG3 screen with its own 3D picture (level-up, respec, the magic mirror; see g_screen):
    // WoW's frame has nothing to do with the close-up camera such a screen renders from, so the
    // backdrop frame takes its place and the picture becomes the character over one dark colour.
    // Without the HUD the frame is left alone: the backdrop must never paint over the screen itself.
    int dark = g_screen && g_cc.screenvoid && !g_screen_dialog;
    if (dark && !ui) GSKIP(1)
    VkImageLayout dlayout = (VkImageLayout)g_role.dlayout, ulayout = (VkImageLayout)g_role.ulayout;
    VkFormat dfmt = g_role.dfmt, ufmt = g_role.ufmt;
    if (!depth || g_role.dlayout < 0 || g_role.ew != c->extent.width || g_role.eh != c->extent.height) GSKIP(1)
    uint8_t cam[96];
    if (!read_framed(bg3cam_path(), cam, sizeof cam, 4, 64)) GSKIP(2)
    if (!gc_init(d, q->family)) return -1;
    if (!g_wr.started) {
        g_wr.d = d;
        if (!g_wr.cv_ok) {
            pthread_condattr_t ca;
            g_wr.cv_ok = pthread_condattr_init(&ca) == 0 && pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0 &&
                         pthread_cond_init(&g_wr.cv, &ca) == 0;
        }
        g_wr.started = pthread_create(&g_wr.th, NULL, wr_main, NULL) == 0 ? 1 : -1;
        if (g_wr.started < 0) { g_gc.failed = 1; logf_("compose: no reader thread; CPU path"); return -1; }
    }
    if (g_gc.queue && g_gc.queue != q->queue) gc_wait_all();  // another queue: no order between the two
    g_gc.queue = q->queue;
    const uint32_t k = g_gc.k;
    if (d->WaitForFences(d->device, 1, &g_gc.fence[k], VK_TRUE, 2000000000ull) != VK_SUCCESS) GSKIP(6)
    if (g_gc.serial[k]) {  // that submission's WoW pixel count and GPU time, for the log
        uint32_t n, sc[3];
        memcpy(&n, g_gc.cnt[k].map, 4);
        memcpy(sc, (uint8_t*)g_gc.cnt[k].map + 4, 12);
        if (sc[0] || sc[1] || sc[2]) surf_counts(sc[0], sc[1], sc[2]);
        g_gc.wow_px += n;
        g_gc.px += (uint64_t)c->extent.width * c->extent.height;
        {   // A short run of frames with little of WoW in them is BG3's void flashing through: each run in the log.
            static uint64_t dip_frames;
            static uint32_t dip_low;
            uint64_t all = (uint64_t)c->extent.width * c->extent.height;
            if ((uint64_t)n * 100 < all * 97) {
                if (!dip_frames) dip_low = n;
                if (n < dip_low) dip_low = n;
                dip_frames++;
            } else if (dip_frames) {
                logf_("compose: dip over: %llu frames with under 97%% WoW, lowest %.1f%%, at frame %llu, screen %d, ui %d, WoW seq %u, %llu skipped",
                      (unsigned long long)dip_frames, 100.0 * dip_low / all, (unsigned long long)g_cc.frames, (int)g_screen,
                      (int)g_role.ulayout, (unsigned)g_pt.seq, (unsigned long long)g_cc.skipped);
                dip_frames = 0;
            }
        }
        uint64_t ts[2];
        if (g_gc.qp && d->GetQueryPoolResults(d->device, g_gc.qp, 2 * k, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            uint64_t ns = (uint64_t)((double)((ts[1] - ts[0]) & g_gc.ts_mask) * g_gc.tick_ns);
            g_gc.gpu_ns += ns; g_gc.gpu_n++;
            if (ns > g_gc.gpu_max) g_gc.gpu_max = ns;
        }
    }
    memset(g_gc.cnt[k].map, 0, 16);
    gc_completed();

    // WoW's frame: the newest whole one, or with pacing the one drawn for this BG3 frame's camera
    // (wr_pick; the one composited last when nothing better came). On a screen the backdrop frame
    // takes its place and no WoW frame is touched at all.
    WowSlot w;
    memset(&w, 0, sizeof w);
    if (dark) {
        if (!gc_dark_frame(d, &w)) GSKIP(3)
    } else {
        pthread_mutex_lock(&g_wr.m);
        // Frames the GPU has finished reading: ready again when newer than the one composited last
        // (pacing stepped back from them), else free. Never freed while a submission still reads them.
        for (int j = 0; j < WR_N; j++)
            if (g_wr.s[j].state == 3 && j != g_wr.cur && g_wr.s[j].use_serial <= g_gc.completed)
                g_wr.s[j].state = g_wr.cur >= 0 && g_wr.s[j].gen == g_wr.s[g_wr.cur].gen && (int32_t)(g_wr.s[j].seq - g_wr.s[g_wr.cur].seq) > 0 ? 2 : 0;
        int pick = wr_pick();
        if (pick >= 0 && pick != g_wr.cur) {
            int old = g_wr.cur;
            g_wr.cur = pick;
            g_wr.s[pick].state = 3;
            if (old >= 0 && g_wr.s[old].state == 3 && g_wr.s[old].use_serial <= g_gc.completed)
                g_wr.s[old].state = g_wr.s[old].gen == g_wr.s[pick].gen && (int32_t)(g_wr.s[old].seq - g_wr.s[pick].seq) > 0 ? 2 : 0;
        }
        // Ready frames older than the composited one are never shown any more.
        for (int j = 0; pick >= 0 && j < WR_N; j++)
            if (g_wr.s[j].state == 2 && (g_wr.s[j].gen != g_wr.s[pick].gen || (int32_t)(g_wr.s[j].seq - g_wr.s[pick].seq) < 0)) g_wr.s[j].state = 0;
        g_gc.cur = g_wr.cur;
        if (g_gc.cur >= 0) {
            w = g_wr.s[g_gc.cur];
            g_wr.s[g_gc.cur].use_serial = g_gc.next_serial + 1;
        }
        pthread_mutex_unlock(&g_wr.m);
        g_pt.used = g_gc.cur >= 0 && w.b.buf;
        g_pt.seq = w.seq; g_pt.packet = w.packet; g_pt.cam = w.cam; g_pt.wtime = w.wtime; g_pt.ready_ns = w.ready_ns;
        if (g_gc.cur < 0 || !w.b.buf) GSKIP(3)
    }
    // A trip to camp: BG3's own picture, and of WoW's frame only its interface (WBF1 flag 2: drawn
    // without the world, its alpha says where the interface is). A frame with the world, from before
    // benilla heard, is not shown.
    int ui_only = 0;
    if (g_cc.camp && !dark) {
        if (!(w.wflags & 2u)) return 0;
        ui_only = 1;
    }
    // Back from camp: WoW's camera stood still and the frames in hand were drawn for the camera BG3 had
    // when the trip began, so they show the wrong angle. The backdrop until a frame with the world
    // follows a camera record newer than the hold's start (BG3's lead is on again); never longer than
    // CAMP_HOLD_NS, should the lead not come.
#define CAMP_HOLD_NS 4000000000ull
    if (g_cc.camp_hold && !g_cc.camp && !dark) {
        if (g_cc.camp_hold == 1) {
            memcpy(&g_cc.camp_hold_seq, cam + 4, 4);
            g_cc.camp_hold_ns = now_ns();
            g_cc.camp_hold = 2;
            logf_("compose: back from camp: holding the backdrop from camera %u", g_cc.camp_hold_seq);
        }
        const int match = !(w.wflags & 2u) && w.cam && (int32_t)(w.cam - g_cc.camp_hold_seq) > 0;
        const uint64_t waited = now_ns() - g_cc.camp_hold_ns;
        if (match || waited > CAMP_HOLD_NS) {
            logf_("compose: back from camp: WoW frame %u for camera %u shown after %.0f ms%s", w.seq, w.cam, waited / 1e6, match ? "" : " (gave up waiting)");
            g_cc.camp_hold = 0;
        } else {
            if (!ui) return 0;
            if (!gc_dark_frame(d, &w)) GSKIP(3)
            dark = 1;
        }
    }
    // Any other frame without the world (the last of a trip, the wait given up) is never the picture.
    if (!ui_only && !dark && (w.wflags & 2u)) return 0;

    const uint32_t W = c->extent.width, H = c->extent.height;
    const int db = depth_bytes(dfmt);
    VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkDeviceSize need[3] = {(VkDeviceSize)W * H * 4, ((VkDeviceSize)W * H * db + 3) & ~3ull, (VkDeviceSize)W * H * 4};
    if (g_gc.out.size < need[0] || g_gc.dep.size < need[1] || (ui && g_gc.ui.size < need[2])) {
        gc_wait_all();  // the scratch buffers are shared by the slots
        if (!gbuf_make(d, &g_gc.out, need[0], su, 0) || !gbuf_make(d, &g_gc.dep, need[1], su, 0) || (ui && !gbuf_make(d, &g_gc.ui, need[2], su, 0)))
            GSKIP(7)
    }
    // With WoW's ground in BG3's G-buffer, the depth BG3 drew itself (copied just before the
    // injection) decides "BG3 nearer", not the depth with the ground in it.
    // On a screen no ground is injected any more, so the last pre-injection copy is an old frame's:
    // the backdrop reads BG3's depth image itself. A party dialog keeps the ground (its pre-injection
    // copy holds BG3's actors, drawn by BG3 on a screen).
    GBuf* pre = dark ? NULL : ground_pre_for_compose(W, H, dfmt);
    // BG3's ground surfaces of the frame presented (wowbg3_surf.inc.c): its images and the cameras.
    VkImageView sv[4];
    uint32_t sfl = surf_for_compose(d, ground_framed_serial(), w.cam, w.glight, sv, (uint8_t*)g_gc.cam[k].map);
    if (sfl == 0xffffffffu) { g_gc.failed = 1; logf_("compose: no stand-in images for the surfaces; CPU path"); return -1; }
    if (dark) memset(g_gc.cam[k].map, 0, 176);  // no ground surfaces over the backdrop
    VkDescriptorBufferInfo bi[6] = {{g_gc.out.buf, 0, VK_WHOLE_SIZE}, {pre && !g_cc.fxbg ? pre->buf : g_gc.dep.buf, 0, VK_WHOLE_SIZE},
        {ui ? g_gc.ui.buf : g_gc.out.buf, 0, VK_WHOLE_SIZE}, {w.b.buf, 0, VK_WHOLE_SIZE}, {g_gc.cnt[k].buf, 0, VK_WHOLE_SIZE},
        {g_gc.cam[k].buf, 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo ii[4];
    for (int i = 0; i < 4; i++) ii[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, sv[i], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[10];
    for (uint32_t i = 0; i < 5; i++)
        wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_gc.set[k], i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[i], NULL};
    for (uint32_t i = 0; i < 4; i++)
        wr[5 + i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_gc.set[k], 5 + i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[i], NULL, NULL};
    wr[9] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_gc.set[k], 9, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[5], NULL};
    d->UpdateDescriptorSets(d->device, 10, wr, 0, NULL);

    GcPush pc = {W, H, w.ww, w.wh, w.cbpr, w.dbpr, w.code, 0, 0, 0, 0, 0, w.wnear, w.wzoff, g_cc.far, g_cc.wgamma};
    memcpy(&pc.P22, cam + 80, 4); memcpy(&pc.P32, cam + 84, 4); memcpy(&pc.P23, cam + 88, 4); memcpy(&pc.P33, cam + 92, 4);
    if (!(pc.wzoff > -100.0f && pc.wzoff < 100.0f)) pc.wzoff = 0;
    if (ui) pc.flags |= 1u | (ufmt == VK_FORMAT_R8G8B8A8_UNORM ? 2u : 0u);
    if (g_cc.light) pc.flags |= 4u;
    if (db == 2) pc.flags |= 8u;
    else if (dfmt == VK_FORMAT_X8_D24_UNORM_PACK32 || dfmt == VK_FORMAT_D24_UNORM_S8_UINT) pc.flags |= 16u;
    if (w.ww != W || w.wh != H) pc.flags |= 32u;
    if (w.code == 2 && !g_cc.wowraw) pc.flags |= 64u;  // WoW's bytes decoded once
    if (ui_only) pc.flags |= 128u;
    if (g_cc.fxbg) pc.flags |= g_cc.fxbgpass ? 256u : 512u;  // fx-route prototype: BG3's own picture as it is; or what BG3's effects changed in it

    VkCommandBuffer cmd = g_gc.cmd[k];
    d->ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    d->BeginCommandBuffer(cmd, &cbi);
    // Everything submitted before (BG3's frame, the previous composite) before these copies.
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    surf_compose_prepare(d, cmd);  // the surfaces' 1x1 stand-ins enter GENERAL once
    if (g_gc.qp) {
        d->CmdResetQueryPool(cmd, g_gc.qp, 2 * k, 2);
        d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_gc.qp, 2 * k);
    }
    VkImage img = c->images[index];
    barrier_aspect(d, cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rc = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    d->CmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_gc.out.buf, 1, &rc);
    if (g_cc.fxbg) d->CmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_gc.dep.buf, 1, &rc);  // binding 1 holds BG3's final picture
    // Layout changes take both aspects of a depth-stencil image (without separateDepthStencilLayouts).
    VkImageAspectFlags da = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (dfmt == VK_FORMAT_D32_SFLOAT_S8_UINT || dfmt == VK_FORMAT_D24_UNORM_S8_UINT || dfmt == VK_FORMAT_D16_UNORM_S8_UINT) da |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!pre && !g_cc.fxbg) {
        barrier_aspect(d, cmd, depth, da, dlayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy rd = {0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
        d->CmdCopyImageToBuffer(cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_gc.dep.buf, 1, &rd);
        barrier_aspect(d, cmd, depth, da, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dlayout);
    }
    if (ui) {
        barrier_aspect(d, cmd, ui, VK_IMAGE_ASPECT_COLOR_BIT, ulayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        d->CmdCopyImageToBuffer(cmd, ui, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_gc.ui.buf, 1, &rc);
        barrier_aspect(d, cmd, ui, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ulayout);
    }
    VkMemoryBarrier m2 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &m2, 0, NULL, 0, NULL);
    d->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_gc.pipe);
    d->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_gc.layout, 0, 1, &g_gc.set[k], 0, NULL);
    d->CmdPushConstants(cmd, g_gc.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    d->CmdDispatch(cmd, (W + 7) / 8, (H + 7) / 8, 1);
    VkMemoryBarrier m3 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT};
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &m3, 0, NULL, 0, NULL);
    barrier_aspect(d, cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    d->CmdCopyBufferToImage(cmd, g_gc.out.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    barrier_aspect(d, cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    surf_compose_copy(d, cmd, &w);  // WoW's frame for the next BG3 frame's albedo pass (wowbg3_surf.inc.c)
    if (g_gc.qp) d->CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_gc.qp, 2 * k + 1);
    d->EndCommandBuffer(cmd);

    VkPipelineStageFlags stages[16];
    uint32_t waits = info->waitSemaphoreCount > 16 ? 16 : info->waitSemaphoreCount;
    for (uint32_t i = 0; i < waits; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, waits, info->pWaitSemaphores, stages, 1, &cmd, 1, &g_gc.done[k]};
    d->ResetFences(d->device, 1, &g_gc.fence[k]);
    if (d->QueueSubmit(q->queue, 1, &si, g_gc.fence[k]) != VK_SUCCESS) {
        // The fence stays unsignalled: this path is done for the session.
        g_gc.failed = 1;
        logf_("compose: GPU submit failed; CPU path from now on");
        return -1;
    }
    g_gc.serial[k] = ++g_gc.next_serial;
    g_gc.k = (k + 1) % GC_SLOTS;
    g_gc.cur_ww = w.ww; g_gc.cur_wh = w.wh;
    *sem = g_gc.done[k];
    if (++g_cc.frames % 300 == 1) {
        pthread_mutex_lock(&g_wr.m);
        uint64_t fr = g_wr.frames, ns = g_wr.ns, torn = g_wr.torn, full = g_wr.full, fails = g_wr.fails, left = g_wr.left;
        g_wr.frames = g_wr.ns = g_wr.torn = g_wr.full = g_wr.fails = g_wr.left = 0;
        pthread_mutex_unlock(&g_wr.m);
        logf_("compose: %llu frames (GPU, %.3f ms of GPU time each, max %.2f), %llu skipped, ui %s (%s, %llu changes), wow %.1f%% of pixels,"
              " WoW frame %ux%u%s, layouts d%d u%d; reader: %llu WoW frames, %.2f ms each, %llu torn, %llu dropped unshown (slots full), %llu failures, %llu leftover looks",
              (unsigned long long)g_cc.frames, g_gc.gpu_n ? g_gc.gpu_ns / 1e6 / g_gc.gpu_n : 0.0, g_gc.gpu_max / 1e6,
              (unsigned long long)g_cc.skipped, ui ? "yes" : "no", g_role.fin_seen ? "final pass" : "guessed",
              (unsigned long long)g_role.switches, g_gc.px ? 100.0 * g_gc.wow_px / g_gc.px : 0.0, w.ww, w.wh, (pc.flags & 32u) ? " scaled" : "",
              (int)dlayout, (int)ulayout, (unsigned long long)fr, fr ? ns / 1e6 / fr : 0.0, (unsigned long long)torn,
              (unsigned long long)full, (unsigned long long)fails, (unsigned long long)left);
        g_gc.wow_px = g_gc.px = 0;
        g_gc.gpu_ns = g_gc.gpu_max = g_gc.gpu_n = 0;
    }
    return 1;
}

// At present (g_lock held): the frame-time account, logged every PERF_EVERY presents.
static void perf_present(uint64_t hook, uint64_t glock, uint64_t tav, uint64_t fxc, uint64_t compose) {
    g_perf.n++;
    g_perf.hook += hook; if (hook > g_perf.hook_max) g_perf.hook_max = hook;
    g_perf.glock += glock; g_perf.tav += tav; g_perf.fxc += fxc;
    g_perf.compose += compose; if (compose > g_perf.compose_max) g_perf.compose_max = compose;
    if (g_perf.n < PERF_EVERY) return;
    double n = (double)g_perf.n;
    uint64_t submit = __atomic_exchange_n(&g_perf.submit, 0, __ATOMIC_RELAXED), tlock = __atomic_exchange_n(&g_perf.tlock, 0, __ATOMIC_RELAXED);
    uint64_t next = __atomic_exchange_n(&g_perf.next, 0, __ATOMIC_RELAXED), next_max = __atomic_exchange_n(&g_perf.next_max, 0, __ATOMIC_RELAXED);
    logf_("perf: per present (ms, %u presents): layer %.3f (max %.2f) = wait for g_lock %.3f, tav %.3f, effects %.3f, compose %.3f (max %.2f), rest;"
          " driver present %.3f (max %.2f); per frame in BG3's threads: submit hooks %.3f, waits for the trace lock %.3f",
          (unsigned)g_perf.n, g_perf.hook / n / 1e6, g_perf.hook_max / 1e6, g_perf.glock / n / 1e6, g_perf.tav / n / 1e6, g_perf.fxc / n / 1e6,
          g_perf.compose / n / 1e6, g_perf.compose_max / 1e6, next / n / 1e6, next_max / 1e6, submit / n / 1e6, tlock / n / 1e6);
    memset(&g_perf, 0, __builtin_offsetof(__typeof__(g_perf), next));
}
static void perf_next(uint64_t ns) {
    perf_add(&g_perf.next, ns);
    uint64_t m = __atomic_load_n(&g_perf.next_max, __ATOMIC_RELAXED);
    while (ns > m && !__atomic_compare_exchange_n(&g_perf.next_max, &m, ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

// The device goes away: stop the reader (its buffers die with the device).
static void gc_device_gone(DeviceData* d) {
    if (g_wr.started == 1 && g_wr.d == d) {
        g_wr.stop = 1;
        pthread_join(g_wr.th, NULL);
        g_wr.started = 0;
        g_wr.stop = 0;
        g_wr.d = NULL;
    }
    if (g_gc.ready && g_gc.dev == d) gc_wait_all();
}
