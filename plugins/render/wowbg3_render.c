// wowbg3_render.c: the WOW-BG3 capture layer as a bg3le plugin (plug-and-play plan
// "Late-hook rendering"; the render-plugin notes).
//
// The layer's own code (plugins/capture/wowbg3_capture.c and its .inc.c files, included below with
// WOWBG3_PLUGIN) runs unchanged: same hooks, same compositor, same files under /logs/capture. Only
// the way in differs. A Vulkan layer sits in the loader's chain from the instance on; this plugin
// arrives at ~1.7 s, after BG3 has its instance, device and swapchain, and gets in the SkyCraft way:
//  1. BG3 keeps its device functions in one table on its heap (it resolves ~200 names itself).
//     bg3le's exported vkQueuePresentKHR forwarder sits in exactly one slot of it; vkCreateImageView's
//     forwarder sits near it. Both slots are swapped at init (an atomic store each).
//  2. BG3's next image views hand us VkDevice handles; at the next present the one whose loader key is
//     the present queue's is BG3's renderer device (bg3le's memory probe makes and destroys a device
//     of its own at startup). Every Vulkan name BG3 carries is then resolved
//     (bg3le's vkGetDeviceProcAddr, and the loader's own exports for names BG3 took from
//     vkGetInstanceProcAddr) and matched against the table: each slot gets its name. The layer's
//     DeviceData is filled from the table's values (what BG3 itself calls), its queues from
//     vkGetDeviceQueue over every family, its physical device from our own small instance (matched
//     to BG3's device by a pipeline cache header: vendor, device and cache UUID).
//  3. Every slot the layer hooks is swapped to the layer's function, by name.
//  4. What existed before: the swapchain was made without the copy usage the compositor needs and
//     the layer never saw it, so the first present after the hooks answers VK_ERROR_OUT_OF_DATE_KHR
//     (after presenting): BG3 builds a new swapchain through the hooked vkCreateSwapchainKHR, which
//     adds TRANSFER_SRC|DST as the layer always did.
//
// Switch (render_switch in wowbg3_capture.c): env WOW_BG3_RENDER or /logs/capture/render, first
// word "plugin" (the layer, if loaded, only passes calls through), "layer" or "off" (this plugin
// stays out). Unset: this plugin draws only when the capture layer is not loaded.
// Harness only until proven on a desktop. Build: plugins/render/build.sh.
#define WOWBG3_PLUGIN 1
#include "../capture/wowbg3_capture.c"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdatomic.h>

#include "bg3le_plugin.h"
#include "wowbg3_vknames.h"

#define RENDER_VERSION "0.1.0"
#define NV (int)(sizeof VKNAMES / sizeof *VKNAMES)
#define SPAN 600  // slots searched each side of Present (BG3's table is ~155 slots)

static const bg3le_host* H;
static bg3le_plugin* S;

static void rlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void rlog(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    logf_("render: %s", b);
    if (H) H->log(S, "render: %s", b);
}

static int rd(const void* at, void* out, size_t n) {  // a read that cannot crash (mapping may go away)
    struct iovec lo = {out, n}, re = {(void*)at, n};
    return process_vm_readv(getpid(), &lo, 1, &re, 1, 0) == (ssize_t)n;
}

// ---------------------------------------------------------------- BG3's device table
static void** g_slot_present;
static uintptr_t g_reg_a, g_reg_b;  // the anonymous mapping holding it
static void* g_fwd_present;          // bg3le's forwarders (the table's original values)
static void* g_fwd_civ;
static void** g_slot_civ;
static void* g_loader;               // libvulkan.so.1
static PFN_vkGetDeviceProcAddr g_bg3le_gdpa;
static PFN_vkGetInstanceProcAddr g_loader_gipa;

typedef struct {
    int n;            // slots holding this name's function
    int32_t at[4];    // their index relative to Present
    void* orig;       // the value BG3 calls
    int ambiguous;    // the value also names another function (not an alias): never hooked
} Named;
static Named g_named[NV];
static void* g_win[2 * SPAN + 1];    // the table window as read at setup (our early hooks undone)

// More than one place in BG3's memory can hold bg3le's Present forwarder (one start of the release found
// four), and only one of them is the table BG3 calls through. Every such slot is a candidate; `grade` says
// how much of BG3's table its window looks like, and the choice is checked against the only proof there is,
// a present through our hook (see `watch_table`).
#define MAXC 32
typedef struct {
    void** slot;      // the slot holding bg3le's Present forwarder
    uintptr_t a, b;   // the mapping it sits in
    void** civ;       // bg3le's vkCreateImageView forwarder in the same window
    int civ_at;       // its distance from Present (BG3's own table keeps it 51 slots before)
    int named;        // how many of the Vulkan names BG3 carries are in the window
    int tried;        // hooked already (and answered no present)
} Cand;
static Cand g_cands[MAXC];
static int g_ncands;
static int g_cand_at = -1;  // the candidate whose slots are hooked now

typedef struct { void** slot; void* orig; void* ours; } Swap;
static Swap g_swaps[256];
static int g_nswaps;

static atomic_int g_state;  // 0 waiting for the device, 1 setting up, 2 hooked, -1 failed (pass-through)
static VkDevice g_cand[4];  // devices seen by vkCreateImageView before the setup
static pthread_mutex_t g_setup = PTHREAD_MUTEX_INITIALIZER;
static atomic_ulong g_presents;
static atomic_ulong g_civs;  // image views made through the hooked table (proof BG3 calls through it)
static int g_force;                 // swapchain re-creations asked (OUT_OF_DATE answers)
static unsigned long g_force_at;    // present index of the last one

// Our own instance: the physical device's properties for the layer (memory types, limits).
static VkInstance g_inst;
static VkPhysicalDevice g_phys[8];
static uint32_t g_nphys;

static int name_index(const char* name) {
    for (int k = 0; k < NV; k++) if (!strcmp(VKNAMES[k], name)) return k;
    return -1;
}
static size_t base_len(const char* n) {  // vkFooKHR and vkFoo are one function (aliases)
    size_t l = strlen(n);
    if (l > 3 && !strcmp(n + l - 3, "KHR")) l -= 3;
    return l;
}
static int aliases(const char* a, const char* b) {
    size_t la = base_len(a), lb = base_len(b);
    return la == lb && !strncmp(a, b, la);
}

// The layer's resolver in plugin mode: what BG3's table holds, else bg3le's vkGetDeviceProcAddr.
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL plugin_gdpa(VkDevice dev, const char* name) {
    int k = name_index(name);
    if (k >= 0 && g_named[k].n && !g_named[k].ambiguous) return (PFN_vkVoidFunction)g_named[k].orig;
    return g_bg3le_gdpa ? g_bg3le_gdpa(dev, name) : NULL;
}

static void name_slots(VkDevice dev) {
    for (int d = -SPAN; d <= SPAN; d++) {
        uintptr_t at = (uintptr_t)(g_slot_present + d);
        void* v = 0;
        if (at < g_reg_a || at + 8 > g_reg_b || !rd((void*)at, &v, 8)) v = 0;
        if (d == 0) v = g_fwd_present;                                   // our early hooks: the originals
        if (g_slot_civ && (void**)at == g_slot_civ) v = g_fwd_civ;
        g_win[d + SPAN] = v;
    }
    int found = 0, device_level = 0, tramp = 0;
    for (int k = 0; k < NV; k++) {
        Named* nk = &g_named[k];
        memset(nk, 0, sizeof *nk);
        void* f1 = g_bg3le_gdpa ? (void*)g_bg3le_gdpa(dev, VKNAMES[k]) : NULL;
        void* f2 = g_loader ? dlsym(g_loader, VKNAMES[k]) : NULL;  // the loader's trampoline
        if (f1) device_level++;
        if (!f1 && !f2) continue;
        for (int d = -SPAN; d <= SPAN && nk->n < 4; d++) {
            void* v = g_win[d + SPAN];
            if (!v || (v != f1 && v != f2)) continue;
            if (!nk->n) nk->orig = v;
            if (v == f2 && v != f1) tramp++;
            nk->at[nk->n++] = d;
        }
        if (nk->n) found++;
    }
    // A slot value shared by two different functions (a common stub) names neither.
    int amb = 0;
    for (int a = 0; a < NV; a++) {
        if (!g_named[a].n) continue;
        for (int b = 0; b < NV; b++) {
            if (a == b || !g_named[b].n || g_named[a].orig != g_named[b].orig) continue;
            if (!aliases(VKNAMES[a], VKNAMES[b])) { g_named[a].ambiguous = 1; amb++; }
        }
    }
    rlog("device %p: %d of %d names (%d device-level) found in BG3's table (%d through loader trampolines), %d ambiguous",
         (void*)dev, found, NV, device_level, tramp, amb);
    // Slots no name matched (for the record: what else BG3 keeps there).
    int shown = 0, unnamed = 0;
    for (int d = -SPAN; d <= SPAN; d++) {
        void* v = g_win[d + SPAN];
        if (!v) continue;
        int named = 0;
        for (int k = 0; k < NV && !named; k++)
            for (int j = 0; j < g_named[k].n; j++) named |= g_named[k].at[j] == d;
        if (named) continue;
        Dl_info di;
        if (!dladdr(v, &di) || !di.dli_fname) continue;  // not code
        unnamed++;
        if (shown++ < 12)
            rlog("  unnamed slot %+d: %p %s %s", d, v, di.dli_fname, di.dli_sname ? di.dli_sname : "?");
    }
    rlog("  %d unnamed code pointers in the window", unnamed);
    const char* show[] = {"vkQueueSubmit", "vkQueueSubmit2KHR", "vkCmdBeginRenderPass", "vkCmdDrawIndexed",
                          "vkCreateSwapchainKHR", "vkAllocateCommandBuffers", "vkGetDeviceQueue"};
    for (unsigned s = 0; s < sizeof show / sizeof *show; s++) {
        int k = name_index(show[s]);
        if (k < 0) continue;
        Dl_info di = {0};
        if (g_named[k].n) dladdr(g_named[k].orig, &di);
        rlog("  %s: %d slot(s), first %+d, %s", show[s], g_named[k].n, g_named[k].n ? g_named[k].at[0] : 0,
             g_named[k].n ? (di.dli_fname ? di.dli_fname : "?") : "-");
    }
}

