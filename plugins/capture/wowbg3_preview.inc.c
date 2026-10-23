// ---------------------------------------------------------------- WoW bodies in BG3's previews
// BG3's examine and trade screens show a character in a picture of their own (a "preview"): a small
// deferred render of that one character that ends in a tonemap pass (fragment shader PV_FS, harness
// traces) into an RGBA8 image the UI samples, opaque on black (525x690 for examine, 700x690 for
// trade at 1920x1080; the trade screen draws two, the party member's and the trader's). For a stand-in
// that picture is the BG3 body under the WoW model (Duokna: a naked human with a shield).
// RenderScreen.lua writes link/wowpreview.txt while such a screen shows a stand-in:
//   1 <screen> <previews> <index> <guid hex> <turn>
// (how many previews the screen draws, which of them is the stand-in's, and BG3's turn of it; "0"
// when none is). benilla (patch 0066) answers with the stand-in's WoW body: a still of its display baked
// ahead (link/wowbody/d<display>.rgba, the display from link/wowbody/<guid>.txt), shown at once, then
// live frames, animated and turned with BG3's preview (the live file named in link/wowbody/live.path:
// a 64-byte header, "WBL1", width, height, newest slot, sequence, guid, covered box, display, and two
// frame slots). This file draws the newest of them over the preview's image right after BG3's tonemap
// pass: cleared to black, the body's covered box scaled and placed where BG3 stands its own figure
// (PV_TOP to PV_FEET of the height, centred). Until a body is there the stand-in's preview stays black,
// and so does any preview in its first PV_HOLD presents while the request does not account for it
// (the screen opened before RenderScreen.lua told), so BG3's stand-in body never shows.
// Which preview is which: the previews drawn in the last PV_SEEN presents in creation order (the image
// table's ids). The trade screen makes the party member's image first (harness); record order is no
// guide, BG3 records the two previews on different threads. BG3 does not always draw both (a
// playtest: most frames only one, "previews seen 1"), so the stand-in's image, once told apart with both
// drawn, stays the stand-in's for the request (and the other one the other's).
// Bodies are uploaded at a present into a ring of PV_RING images (a command buffer and fence each, never
// waited on); a pass takes the newest finished one uploaded since the request named the stand-in, and
// an image is reused only PV_KEEP presents after a pass last took it. layer_CreateImage gives BG3's small sampled RGBA8 colour targets TRANSFER_DST
// for the clear and the blit. Logs `preview:` lines. Included by wowbg3_capture.c after
// wowbg3_surf.inc.c.

#define PV_FS 0x0f97d588b1d4bf83ull  // the preview's tonemap pass (fragment shader)
#define PV_SEEN 3                    // presents a preview image counts as on screen after its last pass
#define PV_HOLD 6                    // presents a newly shown preview stays black unless the request covers it
#define PV_KEEP 6                    // presents a body image stays untouched after a pass last took it
#define PV_RING 6
#define PV_IMGS 8
#define PV_MAX 2048                  // largest body accepted (either side)
#define PV_TOP 0.016f                // where BG3's examine preview stands its figure (harness dump: Tav,
#define PV_FEET 0.84f                // rows 11 to 579 of 690, centred)

typedef struct {
    VkImage img; VkDeviceMemory mem;
    VkBuffer buf; VkDeviceMemory bmem; void* mapped;
    VkCommandBuffer cmd; VkFence fence;
    uint32_t w, h, bbox[4];
    uint64_t guid, made;   // the body it holds and the upload's order
    int pending, ready;    // uploaded and not yet seen finished / finished
    uint64_t taken;        // present count a pass last took it at
} PvPic;
static struct {
    pthread_mutex_t m;
    int on;                                // the request names a stand-in
    uint32_t count, index;                 // the screen's previews and the stand-in's
    uint64_t guid;
    struct timespec req_mt;                // the request file's state
    char screen[32];
    uint64_t still_guid;                   // the still last uploaded for (once per request)
    char live_path[512];
    uint32_t live_seq;
    uint64_t live_guid;                    // the stand-in the last live frame was (its still is then late)
    uint64_t made;
    uint64_t req_made;                     // uploads up to this one came before the request: never shown
    uint64_t mine_img, other_img;          // the stand-in's preview image and the other one, once told apart
    PvPic pic[PV_RING];
    struct { uint64_t img; uint32_t id; uint64_t seen, since; } img[PV_IMGS];
    uint64_t drawn, black, stills, frames, last_log;
    uint32_t last_live, last_ord;
    VkCommandPool pool;                    // the uploads'
    uint32_t family;
} g_pv = {.m = PTHREAD_MUTEX_INITIALIZER};
// The pass being recorded on this thread: its one colour attachment and whether the tonemap drew in it.
static __thread VkCommandBuffer t_pv_cb;
static __thread uint64_t t_pv_img;
static __thread int32_t t_pv_fin;
static __thread int t_pv_hit;
static __thread uint64_t t_pv_pipe;  // last pipeline classified on this thread, and whether it is PV_FS's
static __thread int t_pv_kind;