// ---------------------------------------------------------------- our instance, BG3's physical device
// A symbol of the Vulkan loader itself, from its own dynamic symbol table. dlsym (and the loader's own
// vkGetInstanceProcAddr) hand back bg3le's interposed export for the names bg3le forwards, and through
// vkCreateInstance's bg3se's overlay would take our instance for the game's.
static void* g_lsym_name_obj;
static int lsym_cb(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    const char** want = data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libvulkan.so.1")) return 0;
    const ElfW(Dyn)* dyn = NULL;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    if (!dyn) return 0;
    const ElfW(Sym)* sym = NULL;
    const char* str = NULL;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_SYMTAB) sym = (const ElfW(Sym)*)dyn->d_un.d_ptr;
        if (dyn->d_tag == DT_STRTAB) str = (const char*)dyn->d_un.d_ptr;
    }
    if (!sym || !str || (const char*)sym >= str) return 0;
    size_t n = (size_t)(str - (const char*)sym) / sizeof *sym;  // the symbol table ends where the strings start
    for (size_t k = 1; k < n && k < 200000; k++)
        if (sym[k].st_shndx != SHN_UNDEF && ELF64_ST_TYPE(sym[k].st_info) == STT_FUNC && !strcmp(str + sym[k].st_name, want[0])) {
            g_lsym_name_obj = (void*)(info->dlpi_addr + sym[k].st_value);
            return 1;
        }
    return 0;
}
static void* loader_sym(const char* name) {
    const char* want[1] = {name};
    g_lsym_name_obj = NULL;
    dl_iterate_phdr(lsym_cb, want);
    return g_lsym_name_obj;
}

static int own_instance(void) {
    if (!g_loader_gipa) return 0;
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)loader_sym("vkCreateInstance");
    Dl_info di = {0};
    if (ci) dladdr((void*)ci, &di);
    rlog("loader's vkCreateInstance %p (%s)", (void*)ci, di.dli_fname ? di.dli_fname : "?");
    if (!ci) return 0;
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "wowbg3-render", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL};
    if (ci(&ii, NULL, &g_inst) != VK_SUCCESS) { rlog("own instance failed"); return 0; }
    PFN_vkEnumeratePhysicalDevices ep = (PFN_vkEnumeratePhysicalDevices)g_loader_gipa(g_inst, "vkEnumeratePhysicalDevices");
    g_nphys = 8;
    if (!ep || ep(g_inst, &g_nphys, g_phys) < 0) g_nphys = 0;
    rlog("own instance %p: %u physical devices", (void*)g_inst, g_nphys);
    return g_nphys > 0;
}

// BG3's physical device: a pipeline cache made on BG3's device carries vendor, device and cache
// UUID in its header (the spec's layout); one of ours matches. Through the loader's own functions,
// so bg3se's overlay (which wraps bg3le's vkCreatePipelineCache forwarder) never sees it.
static VkPhysicalDevice match_physical(VkDevice dev) {
    PFN_vkCreatePipelineCache cpc = (PFN_vkCreatePipelineCache)dlsym(g_loader, "vkCreatePipelineCache");
    PFN_vkGetPipelineCacheData gcd = (PFN_vkGetPipelineCacheData)dlsym(g_loader, "vkGetPipelineCacheData");
    PFN_vkDestroyPipelineCache dpc = (PFN_vkDestroyPipelineCache)dlsym(g_loader, "vkDestroyPipelineCache");
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)g_loader_gipa(g_inst, "vkGetPhysicalDeviceProperties");
    if (!cpc || !gcd || !dpc || !gpp) { rlog("pipeline cache functions missing"); return VK_NULL_HANDLE; }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, 0, NULL};
    VkPipelineCache pc;
    VkResult r = cpc(dev, &ci, NULL, &pc);
    if (r != VK_SUCCESS) { rlog("vkCreatePipelineCache: %d", (int)r); return VK_NULL_HANDLE; }
    size_t n = 0;
    r = gcd(dev, pc, &n, NULL);  // the whole size first (a short read may legally write nothing)
    uint8_t* data = (r == VK_SUCCESS && n >= 32 && n < (64u << 20)) ? malloc(n) : NULL;
    uint8_t hdr[32] = {0};
    if (data) {
        r = gcd(dev, pc, &n, data);
        if (r == VK_SUCCESS && n >= 32) memcpy(hdr, data, 32);
        free(data);
    }
    dpc(dev, pc, NULL);
    if (r != VK_SUCCESS || n < 32) { rlog("vkGetPipelineCacheData: %d, %zu bytes", (int)r, n); return VK_NULL_HANDLE; }
    uint32_t vendor, device;
    memcpy(&vendor, hdr + 8, 4); memcpy(&device, hdr + 12, 4);
    for (uint32_t i = 0; i < g_nphys; i++) {
        VkPhysicalDeviceProperties p;
        gpp(g_phys[i], &p);
        if (p.vendorID == vendor && p.deviceID == device && !memcmp(p.pipelineCacheUUID, hdr + 16, VK_UUID_SIZE)) {
            rlog("BG3 renders on %s (%04x:%04x)", p.deviceName, vendor, device);
            return g_phys[i];
        }
    }
    rlog("no physical device matches BG3's cache header %04x:%04x", vendor, device);
    return VK_NULL_HANDLE;
}

// ---------------------------------------------------------------- what existed before the plugin
// Render passes BG3 begins whose render pass, framebuffer, views or images the layer never saw made
// (made before the plugin loaded): counted per present window and logged, so it is measured, not
// guessed, whether the roles (scene depth, HUD) can be found without them.
static struct { uint64_t passes, rp, fb, view, img, swap; uint64_t total; } g_unk;
static uint64_t g_unk_seen[64];
static int g_unk_nseen;
static void unknown_note(uint64_t h, const char* what) {
    for (int i = 0; i < g_unk_nseen; i++) if (g_unk_seen[i] == h) return;
    if (g_unk_nseen < 64) {
        g_unk_seen[g_unk_nseen++] = h;
        logf_("render: unknown %s %llx used in a render pass (made before the plugin)", what, (unsigned long long)h);
    }
}
static void unknown_check(const VkRenderPassBeginInfo* info) {
    if (!g_img) return;
    trace_lock_timed();
    g_unk.total++;
    int any = 0;
    if (!TABLE_FIND(g_rp, RP_CAP, (uint64_t)info->renderPass, 0)) { g_unk.rp++; any = 1; unknown_note((uint64_t)info->renderPass, "render pass"); }
    FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)info->framebuffer, 0);
    if (!f) { g_unk.fb++; any = 1; unknown_note((uint64_t)info->framebuffer, "framebuffer"); }
    for (uint32_t k = 0; f && k < f->n; k++) {
        uint64_t img = view_image(f->views[k]);
        if (!img) { g_unk.view++; any = 1; unknown_note(f->views[k], "view"); continue; }
        if (!TABLE_FIND(g_img, IMG_CAP, img, 0)) {
            if (is_swapchain_image(img)) g_unk.swap++;
            else { g_unk.img++; any = 1; unknown_note(img, "image"); }
        }
    }
    if (any) g_unk.passes++;
    pthread_mutex_unlock(&g_trace_lock);
}
// Render passes made before the plugin (BG3 makes its render passes at startup and keeps them): the
// layer needs each one's attachment formats, load and store ops and initial and final layouts. Mesa's
// drivers keep exactly that in their render pass object (vk_render_pass: a count and a pointer to an
// array of attachments). Where in the object and the array is learnt from render passes the layer saw
// being made (every field must match in every sample), then read for the ones it did not see. Reads
// go through process_vm_readv; if nothing fits (another driver), those passes stay unknown as before.
static struct { int ok, tries; int o_cnt, o_ptr, S, ff, fl, fs, fi, fn; uint64_t recovered; } g_rpc;
static int rpc_read(uint64_t h, int o_cnt, int o_ptr, int S, uint32_t* n, uint8_t* arr, size_t cap) {
    uint64_t ptr = 0;
    if (!rd((void*)(uintptr_t)(h + o_cnt), n, 4) || !rd((void*)(uintptr_t)(h + o_ptr), &ptr, 8)) return 0;
    if (*n < 1 || *n > 8 || !ptr || (size_t)*n * S > cap) return 0;
    return rd((void*)(uintptr_t)ptr, arr, (size_t)*n * S);
}
static uint32_t u32at(const uint8_t* a, size_t off) { uint32_t v; memcpy(&v, a + off, 4); return v; }
// Mesa keeps separate depth and stencil layouts (vk_render_pass_attachment_init, Mesa 26.2: "We require
// separate stencil layouts"): a depth format's initial/final layout becomes its depth-only form, a
// stencil-only format's becomes UNDEFINED, and the stencil part sits 8 bytes further (initial_stencil_layout,
// final_stencil_layout). Comparing the app's layouts as they are never fitted (BG3's
// G-buffer and HUD passes carry depth and S8 attachments), so the calibration compares Mesa's forms and the
// recovery turns them back into the combined layouts BG3 uses.
static int rp_fmt_depth(VkFormat f) { return f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT && f != VK_FORMAT_S8_UINT; }
static int rp_fmt_stencil(VkFormat f) { return f >= VK_FORMAT_S8_UINT && f <= VK_FORMAT_D32_SFLOAT_S8_UINT; }
static uint32_t rp_depth_only(uint32_t l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL: return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    default: return l;
    }
}
// The layout as Mesa keeps it in initial_layout / final_layout.
static uint32_t rp_mesa_layout(VkFormat f, int32_t l) {
    if (rp_fmt_depth(f)) return rp_depth_only((uint32_t)l);
    if (rp_fmt_stencil(f)) return VK_IMAGE_LAYOUT_UNDEFINED;
    return (uint32_t)l;
}
// Back to the combined layout from Mesa's depth (d) and stencil (st) parts.
static int32_t rp_app_layout(VkFormat f, uint32_t d, uint32_t st) {
    int dep = rp_fmt_depth(f), sten = rp_fmt_stencil(f);
    if (!dep && !sten) return (int32_t)d;
    int da = d == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, dr = d == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    int sa = st == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL, sr = st == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
    if (dep && sten) {
        if (da && sa) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (dr && sr) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        if (dr && sa) return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
        if (da && sr) return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
        return (int32_t)d;
    }
    if (dep) return da ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : dr ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : (int32_t)d;
    return sa ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : sr ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : (int32_t)st;
}
static int rpc_fits(const RpInfo* r, int o_cnt, int o_ptr, int S, int ff, int fl, int fs, int fi, int fn) {
    uint32_t n = 0;
    uint8_t arr[8 * 128];
    if (!rpc_read(r->h, o_cnt, o_ptr, S, &n, arr, sizeof arr) || n != r->n) return 0;
    for (uint32_t k = 0; k < n; k++)
        // the layer keeps load and store ops as bytes (STORE_OP_NONE is 1000301000): compare those as bytes
        if (u32at(arr, k * S + ff) != (uint32_t)r->fmts[k] || (uint8_t)u32at(arr, k * S + fl) != r->load[k] ||
            (uint8_t)u32at(arr, k * S + fs) != r->store[k] || u32at(arr, k * S + fi) != rp_mesa_layout(r->fmts[k], r->init[k]) ||
            u32at(arr, k * S + fn) != rp_mesa_layout(r->fmts[k], r->final[k]))
            return 0;
    return 1;
}
static uint32_t w_load(const RpInfo* r, uint32_t k);
static uint32_t w_store(const RpInfo* r, uint32_t k);
static int rpc_field(const uint8_t* arr, uint32_t n, int S, int from, uint32_t (*want)(const RpInfo*, uint32_t), const RpInfo* r) {
    int byte = want == w_load || want == w_store;
    for (int f = from; f + 4 <= S; f += 4) {
        uint32_t k = 0;
        while (k < n && (byte ? (uint8_t)u32at(arr, k * S + f) : u32at(arr, k * S + f)) == want(r, k)) k++;
        if (k == n) return f;
    }
    return -1;
}
static uint32_t w_fmt(const RpInfo* r, uint32_t k) { return (uint32_t)r->fmts[k]; }
static uint32_t w_load(const RpInfo* r, uint32_t k) { return r->load[k]; }
static uint32_t w_store(const RpInfo* r, uint32_t k) { return r->store[k]; }
static uint32_t w_init(const RpInfo* r, uint32_t k) { return rp_mesa_layout(r->fmts[k], r->init[k]); }
static uint32_t w_final(const RpInfo* r, uint32_t k) { return rp_mesa_layout(r->fmts[k], r->final[k]); }
static void rpc_calibrate(void) {
    RpInfo smp[24];
    int ns = 0, multi = -1;
    trace_lock_timed();
    for (uint32_t i = 0; i < RP_CAP && ns < 24; i++)
        if (g_rp[i].h && g_rp[i].n >= 1) { smp[ns] = g_rp[i]; if (smp[ns].n >= 2 && multi < 0) multi = ns; ns++; }
    pthread_mutex_unlock(&g_trace_lock);
    if (ns < 4 || multi < 0) return;
    g_rpc.tries++;
    const RpInfo* r0 = &smp[multi];
    // Mesa's own layouts first (vk_render_pass_attachment: format, flags, aspects, samples, view_mask, load,
    // store, stencil load, stencil store, initial, final, stencil initial, stencil final, external format;
    // Mesa 25.0 without flags or external format, 48 bytes), then the field search below.
    static const int known[3][6] = {{56, 0, 20, 24, 36, 40}, {48, 0, 16, 20, 32, 36}, {52, 0, 16, 20, 32, 36}};
    for (int kk = 0; kk < 3 && !g_rpc.ok; kk++)
        for (int o_cnt = 8; o_cnt < 256 && !g_rpc.ok; o_cnt += 4)
            for (int o_ptr = 8; o_ptr < 256 && !g_rpc.ok; o_ptr += 8) {
                const int* L = known[kk];
                int all = 1;
                for (int k = 0; k < ns && all; k++) all = rpc_fits(&smp[k], o_cnt, o_ptr, L[0], L[1], L[2], L[3], L[4], L[5]);
                if (!all) continue;
                g_rpc.o_cnt = o_cnt; g_rpc.o_ptr = o_ptr; g_rpc.S = L[0];
                g_rpc.ff = L[1]; g_rpc.fl = L[2]; g_rpc.fs = L[3]; g_rpc.fi = L[4]; g_rpc.fn = L[5];
                g_rpc.ok = 1;
            }
    for (int o_cnt = 8; o_cnt < 256 && !g_rpc.ok; o_cnt += 4) {
        uint32_t n = 0;
        if (!rd((void*)(uintptr_t)(r0->h + o_cnt), &n, 4) || n != r0->n) continue;
        for (int o_ptr = 8; o_ptr < 256 && !g_rpc.ok; o_ptr += 8) {
            uint64_t ptr = 0;
            if (!rd((void*)(uintptr_t)(r0->h + o_ptr), &ptr, 8) || ptr < 0x10000) continue;
            uint8_t arr[8 * 128];
            if (!rd((void*)(uintptr_t)ptr, arr, (size_t)n * 16)) continue;
            for (int S = 16; S <= 128 && !g_rpc.ok; S += 4) {
                if (!rd((void*)(uintptr_t)ptr, arr, (size_t)n * S)) break;
                int ff = rpc_field(arr, n, S, 0, w_fmt, r0);
                int fl = ff < 0 ? -1 : rpc_field(arr, n, S, ff + 4, w_load, r0);
                int fs = fl < 0 ? -1 : rpc_field(arr, n, S, fl + 4, w_store, r0);
                int fi = fs < 0 ? -1 : rpc_field(arr, n, S, fs + 4, w_init, r0);
                int fn = fi < 0 ? -1 : rpc_field(arr, n, S, fi + 4, w_final, r0);
                if (fn < 0) continue;
                int all = 1;
                for (int k = 0; k < ns && all; k++) all = rpc_fits(&smp[k], o_cnt, o_ptr, S, ff, fl, fs, fi, fn);
                if (!all) continue;
                g_rpc.o_cnt = o_cnt; g_rpc.o_ptr = o_ptr; g_rpc.S = S;
                g_rpc.ff = ff; g_rpc.fl = fl; g_rpc.fs = fs; g_rpc.fi = fi; g_rpc.fn = fn;
                g_rpc.ok = 1;
            }
        }
    }
    if (g_rpc.ok)
        rlog("render passes made before the plugin: the driver's object read at count +%d, attachments *(+%d), %d bytes each"
             " (format +%d, load +%d, store +%d, initial +%d, final +%d), checked on %d render passes made since",
             g_rpc.o_cnt, g_rpc.o_ptr, g_rpc.S, g_rpc.ff, g_rpc.fl, g_rpc.fs, g_rpc.fi, g_rpc.fn, ns);
    else if (g_rpc.tries <= 3) {
        rlog("render passes made before the plugin: no layout fits the %d known ones yet", ns);
        if (g_rpc.tries == 1) {  // for working out the driver's layout by hand
            uint8_t obj[160] = {0};
            rd((void*)(uintptr_t)r0->h, obj, sizeof obj);
            char hx[400];
            for (int line = 0; line < 160; line += 64) {
                int o = 0;
                for (int b = line; b < line + 64 && b < 160; b += 8) { uint64_t q; memcpy(&q, obj + b, 8); o += snprintf(hx + o, sizeof hx - o, " %llx", (unsigned long long)q); }
                rlog("  sample rp %llx (%u att: fmt %d/%d load %u/%u store %u/%u init %d/%d final %d/%d) +%d:%s",
                     (unsigned long long)r0->h, r0->n, (int)r0->fmts[0], (int)r0->fmts[1], r0->load[0], r0->load[1], r0->store[0], r0->store[1],
                     r0->init[0], r0->init[1], r0->final[0], r0->final[1], line, hx);
            }
        }
    }
}
static void rpc_recover(VkRenderPass rp) {
    if (!g_img || !rp) return;
    trace_lock_timed();
    int known = TABLE_FIND(g_rp, RP_CAP, (uint64_t)rp, 0) != NULL;
    pthread_mutex_unlock(&g_trace_lock);
    if (known || !g_rpc.ok) return;
    uint32_t n = 0;
    uint8_t arr[8 * 128];
    if (!rpc_read((uint64_t)rp, g_rpc.o_cnt, g_rpc.o_ptr, g_rpc.S, &n, arr, sizeof arr)) return;
    VkFormat f[8]; uint8_t l[8], st[8]; int32_t fl[8], il[8];
    for (uint32_t k = 0; k < n; k++) {
        f[k] = (VkFormat)u32at(arr, k * g_rpc.S + g_rpc.ff);
        uint32_t lo = u32at(arr, k * g_rpc.S + g_rpc.fl), so = u32at(arr, k * g_rpc.S + g_rpc.fs);
        // the stencil parts 8 bytes after the depth parts (Mesa's struct), when the record is that big
        uint32_t si = g_rpc.fi + 12 <= g_rpc.S ? u32at(arr, k * g_rpc.S + g_rpc.fi + 8) : 0;
        uint32_t sf = g_rpc.fn + 12 <= g_rpc.S ? u32at(arr, k * g_rpc.S + g_rpc.fn + 8) : 0;
        il[k] = rp_app_layout(f[k], u32at(arr, k * g_rpc.S + g_rpc.fi), si);
        fl[k] = rp_app_layout(f[k], u32at(arr, k * g_rpc.S + g_rpc.fn), sf);
        if ((uint32_t)f[k] > 1000300000u * 2 || (lo > 3 && lo != 1000400000u) || (so > 3 && so != 1000301000u)) return;  // not a render pass
        l[k] = (uint8_t)lo; st[k] = (uint8_t)so;  // as the layer keeps them (remember_rp: bytes)
    }
    remember_rp(rp, n, f, l, st, fl, il);
    if (++g_rpc.recovered <= 3 || g_rpc.recovered % 50 == 0)
        rlog("render pass %llx recovered from the driver (%u attachments, first format %d final %d); %llu so far",
             (unsigned long long)(uint64_t)rp, n, (int)f[0], fl[0], (unsigned long long)g_rpc.recovered);
}
// Buffers made before the plugin. BG3 makes its constant rings at startup, so the
// layer never saw them bound or their memory mapped, and the ground injection found no camera in BG3's
// per-view block ("ground: ... without BG3's camera ... unmapped"). RADV keeps what is needed, laid out by
// Mesa version (vk_buffer {vk_object_base, create_flags, size +72, usage +80, ...}):
//   Mesa 26.x: vk_buffer gains device_address +88; radv_buffer {vk, winsys bo +104, range}: a mapped
//              buffer's bytes are at bo->cpu_map + (device_address - bo->va).
//   Mesa 25.0: radv_buffer {vk, winsys bo +88, offset +96, bo_va +104 (= bo->va), range}: bytes at
//              bo->cpu_map + offset (Wolf's Steam container, Ubuntu 25.04: the 26.x read found no camera).
// The bo {va +0, size +8, ..., cpu_map}. Which layout, and the bo's cpu_map, are checked on buffers the plugin
// makes itself; until every sample fits, nothing is read (g_buf_recover stays unset).
enum { BRC_ADDRESS, BRC_OFFSET };
static struct { int ok, tries, kind; int o_size, o_da, o_bo, o_va, o_map, o_off, o_bova; uint64_t recovered, unmapped; } g_brc;
static int brc_read(uint64_t h, int o_map, const uint8_t** p, uint64_t* size) {
    uint64_t sz = 0, bo = 0, va = 0, map = 0, at = 0;
    if (!rd((void*)(uintptr_t)(h + g_brc.o_size), &sz, 8) || !rd((void*)(uintptr_t)(h + g_brc.o_bo), &bo, 8)) return 0;
    if (bo < 0x10000 || !rd((void*)(uintptr_t)(bo + g_brc.o_va), &va, 8) || !rd((void*)(uintptr_t)(bo + o_map), &map, 8)) return 0;
    if (!sz || sz > (1ull << 32)) return 0;
    if (g_brc.kind == BRC_ADDRESS) {
        uint64_t da = 0;
        if (!rd((void*)(uintptr_t)(h + g_brc.o_da), &da, 8) || da < va || da - va > (1ull << 34)) return 0;
        at = da - va;
    } else {
        uint64_t bova = 0, bsz = 0;
        if (!rd((void*)(uintptr_t)(h + g_brc.o_off), &at, 8) || !rd((void*)(uintptr_t)(h + g_brc.o_bova), &bova, 8) ||
            !rd((void*)(uintptr_t)(bo + 8), &bsz, 8))
            return 0;
        if (bova != va || at > bsz || sz > bsz - at) return 0;
    }
    *size = sz;
    *p = map ? (const uint8_t*)(uintptr_t)(map + at) : NULL;
    return 1;
}
static const uint8_t* brc_recover(uint64_t buffer, uint64_t* size) {
    const uint8_t* p = NULL;
    if (!g_brc.ok || !brc_read(buffer, g_brc.o_map, &p, size)) return NULL;
    if (!p) { g_brc.unmapped++; return NULL; }
    if (++g_brc.recovered <= 3 || g_brc.recovered % 100 == 0)
        rlog("buffer %llx recovered from the driver (%llu bytes, mapped); %llu so far, %llu not mapped",
             (unsigned long long)buffer, (unsigned long long)*size, (unsigned long long)g_brc.recovered, (unsigned long long)g_brc.unmapped);
    return p;
}
static void brc_calibrate(void) {
    // BG3 maps its memory at startup, so the layer has no buffers of BG3's own to learn from: the plugin
    // makes two small host-visible buffers of its own (gbuf_make: the driver's functions, BG3's device)
    // and finds where RADV keeps what it needs on those.
    static GBuf own[2];
    static const struct { const char* mesa; int kind, o_bo, o_da, o_off, o_bova; } L[2] = {
        {"26.x", BRC_ADDRESS, 104, 88, 0, 0},
        {"25.0", BRC_OFFSET, 88, 0, 96, 104},
    };
    DeviceData* d = &g_devices[0];
    if (!d->device) return;
    g_brc.tries++;
    for (int k = 0; k < 2; k++)
        if (!own[k].buf && !gbuf_make(d, &own[k], 4096u << k, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) return;
    if (!own[0].map || !own[1].map) return;
    int which = -1;
    for (int l = 0; l < 2 && !g_brc.ok; l++) {
        g_brc.kind = L[l].kind; g_brc.o_size = 72; g_brc.o_bo = L[l].o_bo; g_brc.o_da = L[l].o_da;
        g_brc.o_off = L[l].o_off; g_brc.o_bova = L[l].o_bova; g_brc.o_va = 0;
        for (int o_map = 40; o_map <= 200 && !g_brc.ok; o_map += 8) {
            int all = 1;
            for (int k = 0; k < 2 && all; k++) {
                const uint8_t* p = NULL;
                uint64_t sz = 0;
                all = brc_read((uint64_t)(uintptr_t)own[k].buf, o_map, &p, &sz) && p == (const uint8_t*)own[k].map && sz >= (4096u << k);
            }
            if (all) { g_brc.o_map = o_map; g_brc.ok = 1; which = l; }
        }
    }
    if (g_brc.ok) {
        if (L[which].kind == BRC_ADDRESS)
            rlog("buffers made before the plugin: the driver's object read, Mesa %s layout (size +72, address +%d, bo +%d;"
                 " bo va +0, cpu map +%d), checked on two buffers of the plugin's own", L[which].mesa, g_brc.o_da, g_brc.o_bo, g_brc.o_map);
        else
            rlog("buffers made before the plugin: the driver's object read, Mesa %s layout (size +72, bo +%d, offset +%d,"
                 " bo va +%d; bo va +0, cpu map +%d), checked on two buffers of the plugin's own", L[which].mesa, g_brc.o_bo,
                 g_brc.o_off, g_brc.o_bova, g_brc.o_map);
        g_buf_recover = brc_recover;
    } else if (g_brc.tries <= 3) {
        uint64_t q[16] = {0};
        rd((void*)(uintptr_t)own[0].buf, q, sizeof q);
        rlog("buffers made before the plugin: no layout fits (own buffer %p mapped at %p; object +64..: %llx %llx %llx %llx %llx %llx)",
             (void*)own[0].buf, own[0].map, (unsigned long long)q[8], (unsigned long long)q[9], (unsigned long long)q[10],
             (unsigned long long)q[11], (unsigned long long)q[12], (unsigned long long)q[13]);
    }
}
// Shader modules made before the plugin. Mesa keeps the SPIR-V in
// its module object (vk_shader_module, Mesa 26.2: vk_object_base, nir pointer, 32-byte blake3 hash, u32 size,
// then the code: size at +104, code at +108). The offset of the size is found on a module the plugin makes
// itself from SPIR-V it knows (the ground pass's vertex shader); the code must follow it and match.
static struct { int ok, tried; int o_size; uint64_t recovered, refused; } g_mrc;
static pthread_mutex_t g_mrc_lock = PTHREAD_MUTEX_INITIALIZER;
static void mrc_calibrate(void) {
    DeviceData* d = &g_devices[0];
    if (!d->device || !d->CreateShaderModule) return;
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof wowbg3_ground_vert, wowbg3_ground_vert};
    VkShaderModule m = VK_NULL_HANDLE;
    if (d->CreateShaderModule(d->device, &ci, NULL, &m) != VK_SUCCESS || !m) return;
    uint8_t obj[256];
    if (rd((void*)(uintptr_t)m, obj, sizeof obj)) {
        for (int o = 64; o + 4 + 64 <= (int)sizeof obj && !g_mrc.ok; o += 4) {
            uint32_t sz;
            memcpy(&sz, obj + o, 4);
            if (sz == sizeof wowbg3_ground_vert && !memcmp(obj + o + 4, wowbg3_ground_vert, 64)) { g_mrc.o_size = o; g_mrc.ok = 1; }
        }
    }
    if (d->DestroyShaderModule) d->DestroyShaderModule(d->device, m, NULL);
    if (g_mrc.ok) rlog("shader modules made before the plugin: the driver's object read (size +%d, SPIR-V +%d), checked on one of its own",
                       g_mrc.o_size, g_mrc.o_size + 4);
    else rlog("shader modules made before the plugin: no layout fits (module %p)", (void*)m);
}
static uint32_t* mrc_recover(uint64_t module, size_t* size) {
    pthread_mutex_lock(&g_mrc_lock);
    if (!g_mrc.tried) { g_mrc.tried = 1; mrc_calibrate(); }
    pthread_mutex_unlock(&g_mrc_lock);
    if (!g_mrc.ok) return NULL;
    uint32_t sz = 0, head[2] = {0, 0};
    if (!rd((void*)(uintptr_t)(module + g_mrc.o_size), &sz, 4) || sz < 20 || sz > (16u << 20) || (sz & 3) ||
        !rd((void*)(uintptr_t)(module + g_mrc.o_size + 4), head, 8) || head[0] != 0x07230203u) {
        g_mrc.refused++;
        return NULL;
    }
    uint32_t* code = malloc(sz);
    if (!code || !rd((void*)(uintptr_t)(module + g_mrc.o_size + 4), code, sz)) { free(code); g_mrc.refused++; return NULL; }
    *size = sz;
    if (++g_mrc.recovered <= 3 || g_mrc.recovered % 500 == 0)
        rlog("shader module %llx recovered from the driver (%u bytes of SPIR-V); %llu so far, %llu refused",
             (unsigned long long)module, sz, (unsigned long long)g_mrc.recovered, (unsigned long long)g_mrc.refused);
    return code;
}
// Pipeline and descriptor set layouts made before the plugin: without
// them a pipeline's recipe misses bindings (recipe_whole leaves it out: BG3's sight overlay among them). RADV
// keeps both: radv_pipeline_layout (Mesa 26.2: vk_object_base, set[32] of {layout pointer, dynamic offset
// start}, num_sets) and radv_descriptor_set_layout (binding_count, then binding[] by binding number: type,
// array_size, ..., immutable_samplers_offset). The offsets are found on layouts the plugin makes itself; a
// set layout with immutable samplers, or with more than 32 bindings, is not read back.
static struct {
    int ok, tried;
    int pl_sets, pl_stride, pl_n;   // radv_pipeline_layout: set pointers, their stride, num_sets
    int dsl_n, dsl_b, dsl_stride, dsl_imm;  // radv_descriptor_set_layout: binding_count, binding[0], stride, imm offset
    uint64_t pls, dsls, refused;
} g_lay;
static pthread_mutex_t g_lay_lock = PTHREAD_MUTEX_INITIALIZER;
static int rd_some(uint64_t at, uint8_t* out, size_t n) {  // n bytes, else the first half (the object may end a mapping)
    if (rd((void*)(uintptr_t)at, out, n)) return (int)n;
    memset(out, 0, n);
    return rd((void*)(uintptr_t)at, out, n / 2) ? (int)(n / 2) : 0;
}
static uint64_t u64at(const uint8_t* p, int o) { uint64_t v; memcpy(&v, p + o, 8); return v; }
static void lay_calibrate(void) {
    DeviceData* d = &g_devices[0];
    if (!d->device || !d->CreateDescriptorSetLayout || !d->CreatePipelineLayout || !d->CreateSampler) return;
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .maxLod = 1.0f};
    VkSampler smp[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (d->CreateSampler(d->device, &sci, NULL, &smp[0]) != VK_SUCCESS) return;
    smp[1] = smp[2] = smp[0];
    static const uint32_t ta[4] = {6, 1, 7, 2}, ca[4] = {1, 3, 2, 5};  // uniform, combined, storage, sampled
    VkDescriptorSetLayoutBinding ba[4], bc[4];
    for (uint32_t i = 0; i < 4; i++) {
        ba[i] = (VkDescriptorSetLayoutBinding){i, (VkDescriptorType)ta[i], ca[i], VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        bc[i] = ba[i];
    }
    bc[1].pImmutableSamplers = smp;
    VkDescriptorSetLayoutBinding bb[3] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                          {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
                                          {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, ba};
    VkDescriptorSetLayout A = VK_NULL_HANDLE, B = VK_NULL_HANDLE, C = VK_NULL_HANDLE;
    if (d->CreateDescriptorSetLayout(d->device, &li, NULL, &A) != VK_SUCCESS) return;
    li.bindingCount = 3; li.pBindings = bb;
    if (d->CreateDescriptorSetLayout(d->device, &li, NULL, &B) != VK_SUCCESS) return;
    li.bindingCount = 4; li.pBindings = bc;
    if (d->CreateDescriptorSetLayout(d->device, &li, NULL, &C) != VK_SUCCESS) return;
    uint8_t oa[768], ob[768], oc[768];
    int na = rd_some((uint64_t)A, oa, sizeof oa), nb = rd_some((uint64_t)B, ob, sizeof ob), nc = rd_some((uint64_t)C, oc, sizeof oc);
    int lim = na < nb ? na : nb;
    if (nc < lim) lim = nc;
    int found = 0, fb = 0, fs = 0;
    for (int S = 16; S <= 64; S += 4)
        for (int o = 64; o + 6 * S <= lim; o += 4) {
            int okk = 1;
            for (int i = 0; i < 4 && okk; i++)
                okk = u32at(oa, o + i * S) == ta[i] && u32at(oa, o + i * S + 4) == ca[i] &&
                      u32at(oc, o + i * S) == ta[i] && u32at(oc, o + i * S + 4) == ca[i];
            okk = okk && u32at(ob, o) == 2 && u32at(ob, o + 4) == 4 && u32at(ob, o + 2 * S) == 6 && u32at(ob, o + 2 * S + 4) == 1 &&
                  u32at(ob, o + 5 * S) == 3 && u32at(ob, o + 5 * S + 4) == 6 && u32at(ob, o + S + 4) == 0 &&
                  u32at(ob, o + 3 * S + 4) == 0 && u32at(ob, o + 4 * S + 4) == 0;
            if (okk) { found++; fb = o; fs = S; }
        }
    int fn = -1, fi = -1;
    if (found == 1) {
        for (int o = 64; o < fb; o += 4) if (u32at(oa, o) == 4 && u32at(ob, o) == 6 && u32at(oc, o) == 4) fn = o;
        for (int o = 8; o < fs; o += 4) {
            int okk = u32at(oc, fb + fs + o) != 0;
            for (int i = 0; i < 4 && okk; i++) okk = u32at(oa, fb + i * fs + o) == 0 && (i == 1 || u32at(oc, fb + i * fs + o) == 0);
            if (okk && fi < 0) fi = o;
        }
    }
    // pipeline layouts: [A, B] and [B, A, C]
    VkDescriptorSetLayout s1[2] = {A, B}, s2[3] = {B, A, C};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, s1, 0, NULL};
    VkPipelineLayout P1 = VK_NULL_HANDLE, P2 = VK_NULL_HANDLE;
    int fps = -1, fpst = 0, fpn = -1;
    if (d->CreatePipelineLayout(d->device, &pi, NULL, &P1) == VK_SUCCESS) {
        pi.setLayoutCount = 3; pi.pSetLayouts = s2;
        if (d->CreatePipelineLayout(d->device, &pi, NULL, &P2) == VK_SUCCESS) {
            uint8_t p1[1024], p2[1024];
            int n1 = rd_some((uint64_t)P1, p1, sizeof p1), n2 = rd_some((uint64_t)P2, p2, sizeof p2);
            int pl = n1 < n2 ? n1 : n2;
            for (int o = 64; o + 24 <= pl && fps < 0; o += 8)
                for (int st = 8; st <= 16 && fps < 0; st += 8)
                    if (o + 3 * st <= pl && u64at(p1, o) == (uint64_t)A && u64at(p1, o + st) == (uint64_t)B && u64at(p2, o) == (uint64_t)B &&
                        u64at(p2, o + st) == (uint64_t)A && u64at(p2, o + 2 * st) == (uint64_t)C) { fps = o; fpst = st; }
            for (int o = fps + 3 * fpst; fps >= 0 && o + 4 <= pl && fpn < 0; o += 4)
                if (u32at(p1, o) == 2 && u32at(p2, o) == 3) fpn = o;
        }
    }
    g_lay.ok = found == 1 && fn >= 0 && fi >= 0 && fps >= 0 && fpn >= 0;
    g_lay.dsl_b = fb; g_lay.dsl_stride = fs; g_lay.dsl_n = fn; g_lay.dsl_imm = fi;
    g_lay.pl_sets = fps; g_lay.pl_stride = fpst; g_lay.pl_n = fpn;
    rlog("layouts made before the plugin: %s (set layout: count +%d, bindings +%d every %d, immutable samplers +%d; pipeline layout: "
         "sets +%d every %d, count +%d; %d fits)", g_lay.ok ? "the driver's object read" : "no layout fits", fn, fb, fs, fi, fps, fpst, fpn, found);
}
static void lay_refuse(uint64_t h, const char* why, uint32_t v) {
    g_lay.refused++;
    if (g_lay.refused <= 4 || !(g_lay.refused & (g_lay.refused - 1)))
        rlog("layout %llx not read back: %s %u (%llu refused so far)", (unsigned long long)h, why, v, (unsigned long long)g_lay.refused);
}
static int lay_dsl(uint64_t h) {  // g_fxl_lock held
    if (!g_dsl) g_dsl = calloc(DSL_CAP, sizeof *g_dsl);
    if (!g_dsl || TABLE_FIND(g_dsl, DSL_CAP, h, 0)) return g_dsl != NULL;
    uint32_t n = 0;
    // binding numbers up to 63 (BG3 has a set with 33); the first DSL_SLOTS present ones are kept, as the layer does
    if (!rd((void*)(uintptr_t)(h + g_lay.dsl_n), &n, 4) || n > 64) { lay_refuse(h, "binding count", n); return 0; }  // 0: an empty set
    uint8_t b[64 * 64];
    if (!rd((void*)(uintptr_t)(h + g_lay.dsl_b), b, (size_t)n * g_lay.dsl_stride)) { lay_refuse(h, "bindings unreadable", n); return 0; }
    DslInfo tmp;
    memset(&tmp, 0, sizeof tmp);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t type = u32at(b, (int)i * g_lay.dsl_stride), count = u32at(b, (int)i * g_lay.dsl_stride + 4);
        if (!count) continue;
        if (type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) { lay_refuse(h, "descriptor type", type); return 0; }
        if (u32at(b, (int)i * g_lay.dsl_stride + g_lay.dsl_imm)) { lay_refuse(h, "immutable samplers at binding", i); return 0; }
        if (tmp.n >= DSL_SLOTS) break;
        tmp.b[tmp.n].binding = i; tmp.b[tmp.n].type = type; tmp.b[tmp.n].count = count;
        tmp.n++;
    }
    DslInfo* l = TABLE_FIND(g_dsl, DSL_CAP, h, 1);
    if (!l) return 0;
    l->n = tmp.n;
    memcpy(l->b, tmp.b, sizeof tmp.b);
    l->rec = 1;
    g_lay.dsls++;
    return 1;
}
static int lay_recover(uint64_t layout) {  // g_layout_recover (wowbg3_fxcap.inc.c), g_fxl_lock held
    pthread_mutex_lock(&g_lay_lock);
    if (!g_lay.tried) { g_lay.tried = 1; lay_calibrate(); }
    pthread_mutex_unlock(&g_lay_lock);
    if (!g_lay.ok || !layout) return 0;
    if (!g_pl) g_pl = calloc(PL_CAP, sizeof *g_pl);
    if (!g_pl) return 0;
    PlInfo* p = TABLE_FIND(g_pl, PL_CAP, layout, 0);
    uint64_t sets[8] = {0};
    uint32_t nsets = 0;
    if (p) {
        nsets = p->nsets;
        memcpy(sets, p->sets, sizeof sets);
    } else {
        uint8_t raw[8 * 16];
        if (!rd((void*)(uintptr_t)(layout + g_lay.pl_n), &nsets, 4) || nsets > 8 ||
            !rd((void*)(uintptr_t)(layout + g_lay.pl_sets), raw, (size_t)8 * g_lay.pl_stride)) { lay_refuse(layout, "pipeline layout sets", nsets); return 0; }
        for (uint32_t i = 0; i < nsets; i++) sets[i] = u64at(raw, (int)i * g_lay.pl_stride);
    }
    for (uint32_t i = 0; i < nsets; i++) if (sets[i] && !lay_dsl(sets[i])) return 0;
    if (!p) {
        p = TABLE_FIND(g_pl, PL_CAP, layout, 1);
        if (!p) return 0;
        p->nsets = nsets; memcpy(p->sets, sets, sizeof p->sets); p->npush = 0; p->rec = 1;
        g_lay.pls++;
        if (g_lay.pls <= 3 || g_lay.pls % 200 == 0)
            rlog("pipeline layout %llx read back from the driver (%u sets); %llu so far, %llu set layouts, %llu refused",
                 (unsigned long long)layout, nsets, (unsigned long long)g_lay.pls, (unsigned long long)g_lay.dsls,
                 (unsigned long long)g_lay.refused);
    }
    return 1;
}
static VKAPI_ATTR void VKAPI_CALL hk_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents c) {
    rpc_recover(info->renderPass);
    unknown_check(info);
    layer_CmdBeginRenderPass(cb, info, c);
}
static VKAPI_ATTR void VKAPI_CALL hk_begin_rp2(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* sb) {
    rpc_recover(info->renderPass);
    unknown_check(info);
    layer_CmdBeginRenderPass2(cb, info, sb);
}