// begin_pass (g_trace_lock held): a pass with one attachment may be a preview's tonemap.
static void pv_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const int32_t* fin) {
    t_pv_cb = VK_NULL_HANDLE;
    if (n != 1) return;
    t_pv_cb = cb;
    t_pv_img = imgs[0];
    t_pv_fin = fin[0];
    t_pv_hit = 0;
}
// A draw in such a pass: is it the tonemap?
static void pv_draw(VkCommandBuffer cb) {
    if (t_pv_cb != cb || t_bind_cb != cb || !t_pipe) return;
    if (t_pv_pipe != t_pipe) {
        t_pv_pipe = t_pipe;
        t_pv_kind = 0;
        RecInfo* ri = g_rec ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
        Recipe* r = ri ? ri->r : NULL;
        for (uint32_t s = 0; r && s < r->nstages; s++)
            if (r->st[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT && r->st[s].key == PV_FS) t_pv_kind = 1;
    }
    if (t_pv_kind) t_pv_hit = 1;
}

// BG3 ended a pass (its EndRenderPass already recorded): after a preview's tonemap, black or the WoW
// body over it when it is the stand-in's or not accounted for yet.
static void pv_pass_end(DeviceData* d, VkCommandBuffer cb) {
    if (t_pv_cb != cb) return;
    t_pv_cb = VK_NULL_HANDLE;
    if (!t_pv_hit || !t_pv_img || t_pv_fin <= 0 || !d->CmdBlitImage || !d->CmdClearColorImage) return;
    pthread_mutex_lock(&g_trace_lock);
    ImgInfo* i = g_img ? TABLE_FIND(g_img, IMG_CAP, t_pv_img, 0) : NULL;
    uint32_t id = i ? i->id : 0, w = i ? i->w : 0, h = i ? i->hgt : 0;
    int fit = i && i->fmt == VK_FORMAT_R8G8B8A8_UNORM && (i->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    pthread_mutex_unlock(&g_trace_lock);
    if (!id || !w || !h) return;
    const uint64_t now = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_pv.m);
    int slot = -1, oldest = 0;
    for (int k = 0; k < PV_IMGS; k++) {
        if (g_pv.img[k].img == t_pv_img) { slot = k; break; }
        if (g_pv.img[k].seen < g_pv.img[oldest].seen) oldest = k;
    }
    if (slot < 0) { slot = oldest; g_pv.img[slot].img = 0; }
    // Shown again after a gap (BG3 may keep a preview's image between screens): new again.
    if (g_pv.img[slot].img != t_pv_img || g_pv.img[slot].id != id || g_pv.img[slot].seen + PV_SEEN < now) g_pv.img[slot].since = now;
    g_pv.img[slot].img = t_pv_img;
    g_pv.img[slot].id = id;
    g_pv.img[slot].seen = now;
    const int fresh = g_pv.img[slot].since + PV_HOLD > now;
    uint32_t live = 0, ord = 0;
    for (int k = 0; k < PV_IMGS; k++) {
        if (!g_pv.img[k].img || g_pv.img[k].seen + PV_SEEN < now) continue;
        live++;
        if (g_pv.img[k].id < id) ord++;
    }
    int counted = g_pv.on && live == g_pv.count;
    if (counted) {
        if (ord == g_pv.index) g_pv.mine_img = t_pv_img;
        else g_pv.other_img = t_pv_img;
    } else if (g_pv.on && (t_pv_img == g_pv.mine_img || t_pv_img == g_pv.other_img)) {
        counted = 1;  // told apart before: the count no longer matters
    }
    const int mine = counted && (g_pv.mine_img ? t_pv_img == g_pv.mine_img : ord == g_pv.index);
    // The newest finished body of the stand-in.
    PvPic* pic = NULL;
    for (int k = 0; mine && k < PV_RING; k++) {
        PvPic* p = &g_pv.pic[k];
        if (p->ready && p->guid == g_pv.guid && p->made > g_pv.req_made && (!pic || p->made > pic->made)) pic = p;
    }
    VkImage src = VK_NULL_HANDLE;
    uint32_t pw = 0, ph = 0, bb[4] = {0, 0, 0, 0};
    if (pic) {
        pic->taken = now;
        src = pic->img;
        pw = pic->w;
        ph = pic->h;
        memcpy(bb, pic->bbox, sizeof bb);
    }
    // The hold is for the screens' previews only, taller than wide (525x690, 700x690 at 1080p): BG3's
    // portrait re-render after a level-up or respec draws through the same tonemap into a square image
    // (300x300) just once, so it was always new, and the hold left the party's new portraits black
    // (a playtest report: Shadowheart's black, Tav's the backdrop alone; harness: a respec, black).
    const int shaped = h > w;
    const int black = !pic && (mine || (fresh && !counted && shaped));
    if (pic) g_pv.drawn++;
    else if (black) g_pv.black++;
    g_pv.last_live = live;
    g_pv.last_ord = ord;
    pthread_mutex_unlock(&g_pv.m);
    if (!fit || !(pic || black)) return;
    VkImage dst = (VkImage)t_pv_img;
    VkImageLayout fin = (VkImageLayout)t_pv_fin;
    sf_barrier(d, cb, dst, fin, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue clear = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d->CmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &all);
    if (pic) {
        sf_barrier(d, cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        // The covered box (the whole body image without one) where BG3 stands its figure: its feet at
        // PV_FEET, its height PV_FEET - PV_TOP of the image's, narrower when it is the wider shape.
        if (bb[2] <= bb[0] || bb[3] <= bb[1] || bb[2] > pw || bb[3] > ph) { bb[0] = 0; bb[1] = 0; bb[2] = pw; bb[3] = ph; }
        const float bw = (float)(bb[2] - bb[0]), bh = (float)(bb[3] - bb[1]);
        float s = (PV_FEET - PV_TOP) * (float)h / bh;
        if (bw * s > 0.94f * (float)w) s = 0.94f * (float)w / bw;
        int32_t dw = (int32_t)(bw * s + 0.5f), dh = (int32_t)(bh * s + 0.5f);
        int32_t y1 = (int32_t)(PV_FEET * (float)h + 0.5f), x0 = ((int32_t)w - dw) / 2;
        int32_t y0 = y1 - dh;
        if (y0 < 0) { y0 = 0; }
        VkImageBlit rg = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{(int32_t)bb[0], (int32_t)bb[1], 0}, {(int32_t)bb[2], (int32_t)bb[3], 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{x0, y0, 0}, {x0 + dw, y1, 1}}};
        d->CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg, VK_FILTER_LINEAR);
    }
    sf_barrier(d, cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fin, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

static void pv_pic_free(DeviceData* d, PvPic* p) {
    if (p->img) d->DestroyImage(d->device, p->img, NULL);
    if (p->mem) d->FreeMemory(d->device, p->mem, NULL);
    if (p->buf) d->DestroyBuffer(d->device, p->buf, NULL);
    if (p->bmem) d->FreeMemory(d->device, p->bmem, NULL);  // unmapped with it
    VkCommandBuffer cmd = p->cmd;
    VkFence fence = p->fence;
    memset(p, 0, sizeof *p);
    p->cmd = cmd;
    p->fence = fence;
}

// A ring slot for a body of w x h (at a present, g_lock held): a finished one no pass took lately, its
// image, staging buffer, command buffer and fence made when missing or of another size. NULL: none free.
static PvPic* pv_slot(QueueData* q, uint32_t w, uint32_t h, uint64_t now) {
    DeviceData* d = q->dev;
    if (!g_pv.pool || g_pv.family != q->family) {
        if (g_pv.pool) return NULL;  // another queue family than the first: not seen; stay out
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, q->family};
        if (d->CreateCommandPool(d->device, &pci, NULL, &g_pv.pool) != VK_SUCCESS) { g_pv.pool = VK_NULL_HANDLE; return NULL; }
        g_pv.family = q->family;
    }
    PvPic* p = NULL;
    for (int k = 0; k < PV_RING; k++) {
        PvPic* c = &g_pv.pic[k];
        if (c->pending || c->taken + PV_KEEP > now) continue;
        if (!p || c->made < p->made) p = c;  // the oldest free one
    }
    if (!p) return NULL;
    if (!p->cmd) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_pv.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (d->AllocateCommandBuffers(d->device, &ai, &p->cmd) != VK_SUCCESS) { p->cmd = VK_NULL_HANDLE; return NULL; }
        // Command buffers made by a layer are dispatchable and need the loader's table.
        if (d->setLoaderData && d->setLoaderData(d->device, p->cmd) != VK_SUCCESS) return NULL;
    }
    if (!p->fence) {
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
        if (d->CreateFence(d->device, &fci, NULL, &p->fence) != VK_SUCCESS) { p->fence = VK_NULL_HANDLE; return NULL; }
    }
    if (p->img && p->w == w && p->h == h) return p;
    pv_pic_free(d, p);
    const VkDeviceSize size = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkMemoryRequirements req;
    int ok = d->CreateBuffer(d->device, &bci, NULL, &p->buf) == VK_SUCCESS;
    if (ok) d->GetBufferMemoryRequirements(d->device, p->buf, &req);
    int32_t type = ok ? memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : -1;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, ok ? req.size : 0, (uint32_t)type};
    ok = ok && type >= 0 && d->AllocateMemory(d->device, &mai, NULL, &p->bmem) == VK_SUCCESS;
    ok = ok && d->BindBufferMemory(d->device, p->buf, p->bmem, 0) == VK_SUCCESS;
    ok = ok && d->MapMemory(d->device, p->bmem, 0, size, 0, &p->mapped) == VK_SUCCESS;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    ok = ok && d->CreateImage(d->device, &ci, NULL, &p->img) == VK_SUCCESS;
    if (ok) d->GetImageMemoryRequirements(d->device, p->img, &req);
    type = ok ? memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : -1;
    VkMemoryAllocateInfo iai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, ok ? req.size : 0, (uint32_t)type};
    ok = ok && type >= 0 && d->AllocateMemory(d->device, &iai, NULL, &p->mem) == VK_SUCCESS;
    ok = ok && d->BindImageMemory(d->device, p->img, p->mem, 0) == VK_SUCCESS;
    if (!ok) { pv_pic_free(d, p); return NULL; }
    p->w = w;
    p->h = h;
    return p;
}