// ---------------------------------------------------------------- hooks
static VKAPI_ATTR VkResult VKAPI_CALL hk_present(VkQueue q, const VkPresentInfoKHR* info);
static VKAPI_ATTR VkResult VKAPI_CALL hk_civ(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* v);
static VKAPI_ATTR void VKAPI_CALL hk_destroy_device(VkDevice device, const VkAllocationCallbacks* alloc);

typedef struct { const char* name; void* fn; } Hook;
#define HK(n) {"vk" #n, (void*)layer_##n}
static const Hook HOOKS[] = {
    {"vkDestroyDevice", (void*)hk_destroy_device}, {"vkQueuePresentKHR", (void*)hk_present},
    HK(GetDeviceQueue), HK(GetDeviceQueue2), HK(CreateSwapchainKHR), HK(DestroySwapchainKHR),
    HK(CreateImage), HK(CreateImageView), HK(CreateFramebuffer), HK(CreateRenderPass),
    HK(CreateRenderPass2), HK(CreateRenderPass2KHR), HK(BeginCommandBuffer), HK(EndCommandBuffer),
    {"vkCmdBeginRenderPass", (void*)hk_begin_rp}, {"vkCmdBeginRenderPass2", (void*)hk_begin_rp2},
    {"vkCmdBeginRenderPass2KHR", (void*)hk_begin_rp2}, HK(CmdEndRenderPass), HK(CmdEndRenderPass2),
    HK(CmdBeginRendering), HK(CmdBeginRenderingKHR), HK(CmdEndRendering), HK(CmdEndRenderingKHR),
    HK(CmdDraw), HK(CmdDrawIndexed), HK(CmdDrawIndirect), HK(CmdDrawIndexedIndirect), HK(CmdDispatch), HK(CmdDispatchIndirect),
    HK(CmdCopyImage), HK(CmdBlitImage), HK(CmdResolveImage), HK(CmdExecuteCommands),
    HK(QueueSubmit), HK(QueueSubmit2), HK(QueueSubmit2KHR),
    HK(CmdPipelineBarrier), HK(CmdPipelineBarrier2), HK(CmdPipelineBarrier2KHR), HK(DestroyImage),
    HK(CreateBuffer), HK(BindBufferMemory), HK(BindBufferMemory2), HK(BindBufferMemory2KHR), HK(DestroyBuffer),
    HK(CmdBindIndexBuffer), HK(CreateGraphicsPipelines), HK(CreateComputePipelines), HK(CmdBindPipeline),
    HK(CmdBindVertexBuffers), HK(CmdBindDescriptorSets), HK(CmdPushConstants), HK(MapMemory), HK(UnmapMemory),
    HK(UpdateDescriptorSets), HK(UpdateDescriptorSetWithTemplate), HK(CreateDescriptorUpdateTemplate),
    HK(CreateShaderModule), HK(CreateSampler), HK(CreateDescriptorSetLayout), HK(CreatePipelineLayout),
    HK(CmdSetViewport), HK(CmdSetScissor),
    HK(AllocateDescriptorSets), HK(FreeDescriptorSets), HK(ResetDescriptorPool), HK(DestroyDescriptorPool),
};
#undef HK

static void swap_slot(void** slot, void* orig, void* ours) {
    for (int i = 0; i < g_nswaps; i++) if (g_swaps[i].slot == slot) { g_swaps[i].ours = ours; __atomic_store_n(slot, ours, __ATOMIC_SEQ_CST); return; }
    if (g_nswaps >= (int)(sizeof g_swaps / sizeof *g_swaps)) return;
    g_swaps[g_nswaps++] = (Swap){slot, orig, ours};
    __atomic_store_n(slot, ours, __ATOMIC_SEQ_CST);
}

// Copies of the table (harness): BG3's command lists keep their own copy of the table's
// command slice (91 of them in the menu at 1280x720, 0x198 bytes apart, the 22 hooked vkCmd* slots at
// the main table's distances), and more are made as BG3 goes on, still with the original functions.
// Every place holding the original vkCmdBeginRenderPass is checked: where at least 80 % of the hooked
// vkCmd* slots hold their originals at the main table's distances it is a copy, and each slot there
// that holds its original gets the same hook (compare-and-swap). Rescanned every 2 s for the first
// two minutes after the setup and every 10 s after that (a scan of BG3's private memory: ms).
// 1: a vkCmd* hook (the copies are recognised by these); 2: vkBegin/EndCommandBuffer, patched in a copy too
// when it holds them (through the copies only ~30 ends of BG3's command buffers in
// 300 frames reached the plugin, so the surfaces' albedo pass owed at the end of the G-buffer's never ran).
static int is_cmd_hook(int i) {
    for (int k = 0; k < NV; k++)
        if (g_named[k].n && g_swaps[i].orig == g_named[k].orig) {
            if (!strncmp(VKNAMES[k], "vkCmd", 5)) return 1;
            if (!strcmp(VKNAMES[k], "vkBeginCommandBuffer") || !strcmp(VKNAMES[k], "vkEndCommandBuffer")) return 2;
            return 0;
        }
    return 0;
}
static int patch_pass(int* found_copies, double* ms, int full);
static uintptr_t g_cpreg[32][2];  // regions where copies were found (rescanned often; the rest seldom)
static int g_ncpreg;
static void* patch_copies(void* arg) {
    (void)arg;
    uint64_t t0 = now_ns();
    int total = 0;
    for (int round = 0; atomic_load(&g_state) == 2; round++) {
        int copies = 0;
        double ms = 0;
        int n = patch_pass(&copies, &ms, round % 5 == 0);
        total += n;
        if (n || round == 0 || round % 30 == 0)
            rlog("table copies: %d copies of the command slice, %d slots newly hooked (%d so far, scan %.1f ms)", copies, n, total, ms);
        sleep(now_ns() - t0 < 120000000000ull ? 2 : 10);
    }
    return NULL;
}
static int patch_pass(int* found_copies, double* ms, int full) {
    int kb = name_index("vkCmdBeginRenderPass");
    if (kb < 0 || !g_named[kb].n) return 0;
    void* want = g_named[kb].orig;
    void** main_at = g_slot_present + g_named[kb].at[0];
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[1024];
    static uint64_t buf[1 << 16];
    int hits = 0, copies = 0, patched = 0;
    uint64_t t0 = now_ns();
    int cmd[256], ncmd = 0, extra[8], nextra = 0;
    for (int i = 0; i < g_nswaps && ncmd < 256; i++) {
        int kind = is_cmd_hook(i);
        if (kind == 1) cmd[ncmd++] = i;
        else if (kind == 2 && nextra < 8) extra[nextra++] = i;
    }
    if (ncmd < 5) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char perms[8] = {0}, path[512] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &a, &b, perms, path) < 3) continue;
        const char* pp = path;
        while (*pp == ' ') pp++;
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p' || *pp || b - a > (16ul << 30)) continue;  // anonymous rw only
        // BG3's engine heap lives in arenas of about 1 GB (the copies made in a level are there): a full
        // pass reads everything, the others only the regions that held copies before.
        int seen_here = 0;
        for (int r = 0; r < g_ncpreg; r++) seen_here |= g_cpreg[r][0] == a;
        if (!full && !seen_here) continue;
        int copies_before = copies;
        for (uintptr_t base = a; base < b; base += sizeof buf) {
            size_t len = b - base < sizeof buf ? b - base : sizeof buf;
            struct iovec lo = {buf, len}, re = {(void*)base, len};
            ssize_t got = process_vm_readv(getpid(), &lo, 1, &re, 1, 0);
            if (got <= 0) break;
            for (size_t k = 0; k < (size_t)got / 8; k++) {
                if (buf[k] != (uint64_t)(uintptr_t)want) continue;
                void** at = (void**)(base + 8 * k);
                if (at == main_at) continue;
                hits++;
                int score = 0, hooked_already = 0;
                for (int c = 0; c < ncmd; c++) {
                    void* v = 0;
                    if (!rd(at + (g_swaps[cmd[c]].slot - main_at), &v, 8)) continue;
                    if (v == g_swaps[cmd[c]].orig) score++;
                    else if (v == g_swaps[cmd[c]].ours) hooked_already++;
                }
                if ((score + hooked_already) * 5 < ncmd * 4) continue;
                copies++;
                for (int c = 0; c < ncmd; c++) {  // the command slice only: that is what a copy holds
                    void** slot = at + (g_swaps[cmd[c]].slot - main_at);
                    void* expect = g_swaps[cmd[c]].orig;
                    if (__atomic_compare_exchange_n(slot, &expect, g_swaps[cmd[c]].ours, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) patched++;
                }
                for (int c = 0; c < nextra; c++) {  // only where the copy holds the original (compare-and-swap)
                    void** slot = at + (g_swaps[extra[c]].slot - main_at);
                    void* v = 0;
                    if (!rd(slot, &v, 8) || v != g_swaps[extra[c]].orig) continue;
                    void* expect = g_swaps[extra[c]].orig;
                    if (__atomic_compare_exchange_n(slot, &expect, g_swaps[extra[c]].ours, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) patched++;
                }
            }
        }
        if (copies > copies_before && !seen_here && g_ncpreg < 32) { g_cpreg[g_ncpreg][0] = a; g_cpreg[g_ncpreg][1] = b; g_ncpreg++; }
    }
    fclose(f);
    (void)hits;
    *found_copies = copies;
    *ms = (now_ns() - t0) / 1e6;
    return patched;
}