// The slot's staging buffer (filled by the caller) into its image, submitted without waiting.
static int pv_submit(QueueData* q, PvPic* p, uint64_t guid, const uint32_t bbox[4]) {
    DeviceData* d = q->dev;
    d->ResetCommandBuffer(p->cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    d->BeginCommandBuffer(p->cmd, &bi);
    sf_barrier(d, p->cmd, p->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {p->w, p->h, 1}};
    d->CmdCopyBufferToImage(p->cmd, p->buf, p->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    sf_barrier(d, p->cmd, p->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    d->EndCommandBuffer(p->cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &p->cmd, 0, NULL};
    d->ResetFences(d->device, 1, &p->fence);
    if (d->QueueSubmit(q->queue, 1, &si, p->fence) != VK_SUCCESS) return 0;
    pthread_mutex_lock(&g_pv.m);
    p->guid = guid;
    memcpy(p->bbox, bbox, sizeof p->bbox);
    p->made = ++g_pv.made;
    p->pending = 1;
    p->ready = 0;
    pthread_mutex_unlock(&g_pv.m);
    return 1;
}

// A whole small file into memory (NULL: none). The caller frees it.
static uint8_t* pv_slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t* b = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len > 0 && len < 32l * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)len + 1)) && fread(b, 1, (size_t)len, f) == (size_t)len) {
            b[len] = 0;
            *n = (size_t)len;
        } else {
            free(b);
            b = NULL;
        }
    }
    fclose(f);
    return b;
}