// The device is known: name the table, fill the layer's records, swap the slots. On BG3's thread
// that made the image view (once; ~ a millisecond).
static int setup_device(VkDevice dev) {
    name_slots(dev);
    VkPhysicalDevice phys = match_physical(dev);
    if (!phys) return 0;
    // the layer's records: our instance stands in for BG3's (only its physical-device queries are used)
    InstanceData* in = &g_instances[0];
    memset(in, 0, sizeof *in);
    in->key = key_of(g_inst);
    in->instance = g_inst;
    in->gipa = g_loader_gipa;
    in->DestroyInstance = (PFN_vkDestroyInstance)g_loader_gipa(g_inst, "vkDestroyInstance");
    in->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_loader_gipa(g_inst, "vkGetPhysicalDeviceMemoryProperties");
    pthread_mutex_lock(&g_lock);
    DeviceData* d = &g_devices[0];
    memset(d, 0, sizeof *d);
    d->key = key_of(dev);
    d->device = dev;
    d->physical = phys;
    d->inst = in;
    d->gdpa = plugin_gdpa;
    d->setLoaderData = NULL;  // our command buffers come from the loader's trampoline, already stamped
    device_fns(d, dev, plugin_gdpa);
    pthread_mutex_unlock(&g_lock);
    if (!d->QueuePresentKHR || !d->CreateImageView || !d->BeginCommandBuffer || !d->CmdBeginRenderPass) {
        rlog("device functions missing; not hooking");
        memset(d, 0, sizeof *d);
        return 0;
    }
    // queues: every (family, index) the device has (vk_common answers NULL for one not made)
    PFN_vkGetPhysicalDeviceQueueFamilyProperties gqf =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)g_loader_gipa(g_inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    VkQueueFamilyProperties qf[16];
    uint32_t nq = 16;
    gqf(phys, &nq, qf);
    int nqueues = 0;
    for (uint32_t f = 0; f < nq; f++)
        for (uint32_t i = 0; i < qf[f].queueCount && i < 16; i++) {
            VkQueue q = VK_NULL_HANDLE;
            d->GetDeviceQueue(dev, f, i, &q);
            if (q) { remember_queue(d, q, f); nqueues++; rlog("  queue family %u index %u: %p", f, i, (void*)q); }
        }
    rlog("%d queues", nqueues);
    // the swaps
    int hooked = 0, missing = 0;
    char miss[512] = {0};
    for (unsigned h = 0; h < sizeof HOOKS / sizeof *HOOKS; h++) {
        int k = name_index(HOOKS[h].name);
        if (k < 0 || !g_named[k].n) continue;  // BG3 does not call it
        if (g_named[k].ambiguous) { rlog("  %s: shared slot value, not hooked", HOOKS[h].name); missing++; continue; }
        for (int j = 0; j < g_named[k].n; j++) {
            void** slot = g_slot_present + g_named[k].at[j];
            swap_slot(slot, g_named[k].orig, HOOKS[h].fn);
            hooked++;
        }
    }
    // names the layer hooks that BG3 calls but that are not in this table
    for (unsigned h = 0; h < sizeof HOOKS / sizeof *HOOKS; h++) {
        int k = name_index(HOOKS[h].name);
        if (k >= 0 && !g_named[k].n && strlen(miss) + strlen(HOOKS[h].name) + 2 < sizeof miss) {
            strcat(miss, " "); strcat(miss, HOOKS[h].name);
        }
    }
    g_mod_recover = mrc_recover;  // release batch: modules made before the plugin (read on first need)
    g_layout_recover = lay_recover;  // and their pipeline and descriptor set layouts
    rlog("hooked %d slots (%d refused); BG3 names not in the table:%s", hooked, missing, miss[0] ? miss : " none");
    pthread_t th;
    static int copies_running;  // one such thread per process, also when the table is hooked again
    if (!copies_running && !pthread_create(&th, NULL, patch_copies, NULL)) {
        copies_running = 1;
        pthread_detach(th);
    }
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_civ(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a,
                                             VkImageView* v) {
    atomic_fetch_add(&g_civs, 1);
    if (atomic_load(&g_state) == 0) {
        pthread_mutex_lock(&g_setup);
        for (int i = 0; i < 4; i++) {
            if (g_cand[i] == d) break;
            if (!g_cand[i]) { g_cand[i] = d; break; }
        }
        pthread_mutex_unlock(&g_setup);
    }
    if (atomic_load(&g_state) == 2) return layer_CreateImageView(d, ci, a, v);
    return ((PFN_vkCreateImageView)g_fwd_civ)(d, ci, a, v);
}

// At a present (the frame's boundary, on BG3's present thread): the candidate device whose loader key
// is the queue's is BG3's; set everything up and swap the slots.
static void setup_at_present(VkQueue q) {
    pthread_mutex_lock(&g_setup);
    if (atomic_load(&g_state) == 0) {
        VkDevice dev = VK_NULL_HANDLE;
        for (int i = 0; i < 4 && g_cand[i]; i++) if (key_of(g_cand[i]) == key_of(q)) dev = g_cand[i];
        if (dev) {
            atomic_store(&g_state, 1);
            uint64_t t = now_ns();
            int ok = setup_device(dev);
            atomic_store(&g_state, ok ? 2 : -1);
            rlog("setup %s in %.2f ms (present #%lu)", ok ? "done" : "FAILED (pass-through from now on)", (now_ns() - t) / 1e6,
                 atomic_load(&g_presents));
            if (!ok) __atomic_store_n(g_slot_civ, g_fwd_civ, __ATOMIC_SEQ_CST);
        }
    }
    pthread_mutex_unlock(&g_setup);
}

static VKAPI_ATTR VkResult VKAPI_CALL hk_present(VkQueue q, const VkPresentInfoKHR* info) {
    unsigned long n = atomic_fetch_add(&g_presents, 1) + 1;
    if (atomic_load(&g_state) == 0 && g_cand[0]) setup_at_present(q);
    if (atomic_load(&g_state) != 2) return ((PFN_vkQueuePresentKHR)g_fwd_present)(q, info);
    if (!queue_data(q)) remember_queue(&g_devices[0], q, 0);  // not seen at setup: assume graphics
    VkResult r = layer_QueuePresentKHR(q, info);
    if (n == 1 || n % 3000 == 0) rlog("present #%lu through the plugin", n);
    if (!g_rpc.ok && g_img && n % 300 == 0 && g_rpc.tries < 40) rpc_calibrate();
    if (!g_brc.ok && n % 300 == 150 && g_brc.tries < 40) brc_calibrate();
    if (n % 600 == 0) {
        rlog("render passes since the last line: %llu, %llu with something made before the plugin (render pass %llu,"
             " framebuffer %llu, view %llu, image %llu; swapchain images %llu)",
             (unsigned long long)g_unk.total, (unsigned long long)g_unk.passes, (unsigned long long)g_unk.rp,
             (unsigned long long)g_unk.fb, (unsigned long long)g_unk.view, (unsigned long long)g_unk.img, (unsigned long long)g_unk.swap);
        memset(&g_unk, 0, sizeof g_unk);
    }
    if (n % 18000 == 0) {  // BG3's descriptor sets in the table: live count stays bounded (sets_log)
        pthread_mutex_lock(&g_fxl_lock);
        sets_log("every 18000 presents");
        pthread_mutex_unlock(&g_fxl_lock);
    }
    // A swapchain made before the hooks: the layer never saw it and it lacks copy usage. Ask BG3
    // for a new one (it presented; the answer only says the chain is stale), at most 3 times.
    if (info->swapchainCount >= 1 && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
        pthread_mutex_lock(&g_lock);
        int known = chain_data(info->pSwapchains[0]) != NULL;
        pthread_mutex_unlock(&g_lock);
        if (!known && g_force < 3 && (g_force == 0 || n - g_force_at > 120)) {
            g_force++;
            g_force_at = n;
            rlog("present #%lu: swapchain %p made before the plugin; answering OUT_OF_DATE so BG3 makes a new one (%d)",
                 n, (void*)info->pSwapchains[0], g_force);
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hk_destroy_device(VkDevice device, const VkAllocationCallbacks* alloc) {
    if (device != g_devices[0].device) {  // not BG3's renderer device: none of ours
        int k = name_index("vkDestroyDevice");
        if (k >= 0 && g_named[k].orig) ((PFN_vkDestroyDevice)g_named[k].orig)(device, alloc);
        return;
    }
    // put BG3's table back first: nothing of ours may be called with a device we no longer know
    for (int i = 0; i < g_nswaps; i++) __atomic_store_n(g_swaps[i].slot, g_swaps[i].orig, __ATOMIC_SEQ_CST);
    __atomic_store_n(g_slot_present, g_fwd_present, __ATOMIC_SEQ_CST);
    atomic_store(&g_state, -1);
    rlog("device %p destroyed: BG3's table restored", (void*)device);
    layer_DestroyDevice(device, alloc);
}

// ---------------------------------------------------------------- init
#define MAXR 4096

// `WOW_BG3_RENDER_TABLE=first`: the old rule, hook only when exactly one slot holds bg3le's Present.
static int table_switch_first(void) {
    const char* v = getenv("WOW_BG3_RENDER_TABLE");
    return v && !strcmp(v, "first");
}

// The window around a candidate, read in one go where the mapping allows it (a hole falls back to
// slot by slot). Slots outside the mapping read as 0.
static void read_window(void** slot, uintptr_t a, uintptr_t b, void** out) {
    size_t n = 2 * SPAN + 1;
    for (size_t i = 0; i < n; i++) out[i] = 0;
    uintptr_t from = (uintptr_t)(slot - SPAN), to = from + n * sizeof(void*);
    uintptr_t lo = from < a ? a : from, hi = to > b ? b : to;
    if (hi <= lo) return;
    if (rd((void*)lo, (char*)out + (lo - from), hi - lo)) return;
    for (uintptr_t at = lo; at + sizeof(void*) <= hi; at += sizeof(void*)) {
        void* v = 0;
        if (rd((void*)at, &v, sizeof v)) out[(at - from) / sizeof(void*)] = v;
    }
}

// How much of BG3's device table this candidate looks like: bg3le's vkCreateImageView forwarder in the
// window (BG3's own table keeps it 51 slots before Present), and how many of the Vulkan names BG3 carries
// are in the window. BG3's table holds about 184 of the 196; a stray copy of a few pointers holds a handful.
// No device is known yet, so the names are resolved through bg3le's interposed exports and the loader's own,
// the same pair `name_slots` matches later.
static void grade_candidate(Cand* c) {
    static void* win[2 * SPAN + 1];
    static void* f1[NV];
    static void* f2[NV];
    static int resolved;
    if (!resolved) {
        resolved = 1;
        for (int k = 0; k < NV; k++) {
            f1[k] = dlsym(RTLD_DEFAULT, VKNAMES[k]);
            f2[k] = g_loader ? dlsym(g_loader, VKNAMES[k]) : NULL;
        }
    }
    read_window(c->slot, c->a, c->b, win);
    c->civ = NULL;
    c->civ_at = 0;
    c->named = 0;
    for (int d = -SPAN; d <= SPAN; d++) {
        if (win[d + SPAN] != g_fwd_civ) continue;
        if (!c->civ || d == -51) { c->civ = c->slot + d; c->civ_at = d; }
    }
    for (int k = 0; k < NV; k++) {
        if (!f1[k] && !f2[k]) continue;
        for (int d = -SPAN; d <= SPAN; d++) {
            void* v = win[d + SPAN];
            if (v && (v == f1[k] || v == f2[k])) { c->named++; break; }
        }
    }
}

// Every slot of BG3's private anonymous memory holding bg3le's Present forwarder, graded. Candidates
// already known keep their place (and their `tried`), so a later scan only appends.
static int scan_candidates(void) {
    static struct { uintptr_t a, b; } R[MAXR];
    int nr = 0;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof line, f) && nr < MAXR) {
        unsigned long a, b;
        char perms[8] = {0}, path[512] = {0};
        int got = sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &a, &b, perms, path);
        if (got < 3 || perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p') continue;
        const char* p = path;
        while (*p == ' ') ++p;
        if (*p || b - a > (256ul << 20)) continue;  // anonymous only, no huge arenas
        R[nr].a = a; R[nr].b = b; nr++;
    }
    fclose(f);
    static uint64_t buf[1 << 16];
    int before = g_ncands, hits = 0;
    uint64_t t0 = now_ns();
    size_t scanned = 0;
    for (int i = 0; i < nr; i++) {
        for (uintptr_t base = R[i].a; base < R[i].b; base += sizeof buf) {
            size_t len = R[i].b - base < sizeof buf ? R[i].b - base : sizeof buf;
            struct iovec lo = {buf, len}, re = {(void*)base, len};
            ssize_t got = process_vm_readv(getpid(), &lo, 1, &re, 1, 0);
            if (got <= 0) break;
            scanned += (size_t)got;
            for (size_t k = 0; k < (size_t)got / 8; k++) {
                if (buf[k] != (uint64_t)(uintptr_t)g_fwd_present) continue;
                hits++;
                void** slot = (void**)(base + 8 * k);
                int known = 0;
                for (int c = 0; c < g_ncands; c++) known |= g_cands[c].slot == slot;
                if (known || g_ncands >= MAXC) continue;
                g_cands[g_ncands].slot = slot;
                g_cands[g_ncands].a = R[i].a;
                g_cands[g_ncands].b = R[i].b;
                grade_candidate(&g_cands[g_ncands]);
                g_ncands++;
            }
        }
    }
    rlog("scanned %zu MB in %.1f ms: %d slot(s) hold bg3le's Present (%d new candidate(s), %d in all)",
         scanned >> 20, (now_ns() - t0) / 1e6, hits, g_ncands - before, g_ncands);
    for (int c = before; c < g_ncands; c++)
        rlog("  candidate %d at %p: vkCreateImageView %s, %d of %d names in the window", c + 1,
             (void*)g_cands[c].slot,
             g_cands[c].civ ? (g_cands[c].civ_at == -51 ? "-51 (BG3's own layout)" : "in the window") : "NOT in the window",
             g_cands[c].named, NV);
    return g_ncands > 0;
}

// The likeliest untried candidate: it must carry bg3le's vkCreateImageView, and the slot 51 before Present
// (BG3's own layout) wins first. The name count only decides between candidates that agree on that, because
// at plugin time BG3 has resolved a part of its table (46 of 196 in one start) while the Vulkan loader's own
// trampoline table, which BG3 never calls through, already holds nearly all of them (179).
static int pick_candidate(void) {
    int best = -1;
    for (int c = 0; c < g_ncands; c++) {
        Cand* k = &g_cands[c];
        if (k->tried || !k->civ) continue;
        if (best < 0) { best = c; continue; }
        Cand* b = &g_cands[best];
        int kown = k->civ_at == -51, bown = b->civ_at == -51;
        if (kown != bown ? kown : k->named > b->named) best = c;
    }
    return best;
}

// Put every slot we swapped back to the value BG3 itself had there.
static void unhook_table(void) {
    for (int i = 0; i < g_nswaps; i++) __atomic_store_n(g_swaps[i].slot, g_swaps[i].orig, __ATOMIC_SEQ_CST);
    g_nswaps = 0;
    if (g_slot_civ) __atomic_store_n(g_slot_civ, g_fwd_civ, __ATOMIC_SEQ_CST);
    if (g_slot_present) __atomic_store_n(g_slot_present, g_fwd_present, __ATOMIC_SEQ_CST);
    g_slot_civ = NULL;
}

static int hook_candidate(int c) {
    Cand* k = &g_cands[c];
    if (!k->civ) return 0;
    g_slot_present = k->slot;
    g_reg_a = k->a;
    g_reg_b = k->b;
    g_slot_civ = k->civ;
    k->tried = 1;
    g_cand_at = c;
    atomic_store(&g_civs, 0);
    __atomic_store_n(g_slot_civ, (void*)hk_civ, __ATOMIC_SEQ_CST);
    __atomic_store_n(g_slot_present, (void*)hk_present, __ATOMIC_SEQ_CST);
    rlog("table slot %p (candidate %d of %d, %d names): Present and CreateImageView (%+d) hooked; the rest at the next present after an image view",
         (void*)g_slot_present, c + 1, g_ncands, k->named, k->civ_at);
    return 1;
}

// The choice is only a guess until BG3 presents through our hook. Nothing within the grace period means the
// wrong table: it goes back and the next candidate is hooked, and with the candidates used up the memory is
// scanned again (BG3 can build the table later). A table that made image views through our hook is one BG3
// calls through: it is kept however long the first present takes (a slow start), because giving it back would
// lose BG3's table for good and race BG3's own threads inside our hooks. Everything it does is logged, so a
// start that does not draw says why.
static void* watch_table(void* arg) {
    (void)arg;
    const double grace = 3.0, every = 1.5, budget = 40.0;
    double t0 = now_ns() / 1e9;
    struct timespec ts = {(time_t)grace, (long)((grace - (long)grace) * 1e9)};
    nanosleep(&ts, NULL);
    for (;;) {
        unsigned long seen = atomic_load(&g_presents);
        if (seen) {
            rlog("candidate %d answered %lu present(s): BG3's table confirmed", g_cand_at + 1, seen);
            return NULL;
        }
        double now = now_ns() / 1e9;
        if (atomic_load(&g_civs)) {  // BG3 calls through it: wait for its first present, never move off it
            static int said;
            if (!said++) rlog("candidate %d made %lu image view(s) but no present yet in %.1f s; keeping it",
                              g_cand_at + 1, atomic_load(&g_civs), now - t0);
            struct timespec st = {(time_t)every, (long)((every - (long)every) * 1e9)};
            nanosleep(&st, NULL);
            if (now - t0 > budget) {
                rlog("candidate %d still has no present after %.0f s; kept (it makes BG3's image views)", g_cand_at + 1, budget);
                return NULL;
            }
            continue;
        }
        if (now - t0 > budget) {
            rlog("no present through any of the %d candidate table(s) in %.0f s; BG3's own picture is not composited",
                 g_ncands, budget);
            return NULL;
        }
        pthread_mutex_lock(&g_setup);
        int next = pick_candidate();
        if (next < 0) {
            scan_candidates();
            next = pick_candidate();
        }
        if (next >= 0) {
            rlog("candidate %d answered no present in %.1f s; going back to BG3's values and trying candidate %d",
                 g_cand_at + 1, now - t0, next + 1);
            unhook_table();
            atomic_store(&g_state, 0);
            memset(g_cand, 0, sizeof g_cand);
            if (hook_candidate(next)) {
                VkDevice dev = g_devices[0].device;
                if (dev) {  // the device is already known (bg3le gives it at init): set the new table up now
                    atomic_store(&g_state, 1);
                    int ok = setup_device(dev);
                    atomic_store(&g_state, ok ? 2 : -1);
                    rlog("setup %s on candidate %d", ok ? "done" : "FAILED (pass-through from now on)", next + 1);
                }
            }
        }
        pthread_mutex_unlock(&g_setup);
        struct timespec st = {(time_t)every, (long)((every - (long)every) * 1e9)};
        nanosleep(&st, NULL);
    }
}

int bg3le_plugin_init(const bg3le_host* host, bg3le_plugin* self) {
    H = host; S = self;
    host->describe(self, "wowbg3-render", RENDER_VERSION);
    (void)out_dir();  // reads the paths (WOWBG3_CAPTURE_DIR and the rest), creates the capture folder
    char sw = render_switch();
    int layer = 0, layer_draws = 0;  // the capture layer in this process (loaded by path: no soname)
    {
        FILE* mf = fopen("/proc/self/maps", "r");
        char ml[1024];
        while (mf && !layer && fgets(ml, sizeof ml, mf)) {
            char* p = strchr(ml, '/');
            if (!p || !strstr(p, "libwowbg3_capture.so")) continue;
            layer = 1;
            p[strcspn(p, "\n")] = 0;
            void* h = dlopen(p, RTLD_LAZY | RTLD_NOLOAD);
            int (*passive)(void) = h ? (int (*)(void))dlsym(h, "wowbg3_layer_passive") : NULL;
            layer_draws = !passive || !passive();  // an older layer always draws
        }
        if (mf) fclose(mf);
    }
    if (sw == 'L' || sw == '0' || (!sw && layer) || layer_draws) {
        host->log(self, "render: staying out (switch %c, capture layer %s)", sw ? sw : '-',
                  layer ? (layer_draws ? "loaded and drawing" : "loaded, passive") : "not loaded");
        return 0;
    }
    char exe[512] = {0};
    ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (el <= 0 || strcmp(exe + (el >= 4 ? el - 4 : 0), "/bg3")) {  // bg3le also loads into BG3's crash reporter
        host->log(self, "render: not BG3 (%s); staying out", exe);
        return 0;
    }
    rlog("wowbg3-render %s: drawing from the plugin (switch %c, capture layer %s)", RENDER_VERSION, sw ? sw : '-',
         layer ? "loaded, passive" : "not loaded");
    g_loader = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_NOLOAD);
    g_fwd_present = dlsym(RTLD_DEFAULT, "vkQueuePresentKHR");
    g_fwd_civ = dlsym(RTLD_DEFAULT, "vkCreateImageView");
    g_bg3le_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(RTLD_DEFAULT, "vkGetDeviceProcAddr");
    g_loader_gipa = (PFN_vkGetInstanceProcAddr)loader_sym("vkGetInstanceProcAddr");
    if (!g_loader_gipa && g_loader) g_loader_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_loader, "vkGetInstanceProcAddr");
    if (!g_loader || !g_fwd_present || !g_fwd_civ || !g_bg3le_gdpa || !g_loader_gipa) {
        rlog("Vulkan loader or bg3le's forwarders missing; not hooking");
        return 1;
    }
    if (!own_instance()) return 1;
    // BG3's table: a slot of private anonymous memory holding bg3le's Present forwarder. Several places
    // can hold it, so every one is kept as a candidate and graded (see `Cand`); `first` is the old rule.
    if (!scan_candidates()) {
        rlog("no slot holds bg3le's Present; not hooking");
        return 1;
    }
    int only = table_switch_first();
    if (only && g_ncands != 1) {
        rlog("want exactly one (WOW_BG3_RENDER_TABLE=first); not hooking");
        return 1;
    }
    int pick = pick_candidate();
    if (pick < 0) {
        rlog("no candidate carries bg3le's vkCreateImageView near Present; not hooking");
        return 1;
    }
    if (!hook_candidate(pick)) return 1;
    // bg3le patch 0005: host->vk_device (after state_gate_can_hold; check host->size covers it) gives BG3's
    // device. Loaded early (EarlyNativePlugins / <name>.early) the plugin starts at BG3's first pipeline cache,
    // before its first pipeline (the sight overlay's is made at about 1.2 s, before the event loop): set up now.
    void* (*vk_device)(void) = NULL;
    if (host->size >= offsetof(bg3le_host, vk_device) + sizeof vk_device) vk_device = host->vk_device;
    VkDevice dev = vk_device ? (VkDevice)vk_device() : VK_NULL_HANDLE;
    if (dev) {
        pthread_mutex_lock(&g_setup);
        atomic_store(&g_state, 1);
        uint64_t t = now_ns();
        int ok = setup_device(dev);
        atomic_store(&g_state, ok ? 2 : -1);
        if (!ok) __atomic_store_n(g_slot_civ, g_fwd_civ, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&g_setup);
        rlog("setup %s in %.2f ms at init (bg3le's device %p, %lu presents so far)", ok ? "done" : "FAILED (pass-through from now on)",
             (now_ns() - t) / 1e6, (void*)dev, atomic_load(&g_presents));
    }
    if (!only) {  // the choice of table is checked against a present through our hook, and corrected
        pthread_t wt;
        if (!pthread_create(&wt, NULL, watch_table, NULL)) pthread_detach(wt);
    }
    return 0;
}