// The still of the stand-in's display (wowbody/<guid>.txt names it), uploaded once per request.
static void pv_still(QueueData* q, uint64_t guid, uint64_t now) {
    char path[640];
    size_t n = 0;
    snprintf(path, sizeof path, "%s/wowbody/%016llx.txt", link_dir(), (unsigned long long)guid);
    uint8_t* info = pv_slurp(path, &n);
    const char* at = info ? strstr((const char*)info, "display ") : NULL;
    unsigned display = at ? (unsigned)strtoul(at + 8, NULL, 10) : 0;
    free(info);
    if (!display) return;  // not written yet: tried again at the next present
    snprintf(path, sizeof path, "%s/wowbody/d%u.rgba", link_dir(), display);
    struct stat st;
    if (stat(path, &st) != 0) return;  // not baked yet: the stream brings it soon
    uint8_t* b = pv_slurp(path, &n);
    uint32_t hdr[8];
    const uint32_t w = b && n >= 32 ? ((uint32_t*)b)[1] : 0, h = b && n >= 32 ? ((uint32_t*)b)[2] : 0;
    if (!b || n < 32 || memcmp(b, "WBR2", 4) != 0 || !w || !h || w > PV_MAX || h > PV_MAX || n < 32 + (size_t)w * h * 4) {
        g_pv.still_guid = guid;  // a broken file: not again for this request
        free(b);
        return;
    }
    memcpy(hdr, b, sizeof hdr);
    PvPic* p = pv_slot(q, w, h, now);
    if (p) {
        memcpy(p->mapped, b + 32, (size_t)w * h * 4);
        if (pv_submit(q, p, guid, hdr + 4)) {
            g_pv.still_guid = guid;
            g_pv.stills++;
            logf_("preview: still of %016llx (display %u) shown", (unsigned long long)guid, display);
        }
    }
    free(b);
}

// The newest live frame, when it is new and the stand-in's.
static void pv_live(QueueData* q, uint64_t guid, uint64_t now) {
    if (!g_pv.live_path[0]) {
        char path[640];
        size_t n = 0;
        snprintf(path, sizeof path, "%s/wowbody/live.path", link_dir());
        uint8_t* b = pv_slurp(path, &n);
        if (b) { snprintf(g_pv.live_path, sizeof g_pv.live_path, "%s", (const char*)b); g_pv.live_path[strcspn(g_pv.live_path, "\r\n")] = 0; }
        free(b);
        if (!g_pv.live_path[0]) return;
    }
    int fd = open(g_pv.live_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { g_pv.live_path[0] = 0; return; }
    uint32_t hdr[16], after[16];
    if (pread(fd, hdr, sizeof hdr, 0) == (ssize_t)sizeof hdr && memcmp(hdr, "WBL1", 4) == 0) {
        const uint32_t w = hdr[1], h = hdr[2], slot = hdr[3], seq = hdr[4];
        uint64_t g;
        memcpy(&g, hdr + 6, sizeof g);
        PvPic* p = NULL;
        if (seq != g_pv.live_seq && g == guid && w && h && w <= PV_MAX && h <= PV_MAX && slot < 2 && (p = pv_slot(q, w, h, now))) {
            const size_t frame = (size_t)w * h * 4;
            // The writer fills the other slot before it moves the sequence: a frame read while the
            // sequence moved at most once is whole.
            if (pread(fd, p->mapped, frame, 64 + (off_t)slot * (off_t)frame) == (ssize_t)frame &&
                pread(fd, after, sizeof after, 0) == (ssize_t)sizeof after && after[4] - seq <= 1) {
                g_pv.live_seq = seq;
                if (pv_submit(q, p, guid, hdr + 8)) {
                    if (g_pv.live_guid != guid) logf_("preview: live frames of %016llx", (unsigned long long)guid);
                    g_pv.live_guid = guid;
                    g_pv.frames++;
                }
            }
        }
    }
    close(fd);
}

// At each present (g_lock held): finished uploads, the request when its file changed, the stand-in's
// still and newest live frame, and a log line every 300 presents while previews are handled.
static void pv_present(QueueData* q) {
    DeviceData* d = q ? q->dev : NULL;
    if (!d) return;
    const uint64_t now = g_req.presents;
    pthread_mutex_lock(&g_pv.m);
    for (int k = 0; k < PV_RING; k++) {
        PvPic* p = &g_pv.pic[k];
        if (p->pending && d->GetFenceStatus(d->device, p->fence) == VK_SUCCESS) { p->pending = 0; p->ready = 1; }
    }
    pthread_mutex_unlock(&g_pv.m);
    // The request, when its file changed; only one written since BG3 started counts (screen_check's rule).
    static time_t born;
    struct stat ps, fs;
    if (!born) born = stat("/proc/self", &ps) == 0 ? ps.st_mtime : time(NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/wowpreview.txt", link_dir());
    int have = stat(path, &fs) == 0 && fs.st_mtime >= born;
    if (have && (fs.st_mtim.tv_sec != g_pv.req_mt.tv_sec || fs.st_mtim.tv_nsec != g_pv.req_mt.tv_nsec)) {
        g_pv.req_mt = fs.st_mtim;
        char line[200] = {0}, screen[32] = {0}, hex[24] = {0};
        FILE* f = fopen(path, "r");
        if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
        unsigned count = 0, index = 0;
        int want = sscanf(line, "1 %31s %u %u %23s", screen, &count, &index, hex) == 4 && count >= 1 && count <= PV_IMGS && index < count;
        uint64_t g = want ? strtoull(hex, NULL, 16) : 0;
        want = want && g;
        if (want != g_pv.on || g != g_pv.guid) {
            if (want) logf_("preview: %s shows stand-in %016llx (preview %u of %u)", screen, (unsigned long long)g, index + 1, count);
            else if (g_pv.on) logf_("preview: no stand-in shown");
            if (g != g_pv.guid) g_pv.still_guid = g_pv.live_guid = 0;
        }
        pthread_mutex_lock(&g_pv.m);
        // A new screen (or none): bodies from an earlier one (the same stand-in turned another way,
        // say) are not shown; black until this one's still or first live frame.
        if (want != g_pv.on || g != g_pv.guid) {
            g_pv.req_made = g_pv.made;
            g_pv.mine_img = g_pv.other_img = 0;
        }
        __atomic_store_n(&g_pv.on, want, __ATOMIC_RELAXED);
        g_pv.guid = g;
        g_pv.count = count;
        g_pv.index = index;
        snprintf(g_pv.screen, sizeof g_pv.screen, "%s", screen);
        pthread_mutex_unlock(&g_pv.m);
    } else if (!have && g_pv.on) {
        pthread_mutex_lock(&g_pv.m);
        __atomic_store_n(&g_pv.on, 0, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g_pv.m);
    }
    if (g_pv.on) {
        if (g_pv.still_guid != g_pv.guid && g_pv.live_guid != g_pv.guid) pv_still(q, g_pv.guid, now);
        pv_live(q, g_pv.guid, now);
    }
    if (now - g_pv.last_log >= 300 && (g_pv.on || g_pv.drawn || g_pv.black)) {
        pthread_mutex_lock(&g_pv.m);
        logf_("preview: drawn %llu, black %llu, uploads %llu still + %llu live (previews seen %u, this one %u)",
              (unsigned long long)g_pv.drawn, (unsigned long long)g_pv.black, (unsigned long long)g_pv.stills,
              (unsigned long long)g_pv.frames, g_pv.last_live, g_pv.last_ord);
        g_pv.drawn = g_pv.black = g_pv.stills = g_pv.frames = 0;
        g_pv.last_log = now;
        pthread_mutex_unlock(&g_pv.m);
    }
}
