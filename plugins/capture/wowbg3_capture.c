// WOW-BG3 capture layer: an explicit Vulkan layer for the private BG3 only.
//
// On request it copies presented swapchain images to host memory and writes them under
// /logs/capture/ (the private, writable log mount of the sandbox). Nothing else changes:
// the swapchain additionally gets TRANSFER_SRC usage so it can be copied. Requests are a
// text file /logs/capture/request: "<count> <every> <tag>", e.g. "40 1 firebolt". A new
// request is noticed by its modification time, checked every 15 presents.
//
// Each frame is written as <tag>-<NNNN>.raw (tightly packed pixels in the swapchain's
// format) plus one <tag>.json describing format, size and frame times.
// Request tags starting with "trace" write a frame trace instead (render passes, attachments,
// draws, copies; see trace_arm). Depth capture is not implemented yet.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#define LAYER_NAME "VK_LAYER_WOWBG3_capture"
#define MAX_ENTRIES 64

#ifdef WOWBG3_PLUGIN  // built into the bg3le render plugin (plugins/render): nothing exported but its init
#define EXPORT
#else
#define EXPORT __attribute__((visibility("default")))
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Paths: read once, from the environment the launcher plugin sets
// (it loads first). WOWBG3_CAPTURE_DIR: this layer's log, requests and dumps, compose words, fx library
// (default /logs/capture, the dev sandbox's log mount; created if missing). WOWBG3_SHM: benilla's frames
// wowframe.a.bin/.b.bin (default /wowshm). WOWBG3_IO: the mod's IO folder, the Lua's Ext.IO wow-bg3/
// (default <data>/io, Mods/WOW-BG3/data/io, as the launcher maps it with bg3le patch 0012).
// WOWBG3_LINK: the link folder, bg3cam.bin, bg3screen.txt, tavplan.txt (default <io>/link).
// Unset (a plugin bg3le starts early, before the launcher plugin sets them: bg3le patch 0005), each is found
// as the launcher's release_env() finds it, so both name the same folders: capture /logs/capture if it exists,
// else <data>/capture; frames /wowshm if it exists, else /dev/shm/wow-bg3-<uid>, else <data>/shm; Tav
// /wowtav beside /wowshm, else <frames>/tav (<data>: $WOWBG3_DATA, else Mods/WOW-BG3/data, as the launcher).
static struct { char out[512], fx[528], wow[2][560], io[1024], link[1040], cam[1060], shm[512], tav[530]; } g_path;
static pthread_once_t g_path_once = PTHREAD_ONCE_INIT;
static int path_is_dir(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static void path_mkdirs(const char* dir) {
    char b[600];
    snprintf(b, sizeof b, "%s", dir);
    for (char* p = b + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(b, 0700); *p = '/'; }
    mkdir(b, 0700);
}
static void path_init(void) {
    // <data> is the launcher's: the mod's own folder (this plugin's, beside launcher.conf) /data, else
    // BG3's Mods/WOW-BG3/data. Nothing the mod makes lives outside its folder.
    char data[400];
    const char* hm = getenv("HOME");
    const char* de = getenv("WOWBG3_DATA");
    Dl_info di;
    char conf[460];
    data[0] = 0;
    if (de && *de) snprintf(data, sizeof data, "%.390s", de);
    else if (dladdr((void*)&path_init, &di) && di.dli_fname) {
        snprintf(data, sizeof data, "%.380s", di.dli_fname);
        char* sl = strrchr(data, '/');
        if (sl) *sl = 0;
        snprintf(conf, sizeof conf, "%s/launcher.conf", data);
        if (sl && access(conf, R_OK) == 0) strcat(data, "/data");
        else data[0] = 0;
    }
    if (!data[0])
        snprintf(data, sizeof data, "%.320s/.local/share/Larian Studios/Baldur's Gate 3/Mods/WOW-BG3/data",
                 hm && *hm ? hm : "/tmp");
    const char* e = getenv("WOWBG3_CAPTURE_DIR");
    if (e && *e) snprintf(g_path.out, sizeof g_path.out, "%s", e);
    else if (path_is_dir("/logs/capture")) snprintf(g_path.out, sizeof g_path.out, "/logs/capture");
    else snprintf(g_path.out, sizeof g_path.out, "%s/capture", data);
    path_mkdirs(g_path.out);
    snprintf(g_path.fx, sizeof g_path.fx, "%s/fx", g_path.out);
    e = getenv("WOWBG3_SHM");
    if (e && *e) snprintf(g_path.shm, sizeof g_path.shm, "%s", e);
    else if (path_is_dir("/wowshm")) snprintf(g_path.shm, sizeof g_path.shm, "/wowshm");
    else if (path_is_dir("/dev/shm") && access("/dev/shm", W_OK) == 0) snprintf(g_path.shm, sizeof g_path.shm, "/dev/shm/wow-bg3-%d", (int)getuid());
    else snprintf(g_path.shm, sizeof g_path.shm, "%s/shm", data);
    e = getenv("WOWBG3_TAV_DIR");
    if (e && *e) snprintf(g_path.tav, sizeof g_path.tav, "%s", e);
    else if (!strcmp(g_path.shm, "/wowshm") && path_is_dir("/wowtav")) snprintf(g_path.tav, sizeof g_path.tav, "/wowtav");
    else snprintf(g_path.tav, sizeof g_path.tav, "%s/tav", g_path.shm);
    if (strcmp(g_path.shm, "/wowshm")) { path_mkdirs(g_path.shm); path_mkdirs(g_path.tav); }
    for (int k = 0; k < 2; k++)
        snprintf(g_path.wow[k], sizeof g_path.wow[k], "%s/wowframe.%c.bin", g_path.shm, "ab"[k]);
    e = getenv("WOWBG3_IO");  // the mod's IO folder (Ext.IO's wow-bg3/, bg3le patch 0012): <data>/io
    if (e && *e) snprintf(g_path.io, sizeof g_path.io, "%s", e);
    else snprintf(g_path.io, sizeof g_path.io, "%s/io", data);
    e = getenv("WOWBG3_LINK");
    if (e && *e) snprintf(g_path.link, sizeof g_path.link, "%s", e);
    else snprintf(g_path.link, sizeof g_path.link, "%s/link", g_path.io);
    snprintf(g_path.cam, sizeof g_path.cam, "%s/bg3cam.bin", g_path.link);
}
static const char* out_dir(void) { pthread_once(&g_path_once, path_init); return g_path.out; }
static const char* fxc_dir(void) { out_dir(); return g_path.fx; }
static const char* wow_frame(int k) { out_dir(); return g_path.wow[k & 1]; }
static const char* bg3cam_path(void) { out_dir(); return g_path.cam; }
static const char* io_dir(void) { out_dir(); return g_path.io; }
static const char* link_dir(void) { out_dir(); return g_path.link; }
static const char* tav_dir(void) { out_dir(); return g_path.tav; }
// out_dir() + name, in one of eight buffers per thread (for an open, stat or a short list of names).
static const char* outp(const char* name) {
    static __thread char ring[8][600];
    static __thread unsigned i;
    char* b = ring[i++ & 7];
    snprintf(b, sizeof ring[0], "%s%s", out_dir(), name);
    return b;
}

static void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char* fmt, ...) {
    FILE* f = fopen(outp("/layer.log"), "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Which side draws: this layer, or the bg3le render plugin (plugins/render), which
// runs this same code through BG3's own device table. Env WOW_BG3_RENDER, else the first word of
// OUT_DIR/render: "plugin" (the layer passes every call straight through, the plugin draws),
// "layer" (the plugin stays out), "off" (neither draws). Unset: the layer draws when it is loaded,
// else the plugin.
static char render_switch(void) {
    char w[32] = {0};
    const char* e = getenv("WOW_BG3_RENDER");
    if (e && *e) snprintf(w, sizeof w, "%s", e);
    else {
        FILE* f = fopen(outp("/render"), "r");
        if (f) { if (!fgets(w, sizeof w, f)) w[0] = 0; fclose(f); }
    }
    if (!strncmp(w, "plugin", 6)) return 'P';
    if (!strncmp(w, "layer", 5)) return 'L';
    if (!strncmp(w, "off", 3)) return '0';
    return 0;
}
#ifndef WOWBG3_PLUGIN
// The layer only passes calls through (decided once, at the first lookup).
static int layer_passive(void) {
    static int p = -1;
    if (p < 0) { char sw = render_switch(); p = sw == 'P' || sw == '0'; }
    return p;
}
// Asked by the render plugin: it draws only when the layer in the process says it does not (a layer
// without this export is older and always draws).
__attribute__((visibility("default"))) int wowbg3_layer_passive(void) { return layer_passive(); }
#endif

// ---------------------------------------------------------------- dispatch tables
typedef struct {
    void* key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
} InstanceData;

typedef struct {
    void* key;
    VkDevice device;
    VkPhysicalDevice physical;
    InstanceData* inst;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData setLoaderData;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkDestroyBuffer DestroyBuffer;
    // trace
    PFN_vkCreateImage CreateImage;
    PFN_vkSetDebugUtilsObjectNameEXT SetDebugUtilsObjectNameEXT;  // NULL unless a debug layer offers it
    PFN_vkCreateImageView CreateImageView;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkCreateRenderPass2 CreateRenderPass2;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdBeginRenderPass2 CmdBeginRenderPass2;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdEndRenderPass2 CmdEndRenderPass2;
    PFN_vkCmdBeginRendering CmdBeginRendering;
    PFN_vkCmdEndRendering CmdEndRendering;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCmdDrawIndexed CmdDrawIndexed;
    PFN_vkCmdDrawIndirect CmdDrawIndirect;
    PFN_vkCmdDrawIndexedIndirect CmdDrawIndexedIndirect;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCmdDispatchIndirect CmdDispatchIndirect;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdClearColorImage CmdClearColorImage;
    PFN_vkCmdResolveImage CmdResolveImage;
    PFN_vkCmdExecuteCommands CmdExecuteCommands;
    PFN_vkQueueSubmit2 QueueSubmit2;
    PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkBindBufferMemory2 BindBufferMemory2;
    PFN_vkDestroyBuffer DestroyBufferNext;
    PFN_vkCmdCopyBuffer CmdCopyBuffer;
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdPushConstants CmdPushConstants;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCmdSetBlendConstants CmdSetBlendConstants;
    PFN_vkUpdateDescriptorSetWithTemplate UpdateDescriptorSetWithTemplate;
    PFN_vkCreateDescriptorUpdateTemplate CreateDescriptorUpdateTemplate;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkCreateSampler CreateSampler;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkCmdSetViewport CmdSetViewport;
    // WoW's ground in BG3's G-buffer (wowbg3_ground.inc.c)
    PFN_vkCmdSetScissor CmdSetScissor;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    // GPU compositor (wowbg3_compose_gpu.inc.c)
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkFreeDescriptorSets FreeDescriptorSets;      // BG3's sets: their entries end with them (set table)
    PFN_vkResetDescriptorPool ResetDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkGetFenceStatus GetFenceStatus;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateQueryPool CreateQueryPool;
    PFN_vkCmdResetQueryPool CmdResetQueryPool;
    PFN_vkCmdWriteTimestamp CmdWriteTimestamp;
    PFN_vkGetQueryPoolResults GetQueryPoolResults;
} DeviceData;

typedef struct {
    VkQueue queue;
    DeviceData* dev;
    uint32_t family;
} QueueData;

typedef struct {
    VkSwapchainKHR chain;
    DeviceData* dev;
    VkFormat format;
    VkExtent2D extent;
    uint32_t count;
    VkImage images[16];
} SwapchainData;

typedef struct {
    DeviceData* dev;
    uint32_t family;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore done[3];
    uint32_t next_done;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void* mapped;
} Staging;

static InstanceData g_instances[MAX_ENTRIES];
static DeviceData g_devices[MAX_ENTRIES];
static QueueData g_queues[MAX_ENTRIES];
static SwapchainData g_chains[MAX_ENTRIES];
static Staging g_staging[MAX_ENTRIES];

static void* key_of(const void* dispatchable) { return *(void* const*)dispatchable; }

static InstanceData* instance_for(const void* obj) {
    void* k = key_of(obj);
    for (int i = 0; i < MAX_ENTRIES; i++) if (g_instances[i].key == k) return &g_instances[i];
    return NULL;
}
static DeviceData* device_for(const void* obj) {
    void* k = key_of(obj);
    for (int i = 0; i < MAX_ENTRIES; i++) if (g_devices[i].key == k) return &g_devices[i];
#ifdef WOWBG3_PLUGIN
    // The plugin knows one device, BG3's; a handle the loader did not stamp still belongs to it.
    if (g_devices[0].key) return &g_devices[0];
#endif
    return NULL;
}
static QueueData* queue_data(VkQueue q) {
    for (int i = 0; i < MAX_ENTRIES; i++) if (g_queues[i].queue == q) return &g_queues[i];
    return NULL;
}
static SwapchainData* chain_data(VkSwapchainKHR c) {
    for (int i = 0; i < MAX_ENTRIES; i++) if (g_chains[i].chain == c) return &g_chains[i];
    return NULL;
}

static void trace_arm(const char* tag);
static int is_depth_format(VkFormat f);
// Request tag "img<id>l<layout>" (e.g. img4064l5): copy that image (id from a trace, 4 bytes a
// texel, or the depth aspect of a depth image) once at the next present, from the given layout.
static struct { int armed; uint32_t id; int layout; char tag[48]; } g_dump;
static void trace_present(SwapchainData* c, uint32_t index);
static volatile int g_fx_dump_request;  // request tag "fxdump": the effects copies, once
// Buffer research (geometry copy): every buffer with its size, usage and memory binding.
// Request "bufs" writes the table to OUT_DIR/bufs.txt; "rb<handle>-<offset>-<size>" (hex) copies
// that range of that buffer to OUT_DIR/rb<handle>-<offset>-<size>.raw at the next present.
typedef struct { uint64_t h; uint64_t size, usage, mem, offset, gen; } BufInfo;  // gen: creation number (tav cache)
#define BUF_CAP (1u << 17)
static BufInfo* g_buf;
static volatile int g_bufs_request;
static struct { int armed; uint64_t h, off, size; char tag[48]; } g_rb;
// The party: the plan holds the hero and her companions (TavPlan.lua figures), about
// 20 to 30 meshes each, so up to 192 meshes; with every LOD of them (about 2.6 times as many), so 512.
#define TAV_MAX 512
// BG3 stops drawing Tav once WoW draws her: draws from her index buffers are dropped while the
// compositor is on and her meshes reached benilla (compose word "bg3tav" keeps BG3's own).
// Written at present, read on the recording threads (64-bit stores, a stale read costs one draw).
// An open-addressing set (0 empty) of every figure's LOD 0 and coarser index buffers, looked
// up once per indexed draw instead of a scan of all of them; rewritten only when it changes.
#define TAV_HIDE_CAP 2048
static volatile uint64_t g_tav_ib[TAV_HIDE_CAP];
static inline uint32_t tav_hash(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (uint32_t)h & (TAV_HIDE_CAP - 1);
}
static volatile int g_tav_hide;
// wdbloom (patch28): every render pass's first attachment (any size), the prefilter pass seen, the composite seen.
static __thread VkCommandBuffer t_any_cb, t_blc_cb, t_blm_cb;
static __thread uint64_t t_any_img0, t_blc_img, t_blm_img, t_blm_b1;
static __thread int32_t t_any_fin0, t_blc_lay, t_blm_lay, t_blm_b1lay;
static uint64_t g_gb_nrm;  // BG3's G-buffer normal image (ground_pass); read through a view of the plugin's own (gbv_view, wowbg3_fxbg.inc.c)
static volatile int g_fxl_frames;  // effects research log (see fxl_pass)
static volatile int g_fxw_frames;  // draw decisions research log (request "fxwhy<N>", see fxw_draw)
static volatile int g_sight_lines;  // BG3's sight overlay research log (request "sight<N>", see sight_note)
static void fxw_secondary(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info);
static volatile uint64_t g_tav_dropped;  // a count for the log (racy, roughly right)
static __thread VkCommandBuffer t_ib_cb;
static __thread uint64_t t_ib;
// The pass on this thread renders into BG3's scene depth: only there does WoW draw the
// figures instead. BG3 also draws them elsewhere: its character portraits (ecl::CharacterIconRenderSystem,
// after a level-up) render the same meshes, same index buffers, into their own targets, and hiding
// those draws too made the new portraits black (a party at level 4). In normal frames every
// hidden draw was in a scene-depth pass (fxwhy, 1937 each: the depth prepass, the G-buffer, the
// forward pass), so this keeps world mode's own picture as it was.
static __thread VkCommandBuffer t_tav_cb;
static volatile int g_tavlog;  // presents left to log BG3's Tav draws (request "tavlog", research)
static volatile int g_shwlog;  // wowbg3_fxcap.inc.c
static void tav_log_draw(VkCommandBuffer cb);
// BG3 screens with their own 3D picture: level-up, respec and the mirror
// are character creation's screen, which draws a copy of the figure on BG3's scene depth with its own
// camera. While link/bg3screen.txt says one is open (RenderScreen.lua) the figures stay BG3's and
// WoW's ground is not injected (screen_check), and WoW's world is never composited into such a
// screen: WoW's frame is drawn for the camera BG3 leads with, which is not the close-up camera the
// screen renders from. What stock BG3 shows behind the character there is the level's own scenery,
// and the only reason ours showed an empty void was that the world mode's voiding kept culling the
// level's draws while the screen was up; it is off there now (word "noscreenlevel" keeps it on).
// "screenvoid" is the other way out: one dark colour over every pixel the character does not cover.
static volatile int g_screen;
// A party dialog ("1 Dialogue", RenderScreen.lua: a companion's talk, whose actors only BG3's own picture
// places and turns right) is such a screen too, but with WoW's world behind its actors: the voiding stays
// on (BG3's host level is a grey void), BG3 draws the figures, WoW (benilla 0070) hides its own, and WoW's
// frame, drawn from the dialog's camera (the lead source), is composited as in play.
static volatile int g_screen_dialog;
static int tav_hidden_draw(VkCommandBuffer cb) {
    if (!g_tav_hide || g_screen || t_ib_cb != cb || !t_ib || t_tav_cb != cb) return 0;
    for (uint32_t i = tav_hash(t_ib), k = 0; k < TAV_HIDE_CAP; k++, i = (i + 1) & (TAV_HIDE_CAP - 1)) {
        uint64_t v = g_tav_ib[i];
        if (!v) return 0;
        if (v == t_ib) {
            if (g_tavlog > 0) tav_log_draw(cb);
            g_tav_dropped++;
            return 1;
        }
    }
    return 0;
}
// Frame pacing (wowbg3_pace.inc.c, included at the end).
static void pace_gbuffer(VkCommandBuffer cb);
static void pace_bind(VkCommandBuffer cb);
static void pace_submit(uint32_t n, const VkCommandBuffer* cbs);
// WoW's ground in BG3's G-buffer (wowbg3_ground.inc.c, included at the end).
static void ground_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const uint8_t* load, const int32_t* fin, uint32_t w, uint32_t h);
static void ground_bind(VkCommandBuffer cb);
static void ground_view_seen(void);
static void ground_end(DeviceData* d, VkCommandBuffer cb);
static void late_pass_begin(DeviceData* d, VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const uint8_t* load, const int32_t* ini, const int32_t* fin);  // wowbg3_fxbg.inc.c
static void late_log_draw(VkCommandBuffer cb);
static void shade_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const uint32_t* layers, const int32_t* fin, uint32_t w, uint32_t h);  // wowbg3_shade.inc.c
static void shade_view(VkCommandBuffer cb);
static void shade_end(DeviceData* d, VkCommandBuffer cb);
static void ground_submit(uint32_t n, const VkCommandBuffer* cbs);
static void ground_forget_image(DeviceData* d, VkImage img);
static void shade_forget_image(DeviceData* d, VkImage img);
static void ground_note_viewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v);
static void ground_note_scissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r);
static void ground_note_pipeline(const VkGraphicsPipelineCreateInfo* ci);
static void ground_present(void);
// BG3's ground surfaces shown on WoW's ground (wowbg3_surf.inc.c, included at the end).
static void surf_pass_begin(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const int32_t* ini, const uint8_t* load, const int32_t* fin);
static int surf_draw(VkCommandBuffer cb);
static void surf_pass_end(DeviceData* d, VkCommandBuffer cb);
static void surf_end_cb(DeviceData* d, VkCommandBuffer cb);
static void surf_present(void);
static void surf_forget_image(DeviceData* d, VkImage img);
static void surf_note_barrier(VkCommandBuffer cb, uint64_t img, int32_t layout);
static void surf_kept(void);
static void surf_log_request(int n);
// The WoW body over BG3's examine and trade previews of a stand-in (wowbg3_preview.inc.c, at the end).
static void pv_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const int32_t* fin);
static void pv_draw(VkCommandBuffer cb);
static void pv_pass_end(DeviceData* d, VkCommandBuffer cb);
static void pv_present(QueueData* q);
static void pace_request(int n);
static void pace_trailer(uint8_t* p, uint32_t frame);
static void pace_before_compose(void);
static void pace_after_compose(uint64_t t_in);
#define PACE_TRAILER 128
static size_t pace_pose_snapshot(uint8_t* out, size_t cap);

// ---------------------------------------------------------------- request state
static struct {
    struct timespec seen;  // request file mtime last acted on
    int active;
    int remaining;
    int every;
    int index;
    int frames_written;
    uint64_t presents;
    char tag[48];
} g_req;

static void check_request(void) {
    struct stat st;
    if (stat(outp("/request"), &st) != 0) return;
    if (st.st_mtim.tv_sec == g_req.seen.tv_sec && st.st_mtim.tv_nsec == g_req.seen.tv_nsec) return;
    g_req.seen = st.st_mtim;
    FILE* f = fopen(outp("/request"), "r");
    if (!f) return;
    int count = 0, every = 1;
    char tag[48] = {0};
    int n = fscanf(f, "%d %d %47s", &count, &every, tag);
    fclose(f);
    if (n != 3 || count < 1 || count > 1500 || every < 1 || every > 60) {
        logf_("request ignored: fields %d count %d every %d", n, count, every);
        return;
    }
    for (char* c = tag; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) {
            logf_("request ignored: bad tag");
            return;
        }
    }
    if (!strncmp(tag, "trace", 5)) {
        trace_arm(tag);
        return;
    }
    if (!strcmp(tag, "fxdump")) { g_fx_dump_request = 1; logf_("fx: dump armed"); return; }
    if (!strcmp(tag, "bufs")) { g_bufs_request = 1; return; }
    if (!strcmp(tag, "tavlog")) { g_tavlog = 2; logf_("tavlog: armed"); return; }
    if (!strcmp(tag, "shwlog")) { g_shwlog = 12; logf_("shwlog: armed"); return; }
    if (!strncmp(tag, "pace", 4)) { pace_request(atoi(tag + 4)); return; }
    if (!strncmp(tag, "surflog", 7)) { surf_log_request(atoi(tag + 7)); return; }
    if (!strncmp(tag, "fxwhy", 5)) { int f = atoi(tag + 5); g_fxw_frames = f > 0 && f <= 150 ? f : 1; logf_("fxwhy: armed for %d presents", g_fxw_frames); return; }
    if (!strncmp(tag, "sight", 5)) { int f = atoi(tag + 5); g_sight_lines = f > 0 && f <= 600 ? f : 1; logf_("sight: armed for %d draws", g_sight_lines); return; }
    if (!strncmp(tag, "fxlog", 5)) { int f = atoi(tag + 5); g_fxl_frames = f > 0 && f <= 60 ? f : 1; logf_("fxlog: armed for %d presents", g_fxl_frames); return; }
    if (tag[0] == 'r' && tag[1] == 'b') {
        unsigned long long h = 0, off = 0, size = 0;
        if (sscanf(tag + 2, "%llx-%llx-%llx", &h, &off, &size) == 3 && size > 0 && size <= (64u << 20)) {
            g_rb.h = h; g_rb.off = off; g_rb.size = size; g_rb.armed = 1;
            snprintf(g_rb.tag, sizeof g_rb.tag, "%s", tag);
            logf_("readback armed: buffer %llx +%llx %llx bytes", h, off, size);
        }
        return;
    }
    if (!strncmp(tag, "img", 3)) {
        unsigned id = 0; int layout = 5;
        if (sscanf(tag + 3, "%ul%d", &id, &layout) >= 1) {
            g_dump.id = id; g_dump.layout = layout; g_dump.armed = 1;
            snprintf(g_dump.tag, sizeof g_dump.tag, "%s", tag);
            logf_("dump armed: image %u from layout %d", id, layout);
        }
        return;
    }
    g_req.active = 1;
    g_req.remaining = count;
    g_req.every = every;
    g_req.index = 0;
    g_req.frames_written = 0;
    snprintf(g_req.tag, sizeof g_req.tag, "%s", tag);
    logf_("request: %d frames every %d presents, tag %s", count, every, tag);
}

// ---------------------------------------------------------------- frame trace (research)
// Request tag "trace..." arms a trace for the next TRACE_PRESENTS presents: every render pass
// (attachments with format and size), draw and dispatch counts, copies and blits, in queue
// submission order, written to OUT_DIR/<tag>.txt. Resources are tracked all the time (cheap
// tables); command recording is only logged while armed.
#define TRACE_PRESENTS 3
#define IMG_CAP (1u << 17)
#define VIEW_CAP (1u << 17)
#define FB_CAP (1u << 14)
#define RP_CAP (1u << 13)
#define CB_CAP (1u << 13)

typedef struct { uint64_t h; uint32_t id; VkFormat fmt; uint32_t w, hgt, depth, layers, mips; VkImageUsageFlags usage; VkSampleCountFlagBits samples; VkImageType type; uint64_t copy_src; } ImgInfo;
typedef struct { uint64_t h; uint64_t image; uint32_t layer; } ViewInfo;  // layer: the view's first array layer
typedef struct { uint64_t h; uint32_t n; uint64_t views[8]; } FbInfo;
typedef struct { uint64_t h; uint32_t n; VkFormat fmts[8]; uint8_t load[8], store[8]; int32_t final[8], init[8]; } RpInfo;
typedef struct {
    uint64_t h;
    char* buf; size_t len, cap;
    int in_pass; char pass[640];
    uint32_t draws, dispatches; uint64_t prims;
    uint64_t reads[12]; uint32_t nreads;  // render targets the pass's draws sample (research: the post chain)
    uint64_t frag[4]; uint32_t nfrag;     // the first fragment shaders (spv keys) the pass draws with
} CbLog;

static ImgInfo* g_img;
static ViewInfo* g_view;
static FbInfo* g_fb;
static RpInfo* g_rp;
static CbLog* g_cb;
static uint32_t g_img_ids;
static pthread_mutex_t g_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_trace_armed;  // presents left to trace
static FILE* g_trace_out;
static uint64_t g_trace_submits;

static uint32_t hslot(uint64_t h, uint32_t cap) {
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return (uint32_t)h & (cap - 1);
}
#define TABLE_FIND(tab, cap, key, create) ({ \
    __typeof__(&(tab)[0]) _r = NULL; uint32_t _i = hslot((key), (cap)); \
    for (uint32_t _n = 0; _n < (cap); _n++, _i = (_i + 1) & ((cap) - 1)) { \
        if ((tab)[_i].h == (key)) { _r = &(tab)[_i]; break; } \
        if ((tab)[_i].h == 0) { if (create) { (tab)[_i].h = (key); _r = &(tab)[_i]; } break; } \
    } _r; })

// Which planned meshes BG3 drew (the LOD format research: heads vanished when
// zoomed out and lagged the body when zoomed back in, as BG3 drew a coarser LOD than the one planned).
// Every LOD is planned (TavPlan.lua M lines with a LOD field). On the recording threads each indexed
// draw of BG3's G-buffer pass (role_pass: depth and 3+ colour targets at the swapchain's size; also its
// secondary command buffers, by their inherited framebuffer) whose index buffer a planned mesh uses
// sets that mesh's bit for the command buffer (before the hiding drops the draw). At submit, in
// order, the G-buffer command buffers' bits are joined until the final pass's command buffer: that
// frame's set (as pace_submit makes its pose snapshot the frame's). tav_write_pose then sets flag 2
// (drawn) on those records and flag 4 (tracking valid) on every record of the frame; frames without a
// tracked G-buffer pass, or with one recorded for another plan, carry neither, and benilla draws LOD 0.
// Per instance: a slot per planned mesh (its plan index). Figures that share a model
// share its index buffers (Gale's and Astarion's HUM_M body), so an index buffer lists every planned
// mesh drawn from it (g_tavd_list); a draw from one that several share marks the mesh whose position in
// the last pose is nearest to where BG3 places the draw (fxc_place: its object constants or instance
// rows), and any other within 0.3 m of that; a draw that cannot be placed near one of them marks all.
#define TAVD_W (TAV_MAX / 64)
#define TAVD_CAP 2048
typedef struct { uint64_t ib; uint32_t first, n; } TavdEnt;
static TavdEnt g_tavd_tab[2][TAVD_CAP];      // index buffer -> its planned meshes (g_tavd_list[first..first+n])
static uint16_t g_tavd_list[2][TAV_MAX];     // per table: the planned meshes, grouped by index buffer
static float g_tavd_pos[TAV_MAX][3];         // each planned mesh's world position in the last pose
static volatile uint64_t g_tavd_placed, g_tavd_unplaced;  // shared-buffer draws placed / marked for all (log)
static volatile uint32_t g_tavd_seq[2];       // the plan each table was built for
static volatile int g_tavd_cur = -1;          // the table recording threads read
typedef struct { uint64_t h; uint32_t seq, valid; uint64_t bits[TAVD_W]; } TavdCb;
#define TAVDCB_CAP (1u << 12)
static TavdCb g_tavdcb[TAVDCB_CAP];           // per command buffer (g_trace_lock)
static struct { uint32_t seq, n, bad; uint64_t bits[TAVD_W]; } g_tavd_acc;  // since the last final pass (g_trace_lock)
static struct { pthread_mutex_t m; uint32_t seq, valid; uint64_t bits[TAVD_W]; uint64_t frames; } g_tavd_frame = {PTHREAD_MUTEX_INITIALIZER, 0, 0, {0}, 0};
static __thread VkCommandBuffer t_tavd_cb;    // a G-buffer pass is being recorded into this, on this thread
static __thread int t_tavd_tab;
static __thread uint64_t t_tavd_bits[TAVD_W];
static void tavd_flush_locked(void) {
    if (!t_tavd_cb) return;
    TavdCb* e = TABLE_FIND(g_tavdcb, TAVDCB_CAP, (uint64_t)(uintptr_t)t_tavd_cb, 1);
    uint32_t seq = g_tavd_seq[t_tavd_tab];
    if (e) {
        if (!e->valid) { e->seq = seq; e->valid = 1; memset(e->bits, 0, sizeof e->bits); }
        if (e->seq != seq) e->valid = 2;  // two plans in one command buffer: no frame from it
        for (int w = 0; w < TAVD_W; w++) e->bits[w] |= t_tavd_bits[w];
    }
    t_tavd_cb = VK_NULL_HANDLE;
}
// A G-buffer pass begins on cb (g_trace_lock held).
static void tavd_begin(VkCommandBuffer cb) {
    if (t_tavd_cb && t_tavd_cb != cb) tavd_flush_locked();
    int k = __atomic_load_n(&g_tavd_cur, __ATOMIC_ACQUIRE);
    if (k < 0) return;
    if (t_tavd_cb != cb) memset(t_tavd_bits, 0, sizeof t_tavd_bits);
    t_tavd_cb = cb;
    t_tavd_tab = k;
}
static void tavd_end(VkCommandBuffer cb) {
    if (t_tavd_cb != cb || !cb) return;
    pthread_mutex_lock(&g_trace_lock);
    tavd_flush_locked();
    pthread_mutex_unlock(&g_trace_lock);
}
static void tavd_note_shared(VkCommandBuffer cb, uint32_t first, uint32_t n, uint32_t fi);
// An indexed draw on cb with index buffer ib, first instance fi (recording thread, no lock).
static inline void tavd_note(VkCommandBuffer cb, uint64_t ib, uint32_t fi) {
    if (t_tavd_cb != cb || !ib) return;
    const TavdEnt* tab = g_tavd_tab[t_tavd_tab];
    for (uint32_t i = tav_hash(ib) & (TAVD_CAP - 1), k = 0; k < TAVD_CAP; k++, i = (i + 1) & (TAVD_CAP - 1)) {
        uint64_t v = tab[i].ib;
        if (!v) return;
        if (v != ib) continue;
        if (tab[i].n > 1) { tavd_note_shared(cb, tab[i].first, tab[i].n, fi); return; }
        uint32_t s = tab[i].n && tab[i].first < TAV_MAX ? g_tavd_list[t_tavd_tab][tab[i].first] : TAV_MAX;
        if (s < TAV_MAX) t_tavd_bits[s >> 6] |= 1ull << (s & 63);
        return;
    }
}

static int trace_tables(void) {
    if (g_img) return 1;
    g_img = calloc(IMG_CAP, sizeof *g_img);
    g_view = calloc(VIEW_CAP, sizeof *g_view);
    g_fb = calloc(FB_CAP, sizeof *g_fb);
    g_rp = calloc(RP_CAP, sizeof *g_rp);
    g_cb = calloc(CB_CAP, sizeof *g_cb);
    g_buf = calloc(BUF_CAP, sizeof *g_buf);
    return g_img && g_view && g_fb && g_rp && g_cb && g_buf;
}

static void cb_printf(CbLog* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void cb_printf(CbLog* c, const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof tmp) n = sizeof tmp - 1;
    if (c->len + (size_t)n + 1 > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 4096;
        while (cap < c->len + (size_t)n + 1) cap *= 2;
        if (cap > (8u << 20)) return;
        char* b = realloc(c->buf, cap);
        if (!b) return;
        c->buf = b; c->cap = cap;
    }
    memcpy(c->buf + c->len, tmp, (size_t)n);
    c->len += (size_t)n;
    c->buf[c->len] = 0;
}

static void img_desc(char* out, size_t cap, uint64_t image) {
    ImgInfo* i = image ? TABLE_FIND(g_img, IMG_CAP, image, 0) : NULL;
    if (!i) { snprintf(out, cap, "?%llx", (unsigned long long)image); return; }
    snprintf(out, cap, "i%u:f%d:%ux%u%s%s%s", i->id, (int)i->fmt, i->w, i->hgt,
             i->samples > 1 ? ":ms" : "", (i->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ? ":s" : "",
             (i->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? ":t" : "");
}
static uint64_t view_image(uint64_t view) {
    ViewInfo* v = view ? TABLE_FIND(g_view, VIEW_CAP, view, 0) : NULL;
    return v ? v->image : 0;
}
static uint32_t view_layer(uint64_t view) {
    ViewInfo* v = view ? TABLE_FIND(g_view, VIEW_CAP, view, 0) : NULL;
    return v ? v->layer : 0;
}

static CbLog* cb_log(VkCommandBuffer cb, int create) {
    return TABLE_FIND(g_cb, CB_CAP, (uint64_t)(uintptr_t)cb, create);
}
static void cb_flush_pass(CbLog* c) {
    if (c->in_pass) {
        cb_printf(c, "  %s draws=%u prims=%llu\n", c->pass, c->draws, (unsigned long long)c->prims);
        for (uint32_t k = 0; k < c->nreads; k++) {
            char d[96];
            img_desc(d, sizeof d, c->reads[k]);
            cb_printf(c, "    reads %s\n", d);
        }
        for (uint32_t k = 0; k < c->nfrag; k++) cb_printf(c, "    frag %016llx\n", (unsigned long long)c->frag[k]);
    } else if (c->draws || c->dispatches) {
        cb_printf(c, "  (outside) draws=%u dispatches=%u\n", c->draws, c->dispatches);
    }
    if (c->in_pass && c->dispatches) cb_printf(c, "  (in pass) dispatches=%u\n", c->dispatches);
    c->draws = 0; c->dispatches = 0; c->prims = 0; c->in_pass = 0; c->nreads = 0; c->nfrag = 0;
}

// Returns the log to append to, with g_trace_lock held, or NULL (lock not held).
static CbLog* trace_cb(VkCommandBuffer cb) {
    if (!g_trace_armed || !g_img) return NULL;
    pthread_mutex_lock(&g_trace_lock);
    CbLog* c = cb_log(cb, 1);
    if (!c) pthread_mutex_unlock(&g_trace_lock);
    return c;
}

static int32_t memory_type(DeviceData* d, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    d->inst->GetPhysicalDeviceMemoryProperties(d->physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return (int32_t)i;
    }
    return -1;
}

static Staging* staging_for(DeviceData* d, uint32_t family, VkDeviceSize size) {
    Staging* s = NULL;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (g_staging[i].dev == d && g_staging[i].family == family) { s = &g_staging[i]; break; }
    }
    if (!s) {
        for (int i = 0; i < MAX_ENTRIES; i++) if (!g_staging[i].dev) { s = &g_staging[i]; break; }
        if (!s) return NULL;
        memset(s, 0, sizeof *s);
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
        if (d->CreateCommandPool(d->device, &pci, NULL, &s->pool) != VK_SUCCESS) return NULL;
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool,
            VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (d->AllocateCommandBuffers(d->device, &ai, &s->cmd) != VK_SUCCESS) return NULL;
        // Command buffers made by a layer are dispatchable and need the loader's table.
        if (d->setLoaderData && d->setLoaderData(d->device, s->cmd) != VK_SUCCESS) return NULL;
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
        if (d->CreateFence(d->device, &fci, NULL, &s->fence) != VK_SUCCESS) return NULL;
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
        // Rotated so a semaphore is never signalled again before the present that waits on it ran.
        for (int k = 0; k < 3; k++) {
            if (d->CreateSemaphore(d->device, &sci, NULL, &s->done[k]) != VK_SUCCESS) return NULL;
        }
        s->dev = d;
        s->family = family;
    }
    if (s->size < size) {
        if (s->buffer) { d->DestroyBuffer(d->device, s->buffer, NULL); s->buffer = VK_NULL_HANDLE; }
        if (s->memory) { d->FreeMemory(d->device, s->memory, NULL); s->memory = VK_NULL_HANDLE; }
        s->mapped = NULL;
        s->size = 0;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (d->CreateBuffer(d->device, &bci, NULL, &s->buffer) != VK_SUCCESS) return NULL;
        VkMemoryRequirements req;
        d->GetBufferMemoryRequirements(d->device, s->buffer, &req);
        int32_t type = memory_type(d, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type < 0) type = memory_type(d, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type < 0) return NULL;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
        if (d->AllocateMemory(d->device, &mai, NULL, &s->memory) != VK_SUCCESS) return NULL;
        if (d->BindBufferMemory(d->device, s->buffer, s->memory, 0) != VK_SUCCESS) return NULL;
        if (d->MapMemory(d->device, s->memory, 0, VK_WHOLE_SIZE, 0, &s->mapped) != VK_SUCCESS) return NULL;
        s->size = size;
    }
    return s;
}

static void image_barrier(DeviceData* d, VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, srcAccess, dstAccess, from, to,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                          0, NULL, 0, NULL, 1, &b);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// Copies one presented image; returns a semaphore the real present must wait on, or null.
static VkSemaphore capture_image(QueueData* q, SwapchainData* c, uint32_t index,
                                 const VkPresentInfoKHR* info, VkSemaphore* waitOut) {
    (void)waitOut;
    DeviceData* d = q->dev;
    VkDeviceSize size = (VkDeviceSize)c->extent.width * c->extent.height * 4;
    Staging* s = staging_for(d, q->family, size);
    if (!s) { logf_("capture: staging unavailable"); return VK_NULL_HANDLE; }
    d->ResetCommandBuffer(s->cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    d->BeginCommandBuffer(s->cmd, &bi);
    VkImage img = c->images[index];
    image_barrier(d, s->cmd, img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
        {c->extent.width, c->extent.height, 1}};
    d->CmdCopyImageToBuffer(s->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->buffer, 1, &region);
    image_barrier(d, s->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    d->EndCommandBuffer(s->cmd);
    VkPipelineStageFlags* stages = NULL;
    VkPipelineStageFlags stageBuf[16];
    uint32_t waits = info->waitSemaphoreCount > 16 ? 16 : info->waitSemaphoreCount;
    for (uint32_t i = 0; i < waits; i++) stageBuf[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    stages = stageBuf;
    VkSemaphore done = s->done[s->next_done++ % 3];
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, waits, info->pWaitSemaphores, stages,
        1, &s->cmd, 1, &done};
    d->ResetFences(d->device, 1, &s->fence);
    if (d->QueueSubmit(q->queue, 1, &si, s->fence) != VK_SUCCESS) {
        logf_("capture: submit failed");
        return VK_NULL_HANDLE;
    }
    // Stall only while capturing: the copy is read back before the frame is shown.
    d->WaitForFences(d->device, 1, &s->fence, VK_TRUE, 2000000000ull);
    char path[160];
    snprintf(path, sizeof path, "%s/%s-%04d.raw", out_dir(), g_req.tag, g_req.frames_written);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(s->mapped, 1, (size_t)size, f);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/%s.json", out_dir(), g_req.tag);
    FILE* j = fopen(path, g_req.frames_written == 0 ? "w" : "a");
    if (j) {
        fprintf(j, "{\"frame\":%d,\"present\":%llu,\"time\":%.6f,\"width\":%u,\"height\":%u,\"format\":%d}\n",
                g_req.frames_written, (unsigned long long)g_req.presents, now_s(), c->extent.width,
                c->extent.height, (int)c->format);
        fclose(j);
    }
    g_req.frames_written++;
    return done;
}

// ---------------------------------------------------------------- compositor
// BG3 is the game window; WoW's world is drawn behind BG3's characters. While the file
// OUT_DIR/compose exists (see compose_config for its words), every present is composited (on the
// GPU, wowbg3_compose_gpu.inc.c; the CPU merge in compose_frame is the fallback)
// from BG3's final picture, its scene depth and its HUD target (found by role, below), WoW's
// frame from WOWFRAME (benilla's WBF1 file: world plus WoW's own UI, and WoW's depth) and BG3's
// projection from BG3CAM (the plugin's lead record). Per pixel: BG3's picture stays where its
// surface is nearer than WoW's (and within "far"); elsewhere WoW with BG3's HUD over it. Any
// missing input leaves the frame untouched.
// benilla writes two slot files alternately; the newer whole one is used.

// Roles, found from the command stream (image ids and creation order change between runs):
//  scene depth = the depth attachment (at swapchain size) of the G-buffer pass (3+ colour targets);
//  main UI     = the HUD BG3 actually shows: the RGBA8/BGRA8 swapchain-sized colour target that
//                BG3's final pass (the one drawing into the swapchain image) samples, taken from
//                the last such pass submitted before the present (premultiplied alpha). BG3 draws
//                its UI through two more HUD-sized RGBA8+S8 targets per frame, Noesis panel caches
//                that are never cleared (garbage from aliased memory, panels on alternate frames);
//                compositing one of them made the UI flash (desktop playtest).
//                Until a final pass has been seen: the RGBA8+S8 target with most draws in the last
//                frame (the old guess; a guess that is not premultiplied is set aside).
// Their layouts at present time come from the render pass final layouts and barriers recorded
// in each command buffer, and the UI's from the final pass's descriptor, applied in queue
// submission order.
typedef struct { uint64_t img; uint32_t draws; } UiCand;
static struct {
    volatile uint64_t depth, ui;
    int dlayout, ulayout;          // layout after the last submitted use, -1 unknown
    UiCand cand[8]; uint32_t ncand;
    uint64_t bad[8]; uint32_t nbad;  // HUD-sized targets that are not the HUD (see compose_frame)
    uint32_t ew, eh;               // swapchain extent
    VkFormat dfmt, ufmt;
    int fin_seen;                  // a final pass sampling a UI target was submitted: main UI from it
    uint64_t hud[8]; uint32_t nhud;  // RGBA8+S8 targets seen (a final pass sampling two prefers these)
    uint64_t logged[4]; uint32_t nlogged;  // UI targets already logged (BG3 alternates two at times)
    uint64_t switches;             // main UI changes, for the log
    // BG3's scene size: its G-buffer pass's (scene_seen). The screen's (ew, eh) unless BG3 upscales: with FSR 1.0 or 2.2
    // it renders its scene smaller and scales it up before its display transform. Every pass of the scene (depth, G-buffer,
    // lighting, composite, effects) is at this size, the UI and the final picture at the screen's. 0: not seen yet.
    uint32_t sw, sh;
    uint64_t seen_at;  // 1 + the present at which the scene's G-buffer pass was last begun (scene_seen); 0: never
} g_role = {0, 0, -1, -1, {{0, 0}}, 0, {0}, 0, 0, 0, 0, 0, 0, {0}, 0, {0}, 0, 0, 0, 0, 0};
#define SCENE_W (g_role.sw ? g_role.sw : g_role.ew)
#define SCENE_H (g_role.sh ? g_role.sh : g_role.eh)
static __thread VkCommandBuffer t_ui_cb;
static __thread uint64_t t_ui_img;
static __thread uint32_t t_ui_draws;
static __thread VkCommandBuffer t_sc_cb;  // the pass on this thread draws into a swapchain image
// Per command buffer: last layouts of the depth and UI roles (ul is uimg's), and the UI target its
// final pass sampled (fin_ui, in descriptor layout fin_ul).
typedef struct { uint64_t h; int dl, ul, init; uint64_t uimg, fin_ui; int fin_ul; } CbRole;
#define CBROLE_CAP (1u << 13)
static CbRole g_cbrole[CBROLE_CAP];

static int is_ui_color(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM;
}
static int is_swapchain_image(uint64_t img) {
    for (int i = 0; i < MAX_ENTRIES; i++)
        for (uint32_t k = 0; g_chains[i].chain && k < g_chains[i].count && k < 16; k++)
            if ((uint64_t)g_chains[i].images[k] == img) return 1;
    return 0;
}
static void role_cb_clear(CbRole* r) { r->dl = r->ul = -1; r->init = 1; r->uimg = r->fin_ui = 0; r->fin_ul = -1; }
// g_trace_lock held.
static void role_set_layout(VkCommandBuffer cb, uint64_t img, int layout) {
    if (!img || (img != g_role.depth && img != g_role.ui)) return;
    CbRole* r = TABLE_FIND(g_cbrole, CBROLE_CAP, (uint64_t)(uintptr_t)cb, 1);
    if (!r) return;
    if (!r->init) role_cb_clear(r);
    if (img == g_role.depth) r->dl = layout;
    if (img == g_role.ui) { r->ul = layout; r->uimg = img; }
}
static void role_reset_cb(VkCommandBuffer cb) {
    pthread_mutex_lock(&g_trace_lock);
    CbRole* r = TABLE_FIND(g_cbrole, CBROLE_CAP, (uint64_t)(uintptr_t)cb, 0);
    if (r) role_cb_clear(r);
    pthread_mutex_unlock(&g_trace_lock);
}
// A pass begins on `cb` with attachment images `imgs` (final layouts `fin`, -1 unknown). g_trace_lock held.
static void role_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const int32_t* fin, const uint8_t* load) {
    if (!g_role.ew) return;
    uint32_t colors = 0, scolors = 0, stencil = 0;
    uint64_t depth = 0, ui = 0; int32_t dfin = -1, ufin = -1;
    VkFormat df = 0, uf = 0;
    int sc = 0, fwd = 0;
    for (uint32_t k = 0; k < n; k++) {
        ImgInfo* i = imgs[k] ? TABLE_FIND(g_img, IMG_CAP, imgs[k], 0) : NULL;
        if (!i) { if (imgs[k] && is_swapchain_image(imgs[k])) sc = 1; continue; }
        // The scene's depth and G-buffer at the scene's size; the UI's target and its stencil at the screen's.
        const int scene = i->w == SCENE_W && i->hgt == SCENE_H, screen = i->w == g_role.ew && i->hgt == g_role.eh;
        if (!scene && !screen) continue;
        if (i->fmt == VK_FORMAT_S8_UINT) { if (screen) stencil++; }
        else if (is_depth_format(i->fmt)) { if (scene) { depth = imgs[k]; dfin = fin[k]; df = i->fmt; } }
        else {
            if (scene) colors++;
            if (scene && i->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && load[k] == VK_ATTACHMENT_LOAD_OP_CLEAR) fwd++;
            if (screen) { scolors++; if (is_ui_color(i->fmt)) { ui = imgs[k]; ufin = fin[k]; uf = i->fmt; } }
        }
    }
    t_sc_cb = sc ? cb : VK_NULL_HANDLE;
    if (depth && colors >= 3) pace_gbuffer(cb);
    if (depth && colors >= 3) tavd_begin(cb);
    else if (t_tavd_cb == cb) tavd_flush_locked();
    // The scene depth is named by BG3's G-buffer pass and by its forward pass (the scene's B10G11R11 target cleared, on the
    // scene depth), whichever is recorded first. BG3 records a frame's passes on several threads at once, and when it makes
    // its targets again (each Shift press, an armed ability) the forward pass was at times recorded before the G-buffer pass:
    // on the old depth's role, so that frame had no feed (a black world, then red ground while TAA forgot it; Nvidia).
    if (depth && (colors >= 3 || (colors == 1 && fwd == 1)) && g_role.depth != depth) {
        g_role.depth = depth; g_role.dfmt = df; g_role.dlayout = -1;
        char s[96]; img_desc(s, sizeof s, depth);
        logf_("roles: scene depth %s (named by its %s pass, present %llu)", s, colors >= 3 ? "G-buffer" : "forward", (unsigned long long)__atomic_load_n(&g_req.presents, __ATOMIC_RELAXED));
    }
    if (stencil && scolors == 1 && ui) { t_ui_cb = cb; t_ui_img = ui; t_ui_draws = 0; if (!g_role.ufmt) g_role.ufmt = uf; }
    for (uint32_t k = 0; k < n; k++) if (fin[k] >= 0) role_set_layout(cb, imgs[k], fin[k]);
    (void)dfin; (void)ufin;
}
static void role_end_pass(VkCommandBuffer cb) {
    if (t_ui_cb != cb || !t_ui_img) { t_ui_cb = VK_NULL_HANDLE; return; }
    pthread_mutex_lock(&g_trace_lock);
    uint32_t k = 0;
    while (k < g_role.ncand && g_role.cand[k].img != t_ui_img) k++;
    if (k == g_role.ncand && k < 8) { g_role.cand[k].img = t_ui_img; g_role.cand[k].draws = 0; g_role.ncand++; }
    if (k < 8) g_role.cand[k].draws += t_ui_draws;
    uint32_t h = 0;
    while (h < g_role.nhud && g_role.hud[h] != t_ui_img) h++;
    if (h == g_role.nhud) {
        if (g_role.nhud < 8) g_role.hud[g_role.nhud++] = t_ui_img;
        else { memmove(g_role.hud, g_role.hud + 1, 7 * sizeof g_role.hud[0]); g_role.hud[7] = t_ui_img; }
    }
    pthread_mutex_unlock(&g_trace_lock);
    t_ui_cb = VK_NULL_HANDLE; t_ui_img = 0;
}
// Submission order is execution order: carry each command buffer's last role layouts, and take
// the main UI from the last submitted final pass. g_trace_lock held.
static void role_submit(uint32_t n, const VkCommandBuffer* cbs) {
    for (uint32_t k = 0; k < n; k++) {
        CbRole* r = TABLE_FIND(g_cbrole, CBROLE_CAP, (uint64_t)(uintptr_t)cbs[k], 0);
        if (!r) continue;
        if (r->dl >= 0) g_role.dlayout = r->dl;
        if (r->fin_ui) {
            if (r->fin_ui != g_role.ui) {
                g_role.ui = r->fin_ui; g_role.ulayout = -1; g_role.switches++;
                ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, r->fin_ui, 0);
                if (i) g_role.ufmt = i->fmt;
            }
            if (r->fin_ul > 0) g_role.ulayout = r->fin_ul;
            g_role.fin_seen = 1;
        }
        if (r->ul >= 0 && r->uimg && r->uimg == g_role.ui) g_role.ulayout = r->ul;
    }
    pace_submit(n, cbs);
    ground_submit(n, cbs);
    for (uint32_t k = 0; k < n; k++) {
        TavdCb* e = TABLE_FIND(g_tavdcb, TAVDCB_CAP, (uint64_t)(uintptr_t)cbs[k], 0);
        if (e && e->valid) {
            if (!g_tavd_acc.n) { g_tavd_acc.seq = e->seq; g_tavd_acc.bad = 0; memset(g_tavd_acc.bits, 0, sizeof g_tavd_acc.bits); }
            if (e->valid != 1 || e->seq != g_tavd_acc.seq) g_tavd_acc.bad = 1;
            for (int w = 0; w < TAVD_W; w++) g_tavd_acc.bits[w] |= e->bits[w];
            g_tavd_acc.n++;
            e->valid = 0;  // counted once
        }
        CbRole* r = TABLE_FIND(g_cbrole, CBROLE_CAP, (uint64_t)(uintptr_t)cbs[k], 0);
        if (r && r->init && r->fin_ui) {
            pthread_mutex_lock(&g_tavd_frame.m);
            g_tavd_frame.valid = g_tavd_acc.n > 0 && !g_tavd_acc.bad;
            g_tavd_frame.seq = g_tavd_acc.seq;
            memcpy(g_tavd_frame.bits, g_tavd_acc.bits, sizeof g_tavd_acc.bits);
            g_tavd_frame.frames++;
            pthread_mutex_unlock(&g_tavd_frame.m);
            g_tavd_acc.n = 0;
        }
    }
}
// At present: once BG3's final pass has been seen the main UI is already set (role_submit);
// before that, the UI target is the candidate with most draws since the last present.
static void role_present(SwapchainData* c) {
    pthread_mutex_lock(&g_trace_lock);
    if (c) { g_role.ew = c->extent.width; g_role.eh = c->extent.height; }
    if (g_role.fin_seen) {
        uint32_t k = 0;
        while (k < g_role.nlogged && g_role.logged[k] != g_role.ui) k++;
        if (g_role.ui && k == g_role.nlogged) {
            if (g_role.nlogged < 4) g_role.nlogged++;
            memmove(g_role.logged + 1, g_role.logged, (g_role.nlogged - 1) * sizeof g_role.logged[0]);
            g_role.logged[0] = g_role.ui;
            char s[96]; img_desc(s, sizeof s, g_role.ui);
            logf_("roles: main UI %s (sampled by BG3's final pass, layout %d; %llu changes)", s, g_role.ulayout,
                  (unsigned long long)g_role.switches);
        }
        g_role.ncand = 0;
        pthread_mutex_unlock(&g_trace_lock);
        return;
    }
    uint32_t best = 0, cur = 0; uint64_t img = 0;
    for (uint32_t k = 0; k < g_role.ncand; k++) {
        int bad = 0;
        for (uint32_t b = 0; b < g_role.nbad; b++) bad |= g_role.bad[b] == g_role.cand[k].img;
        if (!bad && g_role.cand[k].draws > best) { best = g_role.cand[k].draws; img = g_role.cand[k].img; }
        if (g_role.cand[k].img == g_role.ui) cur = g_role.cand[k].draws;
    }
    (void)cur;
    if (img && img != g_role.ui) {
        g_role.ui = img; g_role.ulayout = -1;
        ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, img, 0);
        if (i) g_role.ufmt = i->fmt;
        char s[96]; img_desc(s, sizeof s, img); logf_("roles: main UI %s (%u draws)", s, best);
    }
    g_role.ncand = 0;
    pthread_mutex_unlock(&g_trace_lock);
}
static void role_forget(VkImage img) {  // g_trace_lock held
    if ((uint64_t)img == g_role.depth) { g_role.depth = 0; g_role.dlayout = -1; }
    if ((uint64_t)img == g_role.ui) { g_role.ui = 0; g_role.ulayout = -1; }
    for (uint32_t b = 0; b < g_role.nbad; b++)
        if (g_role.bad[b] == (uint64_t)img) { g_role.bad[b] = g_role.bad[--g_role.nbad]; break; }
    for (uint32_t h = 0; h < g_role.nhud; h++)
        if (g_role.hud[h] == (uint64_t)img) { memmove(g_role.hud + h, g_role.hud + h + 1, (g_role.nhud - h - 1) * sizeof g_role.hud[0]); g_role.nhud--; break; }
    for (uint32_t h = 0; h < g_role.nlogged; h++) if (g_role.logged[h] == (uint64_t)img) g_role.logged[h] = 0;
}

typedef struct {
    int on, ui, light, fx, bg3tav, fxcopy, void_, raw, cpu, fxsync, wowbar, fxdecals, fxsight, ground, fxwhole;
    int camp, camp_hold;           // camp_hold: 1 asked, 2 running (compose_gpu)
    uint32_t camp_hold_seq;        // bg3cam.bin's record when the hold began
    uint64_t camp_hold_ns;
    int fxsolid;   // a spell's unblended pieces in the effects pass captured too (word "fxsolid")
    int fxsolidlog;  // ... and their recipes and textures logged (research, word "fxsolidlog")
    // Research: "fxsolidonly=<hex>[,<hex>]" takes only those recipes as solid, "fxsolidskip=" leaves
    // those out, so one piece of a spell can be looked at on its own without a restart.
    uint64_t solid_only[8], solid_skip[8];
    uint32_t n_solid_only, n_solid_skip;
    int fxhl;      // a figure's hover highlight captured instead of dropped with her (word "fxhl")
    int surf, surfdbg, surfpix;  // BG3's ground surfaces on WoW's ground (wowbg3_surf.inc.c)
    int wdepth, wdfeed, wdlog, wdlock, wdgcam, wdeyelog, wdfog, wdfogdbg, wdfogst, wdlightlog, wdbare, wdbloomlog, wdbloom, wdjit, wdpost, wdpostcas, wdtable, wdpcdbg, noshade, fbstale, wdtime, wdpostdbg, wdexplog, wdexprd, wdlatelog; float wdexp, wdocc, wdevref, wdshade, wdzdbg, wdfloorout, wdbackfill; uint64_t wdskip; int fxbg, fxcal, fxbgmax, fxbgpass;  // BG3 draws its effects over WoW (wowbg3_fxbg.inc.c)
    float wdamb; int wdwfog, wdwlit;
    int fxdrop, fxkeep, fxbgfwd;  // fx-route prototypes: "fxdrop" capture all but a drop list; "fxkeep" the void drops by distance only
    int surfscene;  // ... their look taken from BG3's lit scene, not its pre-lighting layer (word "surfscene")
    int surfpost;   // ... taken AFTER BG3's effects pass instead of before it (word "surfpost", research)
    int fxwib;      // fxwhy groups a draw by its index buffer too, so each mesh is its own group (word "fxwib")
    int fxprops;    // a spell's 3D props kept in the void (word "fxprops"; see void_skip)
    int fxglow;     // a spell's glow on a figure captured, not dropped with her (word "fxglow"; see fxc_figure_extra)
    uint32_t fxpropage;  // ... while their mesh is younger than this many presents ("fxpropage=", 900)
    float sdec, sfwd, sfloor, stol;
    int fxtav;     // Effects kept only near Tav, as before (default: near Tav or BG3's eye)
    int screenvoid;  // BG3's own 3D screens: the character over one dark colour (word "screenvoid")
    int screenlevel; // BG3's own 3D screens: BG3's level drawn behind the character (word "noscreenlevel")
    int slight;    // BG3's surface decals lit by WoW's ground light (word "noslight": unlit, as before)
    int newest, lag, wait_us;  // frame pacing (wowbg3_pace.inc.c): newest WoW frame; fixed lag (-1 auto); wait bound
    float far;  // BG3 surfaces farther than this (metres) never win: Avernus far below the arena
    int wowraw;    // WoW's code-2 bytes used as they are (the old, washed-out look)
    float wgamma;  // benilla's `gamma` CVar ramp applied with the decode (1: none)
    struct timespec seen;
    uint64_t frames, skipped, why[8];
} ComposeCfg;
static ComposeCfg g_cc;

// OUT_DIR/compose present = on. Optional words: "noui" (keep BG3's final picture where WoW shows
// instead of rebuilding it from the UI target), "light" (add BG3's picture over WoW: effects in
// front of a dark void), "far=<metres>" (default 60: BG3 surfaces beyond it never cover WoW),
// "fx" (BG3's effects over WoW, experimental: BG3's level haze comes along; see the effects pass),
// "bg3tav" (BG3 keeps drawing Tav, instead of leaving her to WoW: see the Tav geometry copy),
// "fxcopy" (BG3's effects recorded for replay inside WoW: see wowbg3_fxcap.inc.c),
// "void" (BG3's map off: only characters, effects near Tav and the UI are drawn; see void_pass),
// "raw" (everything above stays on but BG3's own picture is presented: parity captures),
// "cpu" (the CPU compositor instead of the GPU one, for comparisons; wowbg3_compose_gpu.inc.c),
// "fxsync" (the effects readbacks wait at the present, as before; see fxj_main),
// "wowbar" (WoW's frame kept in video memory, written through the BAR; see wr_main).
// "camp" (a trip to BG3's camp, world_bridge.py: BG3's own picture with only WoW's interface over it,
// from frames benilla draws without the world, WBF1 flag 2; see compose_gpu). When the word goes,
// the backdrop stays until a WoW frame drawn for BG3's camera again arrives (camp_hold).
// "fxdecals" (BG3's deferred decal draws recorded too; see fxc_draw).
// "fxsolid" (a spell's unblended pieces in the effects pass captured too; see fxc_solid; "fxsolidlog"
// logs each recipe and texture it takes, research only), with
// "fxsolidonly=<hex>[,<hex>]" and "fxsolidskip=" naming recipes by a shader key for research.
// "fxhl" (a figure's hover highlight captured instead of dropped with her; see fxc_figure_extra. Off
// by default: BG3 draws that highlight on the character's own skinned meshes, which this does not
// cover, and a stand-in's BG3 body is the wrong shape for WoW's model anyway).
// "fxglow" (a spell's glow on a figure captured instead of dropped with her; see glow_draw).
// "fxwhole" (the effects capture as before: BG3's skinned vertex stream and light index
// lists copied whole instead of the parts a draw reads, see spv_meta, and draws benilla cannot
// replay kept, see fxc_unsup_draw).
// "ground" (WoW's ground drawn into BG3's G-buffer every frame, so BG3's sight overlay, surfaces and
// decals have ground to work on; see wowbg3_ground.inc.c).
// "wowraw" (WoW's frame bytes as they are: an earlier version encoded its sRGB target twice,
// the old look), "wowgamma=<g>" (benilla's `gamma` CVar, default 1; see put_pixel_from_wow).
// "surf" (with "ground": BG3's ground surfaces, water, grease, fire..., laid onto WoW's ground in the
// composite with BG3's own look; "surfdbg", "surfpix", "sdec=", "sfwd=", "sfloor=", "stol=", "surfscene", "surfpost", "fxprops", "fxpropage=": see
// wowbg3_surf.inc.c).
// "noscreenlevel" (BG3's own 3D screens keep the world mode's voiding, which is what 0.1.0 did: the
// character stands in an empty void with whatever of the host level the voiding does not reach, his
// waterfall among it. Without the word the voiding is off while such a screen is open, so BG3 draws
// the level's own scenery behind the character as stock BG3 does), "screenvoid" (neither: the
// compositor fills every pixel the character does not cover with one dark colour, see compose_gpu).
// Whether `line` has `w` as a whole word ("fx" is not in "fxcopy").
static int has_word(const char* line, const char* w) {
    size_t n = strlen(w);
    for (const char* p = strstr(line, w); p; p = strstr(p + 1, w)) {
        int start = p == line || p[-1] == ' ' || p[-1] == '\t';
        int end = p[n] == 0 || p[n] == ' ' || p[n] == '\t' || p[n] == '\n' || p[n] == '\r';
        if (start && end) return 1;
    }
    return 0;
}
static void compose_config(void) {
    struct stat st;
    char line[256] = {0};
    // No file: off, and every word off with it (an empty line is read). The words used to stay as they were, and
    // the passes that ask only for their own word (WoW's ground, WoW's depth and picture in BG3's scene) went on
    // drawing WoW's last frame into BG3's picture: seen in a BG3 level entered from world mode (the camp's level).
    const int off = stat(outp("/compose"), &st) != 0;
    if (off) {
        if (!g_cc.on) return;
        logf_("compose off");
        memset(&g_cc.seen, 0, sizeof g_cc.seen);
    } else {
        if (st.st_mtim.tv_sec == g_cc.seen.tv_sec && st.st_mtim.tv_nsec == g_cc.seen.tv_nsec) return;
        g_cc.seen = st.st_mtim;
        FILE* f = fopen(outp("/compose"), "r");
        if (!f) return;
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);
    }
    {
        const int camp = has_word(line, "camp");
        if (g_cc.camp && !camp) g_cc.camp_hold = 1;
        if (camp) g_cc.camp_hold = 0;
        g_cc.camp = camp;
    }
    g_cc.ui = !has_word(line, "noui");
    g_cc.light = has_word(line, "light");
    g_cc.fx = has_word(line, "fx");
    g_cc.bg3tav = has_word(line, "bg3tav");
    g_cc.fxcopy = has_word(line, "fxcopy");
    g_cc.void_ = has_word(line, "void");
    g_cc.raw = has_word(line, "raw");
    g_cc.cpu = has_word(line, "cpu");
    g_cc.fxsync = has_word(line, "fxsync");
    g_cc.fxwhole = has_word(line, "fxwhole");
    g_cc.wowbar = has_word(line, "wowbar");
    g_cc.fxdecals = has_word(line, "fxdecals");
    g_cc.fxsight = has_word(line, "fxsight");
    g_cc.fxsolid = has_word(line, "fxsolid");
    g_cc.fxsolidlog = has_word(line, "fxsolidlog");
    for (int which = 0; which < 2; which++) {
        const char* w = strstr(line, which ? "fxsolidskip=" : "fxsolidonly=");
        uint64_t* out = which ? g_cc.solid_skip : g_cc.solid_only;
        uint32_t* n = which ? &g_cc.n_solid_skip : &g_cc.n_solid_only;
        *n = 0;
        for (const char* q = w ? w + 12 : NULL; q && *n < 8; ) {
            char* end = NULL;
            uint64_t v = strtoull(q, &end, 16);
            if (end == q) break;
            out[(*n)++] = v;
            q = *end == ',' ? end + 1 : NULL;
        }
    }
    g_cc.fxhl = has_word(line, "fxhl");
    g_cc.ground = has_word(line, "ground");
    g_cc.surf = g_cc.ground && has_word(line, "surf");
    g_cc.surfdbg = has_word(line, "surfdbg");
    g_cc.surfpix = has_word(line, "surfpix");
    g_cc.surfscene = has_word(line, "surfscene");
    g_cc.fxbg = has_word(line, "fxbg");
    g_cc.fxcal = has_word(line, "fxcal");
    g_cc.fxbgmax = has_word(line, "fxbgmax");
    g_cc.fxbgpass = has_word(line, "fxbgraw");  // BG3's final picture as it is, not only what its effects changed
    g_cc.fxdrop = has_word(line, "fxdrop");
    g_cc.fxkeep = (has_word(line, "fxkeep") || g_cc.fxdrop || g_cc.fxbg) && !has_word(line, "fxnokeep");
    g_cc.fxbgfwd = has_word(line, "fxbgfwd");
    g_cc.wdepth = has_word(line, "wdepth");
    g_cc.wdfeed = has_word(line, "wdfeed");
    g_cc.wdfog = has_word(line, "wdfog");
    g_cc.wdfogst = has_word(line, "wdfogst");
    g_cc.wdlightlog = has_word(line, "wdlightlog");
    g_cc.wdbloomlog = has_word(line, "wdbloomlog");
    g_cc.wdbloom = has_word(line, "wdbloom");
    g_cc.wdbare = has_word(line, "wdbare");
    g_cc.fbstale = has_word(line, "fbstale");
    g_cc.wdjit = has_word(line, "wdjit");
    g_cc.wdpost = has_word(line, "wdpost");
    g_cc.wdwlit = has_word(line, "wdwlit");  // WoW's point lights on BG3's own pixels
    g_cc.wdwfog = has_word(line, "wdwfog");  // WoW's fog on BG3's own pixels (wowbg3_fxbg.inc.c)
    g_cc.noshade = has_word(line, "noshade");  // WoW's ground shade on BG3's figures off (wowbg3_shade.inc.c)
    g_cc.wdtable = has_word(line, "wdtable");  // research: WoW's pixels only through the feed's table, never their own colour after BG3's display transform
    g_cc.wdpostcas = has_word(line, "wdpostcas");
    g_cc.wdpostdbg = has_word(line, "wdpostdbg");
    g_cc.wdlatelog = has_word(line, "wdlatelog");  // research: BG3's effects passes and the attack preview's figure (wowbg3_fxbg.inc.c)
    g_cc.wdpcdbg = has_word(line, "wdpcdbg");  // wdpostcas shows the mask's classes as colours
    { const char* oc = strstr(line, "wdocc="); g_cc.wdocc = oc ? strtof(oc + 6, NULL) : 1.0f; if (!(g_cc.wdocc >= 0.0f && g_cc.wdocc <= 1.0f)) g_cc.wdocc = 1.0f; }
    { const char* fd = strstr(line, "wdfogdbg="); g_cc.wdfogdbg = fd ? atoi(fd + 9) : 0; }
    g_cc.wdlog = has_word(line, "wdlog");
    g_cc.wdlock = has_word(line, "wdlock");
    g_cc.wdgcam = has_word(line, "wdgcam");
    g_cc.wdeyelog = has_word(line, "wdeyelog");
    g_cc.wdtime = has_word(line, "wdtime");
    g_cc.wdexplog = has_word(line, "wdexplog");  // patch60
    g_cc.wdexprd = has_word(line, "wdexprd");
    { const char* ws = strstr(line, "wdskip="); g_cc.wdskip = ws ? strtoull(ws + 7, NULL, 16) : 0; }
    { const char* we = strstr(line, "wdexp="); g_cc.wdexp = we ? (float)atof(we + 6) : 0.0f; }
    { const char* wv = strstr(line, "wdevref="); g_cc.wdevref = wv ? strtof(wv + 8, NULL) : 0.0f; }  // patch61
    { const char* wv = strstr(line, "wdshade="); g_cc.wdshade = wv ? strtof(wv + 8, NULL) : 0.0f; }  // a test dial
    // Test dials for the look of a figure's side turned from the sun where WoW's outdoor law gives it the ambient alone
    // (a question for the maintainer, 2026-11-12; off unless given): wdfloorout=K a neutral floor of light on BG3's character
    // pixels under the outdoor law as well (the room law has ROOM_FLOOR); wdbackfill=K a fill from the side away from
    // the sun, K of the sun's diffuse, through the room lobes' shape (wowbg3_fxbg2.frag).
    { const char* wv = strstr(line, "wdfloorout="); g_cc.wdfloorout = wv ? strtof(wv + 11, NULL) : 0.0f; }
    { const char* wv = strstr(line, "wdbackfill="); g_cc.wdbackfill = wv ? strtof(wv + 11, NULL) : 0.0f; }
    { const char* wv = strstr(line, "wdzdbg="); g_cc.wdzdbg = wv ? strtof(wv + 7, NULL) : 0.0f; }  // a test view: BG3's pixels by the eye depth the feed reads for them  // a test dial: the sun's share for a figure in WoW's shade (0: the built-in one)
    { const char* wa = strstr(line, "wdamb="); g_cc.wdamb = wa ? strtof(wa + 6, NULL) : 0.0f; if (!(g_cc.wdamb >= 0.0f && g_cc.wdamb <= 4.0f)) g_cc.wdamb = 0.0f; }  // WoW's ambient on BG3's pixels
    if (g_cc.wdepth) { g_cc.fxbg = 1; g_cc.fxbgpass = 1; g_cc.fxbgfwd = 1; }
    g_cc.surfpost = has_word(line, "surfpost");
    g_cc.fxwib = has_word(line, "fxwib");
    g_cc.fxprops = has_word(line, "fxprops");
    g_cc.fxglow = has_word(line, "fxglow");
    g_cc.fxpropage = 900;
    { const char* w = strstr(line, "fxpropage="); if (w) { unsigned long v = strtoul(w + 10, NULL, 10); if (v) g_cc.fxpropage = (uint32_t)v; } }
    g_cc.slight = !has_word(line, "noslight");
    g_cc.screenlevel = !has_word(line, "noscreenlevel");
    g_cc.screenvoid = has_word(line, "screenvoid");
    g_cc.fxtav = has_word(line, "fxtav");
    const char* sd = strstr(line, "sdec=");
    g_cc.sdec = sd ? strtof(sd + 5, NULL) : 1.0f;
    const char* sf = strstr(line, "sfwd=");
    g_cc.sfwd = sf ? strtof(sf + 5, NULL) : 1.0f;
    const char* sl = strstr(line, "sfloor=");
    g_cc.sfloor = sl ? strtof(sl + 7, NULL) : 0.004f;
    const char* tl = strstr(line, "stol=");
    g_cc.stol = tl ? strtof(tl + 5, NULL) : 0.15f;
    if (!(g_cc.sdec >= 0.0f && g_cc.sdec <= 4.0f)) g_cc.sdec = 1.0f;
    if (!(g_cc.sfwd >= 0.0f && g_cc.sfwd <= 4.0f)) g_cc.sfwd = 1.0f;
    if (!(g_cc.sfloor >= 0.0f && g_cc.sfloor <= 1.0f)) g_cc.sfloor = 0.004f;
    if (!(g_cc.stol > 0.0f && g_cc.stol <= 10.0f)) g_cc.stol = 0.15f;
    g_cc.newest = has_word(line, "newest");
    const char* lg = strstr(line, "lag=");
    g_cc.lag = lg ? atoi(lg + 4) : -1;
    if (g_cc.lag > 3) g_cc.lag = 3;
    const char* wt = strstr(line, "wait=");
    // Default 1 ms (frame rates up to 165: a 6 ms frame cannot wait 3).
    g_cc.wait_us = wt ? (int)(strtof(wt + 5, NULL) * 1000.0f) : 1000;
    if (g_cc.wait_us < 0 || g_cc.wait_us > 20000) g_cc.wait_us = 1000;
    const char* fa = strstr(line, "far=");
    g_cc.far = fa ? strtof(fa + 4, NULL) : 60.0f;
    if (!(g_cc.far > 1.0f)) g_cc.far = 60.0f;
    g_cc.wowraw = has_word(line, "wowraw");
    const char* wg = strstr(line, "wowgamma=");
    g_cc.wgamma = wg ? strtof(wg + 9, NULL) : 1.0f;
    if (!(g_cc.wgamma >= 0.3f && g_cc.wgamma <= 3.0f)) g_cc.wgamma = 1.0f;
    g_cc.on = !off;
    if (off) return;
    logf_("compose: WoW colour %s (gamma %.2f)", g_cc.wowraw ? "raw bytes (wowraw)" : "decoded once (code 2)", g_cc.wgamma);
    logf_("compose on: ui %d light %d fx %d bg3tav %d fxcopy %d (whole %d) void %d raw %d cpu %d fxsync %d wowbar %d fxsight %d ground %d surf %d (dbg %d pix %d"
          " decal %.2f forward %.2f floor %.4f tol %.2f m light %d) far %.0f m; pacing %s lag %d wait %.1f ms; screen backdrop %d",
          g_cc.ui, g_cc.light, g_cc.fx, g_cc.bg3tav, g_cc.fxcopy, g_cc.fxwhole, g_cc.void_, g_cc.raw, g_cc.cpu, g_cc.fxsync, g_cc.wowbar, g_cc.fxsight, g_cc.ground,
          g_cc.surf, g_cc.surfdbg, g_cc.surfpix, g_cc.sdec, g_cc.sfwd, g_cc.sfloor, g_cc.stol, g_cc.slight, g_cc.far,
          g_cc.newest ? "off (newest)" : "on", g_cc.lag, g_cc.wait_us / 1000.0, g_cc.screenlevel ? 2 : g_cc.screenvoid);
}

// ---------------------------------------------------------------- effects pass
// BG3 draws its transparent effects (spells, particles, glows) into its HDR target in one render
// pass after lighting, with the scene depth loaded. Copies of that target just before and just
// after the pass (injected into BG3's own command buffer, outside the pass) tell, per pixel, how
// much of BG3's picture the effects made; the compositor adds that share over WoW. The pass is
// the HDR-plus-scene-depth pass with LOAD and the most draws in the last frame, by framebuffer.
// BG3 keeps a ring of framebuffers (one per frame in flight), so the pass is any of them that
// drew at least FX_MIN_DRAWS last time (the other HDR-plus-depth passes draw one to five).
#define FX_MIN_DRAWS 10
typedef struct { uint64_t fb; uint32_t draws, last; VkFormat fmt; } FxCand;
static struct {
    VkImage img[2]; VkDeviceMemory mem[2];
    VkFormat fmt; uint32_t w, h; int ready, failed;
    uint64_t fb;                      // set once any pass qualifies
    FxCand cand[32]; uint32_t ncand;
    uint64_t copies;
} g_fx;
static __thread VkCommandBuffer t_fx_cb;
static __thread uint64_t t_fx_fb, t_fx_img;
static __thread uint32_t t_fx_draws;
static __thread int32_t t_fx_final;
static __thread int t_fx_copy;

static void barrier_aspect(DeviceData* d, VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                           VkImageLayout from, VkImageLayout to);
static int fx_format(VkFormat f) { return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT; }

// A pass begins (g_trace_lock held): note the candidate; for the chosen one copy "before".
static void fx_pass(DeviceData* d, VkCommandBuffer cb, uint64_t fb, uint32_t n, const uint64_t* imgs,
                    const uint8_t* load, const int32_t* ini, const int32_t* fin) {
    t_fx_cb = VK_NULL_HANDLE;
    if (!g_cc.fx || !g_role.depth || !d) return;
    int depth = 0, colors = 0, at = -1;
    for (uint32_t k = 0; k < n; k++) {
        if (imgs[k] == g_role.depth) { depth = 1; continue; }
        ImgInfo* i = imgs[k] ? TABLE_FIND(g_img, IMG_CAP, imgs[k], 0) : NULL;
        if (!i) return;
        colors++;
        if (fx_format(i->fmt) && i->w == SCENE_W && i->hgt == SCENE_H) at = (int)k;
    }
    if (!depth || colors != 1 || at < 0 || load[at] != VK_ATTACHMENT_LOAD_OP_LOAD) return;
    t_fx_cb = cb; t_fx_fb = fb; t_fx_img = imgs[at]; t_fx_draws = 0; t_fx_final = fin[at]; t_fx_copy = 0;
    uint32_t last = 0;
    for (uint32_t k = 0; k < g_fx.ncand; k++) if (g_fx.cand[k].fb == fb) { last = g_fx.cand[k].last; break; }
    if (last < FX_MIN_DRAWS || !g_fx.ready || ini[at] < 0 || fin[at] < 0) return;
    ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, imgs[at], 0);
    if (!i || i->fmt != g_fx.fmt) return;
    VkImageCopy r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                     {g_fx.w, g_fx.h, 1}};
    barrier_aspect(d, cb, (VkImage)imgs[at], VK_IMAGE_ASPECT_COLOR_BIT, (VkImageLayout)ini[at], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    d->CmdCopyImage(cb, (VkImage)imgs[at], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fx.img[0], VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    barrier_aspect(d, cb, (VkImage)imgs[at], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkImageLayout)ini[at]);
    t_fx_copy = 1;
}
// The pass ended (after the driver's end): count its draws; for the chosen one copy "after".
static void fx_end(DeviceData* d, VkCommandBuffer cb) {
    if (t_fx_cb != cb) return;
    t_fx_cb = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_trace_lock);
    uint32_t k = 0;
    while (k < g_fx.ncand && g_fx.cand[k].fb != t_fx_fb) k++;
    if (k == g_fx.ncand && k < 32) {
        ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, t_fx_img, 0);
        g_fx.cand[k] = (FxCand){t_fx_fb, 0, 0, i ? i->fmt : VK_FORMAT_UNDEFINED};
        g_fx.ncand++;
    }
    if (k < 32) g_fx.cand[k].draws += t_fx_draws;
    pthread_mutex_unlock(&g_trace_lock);
    if (!t_fx_copy || !d) return;
    t_fx_copy = 0;
    VkImageCopy r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                     {g_fx.w, g_fx.h, 1}};
    barrier_aspect(d, cb, (VkImage)t_fx_img, VK_IMAGE_ASPECT_COLOR_BIT, (VkImageLayout)t_fx_final, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    d->CmdCopyImage(cb, (VkImage)t_fx_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fx.img[1], VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    barrier_aspect(d, cb, (VkImage)t_fx_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkImageLayout)t_fx_final);
    g_fx.copies++;
}
// At present: the draws each pass made since the last present become its "last" count (a ring
// framebuffer not drawn this time keeps its count).
static void fx_present(void) {
    pthread_mutex_lock(&g_trace_lock);
    for (uint32_t k = 0; k < g_fx.ncand; k++) {
        FxCand* c = &g_fx.cand[k];
        if (c->draws) {
            c->last = c->draws;
            if (c->last >= FX_MIN_DRAWS && !g_fx.fb) {
                g_fx.fb = c->fb; g_fx.fmt = c->fmt;
                logf_("roles: effects pass format %d (%u draws)", (int)c->fmt, c->last);
            }
        }
        c->draws = 0;
    }
    pthread_mutex_unlock(&g_trace_lock);
}

typedef struct {
    DeviceData* dev; uint32_t family;
    VkCommandPool pool; VkCommandBuffer cmd[2]; VkFence fence; VkSemaphore done[3]; uint32_t next_done;
    VkBuffer buf[5]; VkDeviceMemory mem[5]; void* map[5]; VkDeviceSize size[5];
} ComposeRes;
static ComposeRes g_cr;

static int cr_buffer(DeviceData* d, int k, VkDeviceSize size) {
    if (g_cr.size[k] >= size) return 1;
    if (g_cr.buf[k]) { d->DestroyBuffer(d->device, g_cr.buf[k], NULL); g_cr.buf[k] = VK_NULL_HANDLE; }
    if (g_cr.mem[k]) { d->FreeMemory(d->device, g_cr.mem[k], NULL); g_cr.mem[k] = VK_NULL_HANDLE; }
    g_cr.size[k] = 0;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (d->CreateBuffer(d->device, &bci, NULL, &g_cr.buf[k]) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    d->GetBufferMemoryRequirements(d->device, g_cr.buf[k], &req);
    int32_t type = memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0) type = memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) return 0;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
    if (d->AllocateMemory(d->device, &mai, NULL, &g_cr.mem[k]) != VK_SUCCESS) return 0;
    if (d->BindBufferMemory(d->device, g_cr.buf[k], g_cr.mem[k], 0) != VK_SUCCESS) return 0;
    if (d->MapMemory(d->device, g_cr.mem[k], 0, VK_WHOLE_SIZE, 0, &g_cr.map[k]) != VK_SUCCESS) return 0;
    g_cr.size[k] = size;
    return 1;
}
static int cr_init(DeviceData* d, uint32_t family) {
    if (g_cr.dev == d && g_cr.family == family) return 1;
    if (g_cr.dev) return 0;  // one device/queue family only
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    if (d->CreateCommandPool(d->device, &pci, NULL, &g_cr.pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_cr.pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    if (d->AllocateCommandBuffers(d->device, &ai, g_cr.cmd) != VK_SUCCESS) return 0;
    for (int k = 0; k < 2; k++) if (d->setLoaderData && d->setLoaderData(d->device, g_cr.cmd[k]) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    if (d->CreateFence(d->device, &fci, NULL, &g_cr.fence) != VK_SUCCESS) return 0;
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    for (int k = 0; k < 3; k++) if (d->CreateSemaphore(d->device, &sci, NULL, &g_cr.done[k]) != VK_SUCCESS) return 0;
    g_cr.dev = d;
    g_cr.family = family;
    return 1;
}

static void barrier_aspect(DeviceData* d, VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                           VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        img, {aspect, 0, 1, 0, 1}};
    d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

// The aspects a layout change of a depth image takes: both of a depth-stencil image (without
// separateDepthStencilLayouts a barrier must name them together). BG3's depth is D32S8 on AMD, D24S8 on Nvidia: a
// barrier on the depth aspect alone of the latter lost the device there.
static VkImageAspectFlags depth_layout_aspects(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT
               ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
               : VK_IMAGE_ASPECT_DEPTH_BIT;
}
static int depth_bytes(VkFormat f) {
    return (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT) ? 2 : 4;
}
static float depth_value(VkFormat f, const uint8_t* p) {
    if (f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT) { float v; memcpy(&v, p, 4); return v; }
    if (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D16_UNORM_S8_UINT) { uint16_t v; memcpy(&v, p, 2); return v / 65535.0f; }
    uint32_t v; memcpy(&v, p, 4); return (float)(v & 0xffffffu) / 16777215.0f;  // D24 in the low bits
}

// A whole record from a seq-framed file: header at 0, seq at `seqAt`, seq2 at `seq2At`.
static int read_framed(const char* path, void* out, size_t bytes, int seqAt, int seq2At) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    int ok = 0;
    for (int tries = 0; tries < 3 && !ok; tries++) {
        if (fseek(f, 0, SEEK_SET) || fread(out, 1, bytes, f) != bytes) break;
        uint32_t a, b, c;
        memcpy(&a, (uint8_t*)out + seqAt, 4);
        memcpy(&b, (uint8_t*)out + seq2At, 4);
        if (a == 0 || a != b) continue;
        if (fseek(f, 0, SEEK_SET) || fread(&c, 1, 4, f) != 4) break;
        uint32_t hdr[2];
        if (fseek(f, 0, SEEK_SET) || fread(hdr, 1, 8, f) != 8) break;
        ok = hdr[1] == a;  // unchanged while we read
    }
    fclose(f);
    return ok;
}

static uint8_t* g_wow;      // WoW frame file contents
static size_t g_wow_cap;

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, bits;
    if (e == 0) {
        if (!m) bits = sign;
        else { e = 127 - 15 + 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; bits = sign | (e << 23) | (m << 13); }
    } else if (e == 31) bits = sign | 0x7f800000u | (m << 13);
    else bits = sign | ((e + 127 - 15) << 23) | (m << 13);
    float f; memcpy(&f, &bits, 4); return f;
}
static uint8_t g_srgb[4097];  // linear [0,1] in 4096 steps -> sRGB-encoded byte
static void srgb_lut(void) {
    if (g_srgb[4096]) return;
    for (int i = 0; i <= 4096; i++) {
        float l = i / 4096.0f;
        float v = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
        g_srgb[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
}
// benilla's player-UI target is Rgba8UnormSrgb (WBF1 code 2) and
// its world and UI passes write display-gamma values into it, so each byte is those values
// sRGB-encoded a second time. benilla's own window undoes it (ui_gamma.wgsl: sample = decoded once,
// then its `gamma` CVar ramp, then out to an sRGB swapchain): the byte it shows is
// 255 * pow(srgb_to_linear(byte / 255), gamma). Same here, through a table.
static uint8_t g_dec2[256];
static float g_dec2_gamma = -1.0f;
static void dec2_lut(float gamma) {
    if (gamma == g_dec2_gamma) return;
    for (int i = 0; i < 256; i++) {
        float c = i / 255.0f;
        float l = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
        if (gamma != 1.0f) l = powf(l > 1e-6f ? l : 1e-6f, gamma);
        float v = l * 255.0f + 0.5f;
        g_dec2[i] = (uint8_t)(v > 255.0f ? 255.0f : v);
    }
    g_dec2_gamma = gamma;
}
static void put_pixel_from_wow(uint8_t* bgra, const uint8_t* wow, uint32_t code) {
    // Swapchain pixels are B8G8R8A8 (sRGB-encoded values); WoW's codes: 1/2 RGBA8, 3/4 BGRA8,
    // 5 RGBA16F (benilla's tonemapped linear output, encoded here).
    if (code == 3 || code == 4) { bgra[0] = wow[0]; bgra[1] = wow[1]; bgra[2] = wow[2]; }
    else if (code == 2 && !g_cc.wowraw) { bgra[0] = g_dec2[wow[2]]; bgra[1] = g_dec2[wow[1]]; bgra[2] = g_dec2[wow[0]]; }
    else if (code == 5) {
        for (int k = 0; k < 3; k++) {
            uint16_t h; memcpy(&h, wow + 2 * (2 - k), 2);
            float f = half_to_float(h);
            int idx = f > 0 ? (f >= 1 ? 4096 : (int)(f * 4096.0f)) : 0;
            bgra[k] = g_srgb[idx];
        }
    }
    else { bgra[0] = wow[2]; bgra[1] = wow[1]; bgra[2] = wow[0]; }
}

// The two effects copies, device-local, kept in GENERAL (made at the present that chose the pass;
// BG3's command buffers start copying into them from the next frame).
static int fx_create(DeviceData* d) {
    if (!d->GetImageMemoryRequirements || !d->BindImageMemory) return 0;
    g_fx.w = SCENE_W; g_fx.h = SCENE_H;
    for (int k = 0; k < 2; k++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, g_fx.fmt,
            {g_fx.w, g_fx.h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
            VK_IMAGE_LAYOUT_UNDEFINED};
        if (d->CreateImage(d->device, &ci, NULL, &g_fx.img[k]) != VK_SUCCESS) return 0;
        VkMemoryRequirements req;
        d->GetImageMemoryRequirements(d->device, g_fx.img[k], &req);
        int32_t type = memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) return 0;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
        if (d->AllocateMemory(d->device, &mai, NULL, &g_fx.mem[k]) != VK_SUCCESS) return 0;
        if (d->BindImageMemory(d->device, g_fx.img[k], g_fx.mem[k], 0) != VK_SUCCESS) return 0;
    }
    return 1;
}
static float uf11(uint32_t v) {
    uint32_t e = (v >> 6) & 31, m = v & 63;
    return e == 0 ? ldexpf((float)m / 64.0f, -14) : ldexpf(1.0f + (float)m / 64.0f, (int)e - 15);
}
static float uf10(uint32_t v) {
    uint32_t e = (v >> 5) & 31, m = v & 31;
    return e == 0 ? ldexpf((float)m / 32.0f, -14) : ldexpf(1.0f + (float)m / 32.0f, (int)e - 15);
}
// Sum of the three channels of one HDR texel.
static float hdr_sum(VkFormat f, const uint8_t* p) {
    if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
        uint32_t v; memcpy(&v, p, 4);
        return uf11(v & 0x7ff) + uf11((v >> 11) & 0x7ff) + uf10(v >> 22);
    }
    uint16_t h[3]; memcpy(h, p, 6);
    return half_to_float(h[0]) + half_to_float(h[1]) + half_to_float(h[2]);
}

// Skips are counted by reason (1 roles, 2 camera, 3 WoW header, 4 WoW size, 5 WoW frame, 6-7 resources).
#define SKIP(r) { g_cc.skipped++; g_cc.why[(r) & 7]++; \
    if (g_cc.skipped % 300 == 1) logf_("compose: skipping (reason %d; %llu skipped, %llu frames; depth %llx l%d ui %llx l%d ext %ux%u)", (r), \
        (unsigned long long)g_cc.skipped, (unsigned long long)g_cc.frames, (unsigned long long)g_role.depth, g_role.dlayout, \
        (unsigned long long)g_role.ui, g_role.ulayout, g_role.ew, g_role.eh); \
    return VK_NULL_HANDLE; }

#include "wowbg3_compose_gpu.inc.c"

// Returns a semaphore the present must wait on, or null (frame untouched).
static VkSemaphore compose_frame(QueueData* q, SwapchainData* c, uint32_t index, const VkPresentInfoKHR* info) {
    {  // the GPU compositor, unless it does not apply (wowbg3_compose_gpu.inc.c)
        VkSemaphore s = VK_NULL_HANDLE;
        if (compose_gpu(q, c, index, info, &s) >= 0) return s;
    }
    // A screen's dark backdrop is the GPU path's work: the CPU path has no rule for it and WoW's
    // frame does not belong on such a screen, so BG3's own picture is presented as it is.
    // The same for a trip to camp and the wait after it: the rule is the GPU path's.
    if (g_screen || g_cc.camp || g_cc.camp_hold) return VK_NULL_HANDLE;
    DeviceData* d = q->dev;
    if (c->format != VK_FORMAT_B8G8R8A8_UNORM && c->format != VK_FORMAT_B8G8R8A8_SRGB) return VK_NULL_HANDLE;
    VkImage depth = (VkImage)g_role.depth;
    VkImage ui = g_cc.ui && g_role.ulayout >= 0 ? (VkImage)g_role.ui : VK_NULL_HANDLE;
    VkImageLayout dlayout = (VkImageLayout)g_role.dlayout, ulayout = (VkImageLayout)g_role.ulayout;
    VkFormat dfmt = g_role.dfmt, ufmt = g_role.ufmt;
    if (!depth || g_role.dlayout < 0 || g_role.ew != c->extent.width || g_role.eh != c->extent.height) SKIP(1)
    // Inputs from the other processes first: skip the frame if either is missing or torn.
    uint8_t cam[96];
    if (!read_framed(bg3cam_path(), cam, sizeof cam, 4, 64)) SKIP(2)
    uint8_t hdr[64], hb[64];
    int ok0 = read_framed(wow_frame(0), hdr, sizeof hdr, 4, 56), ok1 = read_framed(wow_frame(1), hb, sizeof hb, 4, 56);
    int slot = 0;
    if (ok1) {
        uint32_t s0 = 0, s1; memcpy(&s1, hb + 4, 4);
        if (ok0) memcpy(&s0, hdr + 4, 4);
        if (!ok0 || (int32_t)(s1 - s0) > 0) { memcpy(hdr, hb, 64); slot = 1; }
    }
    if (!ok0 && !ok1) SKIP(3)
    { uint32_t gl; memcpy(&gl, hdr + 60, 4); wowlight_note(gl); }
    uint32_t ww, wh, code, cbpr, dbpr; float wnear, wzoff;
    memcpy(&ww, hdr + 8, 4); memcpy(&wh, hdr + 12, 4); memcpy(&code, hdr + 16, 4);
    memcpy(&cbpr, hdr + 20, 4); memcpy(&dbpr, hdr + 24, 4); memcpy(&wnear, hdr + 28, 4); memcpy(&wzoff, hdr + 32, 4);
    if (!(wzoff > -100.0f && wzoff < 100.0f)) wzoff = 0;
    if (ww != c->extent.width || wh != c->extent.height || code == 0 || code > 5) SKIP(4)
    const uint32_t wbpt = code == 5 ? 8 : 4;
    srgb_lut();
    dec2_lut(g_cc.wgamma);
    size_t wbytes = 64 + (size_t)cbpr * wh + (size_t)dbpr * wh;
    if (g_wow_cap < wbytes) { free(g_wow); g_wow = malloc(wbytes); g_wow_cap = g_wow ? wbytes : 0; }
    if (!g_wow || !read_framed(wow_frame(slot), g_wow, wbytes, 4, 56)) SKIP(5)
    float P22, P32, P23, P33;
    memcpy(&P22, cam + 80, 4); memcpy(&P32, cam + 84, 4); memcpy(&P23, cam + 88, 4); memcpy(&P33, cam + 92, 4);

    if (!cr_init(d, q->family)) SKIP(6)
    const uint32_t W = c->extent.width, H = c->extent.height;
    const int db = depth_bytes(dfmt);
    if (!cr_buffer(d, 0, (VkDeviceSize)W * H * 4) || !cr_buffer(d, 1, (VkDeviceSize)W * H * db) ||
        (ui && !cr_buffer(d, 2, (VkDeviceSize)W * H * 4))) SKIP(7)
    // Effects: make the copies' images once a pass is chosen; read them back once BG3 copies.
    int fx_new = 0;
    if (g_cc.fx && g_fx.fb && !g_fx.ready && !g_fx.failed && fx_format(g_fx.fmt)) {
        if (fx_create(d)) fx_new = 1;
        else { g_fx.failed = 1; logf_("fx: cannot make the copy images; effects off"); }
    }
    const int fxbpt = g_fx.fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    int fx = g_cc.fx && g_fx.ready && g_fx.copies > 0 && g_fx.w == W && g_fx.h == H &&
             cr_buffer(d, 3, (VkDeviceSize)W * H * fxbpt) && cr_buffer(d, 4, (VkDeviceSize)W * H * fxbpt);

    // Pass 1: read back BG3's final picture, scene depth and UI.
    VkCommandBuffer cmd = g_cr.cmd[0];
    d->ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    d->BeginCommandBuffer(cmd, &bi);
    VkImage img = c->images[index];
    barrier_aspect(d, cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rc = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    d->CmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_cr.buf[0], 1, &rc);
    barrier_aspect(d, cmd, depth, depth_layout_aspects(dfmt), dlayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rd = {0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    d->CmdCopyImageToBuffer(cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_cr.buf[1], 1, &rd);
    barrier_aspect(d, cmd, depth, depth_layout_aspects(dfmt), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dlayout);
    if (ui) {
        barrier_aspect(d, cmd, ui, VK_IMAGE_ASPECT_COLOR_BIT, ulayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        d->CmdCopyImageToBuffer(cmd, ui, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_cr.buf[2], 1, &rc);
        barrier_aspect(d, cmd, ui, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ulayout);
    }
    if (fx_new) {
        for (int k = 0; k < 2; k++)
            barrier_aspect(d, cmd, g_fx.img[k], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (fx) {
        for (int k = 0; k < 2; k++) {
            barrier_aspect(d, cmd, g_fx.img[k], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            d->CmdCopyImageToBuffer(cmd, g_fx.img[k], VK_IMAGE_LAYOUT_GENERAL, g_cr.buf[3 + k], 1, &rc);
        }
    }
    barrier_aspect(d, cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    d->EndCommandBuffer(cmd);
    VkPipelineStageFlags stages[16];
    uint32_t waits = info->waitSemaphoreCount > 16 ? 16 : info->waitSemaphoreCount;
    for (uint32_t i = 0; i < waits; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, waits, info->pWaitSemaphores, stages, 1, &cmd, 0, NULL};
    d->ResetFences(d->device, 1, &g_cr.fence);
    if (d->QueueSubmit(q->queue, 1, &si, g_cr.fence) != VK_SUCCESS) { logf_("compose: submit 1 failed"); return VK_NULL_HANDLE; }
    d->WaitForFences(d->device, 1, &g_cr.fence, VK_TRUE, 2000000000ull);
    if (fx_new) { g_fx.ready = 1; logf_("fx: copy images ready (%ux%u format %d)", W, H, (int)g_fx.fmt); }
    if (fx && g_fx_dump_request) {
        g_fx_dump_request = 0;
        const char* names[2] = {outp("/fxa.raw"), outp("/fxb.raw")};
        for (int k = 0; k < 2; k++) {
            FILE* f = fopen(names[k], "wb");
            if (f) { fwrite(g_cr.map[3 + k], 1, (size_t)W * H * fxbpt, f); fclose(f); }
        }
        FILE* f = fopen(outp("/fxo.raw"), "wb");
        if (f) { fwrite(g_cr.map[0], 1, (size_t)W * H * 4, f); fclose(f); }
        logf_("fx: dumped before/after/final");
    }

    // CPU merge. Where BG3's surface (Tav, props) is nearer than WoW's, BG3's final picture stays.
    // Elsewhere: UI over WoW, out = ui.rgb + wow * (1 - ui.a) (the UI target is premultiplied);
    // with "light", BG3's picture minus its UI is added over WoW (effects over a dark void).
    uint8_t* out = g_cr.map[0];
    const uint8_t* dep = g_cr.map[1];
    const uint8_t* up = ui ? g_cr.map[2] : NULL;
    if (up && !g_role.fin_seen) {
        // Only while the UI is still guessed by draw count (no final pass seen yet): BG3 also draws
        // HUD-sized panel caches that are not premultiplied (colour above alpha in most of it); the
        // HUD always is. Such a guess is set aside. The final pass's own UI target is never judged:
        // a garbage frame of it set the real HUD aside for a whole session.
        uint32_t loud = 0, samples = 0;
        for (size_t i = 0; i < (size_t)W * H; i += 97, samples++) {
            const uint8_t* u = up + i * 4;
            int m = u[0] > u[1] ? u[0] : u[1]; if (u[2] > m) m = u[2];
            loud += m > u[3] + 8;
        }
        if (loud * 5 > samples) {
            if (g_role.nbad < 8) g_role.bad[g_role.nbad++] = (uint64_t)ui;
            logf_("roles: HUD-sized target %llx is not premultiplied (%u%%); not the HUD", (unsigned long long)(uint64_t)ui,
                  loud * 100 / samples);
            if ((uint64_t)ui == g_role.ui) { g_role.ui = 0; g_role.ulayout = -1; }
            up = NULL;
        }
    }
    const uint8_t* fa = fx ? g_cr.map[3] : NULL;
    const uint8_t* fb = fx ? g_cr.map[4] : NULL;
    uint64_t fx_px = 0;
    const int urgba = ufmt == VK_FORMAT_R8G8B8A8_UNORM;
    const uint8_t* wc = g_wow + 64;
    const uint8_t* wd = g_wow + 64 + (size_t)cbpr * wh;
    const float yd = 0.9144f;
    uint64_t wow_px = 0;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            size_t i = (size_t)y * W + x;
            uint8_t* o = out + i * 4;
            float dv = depth_value(dfmt, dep + i * db);
            float denom = dv * P23 - P22;
            float bz = fabsf(denom) > 1e-12f ? (P32 - dv * P33) / denom : 1e30f;
            if (!(bz > 0)) bz = 1e30f;
            float wdv; memcpy(&wdv, wd + (size_t)y * dbpr + x * 4, 4);
            // Reverse infinite: view z = near / depth; plus how far WoW's eye sits ahead of BG3's.
            float wz = wdv > 0 ? (wnear / wdv + wzoff) * yd : 1e30f;
            if (bz < wz && bz < g_cc.far) continue;
            wow_px++;
            uint8_t base[3];
            put_pixel_from_wow(base, wc + (size_t)y * cbpr + (size_t)x * wbpt, code);
            int ub = 0, ug = 0, ur = 0, ua = 0;  // UI, premultiplied, in B G R order
            if (up) {
                const uint8_t* u = up + i * 4;
                if (urgba) { ub = u[2]; ug = u[1]; ur = u[0]; } else { ub = u[0]; ug = u[1]; ur = u[2]; }
                ua = u[3];
            }
            int uc[3] = {ub, ug, ur};
            // Effects' share of BG3's picture here: (after - before) / after, on the HDR sums.
            float share = 0;
            if (fa && ua < 250) {
                float sa = hdr_sum(g_fx.fmt, fa + i * fxbpt), sb = hdr_sum(g_fx.fmt, fb + i * fxbpt);
                if (sb > sa * 1.03f + 1e-3f) { share = (sb - sa) / sb; if (share > 1) share = 1; fx_px++; }
            }
            for (int k = 0; k < 3; k++) {
                int b = base[k];
                if (share > 0) {
                    int sc = (o[k] - uc[k]) * 255 / (255 - ua);
                    if (sc > 0) b += (int)(sc * share);
                    if (b > 255) b = 255;
                }
                if (g_cc.light) {
                    // BG3's scene under the UI: (final - ui) / (1 - a).
                    int sc = ua < 250 ? (o[k] - uc[k]) * 255 / (255 - ua) : 0;
                    if (sc > 0) b += sc;
                    if (b > 255) b = 255;
                }
                int v = uc[k] + b * (255 - ua) / 255;
                o[k] = (uint8_t)(v > 255 ? 255 : v);
            }
            o[3] = 255;
        }
    }

    // Pass 2: write the merged picture back and present it.
    cmd = g_cr.cmd[1];
    d->ResetCommandBuffer(cmd, 0);
    d->BeginCommandBuffer(cmd, &bi);
    d->CmdCopyBufferToImage(cmd, g_cr.buf[0], img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    barrier_aspect(d, cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    d->EndCommandBuffer(cmd);
    VkSemaphore done = g_cr.done[g_cr.next_done++ % 3];
    VkSubmitInfo s2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 1, &done};
    if (d->QueueSubmit(q->queue, 1, &s2, VK_NULL_HANDLE) != VK_SUCCESS) { logf_("compose: submit 2 failed"); return VK_NULL_HANDLE; }
    if (++g_cc.frames % 300 == 1)
        logf_("compose: %llu frames, %llu skipped, ui %s (%s, %llu changes), wow %.1f%% of pixels, effects %.2f%%, layouts d%d u%d",
              (unsigned long long)g_cc.frames, (unsigned long long)g_cc.skipped, up ? "yes" : "no",
              g_role.fin_seen ? "final pass" : "guessed", (unsigned long long)g_role.switches,
              100.0 * wow_px / ((double)W * H), 100.0 * fx_px / ((double)W * H), (int)dlayout, (int)ulayout);
    return done;
}

static void dump_image(QueueData* q, const VkPresentInfoKHR* info) {
    DeviceData* d = q->dev;
    g_dump.armed = 0;
    ImgInfo* found = NULL;
    for (uint32_t k = 0; k < IMG_CAP && g_img; k++) if (g_img[k].h && g_img[k].id == g_dump.id) { found = &g_img[k]; break; }
    if (!found) { logf_("dump: image %u not known", g_dump.id); return; }
    int depth = is_depth_format(found->fmt);
    VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageAspectFlags lay_aspect = depth ? depth_layout_aspects(found->fmt) : aspect;  // the copy reads depth alone
    VkDeviceSize size = (VkDeviceSize)found->w * found->hgt * 8;
    Staging* st = staging_for(d, q->family, size);
    if (!st) return;
    d->ResetCommandBuffer(st->cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    d->BeginCommandBuffer(st->cmd, &bi);
    VkImage img = (VkImage)found->h;
    barrier_aspect(d, st->cmd, img, lay_aspect, (VkImageLayout)g_dump.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = {0, 0, 0, {aspect, 0, 0, 1}, {0, 0, 0}, {found->w, found->hgt, 1}};
    d->CmdCopyImageToBuffer(st->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st->buffer, 1, &r);
    barrier_aspect(d, st->cmd, img, lay_aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkImageLayout)g_dump.layout);
    d->EndCommandBuffer(st->cmd);
    (void)info;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &st->cmd, 0, NULL};
    d->ResetFences(d->device, 1, &st->fence);
    if (d->QueueSubmit(q->queue, 1, &si, st->fence) != VK_SUCCESS) { logf_("dump: submit failed"); return; }
    d->WaitForFences(d->device, 1, &st->fence, VK_TRUE, 2000000000ull);
    char path[160];
    snprintf(path, sizeof path, "%s/%s.raw", out_dir(), g_dump.tag);
    FILE* f = fopen(path, "wb");
    size_t bytes = (size_t)found->w * found->hgt * (depth ? 4 : (found->fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4));
    if (f) { fwrite(st->mapped, 1, bytes, f); fclose(f); }
    logf_("dump: image %u f%d %ux%u -> %s", found->id, (int)found->fmt, found->w, found->hgt, path);
}

// ---------------------------------------------------------------- trace intercepts
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
                                                         const VkAllocationCallbacks* alloc, VkBuffer* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    // Every buffer may be copied from (the readback needs TRANSFER_SRC; copying a buffer without
    // it crashed BG3's driver).
    VkBufferCreateInfo ci = *info;
    ci.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkResult r = d->CreateBuffer(device, &ci, alloc, out);
    if (r == VK_SUCCESS && trace_tables()) {
        pthread_mutex_lock(&g_trace_lock);
        BufInfo* b = TABLE_FIND(g_buf, BUF_CAP, (uint64_t)*out, 1);
        static uint64_t gens;
        if (b) { b->size = info->size; b->usage = info->usage; b->mem = 0; b->offset = 0; b->gen = ++gens; }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory mem,
                                                             VkDeviceSize offset) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->BindBufferMemory(device, buffer, mem, offset);
    if (r == VK_SUCCESS && g_buf) {
        pthread_mutex_lock(&g_trace_lock);
        BufInfo* b = TABLE_FIND(g_buf, BUF_CAP, (uint64_t)buffer, 0);
        if (b) { b->mem = (uint64_t)mem; b->offset = offset; }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_BindBufferMemory2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    DeviceData* d = device_for(device);
    if (!d || !d->BindBufferMemory2) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->BindBufferMemory2(device, n, infos);
    if (r == VK_SUCCESS && g_buf) {
        pthread_mutex_lock(&g_trace_lock);
        for (uint32_t k = 0; k < n; k++) {
            BufInfo* b = TABLE_FIND(g_buf, BUF_CAP, (uint64_t)infos[k].buffer, 0);
            if (b) { b->mem = (uint64_t)infos[k].memory; b->offset = infos[k].memoryOffset; }
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_BindBufferMemory2KHR(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    DeviceData* d = device_for(device);
    if (d && !d->BindBufferMemory2) d->BindBufferMemory2 = (PFN_vkBindBufferMemory2)d->gdpa(d->device, "vkBindBufferMemory2KHR");
    return layer_BindBufferMemory2(device, n, infos);
}
static VKAPI_ATTR void VKAPI_CALL layer_DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* alloc) {
    DeviceData* d = device_for(device);
    if (!d) return;
    if (g_buf && buffer) {
        pthread_mutex_lock(&g_trace_lock);
        BufInfo* b = TABLE_FIND(g_buf, BUF_CAP, (uint64_t)buffer, 0);
        if (b) b->size = 0;  // kept in the open-addressed table, marked gone
        pthread_mutex_unlock(&g_trace_lock);
        fxj_retire_buffer((uint64_t)buffer);  // an effects readback still copying it finishes first
    }
    d->DestroyBufferNext(device, buffer, alloc);
}
// At present (g_lock held): the buffer table and armed readbacks.
static void buffer_research(QueueData* q) {
    DeviceData* d = q->dev;
    if (g_bufs_request && g_buf) {
        g_bufs_request = 0;
        FILE* f = fopen(outp("/bufs.txt"), "w");
        if (f) {
            pthread_mutex_lock(&g_trace_lock);
            for (uint32_t k = 0; k < BUF_CAP; k++)
                if (g_buf[k].h && g_buf[k].size)
                    fprintf(f, "%llx %llx %llx %llx %llx\n", (unsigned long long)g_buf[k].h, (unsigned long long)g_buf[k].size,
                            (unsigned long long)g_buf[k].usage, (unsigned long long)g_buf[k].mem, (unsigned long long)g_buf[k].offset);
            pthread_mutex_unlock(&g_trace_lock);
            fclose(f);
            logf_("bufs: table written");
        }
    }
    if (g_rb.armed && d->CmdCopyBuffer) {
        g_rb.armed = 0;
        pthread_mutex_lock(&g_trace_lock);
        BufInfo* b = g_buf ? TABLE_FIND(g_buf, BUF_CAP, g_rb.h, 0) : NULL;
        int ok = b && b->size && g_rb.off + g_rb.size <= b->size;
        pthread_mutex_unlock(&g_trace_lock);
        if (!ok) { logf_("readback: %llx +%llx not a live buffer range", (unsigned long long)g_rb.h, (unsigned long long)g_rb.off); return; }
        Staging* st = staging_for(d, q->family, g_rb.size);
        if (!st) return;
        d->ResetCommandBuffer(st->cmd, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        d->BeginCommandBuffer(st->cmd, &bi);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        d->CmdPipelineBarrier(st->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkBufferCopy rg = {g_rb.off, 0, g_rb.size};
        d->CmdCopyBuffer(st->cmd, (VkBuffer)g_rb.h, st->buffer, 1, &rg);
        d->EndCommandBuffer(st->cmd);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &st->cmd, 0, NULL};
        d->ResetFences(d->device, 1, &st->fence);
        if (d->QueueSubmit(q->queue, 1, &si, st->fence) != VK_SUCCESS) { logf_("readback: submit failed"); return; }
        d->WaitForFences(d->device, 1, &st->fence, VK_TRUE, 2000000000ull);
        char path[160];
        snprintf(path, sizeof path, "%s/%s.raw", out_dir(), g_rb.tag);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(st->mapped, 1, g_rb.size, f); fclose(f); }
        logf_("readback: %s", path);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateImage(VkDevice device, const VkImageCreateInfo* info,
                                                        const VkAllocationCallbacks* alloc, VkImage* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    // Sampled textures can be copied (effects replay exports them once, wowbg3_fxcap.inc.c).
    VkImageCreateInfo copy = *info;
    if ((copy.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
        !(copy.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)))
        copy.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // BG3's small sampled RGBA8 colour targets can be cleared and blitted into: its examine and trade
    // previews get the WoW body (wowbg3_preview.inc.c; 525x690 and 700x690 at 1080p, so under 1600 at 4K).
    if (copy.format == VK_FORMAT_R8G8B8A8_UNORM && (copy.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        (copy.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && copy.samples == VK_SAMPLE_COUNT_1_BIT &&
        copy.extent.width < 1600 && copy.extent.height < 1600)
        copy.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // BG3's 10-bit colour targets (its fragment display transform writes one: DT_FS): wdpostcas copies the output and
    // puts WoW's colours back over it with a copy (wowbg3_fxbg.inc.c post_dtf).
    if (copy.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && (copy.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && copy.samples == VK_SAMPLE_COUNT_1_BIT)
        copy.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info = &copy;
    VkResult r = d->CreateImage(device, info, alloc, out);
    if (r == VK_SUCCESS && trace_tables()) {
        pthread_mutex_lock(&g_trace_lock);
        ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, (uint64_t)*out, 1);
        if (i) {
            i->id = ++g_img_ids; i->fmt = info->format; i->w = info->extent.width; i->hgt = info->extent.height;
            i->layers = info->arrayLayers; i->mips = info->mipLevels; i->usage = info->usage; i->samples = info->samples;
            i->depth = info->extent.depth; i->type = info->imageType;
            __atomic_store_n(&i->copy_src, 0, __ATOMIC_RELAXED);  // a reused handle is a new image
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return r;
}
static void fb_forget_image(DeviceData* d, VkImage img);  // wowbg3_fxbg.inc.c (patch35)
static VKAPI_ATTR void VKAPI_CALL layer_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* alloc) {
    DeviceData* d = device_for(device);
    if (!d) return;
    gc_retire_image(image);  // the compositor's copies of it finish first
    ground_forget_image(d, image);
    shade_forget_image(d, image);
    surf_forget_image(d, image);
    fb_forget_image(d, image);
    pthread_mutex_lock(&g_trace_lock);
    role_forget(image);
    pthread_mutex_unlock(&g_trace_lock);
    d->DestroyImage(device, image, alloc);
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                                            const VkAllocationCallbacks* alloc, VkImageView* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateImageView(device, info, alloc, out);
    if (r == VK_SUCCESS && trace_tables()) {
        pthread_mutex_lock(&g_trace_lock);
        ViewInfo* v = TABLE_FIND(g_view, VIEW_CAP, (uint64_t)*out, 1);
        if (v) { v->image = (uint64_t)info->image; v->layer = info->subresourceRange.baseArrayLayer; }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
                                                              const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateFramebuffer(device, info, alloc, out);
    if (r == VK_SUCCESS && trace_tables()) {
        pthread_mutex_lock(&g_trace_lock);
        FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)*out, 1);
        if (f) {
            int imageless = (info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
            f->n = imageless ? 0 : (info->attachmentCount > 8 ? 8 : info->attachmentCount);
            for (uint32_t k = 0; k < f->n; k++) f->views[k] = (uint64_t)info->pAttachments[k];
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return r;
}
static void remember_rp(VkRenderPass rp, uint32_t n, const VkFormat* fmts, const uint8_t* load, const uint8_t* store,
                        const int32_t* fin, const int32_t* ini) {
    if (!trace_tables()) return;
    pthread_mutex_lock(&g_trace_lock);
    RpInfo* p = TABLE_FIND(g_rp, RP_CAP, (uint64_t)rp, 1);
    if (p) {
        p->n = n > 8 ? 8 : n;
        for (uint32_t k = 0; k < p->n; k++) { p->fmts[k] = fmts[k]; p->load[k] = load[k]; p->store[k] = store[k]; p->final[k] = fin[k]; p->init[k] = ini[k]; }
    }
    pthread_mutex_unlock(&g_trace_lock);
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* info,
                                                             const VkAllocationCallbacks* alloc, VkRenderPass* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateRenderPass(device, info, alloc, out);
    if (r == VK_SUCCESS) {
        VkFormat f[8]; uint8_t l[8], s[8]; int32_t fl[8], il[8];
        uint32_t n = info->attachmentCount > 8 ? 8 : info->attachmentCount;
        for (uint32_t k = 0; k < n; k++) { f[k] = info->pAttachments[k].format; l[k] = (uint8_t)info->pAttachments[k].loadOp; s[k] = (uint8_t)info->pAttachments[k].storeOp; fl[k] = (int32_t)info->pAttachments[k].finalLayout; il[k] = (int32_t)info->pAttachments[k].initialLayout; }
        remember_rp(*out, n, f, l, s, fl, il);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* info,
                                                              const VkAllocationCallbacks* alloc, VkRenderPass* out) {
    DeviceData* d = device_for(device);
    if (!d || !d->CreateRenderPass2) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateRenderPass2(device, info, alloc, out);
    if (r == VK_SUCCESS) {
        VkFormat f[8]; uint8_t l[8], s[8]; int32_t fl[8], il[8];
        uint32_t n = info->attachmentCount > 8 ? 8 : info->attachmentCount;
        for (uint32_t k = 0; k < n; k++) { f[k] = info->pAttachments[k].format; l[k] = (uint8_t)info->pAttachments[k].loadOp; s[k] = (uint8_t)info->pAttachments[k].storeOp; fl[k] = (int32_t)info->pAttachments[k].finalLayout; il[k] = (int32_t)info->pAttachments[k].initialLayout; }
        remember_rp(*out, n, f, l, s, fl, il);
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* info,
                                                                 const VkAllocationCallbacks* alloc, VkRenderPass* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (!d->CreateRenderPass2) d->CreateRenderPass2 = (PFN_vkCreateRenderPass2)d->gdpa(device, "vkCreateRenderPass2KHR");
    return layer_CreateRenderPass2(device, info, alloc, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    DeviceData* d = device_for(cb);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (g_img) role_reset_cb(cb);
    if (g_img && g_cc.on) {
        pthread_mutex_lock(&g_trace_lock);
        TavdCb* e = TABLE_FIND(g_tavdcb, TAVDCB_CAP, (uint64_t)(uintptr_t)cb, 0);
        if (e) e->valid = 0;
        if (t_tavd_cb == cb) t_tavd_cb = VK_NULL_HANDLE;
        if (info && info->pInheritanceInfo && (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) &&
            info->pInheritanceInfo->framebuffer && g_role.ew) {
            FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)info->pInheritanceInfo->framebuffer, 0);
            uint32_t colors = 0, depth = 0;
            for (uint32_t k = 0; f && k < f->n && k < 8; k++) {
                uint64_t img = view_image(f->views[k]);
                ImgInfo* ii = img ? TABLE_FIND(g_img, IMG_CAP, img, 0) : NULL;
                if (!ii || ii->w != SCENE_W || ii->hgt != SCENE_H || ii->fmt == VK_FORMAT_S8_UINT) continue;
                if (is_depth_format(ii->fmt)) depth++;
                else colors++;
            }
            if (depth && colors >= 3) tavd_begin(cb);
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    if (g_fxw_frames > 0 && g_img) fxw_secondary(cb, info);
    CbLog* c = trace_cb(cb);
    if (c) {
        c->len = 0; if (c->buf) c->buf[0] = 0;
        c->in_pass = 0; c->draws = c->dispatches = 0; c->prims = 0;
        cb_printf(c, "  begin%s\n", info && info->pInheritanceInfo && (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) ? " (secondary in pass)" : "");
        pthread_mutex_unlock(&g_trace_lock);
    }
    return d->BeginCommandBuffer(cb, info);
}
// ---------------------------------------------------------------- effects research (VFX option B)
// Goal: BG3's effects drawn inside WoW like Tav, by copying their geometry. To
// learn their shape: request "fxlog<N>" logs every draw of BG3's effects pass (the HDR pass that
// loads the lit scene, as fx_pass finds it) for N presents into OUT_DIR/fxlog.txt: the pipeline's
// vertex layout, topology and blend, the bound vertex and index buffers and descriptor sets.
typedef struct {
    uint64_t h;
    uint32_t blend;  // enable | srcColor<<4 | dstColor<<9 | op<<14 | srcA<<17 | dstA<<22
    uint16_t stride[8];
    uint8_t rate[8];
    uint8_t aloc[12], abind[12];
    uint16_t afmt[12], aoff[12];
    uint8_t nbind, nattr, topo, zwrite, cull, stages;
} PipeInfo;
#define PIPE_CAP (1u << 18)  // BG3 makes tens of thousands of pipelines (libraries included)
static uint32_t g_pipe_count;
static PipeInfo* g_pipe;
static FILE* g_fxl;
static FILE* g_fxd;
static uint32_t draw_no;  // effects draws logged, for matching fxlog.txt to fxdata.bin
static pthread_mutex_t g_fxl_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread VkCommandBuffer t_fxl_cb;
static __thread VkCommandBuffer t_dec_cb;  // BG3's deferred decal pass on this thread (fxl_pass)
static __thread VkCommandBuffer t_void_cb;  // the pass on this thread is one BG3's map draws into (void_pass)
static void void_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs);
static int void_skip(VkCommandBuffer cb, uint32_t fi, uint32_t count, uint32_t first, int32_t vo);
static __thread VkCommandBuffer t_prop_cb;  // void_skip kept this draw as a spell's prop (fxwhy decision Y)
static __thread VkCommandBuffer t_bind_cb;
static __thread uint64_t t_pipe, t_vb[8], t_vbo[8], t_set[8], t_ibo;
static __thread uint32_t t_ibt, t_pc, t_dyn[8][8], t_ndyn[8];
static __thread VkCommandBuffer t_cbind_cb;  // compute descriptor sets (research: post-processing)
static __thread uint64_t t_cset[8];
static __thread uint64_t t_cpipe;  // bound compute pipeline (with t_cbind_cb)
static __thread uint64_t t_clayout;  // the layout BG3 last bound compute sets with (wdpostcas binds them again)
static void trace_draw_reads(CbLog* c, VkCommandBuffer cb);
static void trace_dispatch_reads(CbLog* c, VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z);

// Host-mapped memory (vkMapMemory): BG3 writes its per-frame effect vertices through these.
typedef struct { uint64_t h; uint8_t* ptr; uint64_t off, size; } MemInfo;
#define MEM_CAP (1u << 14)
static MemInfo* g_mem;
static VKAPI_ATTR VkResult VKAPI_CALL layer_MapMemory(VkDevice device, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size,
                                                      VkMemoryMapFlags fl, void** out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->MapMemory(device, mem, off, size, fl, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_fxl_lock);
        if (!g_mem) g_mem = calloc(MEM_CAP, sizeof *g_mem);
        MemInfo* m = g_mem ? TABLE_FIND(g_mem, MEM_CAP, (uint64_t)mem, 1) : NULL;
        if (m) { m->ptr = *out; m->off = off; m->size = size; }
        pthread_mutex_unlock(&g_fxl_lock);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL layer_UnmapMemory(VkDevice device, VkDeviceMemory mem) {
    DeviceData* d = device_for(device);
    if (!d) return;
    pthread_mutex_lock(&g_fxl_lock);
    MemInfo* m = g_mem ? TABLE_FIND(g_mem, MEM_CAP, (uint64_t)mem, 0) : NULL;
    if (m) m->ptr = NULL;
    pthread_mutex_unlock(&g_fxl_lock);
    d->UnmapMemory(device, mem);
}
// Buffers made before the render plugin: BG3 makes its constant rings at startup
// (its per-view camera block among them), so the plugin never saw them bound or their memory mapped and
// the ground injection found no camera ("unmapped"). The plugin asks bg3le, which records every buffer of the game
// from its first call (g_buf_recover, wowbg3_render.c; bg3le patch 0013); the answers (also "not mapped") are kept here, apart from the tables
// the hooks fill, under their own lock.
static const uint8_t* (*g_buf_recover)(uint64_t buffer, uint64_t* size);
typedef struct { uint64_t h; const uint8_t* ptr; uint64_t size; } RBufInfo;
#define RBUF_CAP (1u << 12)
static RBufInfo g_rbuf[RBUF_CAP];
static uint32_t g_nrbuf;
static pthread_mutex_t g_rbuf_lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t* buf_cpu_recovered(uint64_t buffer, uint64_t off, uint64_t* left) {
    if (!g_buf_recover || !buffer) return NULL;
    pthread_mutex_lock(&g_rbuf_lock);
    RBufInfo* r = TABLE_FIND(g_rbuf, RBUF_CAP, buffer, 0);
    if (!r && g_nrbuf < RBUF_CAP / 2) {
        uint64_t size = 0;
        const uint8_t* p = g_buf_recover(buffer, &size);
        r = TABLE_FIND(g_rbuf, RBUF_CAP, buffer, 1);
        if (r) { r->ptr = p; r->size = p ? size : 0; g_nrbuf++; }
    }
    const uint8_t* out = r && r->ptr && off < r->size ? r->ptr + off : NULL;
    if (out && left) *left = r->size - off;
    pthread_mutex_unlock(&g_rbuf_lock);
    return out;
}
// A CPU pointer to a buffer's bytes at OFF when its memory is mapped (else NULL), and the size left.
static const uint8_t* buf_cpu(uint64_t buffer, uint64_t off, uint64_t* left) {
    if (!g_buf || !buffer) return NULL;
    BufInfo* b = TABLE_FIND(g_buf, BUF_CAP, buffer, 0);
    if (!b) return buf_cpu_recovered(buffer, off, left);  // (the plugin may have seen no vkMapMemory at all: g_mem unset)
    if (!b->size || off >= b->size) return NULL;
    MemInfo* m = (g_mem && b->mem) ? TABLE_FIND(g_mem, MEM_CAP, b->mem, 0) : NULL;
    // memory mapped before the render plugin (an early plugin still starts after BG3 maps its first
    // memory): the driver's own mapping of the buffer, as for a buffer made before it (release batch)
    if (!m || !m->ptr) return buf_cpu_recovered(buffer, off, left);
    if (b->offset + off < m->off) return NULL;
    if (left) *left = b->size - off;
    return m->ptr + (b->offset + off - m->off);
}

// Descriptor sets: what each binding points at (buffers with offset and range, image views), from
// vkUpdateDescriptorSets and update templates. Effects keep their object transforms in uniforms.
typedef struct { uint8_t type; int32_t layout; uint64_t res, off, range, samp; } DescSlot;  // layout: an image's, from its descriptor
#define DESC_SLOTS 32  // indexed by binding number (BG3's scene set uses bindings up to 30)
// A set's entry: made at vkAllocateDescriptorSets, retired at vkFreeDescriptorSets or the reset or
// destruction of its pool, and let go once BG3 has not bound it for SET_KEEP presents.
// Retired, not dropped: a bind of a set BG3 has freed would find nothing if the entry went at the
// free (suspected, not seen, behind missing vision cones, the spider familiar's black
// effects and a hit's black ring in a 30-minute session; in fresh harness sessions
// and .68 alike BG3 bound no freed set and all three looked right). The driver keeps a freed set's
// descriptors until its memory is handed out again, so such a bind still draws right in BG3; the
// entry lasts as long. Each bind marks its set (set_bound): a retired set BG3 still binds stays, one
// it no longer binds goes SET_KEEP presents after its last bind or its free; the handle allocated
// again starts a new entry in its place. Kept forever, as it used to be, the dead
// handles filled the table within half an hour (BG3's HUD gone). The "sets:" lines count binds of
// retired sets (and how long after the free) and of sets never seen: the test of that suspicion.
// pool: the pool of a live set (0: allocated before the hooks, or retired); pidx: its place in
// that pool's list. Entries never move or get freed (slabs, a free list), so recording threads
// can read them without the lock; writers hold g_fxl_lock.
typedef struct SetInfo {
    uint64_t h, pool;
    uint32_t pidx, freed;            // freed: retired (BG3 freed it)
    uint64_t freed_at, bound_at;     // presents (g_req.presents) at the free and at the last bind
    uint64_t q_at;                   // its place in the retired queue (presents)
    struct SetInfo *qprev, *qnext;   // retired queue (oldest first); qnext also links the free list
    DescSlot slot[DESC_SLOTS];
} SetInfo;
#define SET_KEEP 300u      // presents a retired set stays after its free or its last bind
#define SET_SLAB 1024u     // entries per slab
// The index: handle -> entry, open addressing, a removed handle becomes a tombstone (SET_TOMB), so
// index slots never move under set_find. When handles and tombstones pass 3/4 of it, the handles
// are copied into a new index (twice the size while they would fill a quarter of it; 16 bytes a
// slot, 512 KB at the start) and the pointer swapped; an old index is freed 2 s later, after any
// lookup still reading it is long done (at most SET_OLD of them wait).
typedef struct { uint64_t h; SetInfo* s; } SetIdx;
typedef struct { uint32_t cap, used, tombs, pad; SetIdx e[]; } SetTab;  // used: slots not empty (handles + tombstones)
#define SET_CAP0 (1u << 15)
#define SET_TOMB (~0ull)
static SetTab* volatile g_settab;
#define SET_OLD 3
static SetTab* g_settab_old[SET_OLD];
static double g_settab_old_t[SET_OLD];
static SetInfo* g_set_free;                    // entries to reuse
static SetInfo *g_ret_head, *g_ret_tail;       // retired entries, oldest first
static uint32_t g_set_slabs, g_set_retired;
// The pools' sets (g_fxl_lock held): sets[pidx] is each live set of the pool.
typedef struct { uint64_t h; uint64_t* sets; uint32_t n, cap; } PoolInfo;
#define POOL_CAP (1u << 13)
static PoolInfo* g_pool;
static struct {
    uint64_t allocs, frees, resets, destroys, retired, reclaimed, rebuilds, unseen, failed, pools_full;
    uint64_t bound_freed, bound_freed_age, bound_unknown;  // binds of retired sets (max presents since the free), of unknown ones
} g_sets;
static uint64_t sets_now(void) { return __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED); }
static SetInfo* set_find(uint64_t key) {
    SetTab* t = __atomic_load_n(&g_settab, __ATOMIC_ACQUIRE);
    if (!t || !key || key == SET_TOMB) return NULL;
    uint32_t i = hslot(key, t->cap);
    for (uint32_t n = 0; n < t->cap; n++, i = (i + 1) & (t->cap - 1)) {
        uint64_t h = __atomic_load_n(&t->e[i].h, __ATOMIC_ACQUIRE);
        if (h == key) return __atomic_load_n(&t->e[i].s, __ATOMIC_ACQUIRE);
        if (!h) return NULL;
    }
    return NULL;
}
// A bind of `key` (recording threads, no lock): marks its entry as in use now.
static SetInfo* set_bound(uint64_t key) {
    SetInfo* s = set_find(key);
    if (!s) {
        if (key && !__atomic_fetch_add(&g_sets.bound_unknown, 1, __ATOMIC_RELAXED))
            logf_("sets: BG3 bound set %llx, never allocated, updated or copied through the hooks", (unsigned long long)key);
        return NULL;
    }
    uint64_t now = sets_now();
    __atomic_store_n(&s->bound_at, now, __ATOMIC_RELAXED);
    if (__atomic_load_n(&s->freed, __ATOMIC_RELAXED)) {
        uint64_t age = now - __atomic_load_n(&s->freed_at, __ATOMIC_RELAXED), max = __atomic_load_n(&g_sets.bound_freed_age, __ATOMIC_RELAXED);
        while (age > max && !__atomic_compare_exchange_n(&g_sets.bound_freed_age, &max, age, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        if (!__atomic_fetch_add(&g_sets.bound_freed, 1, __ATOMIC_RELAXED))
            logf_("sets: BG3 bound set %llx %llu presents after it freed it (kept: its entry is retired, not gone)",
                  (unsigned long long)key, (unsigned long long)age);
    }
    return s;
}
// A set's dynamic offsets: how many it takes and where a binding's first one is. They follow its layout (each dynamic
// binding, in binding order, takes one offset per descriptor), which bg3le records for every set the game makes
// (WOW-BG3's bg3le patch 0013): BG3 makes its layouts, many sets and its update templates before the plugin starts,
// so counting the dynamic slots the plugin saw written undercounted (7 offsets read as 5: on Nvidia's driver BG3's
// state rebound with those lost the device; RADV let it pass). A set bg3le does not know falls back to that count.
static int (*g_set_dyn)(uint64_t set, uint32_t* binding, uint32_t* count, int cap);
static int g_set_dyn_tried;
static uint64_t g_set_dyn_unknown;
static int set_dyn_layout(uint64_t set, uint32_t* b, uint32_t* c, int cap) {
    if (!__atomic_load_n(&g_set_dyn_tried, __ATOMIC_ACQUIRE)) {
        g_set_dyn = (int (*)(uint64_t, uint32_t*, uint32_t*, int))dlsym(RTLD_DEFAULT, "bg3le_vk_set_dynamic");
        logf_("sets: dynamic offsets from %s", g_set_dyn ? "bg3le's record of every set's layout (patch 0013)"
                                                         : "the slots seen written (this bg3le keeps no layouts: patch 0013 missing)");
        __atomic_store_n(&g_set_dyn_tried, 1, __ATOMIC_RELEASE);
    }
    int n = g_set_dyn && set ? g_set_dyn(set, b, c, cap) : -1;
    if (n < 0 && set && g_set_dyn && __atomic_fetch_add(&g_set_dyn_unknown, 1, __ATOMIC_RELAXED) % 10000 == 0)
        logf_("sets: set %llx not in bg3le's record (%llu so far): its dynamic offsets counted from the slots seen",
              (unsigned long long)set, (unsigned long long)g_set_dyn_unknown + 1);
    return n;
}
static int slot_dynamic(const SetInfo* si, uint32_t b) {
    return si->slot[b].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || si->slot[b].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
// How many dynamic offsets SET takes.
static uint32_t set_dyn_count(uint64_t set, const SetInfo* si) {
    uint32_t b[16], c[16], total = 0;
    int n = set_dyn_layout(set, b, c, 16);
    if (n >= 0) { for (int i = 0; i < n && i < 16; i++) total += c[i]; return total; }
    for (uint32_t k = 0; si && k < DESC_SLOTS; k++) total += (uint32_t)slot_dynamic(si, k);
    return total;
}
// Where BINDING's first dynamic offset is among SET's, or -1 when it is not a dynamic binding.
static int set_dyn_at(uint64_t set, const SetInfo* si, uint32_t binding) {
    uint32_t b[16], c[16];
    int n = set_dyn_layout(set, b, c, 16), at = 0;
    if (n >= 0) {
        for (int i = 0; i < n && i < 16; i++) {
            if (b[i] == binding) return at;
            at += (int)c[i];
        }
        return -1;
    }
    if (!si || binding >= DESC_SLOTS || !slot_dynamic(si, binding)) return -1;
    for (uint32_t k = 0; k < binding; k++) at += slot_dynamic(si, k);
    return at;
}
// Puts (key, s) into index t (g_fxl_lock held; no room check). 0 when t has no slot left.
static int set_put(SetTab* t, uint64_t key, SetInfo* s) {
    uint32_t i = hslot(key, t->cap), tomb = UINT32_MAX, n = 0;
    for (; n < t->cap; n++, i = (i + 1) & (t->cap - 1)) {
        uint64_t h = t->e[i].h;
        if (h == key) { __atomic_store_n(&t->e[i].s, s, __ATOMIC_RELEASE); return 1; }
        if (h == SET_TOMB) { if (tomb == UINT32_MAX) tomb = i; continue; }
        if (!h) break;
    }
    if (tomb != UINT32_MAX) { i = tomb; t->tombs--; }
    else if (n == t->cap) return 0;
    else t->used++;
    __atomic_store_n(&t->e[i].s, s, __ATOMIC_RELEASE);
    __atomic_store_n(&t->e[i].h, key, __ATOMIC_RELEASE);
    return 1;
}
static void set_unindex(uint64_t key) {  // g_fxl_lock held
    SetTab* t = g_settab;
    if (!t) return;
    uint32_t i = hslot(key, t->cap);
    for (uint32_t n = 0; n < t->cap; n++, i = (i + 1) & (t->cap - 1)) {
        uint64_t h = t->e[i].h;
        if (h == key) { __atomic_store_n(&t->e[i].h, SET_TOMB, __ATOMIC_RELEASE); t->tombs++; return; }
        if (!h) return;
    }
}
static uint32_t set_count(const SetTab* t) { return t ? t->used - t->tombs : 0; }
static void sets_log(const char* why) {
    SetTab* t = g_settab;
    uint32_t n = set_count(t);
    logf_("sets: %s: %u live and %u retired (index %u of %u slots, %u removed; %u entries made), %llu allocated, %llu freed one by one,"
          " %llu pool resets, %llu pools destroyed, %llu retired, %llu let go, %llu index rebuilds, %llu updated with no allocation seen,"
          " %llu not stored; binds of retired sets %llu (at most %llu presents after the free), of unknown sets %llu",
          why, n - g_set_retired, g_set_retired, t ? t->used : 0, t ? t->cap : 0, t ? t->tombs : 0, g_set_slabs * SET_SLAB,
          (unsigned long long)g_sets.allocs, (unsigned long long)g_sets.frees, (unsigned long long)g_sets.resets,
          (unsigned long long)g_sets.destroys, (unsigned long long)g_sets.retired, (unsigned long long)g_sets.reclaimed,
          (unsigned long long)g_sets.rebuilds, (unsigned long long)g_sets.unseen, (unsigned long long)g_sets.failed,
          (unsigned long long)__atomic_load_n(&g_sets.bound_freed, __ATOMIC_RELAXED),
          (unsigned long long)__atomic_load_n(&g_sets.bound_freed_age, __ATOMIC_RELAXED),
          (unsigned long long)__atomic_load_n(&g_sets.bound_unknown, __ATOMIC_RELAXED));
}
// Room in the index for one more handle (g_fxl_lock held).
static void set_room(void) {
    int ring = -1;
    for (int k = 0; k < SET_OLD; k++) {
        if (g_settab_old[k] && now_s() - g_settab_old_t[k] > 2.0) { free(g_settab_old[k]); g_settab_old[k] = NULL; }
        if (!g_settab_old[k]) ring = k;
    }
    SetTab* t = g_settab;
    if (!t) {
        t = calloc(1, sizeof *t + (size_t)SET_CAP0 * sizeof t->e[0]);
        if (t) t->cap = SET_CAP0;
        __atomic_store_n(&g_settab, t, __ATOMIC_RELEASE);
        return;
    }
    if ((uint64_t)t->used * 4 < (uint64_t)t->cap * 3 || ring < 0) return;
    uint32_t n = set_count(t), cap = t->cap;
    while ((uint64_t)n * 4 > cap) cap *= 2;
    SetTab* nt = calloc(1, sizeof *nt + (size_t)cap * sizeof nt->e[0]);
    if (!nt) { sets_log("no memory for a new index"); return; }
    nt->cap = cap;
    for (uint32_t i = 0; i < t->cap; i++)
        if (t->e[i].h && t->e[i].h != SET_TOMB) set_put(nt, t->e[i].h, t->e[i].s);
    __atomic_store_n(&g_settab, nt, __ATOMIC_RELEASE);
    g_settab_old[ring] = t; g_settab_old_t[ring] = now_s();
    g_sets.rebuilds++;
    if (g_sets.rebuilds <= 4 || !(g_sets.rebuilds & (g_sets.rebuilds - 1))) sets_log("index rebuilt");
}
static void ret_unlink(SetInfo* s) {  // g_fxl_lock held
    if (s->qprev) s->qprev->qnext = s->qnext; else g_ret_head = s->qnext;
    if (s->qnext) s->qnext->qprev = s->qprev; else g_ret_tail = s->qprev;
    s->qprev = s->qnext = NULL;
}
static void ret_push(SetInfo* s, uint64_t at) {  // g_fxl_lock held
    s->q_at = at;
    s->qprev = g_ret_tail; s->qnext = NULL;
    if (g_ret_tail) g_ret_tail->qnext = s; else g_ret_head = s;
    g_ret_tail = s;
}
// Lets go of retired entries BG3 has neither freed nor bound for SET_KEEP presents (g_fxl_lock held).
static void sets_reclaim(void) {
    uint64_t now = sets_now();
    while (g_ret_head && g_ret_head->q_at + SET_KEEP <= now) {
        SetInfo* s = g_ret_head;
        ret_unlink(s);
        uint64_t b = __atomic_load_n(&s->bound_at, __ATOMIC_RELAXED);
        if (b + SET_KEEP > now) { ret_push(s, b); continue; }  // still bound: its time runs from that bind
        set_unindex(s->h);
        __atomic_store_n(&s->h, 0, __ATOMIC_RELAXED);
        s->qnext = g_set_free; g_set_free = s;
        g_set_retired--;
        g_sets.reclaimed++;
    }
}
// A new entry for `key`, live and empty (g_fxl_lock held).
static SetInfo* set_new(uint64_t key) {
    set_room();
    if (!g_settab) return NULL;
    SetInfo* s = g_set_free;
    if (s) g_set_free = s->qnext;
    else {
        SetInfo* slab = calloc(SET_SLAB, sizeof *slab);
        if (!slab) return NULL;
        g_set_slabs++;
        for (uint32_t k = 1; k < SET_SLAB; k++) { slab[k].qnext = g_set_free; g_set_free = &slab[k]; }
        s = &slab[0];
    }
    memset(s, 0, sizeof *s);
    s->h = key;
    if (!set_put(g_settab, key, s)) { s->h = 0; s->qnext = g_set_free; g_set_free = s; return NULL; }
    return s;
}
// The entry of `key`, made if missing (g_fxl_lock held).
static SetInfo* set_get(uint64_t key) {
    if (!key || key == SET_TOMB) return NULL;
    SetInfo* s = set_find(key);
    if (!s) s = set_new(key);
    if (!s && !g_sets.failed++) sets_log("index full, a set not stored");
    return s;
}
static PoolInfo* pool_info(uint64_t pool, int create) {  // g_fxl_lock held
    if (!pool) return NULL;
    if (!g_pool) g_pool = calloc(POOL_CAP, sizeof *g_pool);
    PoolInfo* p = g_pool ? TABLE_FIND(g_pool, POOL_CAP, pool, create) : NULL;
    if (!p && create && !g_sets.pools_full++) logf_("sets: pool table full; sets of new pools are retired only when freed one by one");
    return p;
}
// Takes pool p's entry out of its table: the entries after it move back (only writers, under the lock, read it).
static void pool_remove(PoolInfo* p) {
    free(p->sets);
    uint32_t i = (uint32_t)(p - g_pool), j = i;
    for (;;) {
        memset(&g_pool[i], 0, sizeof g_pool[i]);
        for (;;) {
            j = (j + 1) & (POOL_CAP - 1);
            if (!g_pool[j].h) return;
            uint32_t k = hslot(g_pool[j].h, POOL_CAP);
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;  // its home lies after the hole: it stays
            break;
        }
        g_pool[i] = g_pool[j];
        i = j;
    }
}
// Live set s leaves its pool's list (g_fxl_lock held).
static void pool_unlist(SetInfo* s) {
    PoolInfo* p = pool_info(s->pool, 0);
    if (!p || s->pidx >= p->n || p->sets[s->pidx] != s->h) return;
    uint64_t last = p->sets[--p->n];
    if (s->pidx < p->n) {
        p->sets[s->pidx] = last;
        SetInfo* m = set_find(last);
        if (m && m->pool == s->pool) m->pidx = s->pidx;
    }
}
// BG3 freed live set s (its pool list already updated; g_fxl_lock held).
static void set_retire(SetInfo* s) {
    uint64_t now = sets_now();
    s->pool = 0; s->pidx = 0;
    __atomic_store_n(&s->freed_at, now, __ATOMIC_RELAXED);
    __atomic_store_n(&s->freed, 1, __ATOMIC_RELAXED);
    ret_push(s, now);
    g_set_retired++;
    g_sets.retired++;
}
static void sets_allocated(uint64_t pool, uint32_t n, const VkDescriptorSet* sets) {  // g_fxl_lock held
    sets_reclaim();
    for (uint32_t k = 0; k < n; k++) {
        SetInfo* s = set_get((uint64_t)sets[k]);
        if (!s) continue;
        // The handle again: a new set in place of the retired one, or of one whose end was never seen.
        if (s->freed) { ret_unlink(s); g_set_retired--; }
        else if (s->pool) pool_unlist(s);
        s->pool = 0; s->pidx = 0; s->q_at = 0;
        __atomic_store_n(&s->freed_at, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&s->bound_at, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&s->freed, 0, __ATOMIC_RELAXED);
        memset(s->slot, 0, sizeof s->slot);
        g_sets.allocs++;
        PoolInfo* p = pool_info(pool, 1);
        if (!p) continue;
        if (p->n == p->cap) {
            uint32_t cap = p->cap ? p->cap * 2 : 64;
            uint64_t* ns = realloc(p->sets, (size_t)cap * sizeof *ns);
            if (!ns) continue;
            p->sets = ns; p->cap = cap;
        }
        s->pool = pool; s->pidx = p->n;
        p->sets[p->n++] = s->h;
    }
}
static void sets_freed(uint64_t pool, uint32_t n, const VkDescriptorSet* sets) {  // g_fxl_lock held
    sets_reclaim();
    for (uint32_t k = 0; k < n; k++) {
        SetInfo* s = set_find((uint64_t)sets[k]);
        if (!s || s->freed || (s->pool && pool && s->pool != pool)) continue;
        pool_unlist(s);
        set_retire(s);
        g_sets.frees++;
    }
}
static void sets_pool_end(uint64_t pool, int destroyed) {  // g_fxl_lock held
    sets_reclaim();
    PoolInfo* p = pool_info(pool, 0);
    if (destroyed) g_sets.destroys++; else g_sets.resets++;
    if (!p) return;
    for (uint32_t k = 0; k < p->n; k++) {
        SetInfo* s = set_find(p->sets[k]);
        if (s && !s->freed && s->pool == pool) set_retire(s);
    }
    p->n = 0;
    if (destroyed) pool_remove(p);
}
typedef struct { uint64_t h; uint32_t n; struct { uint32_t binding, elem, count, type; uint64_t off, stride; } e[16]; } TmplInfo;
#define TMPL_CAP (1u << 12)
static TmplInfo* g_tmpl;
static volatile uint32_t g_tmpl_updates, g_set_updates;
static void set_slot(uint64_t set, uint32_t binding, uint32_t type, uint64_t res, uint64_t off, uint64_t range, uint64_t samp,
                     int32_t layout) {
    if (binding >= DESC_SLOTS) return;
    SetInfo* si = set_find(set);
    if (!si) { si = set_get(set); if (si) g_sets.unseen++; }  // allocated before the hooks
    if (!si) return;
    si->slot[binding] = (DescSlot){(uint8_t)type, layout, res, off, range, samp};
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* ai,
                                                                   VkDescriptorSet* sets) {
    DeviceData* d = device_for(device);
    if (!d || !d->AllocateDescriptorSets) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->AllocateDescriptorSets(device, ai, sets);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_fxl_lock);
        sets_allocated((uint64_t)ai->descriptorPool, ai->descriptorSetCount, sets);
        pthread_mutex_unlock(&g_fxl_lock);
    }
    return r;
}
// The entries go before the driver frees: once freed, a handle can come back from another thread's allocation.
static VKAPI_ATTR VkResult VKAPI_CALL layer_FreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t n,
                                                               const VkDescriptorSet* sets) {
    DeviceData* d = device_for(device);
    if (!d || !d->FreeDescriptorSets) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&g_fxl_lock);
    sets_freed((uint64_t)pool, n, sets);
    pthread_mutex_unlock(&g_fxl_lock);
    return d->FreeDescriptorSets(device, pool, n, sets);
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags fl) {
    DeviceData* d = device_for(device);
    if (!d || !d->ResetDescriptorPool) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&g_fxl_lock);
    sets_pool_end((uint64_t)pool, 0);
    pthread_mutex_unlock(&g_fxl_lock);
    return d->ResetDescriptorPool(device, pool, fl);
}
static VKAPI_ATTR void VKAPI_CALL layer_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks* alloc) {
    DeviceData* d = device_for(device);
    if (!d || !d->DestroyDescriptorPool) return;
    pthread_mutex_lock(&g_fxl_lock);
    if (pool) sets_pool_end((uint64_t)pool, 1);
    pthread_mutex_unlock(&g_fxl_lock);
    d->DestroyDescriptorPool(device, pool, alloc);
}
static VKAPI_ATTR void VKAPI_CALL layer_UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet* w,
                                                             uint32_t nc, const VkCopyDescriptorSet* c) {
    DeviceData* d = device_for(device);
    if (!d) return;
    pthread_mutex_lock(&g_fxl_lock);
    g_set_updates++;
    for (uint32_t i = 0; i < nw; i++) {
        const VkWriteDescriptorSet* x = &w[i];
        for (uint32_t k = 0; k < x->descriptorCount && k < 1; k++) {
            if (x->pBufferInfo && (x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                                   x->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || x->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC))
                set_slot((uint64_t)x->dstSet, x->dstBinding, x->descriptorType, (uint64_t)x->pBufferInfo[k].buffer, x->pBufferInfo[k].offset, x->pBufferInfo[k].range, 0, -1);
            else if (x->pImageInfo && (x->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || x->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                                       x->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || x->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                                       x->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT))
                set_slot((uint64_t)x->dstSet, x->dstBinding, x->descriptorType, (uint64_t)x->pImageInfo[k].imageView, 0, 0,
                         (uint64_t)x->pImageInfo[k].sampler, (int32_t)x->pImageInfo[k].imageLayout);
        }
    }
    for (uint32_t i = 0; i < nc; i++) {  // copies carry slots between sets
        SetInfo* from = set_find((uint64_t)c[i].srcSet);
        for (uint32_t k = 0; from && k < c[i].descriptorCount && k < 1; k++) {
            uint32_t sb = c[i].srcBinding, db = c[i].dstBinding;
            if (sb >= DESC_SLOTS || db >= DESC_SLOTS || c[i].srcArrayElement || c[i].dstArrayElement) continue;
            DescSlot sl = from->slot[sb];
            set_slot((uint64_t)c[i].dstSet, db, sl.type, sl.res, sl.off, sl.range, sl.samp, sl.layout);
        }
    }
    pthread_mutex_unlock(&g_fxl_lock);
    d->UpdateDescriptorSets(device, nw, w, nc, c);
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* ci,
                                                                         const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateDescriptorUpdateTemplate(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_fxl_lock);
        if (!g_tmpl) g_tmpl = calloc(TMPL_CAP, sizeof *g_tmpl);
        TmplInfo* t = g_tmpl ? TABLE_FIND(g_tmpl, TMPL_CAP, (uint64_t)*out, 1) : NULL;
        if (t) {
            t->n = 0;
            for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount && t->n < 16; i++, t->n++) {
                const VkDescriptorUpdateTemplateEntry* e = &ci->pDescriptorUpdateEntries[i];
                t->e[t->n].binding = e->dstBinding; t->e[t->n].elem = e->dstArrayElement; t->e[t->n].count = e->descriptorCount;
                t->e[t->n].type = e->descriptorType; t->e[t->n].off = e->offset; t->e[t->n].stride = e->stride;
            }
        }
        pthread_mutex_unlock(&g_fxl_lock);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL layer_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
                                                                        VkDescriptorUpdateTemplate tmpl, const void* data) {
    DeviceData* d = device_for(device);
    if (!d) return;
    pthread_mutex_lock(&g_fxl_lock);
    g_tmpl_updates++;
    TmplInfo* t = g_tmpl ? TABLE_FIND(g_tmpl, TMPL_CAP, (uint64_t)tmpl, 0) : NULL;
    for (uint32_t i = 0; t && i < t->n; i++) {
        const uint8_t* at = (const uint8_t*)data + t->e[i].off;
        uint32_t ty = t->e[i].type;
        if (ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            const VkDescriptorBufferInfo* bi = (const VkDescriptorBufferInfo*)at;
            set_slot((uint64_t)set, t->e[i].binding, ty, (uint64_t)bi->buffer, bi->offset, bi->range, 0, -1);
        } else if (ty == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || ty == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || ty == VK_DESCRIPTOR_TYPE_SAMPLER ||
                   ty == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || ty == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
            const VkDescriptorImageInfo* ii = (const VkDescriptorImageInfo*)at;
            set_slot((uint64_t)set, t->e[i].binding, ty, (uint64_t)ii->imageView, 0, 0, (uint64_t)ii->sampler, (int32_t)ii->imageLayout);
        }
    }
    pthread_mutex_unlock(&g_fxl_lock);
    d->UpdateDescriptorSetWithTemplate(device, set, tmpl, data);
}

static volatile int g_fxc_on;  // effects capture on (fxc_present keeps it)
static void fxc_pass_target(uint64_t image);
static int fxc_draw(VkCommandBuffer cb, int indexed, uint32_t count, uint32_t inst, uint32_t first, int32_t vo, uint32_t fi);
static void fxp_draw(VkCommandBuffer cb);
static int fxc_figure_extra(VkCommandBuffer cb);
static __thread int t_fxc_glow;
static __thread int t_fx_surf;  // this draw is one of BG3's ground surfaces (surf_draw)
static void fxw_pass(VkCommandBuffer cb, const char* what, uint32_t n, const uint64_t* imgs, const uint8_t* load);
static void fxw_draw(VkCommandBuffer cb, char kind, uint32_t count, uint32_t inst, uint32_t first, int32_t vo, uint32_t fi, int why);
static void fxp_dispatch(VkCommandBuffer cb);
static int wd_skip_cs(VkCommandBuffer cb);
static void wd_eye_log(VkCommandBuffer cb);
static void wd_eye_after(DeviceData* d, VkCommandBuffer cb);
static void wdx_dispatch_pre(VkCommandBuffer cb);
static void wdx_dispatch_after(DeviceData* d, VkCommandBuffer cb);  // patch60
static void wdx_readers(VkCommandBuffer cb, int compute);
static void post_cas(DeviceData* d, VkCommandBuffer cb);
static void post_dt(DeviceData* d, VkCommandBuffer cb);
static void fb_retire_sweep(DeviceData* d);
static void fxc_copy(uint64_t src, uint64_t dst);
static void fb_copy_hook(DeviceData* d, VkCommandBuffer cb, uint64_t s, uint64_t t, VkImageLayout sl);
static void fog_draw(VkCommandBuffer cb);
static void wd_light_log(VkCommandBuffer cb);
static void wd_light_ind(VkCommandBuffer cb, VkDeviceSize o);
static __thread uint32_t t_cdyn[8][8], t_cndyn[8];  // compute dynamic offsets per set
static __thread uint8_t t_pcb[256];
static void recipe_record(const VkGraphicsPipelineCreateInfo* ci, VkPipeline out);

static void fxl_pass(VkCommandBuffer cb, uint32_t n, const uint64_t* imgs, const uint8_t* load) {
    t_fxl_cb = VK_NULL_HANDLE;
    t_dec_cb = VK_NULL_HANDLE;
    if ((g_fxl_frames <= 0 && !g_fxc_on && !g_cc.fxbg) || !g_role.depth) return;
    int depth = 0, colors = 0, at = -1, gbuf = 0, loaded = 1;
    for (uint32_t k = 0; k < n; k++) {
        if (imgs[k] == g_role.depth) { depth = 1; continue; }
        ImgInfo* i = imgs[k] ? TABLE_FIND(g_img, IMG_CAP, imgs[k], 0) : NULL;
        if (!i) return;
        colors++;
        if (load[k] != VK_ATTACHMENT_LOAD_OP_LOAD) loaded = 0;
        if (fx_format(i->fmt) && i->w == SCENE_W && i->hgt == SCENE_H) at = (int)k;
        if (colors == 1 && i->fmt == VK_FORMAT_R16G16_SFLOAT && i->w == SCENE_W && i->hgt == SCENE_H) gbuf = 1;
    }
    // BG3's deferred decal pass: the scene depth and four G-buffer targets (R16G16
    // normal first, then RGBA8), all loaded; the G-buffer pass itself clears five.
    if (depth && colors == 4 && gbuf && loaded) { t_dec_cb = cb; return; }
    if (!depth || colors != 1 || at < 0 || load[at] != VK_ATTACHMENT_LOAD_OP_LOAD) return;
    t_fxl_cb = cb;
    fxc_pass_target(imgs[at]);
    pthread_mutex_lock(&g_fxl_lock);
    if (g_fxl) fprintf(g_fxl, "PASS cb %llx target %llx\n", (unsigned long long)(uintptr_t)cb, (unsigned long long)imgs[at]);
    pthread_mutex_unlock(&g_fxl_lock);
}

static void fxl_draw(VkCommandBuffer cb, const char* kind, uint32_t count, uint32_t inst, uint32_t first, int32_t vo, uint32_t fi) {
    if (t_fxl_cb != cb || !g_fxl) return;
    PipeInfo* p = (g_pipe && t_bind_cb == cb && t_pipe) ? TABLE_FIND(g_pipe, PIPE_CAP, t_pipe, 0) : NULL;
    char attrs[400] = {0}, maps[64] = {0};
    int n = 0;
    for (int k = 0; k < 8; k++) {
        uint64_t left = 0;
        maps[k] = !t_vb[k] ? '-' : buf_cpu(t_vb[k], t_vbo[k], &left) ? 'M' : 'g';
    }
    maps[8] = !t_ib ? '-' : buf_cpu(t_ib, t_ibo, NULL) ? 'M' : 'g';
    if (p)
        for (uint32_t a = 0; a < p->nattr && n < 360; a++)
            n += snprintf(attrs + n, sizeof attrs - n, " L%u:b%u:f%u:+%u", p->aloc[a], p->abind[a], p->afmt[a], p->aoff[a]);
    pthread_mutex_lock(&g_fxl_lock);
    if (g_fxl) {
        fprintf(g_fxl, "#%u%s %s n%u i%u first%u vo%d fi%u pipe %llx topo %u blend %x zw %u cull %u stages %x strides %u/%u/%u rate %u/%u%s"
                 " vb %llx+%llx %llx+%llx %llx+%llx ib %llx+%llx t%u sets %llx %llx %llx %llx pc %u map %s s4-7 %u/%u/%u/%u r2-7 %u%u%u%u%u%u\n",
                draw_no + 1, p ? "" : (t_bind_cb != cb ? "?cb" : "?pipe"), kind, count, inst, first, vo, fi, (unsigned long long)t_pipe, p ? p->topo : 99, p ? p->blend : 0,
                p ? p->zwrite : 0, p ? p->cull : 0, p ? p->stages : 0, p ? p->stride[0] : 0, p ? p->stride[1] : 0,
                p ? p->stride[2] : 0, p ? p->rate[0] : 0, p ? p->rate[1] : 0, attrs,
                (unsigned long long)t_vb[0], (unsigned long long)t_vbo[0], (unsigned long long)t_vb[1], (unsigned long long)t_vbo[1],
                (unsigned long long)t_vb[2], (unsigned long long)t_vbo[2], (unsigned long long)t_ib, (unsigned long long)t_ibo, t_ibt,
                (unsigned long long)t_set[0], (unsigned long long)t_set[1], (unsigned long long)t_set[2], (unsigned long long)t_set[3], t_pc,
                maps, p ? p->stride[4] : 0, p ? p->stride[5] : 0, p ? p->stride[6] : 0, p ? p->stride[7] : 0,
                p ? p->rate[2] : 0, p ? p->rate[3] : 0, p ? p->rate[4] : 0, p ? p->rate[5] : 0, p ? p->rate[6] : 0, p ? p->rate[7] : 0);
    }
    // The mapped vertex data of this draw (OUT_DIR/fxdata.bin, records "FXD1", draw number, binding,
    // stride, rate, byte count, bytes; binding 8 is the index buffer).
    draw_no++;
    if (g_fxl && p) {
        for (int k = 0; k < 9; k++) {
            uint64_t h = k < 8 ? t_vb[k] : t_ib, off = k < 8 ? t_vbo[k] : t_ibo, left = 0;
            uint32_t stride = k < 8 ? p->stride[k] : (t_ibt == 1 ? 4 : 2), rate = k < 8 ? p->rate[k] : 0;
            if (!h || (k < 8 && !stride)) continue;
            if (k == 8) off += (uint64_t)first * stride;
            const uint8_t* src = buf_cpu(h, off, &left);
            if (!src) continue;
            uint64_t want = k == 8 ? (uint64_t)count * stride : rate ? (uint64_t)stride * inst : (uint64_t)stride * (count + 64);
            if (want > left) want = left;
            if (want > (1u << 20)) want = 1u << 20;
            uint32_t hdr[6] = {0x31445846u, draw_no, (uint32_t)k, stride, rate, (uint32_t)want};
            if (!g_fxd) g_fxd = fopen(outp("/fxdata.bin"), "wb");
            if (g_fxd) { fwrite(hdr, 4, 6, g_fxd); fwrite(src, 1, want, g_fxd); }
        }
        // Uniform and storage buffers of sets 0..3 (record binding 16 + set * 16 + binding; images
        // are logged in fxlog.txt only).
        for (int st = 0; st < 4; st++) {
            SetInfo* si = set_find(t_set[st]);
            if (!si) continue;
            uint32_t dn = 0;
            char line[512];
            int ln = snprintf(line, sizeof line, "   set%d %llx:", st, (unsigned long long)t_set[st]);
            for (uint32_t b = 0; b < DESC_SLOTS; b++) {
                DescSlot* sl = &si->slot[b];
                if (!sl->type && !sl->res) continue;
                uint64_t off = sl->off;
                int dynamic = sl->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || sl->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
                if (dynamic) { if (dn < t_ndyn[st] && dn < 8) off += t_dyn[st][dn]; dn++; }
                if (ln < 480) ln += snprintf(line + ln, sizeof line - ln, " b%u t%u %llx+%llx/%llx", b, sl->type, (unsigned long long)sl->res,
                                             (unsigned long long)off, (unsigned long long)sl->range);
                if (sl->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sl->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
                uint64_t left = 0;
                const uint8_t* src = buf_cpu(sl->res, off, &left);
                if (!src) continue;
                uint64_t want = sl->range && sl->range != VK_WHOLE_SIZE ? sl->range : 4096;
                if (want > left) want = left;
                if (want > 16384) want = 16384;
                uint32_t hdr[6] = {0x31445846u, draw_no, 16u + (uint32_t)st * 16u + b, 0, 0, (uint32_t)want};
                if (!g_fxd) g_fxd = fopen(outp("/fxdata.bin"), "wb");
                if (g_fxd) { fwrite(hdr, 4, 6, g_fxd); fwrite(src, 1, want, g_fxd); }
            }
            if (g_fxl) fprintf(g_fxl, "%s\n", line);
        }
    }
    pthread_mutex_unlock(&g_fxl_lock);
}

static void fxl_present(void) {
    if (g_fxl_frames <= 0) return;
    pthread_mutex_lock(&g_fxl_lock);
    if (!g_fxl) g_fxl = fopen(outp("/fxlog.txt"), "w");
    if (g_fxl) fprintf(g_fxl, "PRESENT %d pipelines %u set updates %u template updates %u\n", g_fxl_frames, g_pipe_count, g_set_updates, g_tmpl_updates);
    if (--g_fxl_frames <= 0 && g_fxl) {
        fclose(g_fxl); g_fxl = NULL;
        if (g_fxd) { fclose(g_fxd); g_fxd = NULL; }
        logf_("fxlog: written");
    }
    pthread_mutex_unlock(&g_fxl_lock);
}

// Seconds since this process started (/proc/self/stat start time against CLOCK_BOOTTIME), for the log.
static double proc_age_s(void) {
    char b[1024];
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return -1;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char* p = strrchr(b, ')');
    if (!p) return -1;
    unsigned long long st = 0;
    int field = 2;
    for (char* q = p + 1; *q && field < 22; q++)
        if (*q == ' ' && ++field == 22) st = strtoull(q + 1, NULL, 10);
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    long hz = sysconf(_SC_CLK_TCK);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9 - (double)st / (double)(hz > 0 ? hz : 100);
}
static void pipe_watch(const VkGraphicsPipelineCreateInfo* ci);  // after wowbg3_fxcap.inc.c (ShaderInfo)
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                                    const VkGraphicsPipelineCreateInfo* infos,
                                                                    const VkAllocationCallbacks* alloc, VkPipeline* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->CreateGraphicsPipelines(device, cache, n, infos, alloc, out);
    if (r == VK_SUCCESS || r == VK_PIPELINE_COMPILE_REQUIRED) {
        pthread_mutex_lock(&g_fxl_lock);
        if (!g_pipe) g_pipe = calloc(PIPE_CAP, sizeof *g_pipe);  // under the lock: pipelines come from many threads
        for (uint32_t k = 0; k < n && g_pipe; k++) {
            if (!out[k]) continue;
            recipe_record(&infos[k], out[k]);
            ground_note_pipeline(&infos[k]);
            PipeInfo* p = TABLE_FIND(g_pipe, PIPE_CAP, (uint64_t)out[k], 1);
            if (!p) continue;
            g_pipe_count++;
            pipe_watch(&infos[k]);
            const VkGraphicsPipelineCreateInfo* ci = &infos[k];
            const VkPipelineVertexInputStateCreateInfo* vi = ci->pVertexInputState;
            p->nbind = p->nattr = 0;
            if (vi) {
                for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount && b < 8; b++) {
                    uint32_t at = vi->pVertexBindingDescriptions[b].binding;
                    if (at < 8) { p->stride[at] = vi->pVertexBindingDescriptions[b].stride; p->rate[at] = vi->pVertexBindingDescriptions[b].inputRate; }
                    p->nbind++;
                }
                for (uint32_t a = 0; a < vi->vertexAttributeDescriptionCount && a < 12; a++) {
                    p->aloc[a] = vi->pVertexAttributeDescriptions[a].location;
                    p->abind[a] = vi->pVertexAttributeDescriptions[a].binding;
                    p->afmt[a] = vi->pVertexAttributeDescriptions[a].format;
                    p->aoff[a] = vi->pVertexAttributeDescriptions[a].offset;
                    p->nattr++;
                }
            }
            p->topo = ci->pInputAssemblyState ? ci->pInputAssemblyState->topology : 99;
            p->blend = 0;
            if (ci->pColorBlendState && ci->pColorBlendState->attachmentCount) {
                const VkPipelineColorBlendAttachmentState* b = &ci->pColorBlendState->pAttachments[0];
                p->blend = b->blendEnable | (b->srcColorBlendFactor << 4) | (b->dstColorBlendFactor << 9) | (b->colorBlendOp << 14) |
                           (b->srcAlphaBlendFactor << 17) | (b->dstAlphaBlendFactor << 22);
            }
            p->zwrite = ci->pDepthStencilState ? (ci->pDepthStencilState->depthWriteEnable | (ci->pDepthStencilState->depthTestEnable << 1)) : 9;
            p->cull = ci->pRasterizationState ? ci->pRasterizationState->cullMode : 9;
            p->stages = 0;
            for (uint32_t st = 0; st < ci->stageCount; st++) p->stages |= ci->pStages[st].stage;
            // A pipeline linked from libraries (VK_EXT_graphics_pipeline_library) takes what its
            // libraries had: vertex input, topology, blend, depth.
            for (const VkBaseInStructure* e = ci->pNext; e; e = e->pNext) {
                if (e->sType != VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) continue;
                const VkPipelineLibraryCreateInfoKHR* li = (const VkPipelineLibraryCreateInfoKHR*)e;
                for (uint32_t l = 0; l < li->libraryCount; l++) {
                    PipeInfo* q = TABLE_FIND(g_pipe, PIPE_CAP, (uint64_t)li->pLibraries[l], 0);
                    if (!q) continue;
                    if (!p->nattr && q->nattr) {
                        p->nattr = q->nattr; p->nbind = q->nbind;
                        memcpy(p->aloc, q->aloc, sizeof p->aloc); memcpy(p->abind, q->abind, sizeof p->abind);
                        memcpy(p->afmt, q->afmt, sizeof p->afmt); memcpy(p->aoff, q->aoff, sizeof p->aoff);
                        memcpy(p->stride, q->stride, sizeof p->stride); memcpy(p->rate, q->rate, sizeof p->rate);
                    }
                    if (p->topo == 99 && q->topo != 99) p->topo = q->topo;
                    if (!p->blend && q->blend) p->blend = q->blend;
                    if (p->zwrite == 9 && q->zwrite != 9) p->zwrite = q->zwrite;
                    if (p->cull == 9 && q->cull != 9) p->cull = q->cull;
                    p->stages |= q->stages;
                }
            }
        }
        pthread_mutex_unlock(&g_fxl_lock);
    }
    return r;
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pl) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        if (t_bind_cb != cb) { t_bind_cb = cb; memset(t_vb, 0, sizeof t_vb); memset(t_set, 0, sizeof t_set); }
        t_pipe = (uint64_t)pl;
    }
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        if (t_cbind_cb != cb) { t_cbind_cb = cb; memset(t_cset, 0, sizeof t_cset); }
        t_cpipe = (uint64_t)pl;
    }
    d->CmdBindPipeline(cb, bp, pl);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b,
                                                             const VkDeviceSize* o) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (t_bind_cb != cb) { t_bind_cb = cb; t_pipe = 0; memset(t_vb, 0, sizeof t_vb); memset(t_set, 0, sizeof t_set); }
    for (uint32_t k = 0; k < n; k++) if (first + k < 8) { t_vb[first + k] = (uint64_t)b[k]; t_vbo[first + k] = o[k]; }
    d->CmdBindVertexBuffers(cb, first, n, b, o);
}
// BG3's descriptor binds in a command buffer, kept as it made them, to put its state back after the plugin draws in
// it: for each set number the newest call that bound it (at most 8 calls), replayed in BG3's order. The layout, the
// sets and every dynamic offset exactly as BG3 passed them, so nothing is rebuilt from what the plugin knows of
// BG3's layouts (a rebuilt bind with too few dynamic offsets lost the device on Nvidia's driver).
typedef struct { VkPipelineLayout lay; uint32_t first, n, nd; uint64_t seq; VkDescriptorSet sets[8]; uint32_t dyn[64]; } BindCall;
typedef struct { VkCommandBuffer cb; uint64_t seq; BindCall call[8]; int of[8]; } BindLog;
static __thread BindLog t_gbinds, t_cbinds;
static void binds_note(BindLog* L, VkCommandBuffer cb, VkPipelineLayout lay, uint32_t first, uint32_t n,
                       const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn) {
    if (L->cb != cb) { memset(L, 0, sizeof *L); L->cb = cb; for (int k = 0; k < 8; k++) L->of[k] = -1; }
    if (!n || first >= 8 || n > 8 || first + n > 8 || nd > 64) {  // never seen from BG3; the replay then leaves this out
        for (uint32_t k = first; k < first + n && k < 8; k++) L->of[k] = -1;
        return;
    }
    int slot = -1;
    for (int c = 0; c < 8 && slot < 0; c++) {  // a call no set number points at any more
        int used = 0;
        for (int k = 0; k < 8; k++) used |= L->of[k] == c;
        if (!used) slot = c;
    }
    if (slot < 0) return;  // cannot happen: 8 set numbers, 8 calls
    BindCall* c = &L->call[slot];
    c->lay = lay; c->first = first; c->n = n; c->nd = nd; c->seq = ++L->seq;
    memcpy(c->sets, sets, n * sizeof *sets);
    if (nd) memcpy(c->dyn, dyn, nd * sizeof *dyn);
    for (uint32_t k = first; k < first + n; k++) L->of[k] = slot;
}
// Replays BG3's binds of this command buffer (graphics or compute), oldest first.
static void binds_restore(DeviceData* d, VkCommandBuffer cb, int compute) {
    BindLog* L = compute ? &t_cbinds : &t_gbinds;
    if (L->cb != cb) return;
    int order[8], m = 0;
    for (int c = 0; c < 8; c++) {
        int used = 0;
        for (int k = 0; k < 8; k++) used |= L->of[k] == c;
        if (used) order[m++] = c;
    }
    for (int i = 1; i < m; i++)
        for (int j = i; j > 0 && L->call[order[j]].seq < L->call[order[j - 1]].seq; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    for (int i = 0; i < m; i++) {
        const BindCall* c = &L->call[order[i]];
        d->CmdBindDescriptorSets(cb, compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS, c->lay, c->first, c->n,
                                 c->sets, c->nd, c->nd ? c->dyn : NULL);
    }
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout lay,
                                                              uint32_t first, uint32_t n, const VkDescriptorSet* sets,
                                                              uint32_t nd, const uint32_t* dyn) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        if (t_cbind_cb != cb) { t_cbind_cb = cb; t_cpipe = 0; memset(t_cset, 0, sizeof t_cset); }
        t_clayout = (uint64_t)lay;
        uint32_t di = 0;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t s = first + k;
            if (s < 8) t_cset[s] = (uint64_t)sets[k];
            SetInfo* si = set_bound((uint64_t)sets[k]);
            const uint32_t nd_here = set_dyn_count((uint64_t)sets[k], si);
            for (uint32_t j = 0; j < nd_here; j++, di++)
                if (s < 8 && j < 8 && di < nd) t_cdyn[s][j] = dyn[di];
            if (s < 8) t_cndyn[s] = nd_here;
        }
        binds_note(&t_cbinds, cb, lay, first, n, sets, nd, dyn);
    }
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        if (t_bind_cb != cb) { t_bind_cb = cb; t_pipe = 0; memset(t_vb, 0, sizeof t_vb); memset(t_set, 0, sizeof t_set); }
        // Dynamic offsets are consumed in set order, binding order; kept per set in that order.
        uint32_t di = 0;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t s = first + k;
            if (s < 8) t_set[s] = (uint64_t)sets[k];
            SetInfo* si = set_bound((uint64_t)sets[k]);
            const uint32_t nd_here = set_dyn_count((uint64_t)sets[k], si);
            for (uint32_t j = 0; j < nd_here; j++, di++)
                if (s < 8 && j < 8 && di < nd) t_dyn[s][j] = dyn[di];
            if (s < 8) t_ndyn[s] = nd_here;
        }
        binds_note(&t_gbinds, cb, lay, first, n, sets, nd, dyn);
        pace_bind(cb);
        ground_bind(cb);
        ground_view_seen();
    }
    d->CmdBindDescriptorSets(cb, bp, lay, first, n, sets, nd, dyn);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout lay, VkShaderStageFlags st,
                                                         uint32_t off, uint32_t size, const void* v) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    t_pc = off + size;
    if (off < sizeof t_pcb) memcpy(t_pcb + off, v, off + size <= sizeof t_pcb ? size : sizeof t_pcb - off);
    d->CmdPushConstants(cb, lay, st, off, size, v);
}

// BG3's G-buffer pass, at whatever size: its scene depth and five colour targets, the first the R16G16 normal. Its size is
// the scene's (g_role.sw, sh); it is the screen's unless BG3 upscales (FSR), when it is smaller.
static void scene_seen(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb, const VkRenderPassAttachmentBeginInfo* il, uint32_t w, uint32_t h) {
    (void)cb; (void)rp;
    const uint64_t now = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
    trace_lock_timed();
    FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)fb, 0);
    uint32_t n = il ? (il->attachmentCount > 8 ? 8 : il->attachmentCount) : (f ? f->n : 0);
    int depth = 0, colors = 0, normal = 0, same = 1;
    for (uint32_t k = 0; k < n; k++) {
        uint64_t im = view_image(il ? (uint64_t)il->pAttachments[k] : f->views[k]);
        ImgInfo* i = im ? TABLE_FIND(g_img, IMG_CAP, im, 0) : NULL;
        if (!i) continue;
        if (i->w != w || i->hgt != h) same = 0;
        if (is_depth_format(i->fmt)) depth = 1;
        else { if (!colors && i->fmt == VK_FORMAT_R16G16_SFLOAT) normal = 1; colors++; }
    }
    pthread_mutex_unlock(&g_trace_lock);
    // BG3 draws more than one deferred scene in a frame at times: the character preview of the inventory and the examine
    // screens is a G-buffer pass of its own (700x690 beside a 2560x1080 world on Nvidia, 752x423 on AMD). While the scene
    // is still drawn (a pass at its size this present or the last), a smaller one is such a preview and BG3's own picture;
    // only when the scene's passes stop (a new resolution or upscaling) does a new size take over. Taken twice a frame,
    // every image a pixel of the scene was made again twice a frame and the world's passes ran on the preview's images.
    if (!(depth && normal && colors >= 4 && same && w >= 64 && h >= 64 && w <= g_role.ew && h <= g_role.eh)) return;
    if (w == SCENE_W && h == SCENE_H) { __atomic_store_n(&g_role.seen_at, now + 1, __ATOMIC_RELAXED); return; }  // the scene's G-buffer pass
    const uint64_t at = __atomic_load_n(&g_role.seen_at, __ATOMIC_RELAXED);
    const int scene_drawn = at && now + 1 - at <= 1;
    if (!scene_drawn || (uint64_t)w * h > (uint64_t)SCENE_W * SCENE_H) {
        logf_("roles: BG3's scene is %ux%u (the screen %ux%u)%s", w, h, g_role.ew, g_role.eh, w < g_role.ew ? ": BG3 upscales it" : "");
        g_role.sw = w; g_role.sh = h;
        __atomic_store_n(&g_role.seen_at, now + 1, __ATOMIC_RELAXED);
    }
}
static void begin_pass(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb, const VkRenderPassAttachmentBeginInfo* il,
                       uint32_t w, uint32_t h) {
    t_void_cb = VK_NULL_HANDLE;
    t_tav_cb = VK_NULL_HANDLE;
    if (g_tav_hide && g_img && g_role.depth) {  // a scene-depth pass: the figures are WoW's (tav_hidden_draw)
        trace_lock_timed();
        FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)fb, 0);
        uint32_t n = il ? (il->attachmentCount > 8 ? 8 : il->attachmentCount) : (f ? f->n : 0);
        for (uint32_t k = 0; k < n; k++)
            if (view_image(il ? (uint64_t)il->pAttachments[k] : f->views[k]) == g_role.depth) { t_tav_cb = cb; break; }
        pthread_mutex_unlock(&g_trace_lock);
    }
    if (g_cc.on && g_cc.void_ && g_img) {
        trace_lock_timed();
        FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)fb, 0);
        uint32_t n = il ? (il->attachmentCount > 8 ? 8 : il->attachmentCount) : (f ? f->n : 0);
        uint64_t imgs[8];
        for (uint32_t k = 0; k < n; k++) imgs[k] = view_image(il ? (uint64_t)il->pAttachments[k] : f->views[k]);
        void_pass(cb, n, imgs);
        {  // the pass's layouts after it, for a draw of our own into a shadow map once it has ended (wowbg3_shade.inc.c)
            RpInfo* ps = TABLE_FIND(g_rp, RP_CAP, (uint64_t)rp, 0);
            int32_t sfin[8]; uint32_t layers[8];
            for (uint32_t k = 0; k < n; k++) {
                sfin[k] = ps && k < ps->n ? ps->final[k] : -1;
                layers[k] = view_layer(il ? (uint64_t)il->pAttachments[k] : f->views[k]);
            }
            shade_pass(cb, n, imgs, layers, sfin, w, h);
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    if (g_cc.on && g_cc.wdbloom && g_img) {  // patch28: any pass's first attachment (the bloom prefilter's is half size)
        trace_lock_timed();
        RpInfo* p0 = TABLE_FIND(g_rp, RP_CAP, (uint64_t)rp, 0);
        FbInfo* f0 = TABLE_FIND(g_fb, FB_CAP, (uint64_t)fb, 0);
        uint32_t n0 = il ? il->attachmentCount : (f0 ? f0->n : 0);
        t_any_cb = cb;
        t_any_img0 = n0 ? view_image(il ? (uint64_t)il->pAttachments[0] : f0->views[0]) : 0;
        t_any_fin0 = (p0 && p0->n) ? p0->final[0] : -1;
        pthread_mutex_unlock(&g_trace_lock);
    }
    if (g_img) scene_seen(cb, rp, fb, il, w, h);
    const int scene_pass = w == SCENE_W && h == SCENE_H;
    if (g_cc.on && g_img && (scene_pass || (w == g_role.ew && h == g_role.eh))) {
        trace_lock_timed();
        RpInfo* p = TABLE_FIND(g_rp, RP_CAP, (uint64_t)rp, 0);
        FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)fb, 0);
        uint32_t n = il ? (il->attachmentCount > 8 ? 8 : il->attachmentCount) : (f ? f->n : 0);
        uint64_t imgs[8]; int32_t fin[8], ini[8]; uint8_t load[8];
        for (uint32_t k = 0; k < n; k++) {
            imgs[k] = view_image(il ? (uint64_t)il->pAttachments[k] : f->views[k]);
            fin[k] = p && k < p->n ? p->final[k] : -1;
            ini[k] = p && k < p->n ? p->init[k] : -1;
            load[k] = p && k < p->n ? p->load[k] : 0xff;
        }
        role_pass(cb, n, imgs, fin, load);
        ground_pass(cb, n, imgs, load, fin, w, h);
        surf_pass_begin(cb, n, imgs, ini, load, fin);
        late_pass_begin(device_for(cb), cb, n, imgs, load, ini, fin);
        fx_pass(device_for(cb), cb, (uint64_t)fb, n, imgs, load, ini, fin);
        fxl_pass(cb, n, imgs, load);
        pthread_mutex_unlock(&g_trace_lock);
    }
    if (g_img) {  // a preview's tonemap pass, maybe (wowbg3_preview.inc.c)
        trace_lock_timed();
        RpInfo* p = TABLE_FIND(g_rp, RP_CAP, (uint64_t)rp, 0);
        FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)fb, 0);
        uint32_t n = il ? (il->attachmentCount > 8 ? 8 : il->attachmentCount) : (f ? f->n : 0);
        uint64_t imgs[8]; int32_t fin[8];
        for (uint32_t k = 0; k < n; k++) {
            imgs[k] = view_image(il ? (uint64_t)il->pAttachments[k] : f->views[k]);
            fin[k] = p && k < p->n ? p->final[k] : -1;
        }
        pv_pass(cb, n, imgs, fin);
        pthread_mutex_unlock(&g_trace_lock);
    }
    if (g_fxw_frames > 0 && g_img) {
        trace_lock_timed();
        RpInfo* p = TABLE_FIND(g_rp, RP_CAP, (uint64_t)rp, 0);
        FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)fb, 0);
        uint32_t n = il ? (il->attachmentCount > 8 ? 8 : il->attachmentCount) : (f ? f->n : 0);
        uint64_t imgs[8]; uint8_t load[8];
        for (uint32_t k = 0; k < n; k++) {
            imgs[k] = view_image(il ? (uint64_t)il->pAttachments[k] : f->views[k]);
            load[k] = p && k < p->n ? p->load[k] : 0xff;
        }
        fxw_pass(cb, "RP", n, imgs, load);
        pthread_mutex_unlock(&g_trace_lock);
    }
    CbLog* c = trace_cb(cb);
    if (!c) return;
    cb_flush_pass(c);
    RpInfo* p = TABLE_FIND(g_rp, RP_CAP, (uint64_t)rp, 0);
    FbInfo* f = TABLE_FIND(g_fb, FB_CAP, (uint64_t)fb, 0);
    int n = snprintf(c->pass, sizeof c->pass, "RP %ux%u", w, h);
    uint32_t count = f ? f->n : 0;
    const VkImageView* views = NULL;
    if (il) { count = il->attachmentCount > 8 ? 8 : il->attachmentCount; views = il->pAttachments; }
    for (uint32_t k = 0; k < count && n < (int)sizeof c->pass - 80; k++) {
        char d[96];
        img_desc(d, sizeof d, view_image(views ? (uint64_t)views[k] : f->views[k]));
        n += snprintf(c->pass + n, sizeof c->pass - n, " [%s L%d S%d F%d]", d, p && k < p->n ? p->load[k] : -1,
                      p && k < p->n ? p->store[k] : -1, p && k < p->n ? p->final[k] : -1);
    }
    c->in_pass = 1;
    pthread_mutex_unlock(&g_trace_lock);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info,
                                                           VkSubpassContents contents) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    const VkRenderPassAttachmentBeginInfo* il = NULL;
    for (const VkBaseInStructure* s = info->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) il = (const VkRenderPassAttachmentBeginInfo*)s;
    begin_pass(cb, info->renderPass, info->framebuffer, il, info->renderArea.extent.width, info->renderArea.extent.height);
    d->CmdBeginRenderPass(cb, info, contents);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* info,
                                                            const VkSubpassBeginInfo* sub) {
    DeviceData* d = device_for(cb);
    if (!d || !d->CmdBeginRenderPass2) return;
    const VkRenderPassAttachmentBeginInfo* il = NULL;
    for (const VkBaseInStructure* s = info->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) il = (const VkRenderPassAttachmentBeginInfo*)s;
    begin_pass(cb, info->renderPass, info->framebuffer, il, info->renderArea.extent.width, info->renderArea.extent.height);
    d->CmdBeginRenderPass2(cb, info, sub);
}
static void end_pass(VkCommandBuffer cb) {
    tavd_end(cb);
    if (t_ui_cb) role_end_pass(cb);
    if (t_sc_cb == cb) t_sc_cb = VK_NULL_HANDLE;
    CbLog* c = trace_cb(cb);
    if (!c) return;
    cb_flush_pass(c);
    pthread_mutex_unlock(&g_trace_lock);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdEndRenderPass(VkCommandBuffer cb) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    end_pass(cb);
    if (t_fxl_cb == cb) t_fxl_cb = VK_NULL_HANDLE;
    if (t_dec_cb == cb) t_dec_cb = VK_NULL_HANDLE;
    if (t_void_cb == cb) t_void_cb = VK_NULL_HANDLE;
    d->CmdEndRenderPass(cb);
    if (t_fx_cb == cb) fx_end(d, cb);
    ground_end(d, cb);
    shade_end(d, cb);
    surf_pass_end(d, cb);
    pv_pass_end(d, cb);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo* info) {
    DeviceData* d = device_for(cb);
    if (!d || !d->CmdEndRenderPass2) return;
    end_pass(cb);
    if (t_fxl_cb == cb) t_fxl_cb = VK_NULL_HANDLE;
    if (t_dec_cb == cb) t_dec_cb = VK_NULL_HANDLE;
    if (t_void_cb == cb) t_void_cb = VK_NULL_HANDLE;
    d->CmdEndRenderPass2(cb, info);
    if (t_fx_cb == cb) fx_end(d, cb);
    ground_end(d, cb);
    shade_end(d, cb);
    surf_pass_end(d, cb);
    pv_pass_end(d, cb);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
    DeviceData* d = device_for(cb);
    if (!d || !d->CmdBeginRendering) return;
    t_void_cb = VK_NULL_HANDLE;
    t_tav_cb = VK_NULL_HANDLE;
    if (g_tav_hide && g_img && g_role.depth && info->pDepthAttachment && info->pDepthAttachment->imageView) {
        pthread_mutex_lock(&g_trace_lock);
        if (view_image((uint64_t)info->pDepthAttachment->imageView) == g_role.depth) t_tav_cb = cb;
        pthread_mutex_unlock(&g_trace_lock);
    }
    if (g_cc.on && g_cc.void_ && g_img) {
        uint64_t imgs[9]; uint32_t n = 0;
        for (uint32_t k = 0; k < info->colorAttachmentCount && n < 8; k++) imgs[n++] = view_image((uint64_t)info->pColorAttachments[k].imageView);
        if (info->pDepthAttachment && info->pDepthAttachment->imageView) imgs[n++] = view_image((uint64_t)info->pDepthAttachment->imageView);
        pthread_mutex_lock(&g_trace_lock);
        void_pass(cb, n, imgs);
        pthread_mutex_unlock(&g_trace_lock);
    }
    if (g_fxw_frames > 0 && g_img) {
        uint64_t imgs[9]; uint8_t load[9]; uint32_t n = 0;
        for (uint32_t k = 0; k < info->colorAttachmentCount && n < 8; k++) {
            load[n] = (uint8_t)info->pColorAttachments[k].loadOp;
            imgs[n++] = view_image((uint64_t)info->pColorAttachments[k].imageView);
        }
        if (info->pDepthAttachment && info->pDepthAttachment->imageView) {
            load[n] = (uint8_t)info->pDepthAttachment->loadOp;
            imgs[n++] = view_image((uint64_t)info->pDepthAttachment->imageView);
        }
        pthread_mutex_lock(&g_trace_lock);
        fxw_pass(cb, "DR", n, imgs, load);
        pthread_mutex_unlock(&g_trace_lock);
    }
    CbLog* c = trace_cb(cb);
    if (c) {
        cb_flush_pass(c);
        int n = snprintf(c->pass, sizeof c->pass, "DR %ux%u", info->renderArea.extent.width, info->renderArea.extent.height);
        for (uint32_t k = 0; k < info->colorAttachmentCount && k < 6; k++) {
            char s[96];
            img_desc(s, sizeof s, view_image((uint64_t)info->pColorAttachments[k].imageView));
            n += snprintf(c->pass + n, sizeof c->pass - n, " [%s L%d S%d]", s, (int)info->pColorAttachments[k].loadOp,
                          (int)info->pColorAttachments[k].storeOp);
        }
        if (info->pDepthAttachment && info->pDepthAttachment->imageView) {
            char s[96];
            img_desc(s, sizeof s, view_image((uint64_t)info->pDepthAttachment->imageView));
            n += snprintf(c->pass + n, sizeof c->pass - n, " D[%s L%d S%d]", s, (int)info->pDepthAttachment->loadOp,
                          (int)info->pDepthAttachment->storeOp);
        }
        c->in_pass = 1;
        pthread_mutex_unlock(&g_trace_lock);
    }
    d->CmdBeginRendering(cb, info);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdBeginRenderingKHR(VkCommandBuffer cb, const VkRenderingInfo* info) {
    DeviceData* d = device_for(cb);
    if (d && !d->CmdBeginRendering) d->CmdBeginRendering = (PFN_vkCmdBeginRendering)d->gdpa(d->device, "vkCmdBeginRenderingKHR");
    layer_CmdBeginRendering(cb, info);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdEndRendering(VkCommandBuffer cb) {
    DeviceData* d = device_for(cb);
    if (!d || !d->CmdEndRendering) return;
    end_pass(cb);
    if (t_void_cb == cb) t_void_cb = VK_NULL_HANDLE;
    d->CmdEndRendering(cb);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdEndRenderingKHR(VkCommandBuffer cb) {
    DeviceData* d = device_for(cb);
    if (d && !d->CmdEndRendering) d->CmdEndRendering = (PFN_vkCmdEndRendering)d->gdpa(d->device, "vkCmdEndRenderingKHR");
    layer_CmdEndRendering(cb);
}
// A draw of BG3's final pass (its colour attachment is a swapchain image): note the HUD it samples,
// the swapchain-sized RGBA8/BGRA8 colour target among its bound images (the other is BG3's scene,
// A2B10G10R10), with the layout its descriptor gives. role_submit makes it the main UI.
static void role_final_draw(VkCommandBuffer cb) {
    if (t_bind_cb != cb || !g_settab || !g_img) return;
    uint64_t views[16]; int32_t lays[16]; uint32_t nv = 0;
    pthread_mutex_lock(&g_fxl_lock);
    for (int s = 0; s < 8; s++) {
        SetInfo* si = t_set[s] ? set_find(t_set[s]) : NULL;
        for (uint32_t b = 0; si && b < DESC_SLOTS && nv < 16; b++) {
            const DescSlot* sl = &si->slot[b];
            if (sl->res && (sl->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || sl->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                            sl->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) { views[nv] = sl->res; lays[nv] = sl->layout; nv++; }
        }
    }
    pthread_mutex_unlock(&g_fxl_lock);
    pthread_mutex_lock(&g_trace_lock);
    uint64_t pick = 0; int32_t pl = -1; int best = 0;
    for (uint32_t k = 0; k < nv; k++) {
        uint64_t img = view_image(views[k]);
        ImgInfo* i = img ? TABLE_FIND(g_img, IMG_CAP, img, 0) : NULL;
        if (!i || !is_ui_color(i->fmt) || i->w != g_role.ew || i->hgt != g_role.eh || !(i->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            continue;
        int score = 1;
        for (uint32_t h = 0; h < g_role.nhud; h++) if (g_role.hud[h] == img) score = 2;
        if (score > best) { best = score; pick = img; pl = lays[k]; }
    }
    if (pick) {
        CbRole* r = TABLE_FIND(g_cbrole, CBROLE_CAP, (uint64_t)(uintptr_t)cb, 1);
        if (r) {
            if (!r->init) role_cb_clear(r);
            r->fin_ui = pick; r->fin_ul = pl;
        }
    }
    pthread_mutex_unlock(&g_trace_lock);
}
#define COUNT_DRAW(cb, prims_expr) do { if (t_ui_cb == (cb)) t_ui_draws++; if (t_fx_cb == (cb)) t_fx_draws++; if (t_sc_cb == (cb)) role_final_draw(cb); \
    if (g_trace_armed) { CbLog* _c = trace_cb(cb); \
    if (_c) { _c->draws++; _c->prims += (prims_expr); trace_draw_reads(_c, (cb)); pthread_mutex_unlock(&g_trace_lock); } } } while (0)
static VKAPI_ATTR void VKAPI_CALL layer_CmdDraw(VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    t_fx_surf = 0;  // surfaces are indexed draws; do not leave the last one's answer standing
    fxl_draw(cb, "D", v, i, fv, 0, fi);
    int why = fxc_draw(cb, 0, v, i, fv, 0, fi);
    if (g_fxw_frames > 0) fxw_draw(cb, 'D', v, i, fv, 0, fi, why);
    fxp_draw(cb);
    pv_draw(cb);
    fog_draw(cb);
    if (g_cc.wdexprd) wdx_readers(cb, 0);
    COUNT_DRAW(cb, (uint64_t)v * i);
    d->CmdDraw(cb, v, i, fv, fi);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    t_ib_cb = cb;
    t_ib = (uint64_t)b;
    t_ibo = o;
    t_ibt = (uint32_t)t;
    d->CmdBindIndexBuffer(cb, b, o, t);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t fi,
                                                       int32_t vo, uint32_t fin) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (t_tavd_cb == cb && t_ib_cb == cb) tavd_note(cb, t_ib, fin);
    t_fxc_glow = 0;
    late_log_draw(cb);
    if (tav_hidden_draw(cb) && !fxc_figure_extra(cb) && !(g_cc.fxdrop && t_fxl_cb == cb)) {
        if (g_fxw_frames > 0) fxw_draw(cb, 'X', n, i, fi, vo, fin, 'H');
        return;
    }
    // BG3's ground surfaces stay drawn in the void (they lie on WoW's ground; wowbg3_surf.inc.c).
    int surf = g_cc.surf ? surf_draw(cb) : 0;
    if (g_cc.fxbg && surf == 2 && !g_cc.fxbgfwd) return;  // fx-route prototype: BG3's forward surface layer would blend over the fed WoW picture; surf carries surfaces
    // ... and the effects capture must not take them as well: a surface is an unblended mesh of the
    // effects pass like a spell's solid pieces (fxc_solid), and replaying one put a black quad the
    // size of the puddle over the ground Grease had just drawn properly.
    t_fx_surf = surf;
    if (void_skip(cb, fin, n, fi, vo)) {
        if (!surf) { if (g_fxw_frames > 0) fxw_draw(cb, 'X', n, i, fi, vo, fin, 'V'); return; }
        surf_kept();
    }
    fxl_draw(cb, "DI", n, i, fi, vo, fin);
    int why = fxc_draw(cb, 1, n, i, fi, vo, fin);
    if (!why && t_prop_cb == cb) why = 'Y';
    if (g_fxw_frames > 0) fxw_draw(cb, 'X', n, i, fi, vo, fin, why);
    COUNT_DRAW(cb, (uint64_t)n * i);
    if (g_cc.wdexprd) wdx_readers(cb, 0);
    d->CmdDrawIndexed(cb, n, i, fi, vo, fin);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (g_fxw_frames > 0) fxw_draw(cb, 'd', n, 0, 0, 0, 0, 'I');
    COUNT_DRAW(cb, 0);
    d->CmdDrawIndirect(cb, b, o, n, s);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (t_tavd_cb == cb && t_ib_cb == cb) tavd_note(cb, t_ib, 0);
    if (tav_hidden_draw(cb)) { if (g_fxw_frames > 0) fxw_draw(cb, 'x', n, 0, 0, 0, 0, 'H'); return; }
    if (g_fxw_frames > 0) fxw_draw(cb, 'x', n, 0, 0, 0, 0, 'I');
    COUNT_DRAW(cb, 0);
    d->CmdDrawIndexedIndirect(cb, b, o, n, s);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (g_trace_armed) { CbLog* c = trace_cb(cb); if (c) { c->dispatches++; trace_dispatch_reads(c, cb, x, y, z); pthread_mutex_unlock(&g_trace_lock); } }
    fxp_dispatch(cb);
    if (wd_skip_cs(cb)) return;
    if (g_cc.wdeyelog) wd_eye_log(cb);
    wd_light_log(cb);
    wdx_dispatch_pre(cb);
    if (g_cc.wdexprd) wdx_readers(cb, 1);
    d->CmdDispatch(cb, x, y, z);
    wdx_dispatch_after(d, cb);
    if (g_cc.wdeyelog) wd_eye_after(d, cb);
    if (g_cc.wdpostcas) { post_dt(d, cb); post_cas(d, cb); }  // after BG3's display transform; after its CAS where that runs
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) {
    DeviceData* d = device_for(cb);
    if (!d || !d->CmdDispatchIndirect) return;
    if (g_trace_armed) { CbLog* c = trace_cb(cb); if (c) { c->dispatches++; trace_dispatch_reads(c, cb, 0, 0, 0); pthread_mutex_unlock(&g_trace_lock); } }
    wd_light_ind(cb, o);
    if (g_cc.wdexprd) wdx_readers(cb, 1);
    d->CmdDispatchIndirect(cb, b, o);
}
static void log_transfer(VkCommandBuffer cb, const char* what, uint64_t src, uint64_t dst) {
    CbLog* c = trace_cb(cb);
    if (!c) return;
    char a[96], b[96];
    img_desc(a, sizeof a, src);
    img_desc(b, sizeof b, dst);
    if (!c->in_pass && (c->draws || c->dispatches)) cb_flush_pass(c);
    cb_printf(c, "  %s %s -> %s\n", what, a, b);
    pthread_mutex_unlock(&g_trace_lock);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage t,
                                                     VkImageLayout tl, uint32_t n, const VkImageCopy* r) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (g_trace_armed) log_transfer(cb, "COPY", (uint64_t)s, (uint64_t)t);
    fxc_copy((uint64_t)s, (uint64_t)t);
    fb_copy_hook(d, cb, (uint64_t)s, (uint64_t)t, sl);
    d->CmdCopyImage(cb, s, sl, t, tl, n, r);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage t,
                                                     VkImageLayout tl, uint32_t n, const VkImageBlit* r, VkFilter f) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (g_trace_armed) log_transfer(cb, "BLIT", (uint64_t)s, (uint64_t)t);
    d->CmdBlitImage(cb, s, sl, t, tl, n, r, f);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage t,
                                                        VkImageLayout tl, uint32_t n, const VkImageResolve* r) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (g_trace_armed) log_transfer(cb, "RESOLVE", (uint64_t)s, (uint64_t)t);
    d->CmdResolveImage(cb, s, sl, t, tl, n, r);
}
static int is_depth_format(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier* mb, uint32_t nb, const VkBufferMemoryBarrier* bb,
    uint32_t ni, const VkImageMemoryBarrier* ib) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    if (g_cc.surf) for (uint32_t k = 0; k < ni; k++) surf_note_barrier(cb, (uint64_t)ib[k].image, (int32_t)ib[k].newLayout);
    if (g_cc.on && ni && (g_role.depth || g_role.ui)) {
        for (uint32_t k = 0; k < ni; k++) {
            uint64_t im = (uint64_t)ib[k].image;
            if (im && (im == g_role.depth || im == g_role.ui)) {
                pthread_mutex_lock(&g_trace_lock);
                role_set_layout(cb, im, (int)ib[k].newLayout);
                pthread_mutex_unlock(&g_trace_lock);
            }
        }
    }
    if (g_trace_armed && ni) {
        CbLog* c = trace_cb(cb);
        if (c) {
            for (uint32_t k = 0; k < ni; k++) {
                ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, (uint64_t)ib[k].image, 0);
                if (!i || !(is_depth_format(i->fmt) || (i->w >= 1280 && i->hgt >= 720 && i->w <= 4096))) continue;
                char t[96];
                img_desc(t, sizeof t, (uint64_t)ib[k].image);
                cb_printf(c, "  BARRIER %s layout %d -> %d\n", t, (int)ib[k].oldLayout, (int)ib[k].newLayout);
            }
            pthread_mutex_unlock(&g_trace_lock);
        }
    }
    d->CmdPipelineBarrier(cb, src, dst, dep, nm, mb, nb, bb, ni, ib);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* info) {
    DeviceData* d = device_for(cb);
    if (!d || !d->CmdPipelineBarrier2) return;
    if (g_cc.surf)
        for (uint32_t k = 0; k < info->imageMemoryBarrierCount; k++)
            surf_note_barrier(cb, (uint64_t)info->pImageMemoryBarriers[k].image, (int32_t)info->pImageMemoryBarriers[k].newLayout);
    if (g_cc.on && info->imageMemoryBarrierCount && (g_role.depth || g_role.ui)) {
        for (uint32_t k = 0; k < info->imageMemoryBarrierCount; k++) {
            uint64_t im = (uint64_t)info->pImageMemoryBarriers[k].image;
            if (im && (im == g_role.depth || im == g_role.ui)) {
                pthread_mutex_lock(&g_trace_lock);
                role_set_layout(cb, im, (int)info->pImageMemoryBarriers[k].newLayout);
                pthread_mutex_unlock(&g_trace_lock);
            }
        }
    }
    if (g_trace_armed && info->imageMemoryBarrierCount) {
        CbLog* c = trace_cb(cb);
        if (c) {
            for (uint32_t k = 0; k < info->imageMemoryBarrierCount; k++) {
                const VkImageMemoryBarrier2* b = &info->pImageMemoryBarriers[k];
                ImgInfo* i = TABLE_FIND(g_img, IMG_CAP, (uint64_t)b->image, 0);
                if (!i || !(is_depth_format(i->fmt) || (i->w >= 1280 && i->hgt >= 720 && i->w <= 4096))) continue;
                char t[96];
                img_desc(t, sizeof t, (uint64_t)b->image);
                cb_printf(c, "  BARRIER2 %s layout %d -> %d\n", t, (int)b->oldLayout, (int)b->newLayout);
            }
            pthread_mutex_unlock(&g_trace_lock);
        }
    }
    d->CmdPipelineBarrier2(cb, info);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdPipelineBarrier2KHR(VkCommandBuffer cb, const VkDependencyInfo* info) {
    DeviceData* d = device_for(cb);
    if (d && !d->CmdPipelineBarrier2) d->CmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)d->gdpa(d->device, "vkCmdPipelineBarrier2KHR");
    layer_CmdPipelineBarrier2(cb, info);
}
static VKAPI_ATTR void VKAPI_CALL layer_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* cbs) {
    DeviceData* d = device_for(cb);
    if (!d) return;
    CbLog* c = g_trace_armed ? trace_cb(cb) : NULL;
    if (c) {
        cb_printf(c, "  EXEC %u secondary:\n", n);
        for (uint32_t k = 0; k < n; k++) {
            CbLog* s = cb_log(cbs[k], 0);
            if (s && s->len) cb_printf(c, "%.*s", (int)(s->len > 900 ? 900 : s->len), s->buf);
            if (s) cb_flush_pass(s);
            if (s && s->len > 900) cb_printf(c, "    (secondary log cut)\n");
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    if (t_tavd_cb == cb) {
        pthread_mutex_lock(&g_trace_lock);
        for (uint32_t k = 0; k < n; k++) {
            TavdCb* e = TABLE_FIND(g_tavdcb, TAVDCB_CAP, (uint64_t)(uintptr_t)cbs[k], 0);
            if (e && e->valid == 1 && e->seq == g_tavd_seq[t_tavd_tab])
                for (int w = 0; w < TAVD_W; w++) t_tavd_bits[w] |= e->bits[w];
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    d->CmdExecuteCommands(cb, n, cbs);
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_EndCommandBuffer(VkCommandBuffer cb) {
    DeviceData* d = device_for(cb);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    surf_end_cb(d, cb);  // the surfaces' albedo pass, owed by BG3's G-buffer command buffer
    tavd_end(cb);
    if (g_trace_armed) {
        CbLog* c = trace_cb(cb);
        if (c) { cb_flush_pass(c); pthread_mutex_unlock(&g_trace_lock); }
    }
    return d->EndCommandBuffer(cb);
}
static void trace_submit_cbs(uint32_t n, const VkCommandBuffer* cbs) {
    for (uint32_t k = 0; k < n; k++) {
        CbLog* c = cb_log(cbs[k], 0);
        fprintf(g_trace_out, " cb %llx%s\n", (unsigned long long)(uintptr_t)cbs[k], c && c->len ? "" : " (not traced)");
        if (c && c->len) { fwrite(c->buf, 1, c->len, g_trace_out); c->len = 0; c->buf[0] = 0; }
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
    QueueData* q = queue_data(queue);
    DeviceData* d = q ? q->dev : device_for(queue);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (g_cc.on && g_img) {
        uint64_t t = now_ns();
        trace_lock_timed();
        for (uint32_t s = 0; s < count; s++) role_submit(infos[s].commandBufferCount, infos[s].pCommandBuffers);
        pthread_mutex_unlock(&g_trace_lock);
        perf_add(&g_perf.submit, now_ns() - t);
    }
    if (g_trace_armed && g_trace_out) {
        pthread_mutex_lock(&g_trace_lock);
        for (uint32_t s = 0; s < count; s++) {
            fprintf(g_trace_out, "SUBMIT %llu queue %llx family %u: %u cbs, waits %u, signals %u\n",
                    (unsigned long long)++g_trace_submits, (unsigned long long)(uintptr_t)queue, q ? q->family : 99,
                    infos[s].commandBufferCount, infos[s].waitSemaphoreCount, infos[s].signalSemaphoreCount);
            trace_submit_cbs(infos[s].commandBufferCount, infos[s].pCommandBuffers);
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return d->QueueSubmit(queue, count, infos, fence);
}
static void wd_lock_submit(DeviceData* d, VkQueue queue, uint32_t family);
static VKAPI_ATTR VkResult VKAPI_CALL layer_QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence) {
    QueueData* q = queue_data(queue);
    DeviceData* d = q ? q->dev : device_for(queue);
    if (!d || !d->QueueSubmit2) return VK_ERROR_INITIALIZATION_FAILED;
    if (g_cc.on && g_cc.wdlock && q && q->family == 0) wd_lock_submit(d, queue, q->family);
    { static uint64_t lastp; extern void wd_eye_readback_(DeviceData*, VkQueue, uint32_t); uint64_t pr = __atomic_load_n(&g_req.presents, __ATOMIC_RELAXED);
      if (g_cc.on && g_cc.wdeyelog && q && q->family == 0 && pr != lastp) { lastp = pr; wd_eye_readback_(d, queue, q->family); } }
    if (g_cc.on && g_img) {
        uint64_t t = now_ns();
        trace_lock_timed();
        for (uint32_t s = 0; s < count; s++)
            for (uint32_t k = 0; k < infos[s].commandBufferInfoCount; k++) {
                VkCommandBuffer one = infos[s].pCommandBufferInfos[k].commandBuffer;
                role_submit(1, &one);
            }
        pthread_mutex_unlock(&g_trace_lock);
        perf_add(&g_perf.submit, now_ns() - t);
    }
    if (g_trace_armed && g_trace_out) {
        pthread_mutex_lock(&g_trace_lock);
        for (uint32_t s = 0; s < count; s++) {
            fprintf(g_trace_out, "SUBMIT2 %llu queue %llx family %u: %u cbs\n", (unsigned long long)++g_trace_submits,
                    (unsigned long long)(uintptr_t)queue, q ? q->family : 99, infos[s].commandBufferInfoCount);
            for (uint32_t k = 0; k < infos[s].commandBufferInfoCount; k++) {
                VkCommandBuffer one = infos[s].pCommandBufferInfos[k].commandBuffer;
                trace_submit_cbs(1, &one);
            }
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
    return d->QueueSubmit2(queue, count, infos, fence);
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence) {
    QueueData* q = queue_data(queue);
    DeviceData* d = q ? q->dev : device_for(queue);
    if (d && !d->QueueSubmit2) d->QueueSubmit2 = (PFN_vkQueueSubmit2)d->gdpa(d->device, "vkQueueSubmit2KHR");
    return layer_QueueSubmit2(queue, count, infos, fence);
}

// Called on every present (g_lock held): arms and ends traces.
static void trace_present(SwapchainData* c, uint32_t index) {
    if (g_trace_armed && g_trace_out) {
        char s[96];
        pthread_mutex_lock(&g_trace_lock);
        img_desc(s, sizeof s, c ? (uint64_t)c->images[index] : 0);
        fprintf(g_trace_out, "PRESENT swapchain image %u %s (format %d)\n", index, s, c ? (int)c->format : -1);
        if (--g_trace_armed == 0) {
            fclose(g_trace_out);
            g_trace_out = NULL;
            logf_("trace done");
        }
        pthread_mutex_unlock(&g_trace_lock);
    }
}
static void trace_arm(const char* tag) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s.txt", out_dir(), tag);
    pthread_mutex_lock(&g_trace_lock);
    if (!g_trace_out && trace_tables()) {
        g_trace_out = fopen(path, "w");
        if (g_trace_out) { g_trace_submits = 0; g_trace_armed = TRACE_PRESENTS; logf_("trace armed: %s", path); }
    }
    pthread_mutex_unlock(&g_trace_lock);
}

// ---------------------------------------------------------------- Tav geometry copy
// Goal: Tav is drawn inside WoW, with WoW's light and fog, the way SkyCraft
// copies its geometry into the other engine. WorldMode.lua (TavPlan.lua) writes what Tav is made
// of to wow-bg3/link/tavplan.txt; here, inside BG3's process, the meshes are copied off the GPU
// once per plan (tavmesh.bin) and the bone palettes and world transforms every present
// (tavpose.bin), both into $WOWBG3_TAV_DIR (default /wowtav, a tmpfs shared with benilla).
// Files are replaced whole (write a temporary, rename), so readers never see a torn one.
//   tavmesh.bin: "TVM1", version 1, plan seq, mesh count (u32), Centre xyz, WowOrigin xyz (f32);
//     per mesh 64 bytes: key u64; kind, part, vcount, icount, isize, vsize, uvoff, src stride,
//     src bytes, skin bytes, vb bytes, ib bytes (u32); figure u32 (0 the hero, 1... her
//     companions, TavPlan.lua's M lines; 0 in files before the party); lod u32 (the M
//     line's LOD, 0 the finest; 0 before); then the four blobs in that order.
//   tavpose.bin: "TVP1", plan seq, frame, mesh count (u32), wall time f64; per mesh: key u64,
//     bone count u32, flags u32 (1: read; 2 BG3 drew it in this frame's G-buffer pass, 4 draw
//     tracking valid this frame, on every record; 8 BG3 draws it itself, patch27), world transform 10 f32 (quat xyzw, translate, scale),
//     then bone count x 12 f32 (3x4 rows).
// Memory is read with process_vm_readv on our own process: a pointer gone stale since the plan
// fails the read instead of faulting.
#define TAV_MAX_BONES 512
typedef struct {
    uint64_t key, src, skin, vb, ib, mb, wt;
    uint32_t kind, part, vcount, icount, isize, vsize, uvoff, figure, lod;
} TavMesh;
static struct {
    int init, off;
    char plan[512], dir[256];
    struct timespec seen;
    uint32_t seq, frame;
    int n, mesh_ok, nlod, figures;
    uint64_t lod_ib[TAV_MAX];  // plan "H" lines: coarser LODs, hidden in BG3 (copied too when they also have M lines)
    uint32_t dslot[TAV_MAX];   // each mesh's slot in the drawn sets
    uint64_t tavd_valid, tavd_frames, tavd_drawn0, tavd_drawn1;  // for the log
    uint64_t hide_key;         // what g_tav_ib holds (tav_publish_hide)
    uint64_t retry_at;
    struct timespec born;  // plans older than this BG3 run are a previous session's
    float centre[3], wow[3];
    TavMesh m[TAV_MAX];
    uint8_t* pose;
    size_t pose_cap;
    uint64_t last_check;
} g_tav;

static void tav_publish_hide(void) {
    int on = g_cc.on && !g_cc.bg3tav && g_tav.mesh_ok && g_tav.n > 0;
    // The set changes with the plan (its number and LOD count) and the switch (rebuilt only then).
    uint64_t key = on ? ((uint64_t)g_tav.seq << 32 | (uint64_t)(uint32_t)g_tav.nlod << 1 | 1u) : 0;
    if (key != g_tav.hide_key) {
        g_tav.hide_key = key;
        uint64_t next[TAV_HIDE_CAP];
        memset(next, 0, sizeof next);
        int lists = 0;
        for (int i = 0; on && i < g_tav.n + g_tav.nlod; i++) {
            uint64_t ib = i < g_tav.n ? g_tav.m[i].ib : g_tav.lod_ib[i - g_tav.n];
            if (!ib) continue;
            for (uint32_t h = tav_hash(ib), k = 0; k < TAV_HIDE_CAP; k++, h = (h + 1) & (TAV_HIDE_CAP - 1)) {
                if (next[h] == ib) break;
                if (!next[h]) { next[h] = ib; lists++; break; }
            }
        }
        for (int i = 0; i < TAV_HIDE_CAP; i++) g_tav_ib[i] = next[i];
        if (on) logf_("tav: %d index buffers of %d figure(s) hidden in BG3's own frame", lists, g_tav.figures);
    }
    if (on != g_tav_hide) logf_("tav: BG3's own Tav %s", on ? "hidden" : "drawn");
    if (on && g_tav.frame % 900 == 1) logf_("tav: pose %u, %llu of BG3's Tav draws dropped so far", g_tav.frame, (unsigned long long)g_tav_dropped);
    g_tav_hide = on;
}

static float g_tav_root[3];  // Tav's position in BG3's world (metres), from the last pose
static int g_tav_root_ok;

static int tav_rd(uint64_t addr, void* out, size_t n) {
    if (!addr) return 0;
    struct iovec l = {out, n}, r = {(void*)(uintptr_t)addr, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
}

static int tav_write(const char* name, const void* a, size_t na) {
    char tmp[300], path[300];
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", g_tav.dir, name);
    snprintf(path, sizeof path, "%s/%s", g_tav.dir, name);
    FILE* f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(a, 1, na, f) == na;
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp, path) == 0;
}

static void tav_read_plan(void) {
    struct stat st;
    if (stat(g_tav.plan, &st) != 0) return;
    if (st.st_mtim.tv_sec == g_tav.seen.tv_sec && st.st_mtim.tv_nsec == g_tav.seen.tv_nsec) return;
    g_tav.seen = st.st_mtim;
    if (st.st_mtim.tv_sec < g_tav.born.tv_sec) return;
    FILE* f = fopen(g_tav.plan, "r");
    if (!f) return;
    char line[512];
    unsigned seq = 0;
    float c[3] = {0}, w[3] = {0};
    int n = 0, head = 0, nlod = 0;
    TavMesh m[TAV_MAX];
    uint64_t lod[TAV_MAX];
    int figs = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "TAVPLAN 1 ", 10)) {
            head = sscanf(line + 10, "%u %f %f %f %f %f %f", &seq, &c[0], &c[1], &c[2], &w[0], &w[1], &w[2]) >= 1;
        } else if (line[0] == 'H' && nlod < TAV_MAX) {
            unsigned long long ib;
            if (sscanf(line + 1, "%llx", &ib) == 1 && ib) lod[nlod++] = ib;
        } else if (line[0] == 'M' && n < TAV_MAX) {
            TavMesh* t = &m[n];
            unsigned long long key, src, skin, vb, ib, mb, wt;
            // The material, then the figure: 0 the hero, 1... her companions; absent before the party.
            t->figure = 0;
            t->lod = 0;  // The LOD, 0 when absent
            if (sscanf(line + 1, "%llx %u %u %u %u %u %u %u %llx %llx %llx %llx %llx %llx %*s %u %u", &key, &t->kind, &t->part,
                       &t->vcount, &t->icount, &t->isize, &t->vsize, &t->uvoff, &src, &skin, &vb, &ib, &mb, &wt, &t->figure,
                       &t->lod) >= 14 &&
                t->lod < 16 &&
                t->figure < 64 &&
                t->vcount > 0 && t->vcount < (1u << 22) && t->icount > 0 && t->icount < (1u << 24) &&
                (t->isize == 2 || t->isize == 4) && t->vsize >= 4 && t->vsize <= 64 && t->uvoff + 4 <= t->vsize &&
                t->kind <= 1) {
                t->key = key; t->src = src; t->skin = skin; t->vb = vb; t->ib = ib; t->mb = mb; t->wt = wt;
                if ((int)t->figure + 1 > figs) figs = (int)t->figure + 1;
                n++;
            }
        }
    }
    fclose(f);
    if (!head) return;
    if (!seq) n = nlod = 0;
    g_tav.nlod = nlod;
    memcpy(g_tav.lod_ib, lod, sizeof(uint64_t) * (size_t)nlod);
    if (seq == g_tav.seq && n == g_tav.n && !memcmp(m, g_tav.m, sizeof(TavMesh) * (size_t)n)) return;
    g_tav.seq = seq;
    g_tav.n = n;
    g_tav.figures = figs;
    g_tav.mesh_ok = 0;
    g_tav.retry_at = 0;
    memcpy(g_tav.m, m, sizeof(TavMesh) * (size_t)n);
    memcpy(g_tav.centre, c, sizeof c);
    memcpy(g_tav.wow, w, sizeof w);
    // The drawn-set table for this plan (a slot per planned mesh): built aside, then
    // published. Each index buffer lists the planned meshes drawn from it.
    {
        int k = g_tavd_cur < 0 ? 0 : 1 - g_tavd_cur;
        memset(g_tavd_tab[k], 0, sizeof g_tavd_tab[k]);
        uint8_t listed[TAV_MAX];
        memset(listed, 0, sizeof listed);
        uint32_t used = 0;
        for (int i = 0; i < g_tav.n; i++) {
            uint64_t ib = g_tav.m[i].ib;
            g_tav.dslot[i] = (uint32_t)i;
            if (!ib || listed[i]) continue;
            uint32_t first = used;
            for (int j = i; j < g_tav.n; j++)
                if (!listed[j] && g_tav.m[j].ib == ib) { listed[j] = 1; g_tavd_list[k][used++] = (uint16_t)j; }
            for (uint32_t h = tav_hash(ib) & (TAVD_CAP - 1), q = 0; q < TAVD_CAP; q++, h = (h + 1) & (TAVD_CAP - 1)) {
                if (!g_tavd_tab[k][h].ib) { g_tavd_tab[k][h].first = first; g_tavd_tab[k][h].n = used - first; g_tavd_tab[k][h].ib = ib; break; }
            }
        }
        g_tavd_seq[k] = seq;
        __atomic_store_n(&g_tavd_cur, k, __ATOMIC_RELEASE);
    }
    int lods = 0;
    for (int i = 0; i < g_tav.n; i++) lods += g_tav.m[i].lod > 0;
    logf_("tav: plan %u, %d meshes (%d of a coarser LOD) of %d figure(s), %d coarser LODs hidden", seq, g_tav.n, lods, figs, g_tav.nlod);
}

// Sizes of a mesh's four blobs.
static void tav_sizes(const TavMesh* t, uint64_t s[4]) {
    s[0] = (uint64_t)t->vcount * (t->kind ? t->vsize : 20);
    s[1] = t->kind ? 0 : (uint64_t)t->vcount * 8;
    s[2] = t->kind ? 0 : (uint64_t)t->vcount * t->vsize;  // rigid: src is the vertex buffer
    s[3] = (uint64_t)t->icount * t->isize;
}

// Meshes already copied: TavPlan.lua's plan changes often (48 times in 8.5 minutes of
// a harness soak, 25 and 27 meshes in turn), and every copy waited for BG3's GPU frame on its render
// thread. A mesh whose buffers are the same live buffers as when it was copied (handle, size and
// creation number, BufInfo.gen) comes from here, so a plan seen before costs no GPU work.
#define TAVC_N 512
typedef struct { uint64_t key, h[4], gen[4], s[4], bytes, used; uint8_t* data; } TavCache;
static TavCache g_tavc[TAVC_N];
static uint64_t g_tavc_clock;
// The cached blobs of mesh `t` (sizes `s`), or NULL. g_trace_lock held (the buffer table).
static TavCache* tavc_find(const TavMesh* t, const uint64_t s[4]) {
    uint64_t h[4] = {t->src, t->skin, t->vb, t->ib};
    for (int c = 0; c < TAVC_N; c++) {
        TavCache* e = &g_tavc[c];
        if (!e->data || e->key != t->key) continue;
        int same = 1;
        for (int k = 0; k < 4 && same; k++) {
            if (e->s[k] != s[k] || (s[k] && e->h[k] != h[k])) { same = 0; break; }
            if (!s[k]) continue;
            BufInfo* b = TABLE_FIND(g_buf, BUF_CAP, h[k], 0);
            same = b && b->size >= s[k] && b->gen == e->gen[k];
        }
        if (same) { e->used = ++g_tavc_clock; return e; }
    }
    return NULL;
}
static void tavc_put(const TavMesh* t, const uint64_t s[4], const uint64_t gen[4], const uint8_t* data, uint64_t bytes) {
    TavCache* e = NULL;  // the same mesh's old entry, else a free one, else the least recently used
    for (int c = 0; c < TAVC_N && !e; c++)
        if (g_tavc[c].data && g_tavc[c].key == t->key && g_tavc[c].h[0] == t->src && g_tavc[c].h[1] == t->skin &&
            g_tavc[c].h[2] == t->vb && g_tavc[c].h[3] == t->ib) e = &g_tavc[c];
    for (int c = 0; c < TAVC_N && !e; c++) if (!g_tavc[c].data) e = &g_tavc[c];
    if (!e) {
        e = &g_tavc[0];
        for (int c = 1; c < TAVC_N; c++) if (g_tavc[c].used < e->used) e = &g_tavc[c];
    }
    uint8_t* copy = malloc(bytes);
    if (!copy) return;
    memcpy(copy, data, bytes);
    free(e->data);
    *e = (TavCache){t->key, {t->src, t->skin, t->vb, t->ib}, {gen[0], gen[1], gen[2], gen[3]}, {s[0], s[1], s[2], s[3]}, bytes,
                    ++g_tavc_clock, copy};
}

// Copies every mesh of the plan off the GPU (or from the cache) and writes tavmesh.bin (g_lock held).
static void tav_copy_meshes(QueueData* q) {
    DeviceData* d = q->dev;
    g_tav.retry_at = g_req.presents + 90;  // after a failure (a buffer being replaced), try again
    if (!d->CmdCopyBuffer || !g_buf) return;
    uint64_t blobs = 0;
    for (int i = 0; i < g_tav.n; i++) {
        uint64_t s[4];
        tav_sizes(&g_tav.m[i], s);
        blobs += s[0] + s[1] + s[2] + s[3];
    }
    if (!blobs || blobs > (256u << 20)) return;
    size_t head = 40 + 64 * (size_t)g_tav.n;
    uint8_t* out = malloc(head + blobs);
    if (!out) return;
    // A plan seen before: every mesh from the cache, no GPU work.
    int cached = 1;
    uint64_t at = 0;
    pthread_mutex_lock(&g_trace_lock);
    for (int i = 0; i < g_tav.n && cached; i++) {
        uint64_t s[4];
        tav_sizes(&g_tav.m[i], s);
        TavCache* e = tavc_find(&g_tav.m[i], s);
        if (e) { memcpy(out + head + at, e->data, e->bytes); at += e->bytes; }
        else cached = 0;
    }
    pthread_mutex_unlock(&g_trace_lock);
    uint64_t gen[TAV_MAX][4];
    if (!cached) {
        Staging* st = staging_for(d, q->family, blobs);
        if (!st) { free(out); return; }
        // Under the trace lock, so no buffer is destroyed between the check and the finished copy.
        pthread_mutex_lock(&g_trace_lock);
        VkBufferCopy rg[4];
        at = 0;
        int bad = 0;
        d->ResetCommandBuffer(st->cmd, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        d->BeginCommandBuffer(st->cmd, &bi);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        d->CmdPipelineBarrier(st->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        for (int i = 0; i < g_tav.n && !bad; i++) {
            TavMesh* t = &g_tav.m[i];
            uint64_t s[4], h[4] = {t->src, t->skin, t->vb, t->ib};
            tav_sizes(t, s);
            for (int k = 0; k < 4; k++) {
                gen[i][k] = 0;
                if (!s[k]) continue;
                BufInfo* b = TABLE_FIND(g_buf, BUF_CAP, h[k], 0);
                if (!b || !b->size || b->size < s[k]) {
                    logf_("tav: mesh %llx blob %d: buffer %llx not live or too small", (unsigned long long)t->key, k, (unsigned long long)h[k]);
                    bad = 1;
                    break;
                }
                gen[i][k] = b->gen;
                rg[0] = (VkBufferCopy){0, at, s[k]};
                d->CmdCopyBuffer(st->cmd, (VkBuffer)h[k], st->buffer, 1, rg);
                at += s[k];
            }
        }
        d->EndCommandBuffer(st->cmd);
        int ok = !bad;
        if (ok) {
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &st->cmd, 0, NULL};
            d->ResetFences(d->device, 1, &st->fence);
            ok = d->QueueSubmit(q->queue, 1, &si, st->fence) == VK_SUCCESS &&
                 d->WaitForFences(d->device, 1, &st->fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
        }
        pthread_mutex_unlock(&g_trace_lock);
        if (!ok) { logf_("tav: mesh copy failed"); free(out); return; }
        memcpy(out + head, st->mapped, blobs);
        at = 0;
        for (int i = 0; i < g_tav.n; i++) {
            uint64_t s[4];
            tav_sizes(&g_tav.m[i], s);
            uint64_t bytes = s[0] + s[1] + s[2] + s[3];
            tavc_put(&g_tav.m[i], s, gen[i], out + head + at, bytes);
            at += bytes;
        }
    }
    memset(out, 0, head);
    uint32_t h32[4] = {0x314d5654u /* TVM1 */, 1, g_tav.seq, (uint32_t)g_tav.n};
    memcpy(out, h32, 16);
    memcpy(out + 16, g_tav.centre, 12);
    memcpy(out + 28, g_tav.wow, 12);
    for (int i = 0; i < g_tav.n; i++) {
        TavMesh* t = &g_tav.m[i];
        uint64_t s[4];
        tav_sizes(t, s);
        uint8_t* p = out + 40 + 64 * i;
        uint32_t u[12] = {t->kind, t->part, t->vcount, t->icount, t->isize, t->vsize, t->uvoff, t->kind ? t->vsize : 20,
                          (uint32_t)s[0], (uint32_t)s[1], (uint32_t)s[2], (uint32_t)s[3]};
        memcpy(p, &t->key, 8);
        memcpy(p + 8, u, sizeof u);
        memcpy(p + 56, &t->figure, 4);
        memcpy(p + 60, &t->lod, 4);
    }
    int w = tav_write("tavmesh.bin", out, head + blobs);
    free(out);
    g_tav.mesh_ok = w;
    logf_("tav: meshes for plan %u written (%d meshes, %llu bytes%s): %s", g_tav.seq, g_tav.n, (unsigned long long)blobs,
          cached ? ", from the cache" : "", w ? "ok" : "failed");
}

// Tav's pose records (the part of tavpose.bin after its header) read from memory now, into `p`
// (room for g_tav.n meshes of TAV_MAX_BONES); returns their size. Also called on BG3's recording
// threads for the G-buffer pass's snapshot (wowbg3_pace.inc.c): reads only, through tav_rd.
static size_t tav_pose_records(uint8_t* p) {
    size_t at = 0;
    int n = g_tav.n < TAV_MAX ? g_tav.n : TAV_MAX;
    for (int i = 0; i < n; i++) {
        TavMesh* m = &g_tav.m[i];
        uint32_t bones = 0, flags = 0;
        float wt[10] = {0};
        uint8_t* rec = p + at;
        if (tav_rd(m->wt, wt, sizeof wt)) flags = 1;
        if (!g_tav_hide) flags |= 8;  // patch27: BG3 draws this figure itself (bg3tav): benilla 0122 hides its copy
        if (flags && m->kind == 0) {
            uint64_t pal = 0;
            uint16_t count = 0;
            if (tav_rd(m->mb + 16, &pal, 8) && tav_rd(m->mb + 24, &count, 2) && count > 0 && count <= TAV_MAX_BONES &&
                tav_rd(pal, rec + 56, (size_t)count * 48)) {
                bones = count;
            } else {
                flags = 0;
            }
        }
        memcpy(rec, &m->key, 8);
        memcpy(rec + 8, &bones, 4);
        memcpy(rec + 12, &flags, 4);
        memcpy(rec + 16, wt, sizeof wt);
        at += 56 + (size_t)bones * 48;
    }
    return at;
}

// The poses, every present (g_lock held). With frame pacing the records come from
// the presented frame's G-buffer pass when it took them (pace_pose_snapshot): read at the present,
// 1% of them already held the next BG3 tick (a 6 cm jump of Tav against the camera when walking).
static void tav_write_pose(void) {
    size_t need = 24 + (size_t)g_tav.n * (56 + TAV_MAX_BONES * 48) + PACE_TRAILER;
    if (g_tav.pose_cap < need) {
        free(g_tav.pose);
        g_tav.pose = malloc(need);
        g_tav.pose_cap = g_tav.pose ? need : 0;
        if (!g_tav.pose) return;
    }
    uint8_t* p = g_tav.pose;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    double t = (double)now.tv_sec + now.tv_nsec * 1e-9;
    uint32_t h32[4] = {0x31505654u /* TVP1 */, g_tav.seq, ++g_tav.frame, (uint32_t)g_tav.n};
    memcpy(p, h32, 16);
    memcpy(p + 16, &t, 8);
    size_t at = 24;
    size_t got = pace_pose_snapshot(p + at, need - at - PACE_TRAILER);
    at += got ? got : tav_pose_records(p + at);
    if (g_tav.n > 0) {  // Tav's root: the first mesh's object origin
        uint32_t flags;
        memcpy(&flags, p + 24 + 12, 4);
        if (flags & 1) { memcpy(g_tav_root, p + 24 + 16 + 16, 12); g_tav_root_ok = 1; }
    }
    // Which of them BG3 drew in this frame's G-buffer pass (flags 2 and 4).
    {
        uint64_t bits[TAVD_W];
        pthread_mutex_lock(&g_tavd_frame.m);
        int valid = g_tavd_frame.valid && g_tavd_frame.seq == g_tav.seq;
        memcpy(bits, g_tavd_frame.bits, sizeof bits);
        pthread_mutex_unlock(&g_tavd_frame.m);
        size_t r = 24;
        uint32_t d0 = 0, d1 = 0;
        for (int i = 0; i < g_tav.n && r + 56 <= at; i++) {
            uint32_t bones, flags;
            memcpy(&bones, p + r + 8, 4);
            memcpy(&flags, p + r + 12, 4);
            if (flags & 1) memcpy(g_tavd_pos[i], p + r + 32, 12);  // Where the shared draws are placed against
            if (valid) {
                uint32_t s = g_tav.dslot[i];
                flags |= 4;
                if (s < TAV_MAX && (bits[s >> 6] >> (s & 63) & 1)) { flags |= 2; if (g_tav.m[i].lod) d1++; else d0++; }
                memcpy(p + r + 12, &flags, 4);
            }
            r += 56 + (size_t)bones * 48;
        }
        g_tav.tavd_frames++;
        if (valid) { g_tav.tavd_valid++; g_tav.tavd_drawn0 += d0; g_tav.tavd_drawn1 += d1; }
        if (g_tav.frame % 900 == 1)
            logf_("tav: drawn tracking %llu of %llu frames, meshes BG3 drew per frame: %.1f of LOD 0, %.1f coarser (plan %u, %d meshes);"
                  " draws from shared index buffers: %llu placed, %llu marked for all",
                  (unsigned long long)g_tav.tavd_valid, (unsigned long long)g_tav.tavd_frames,
                  g_tav.tavd_valid ? (double)g_tav.tavd_drawn0 / g_tav.tavd_valid : 0.0,
                  g_tav.tavd_valid ? (double)g_tav.tavd_drawn1 / g_tav.tavd_valid : 0.0, g_tav.seq, g_tav.n,
                  (unsigned long long)g_tavd_placed, (unsigned long long)g_tavd_unplaced);
    }
    // The frame packet's camera (wowbg3_pace.inc.c): benilla draws this pose with this camera.
    pace_trailer(p + at, g_tav.frame);
    at += PACE_TRAILER;
    tav_write("tavpose.bin", p, at);
}

// At present (g_lock held).
static char g_screen_path[512];
// BG3's client game state (the launcher plugin's wow-bg3/bg3-state.json, rewritten the frame it changes):
// WoW's picture is composited only while it is Running. A playtest report: after "Load Game" on the
// game-over screen the last WoW frame (where he died) stayed on screen through the whole load, as the
// compositor kept painting it over BG3's loading screen until the coordinator's compose.off (written once
// the WoW side is down) was read at the next 15-present check. A load is StartLoading, UnloadLevel,
// LoadLevel... and the PrepareRunning hold after it: BG3's own loading screen then. 1 Running (or no
// file: as before), 0 anything else. Read at each present: one stat, and the file only when it changed.
static int g_bg3_running = 1;
static void bg3_state_check(void) {
    static char path[1024];
    static struct timespec seen;
    if (!path[0]) {
        snprintf(path, sizeof path, "%s", link_dir());
        char* slash = strrchr(path, '/');  // .../wow-bg3/link -> .../wow-bg3
        if (slash) *slash = 0;
        strncat(path, "/bg3-state.json", sizeof path - strlen(path) - 1);
    }
    struct stat st;
    if (stat(path, &st) != 0) { g_bg3_running = 1; return; }
    if (st.st_mtim.tv_sec == seen.tv_sec && st.st_mtim.tv_nsec == seen.tv_nsec) return;
    seen = st.st_mtim;
    char buf[512] = {0};
    FILE* f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char* c = strstr(buf, "\"Client\"");
    if (!c) return;  // a partial write: the next change tells
    const int running = strstr(c, "\"Running\"") == strchr(c + 8, '"');
    if (running != g_bg3_running) logf_("bg3 client %s: WoW's picture %s", running ? "running" : "loading or held", running ? "composited" : "not composited");
    if (!running && g_bg3_running) wr_forget();  // the world before the load is gone (wowbg3_compose_gpu.inc.c)
    g_bg3_running = running;
}
static void screen_check(void) {
    if (!g_screen_path[0]) {
        snprintf(g_screen_path, sizeof g_screen_path, "%s/bg3screen.txt", link_dir());
    }
    // Only a file written since BG3 started counts: one left by an earlier session (a screen open when it
    // ended) must not hold this one in BG3's own picture (seen in the harness).
    static time_t born;
    struct stat ps, fs;
    if (!born) born = stat("/proc/self", &ps) == 0 ? ps.st_mtime : time(NULL);
    char line[96] = {0};
    FILE* f = stat(g_screen_path, &fs) == 0 && fs.st_mtime >= born ? fopen(g_screen_path, "r") : NULL;
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    int on = line[0] == '1';
    if (on != g_screen) {
        line[strcspn(line, "\r\n")] = 0;
        logf_("screen: %s", on ? line + (line[1] ? 2 : 1) : "closed: WoW's world again");
        if (on) logf_("screen: BG3's own picture, figures BG3's, no WoW ground (%s)", g_screen_path);
        g_screen = on;
    }
    g_screen_dialog = on && strstr(line, "Dialogue") != NULL;
}
static void tav_present(QueueData* q) {
    if (!g_tav.init) {
        g_tav.init = 1;
        snprintf(g_tav.dir, sizeof g_tav.dir, "%s", tav_dir());  // WOWBG3_TAV_DIR, or as the launcher finds it
        snprintf(g_tav.plan, sizeof g_tav.plan, "%s/tavplan.txt", link_dir());
        struct stat st;
        g_tav.off = stat(g_tav.dir, &st) != 0 || !S_ISDIR(st.st_mode);
        clock_gettime(CLOCK_REALTIME, &g_tav.born);
        logf_("tav: %s (plan %s, out %s)", g_tav.off ? "off, no output directory" : "on", g_tav.plan, g_tav.dir);
    }
    if (g_tav.off) return;
    if (g_tav_hide && !g_cc.on) tav_publish_hide();
    if (g_req.presents - g_tav.last_check >= 15) {
        g_tav.last_check = g_req.presents;
        tav_read_plan();
    }
    if (g_tav.seq == 0 && g_tav.frame == 0) return;
    if (g_tav.n && !g_tav.mesh_ok && g_req.presents >= g_tav.retry_at) tav_copy_meshes(q);
    // An empty plan still writes one: Tav left, benilla hides her.
    { extern int g_lk_sent_; if (!g_lk_sent_ && (g_tav.mesh_ok || !g_tav.n)) tav_write_pose(); g_lk_sent_ = 0; }
    tav_publish_hide();
}

#include "wowbg3_fxcap.inc.c"
// When BG3 makes the pipelines its ground surfaces and its sight
// overlay are known by, against when the render plugin starts seeing pipelines (it hooks ~2 s in).
static void pipe_watch(const VkGraphicsPipelineCreateInfo* ci) {
    static const uint64_t keys[3] = {0x36d714a019831c05ull, 0xe0eca4a6adff9a7aull, 0xf9ac3c52a928d4f4ull};
    static const char* const names[3] = {"surface decal", "surface forward layer", "sight overlay"};
    static int said[3];
    for (uint32_t st = 0; st < ci->stageCount; st++) {
        ShaderInfo* sh = shader_info((uint64_t)ci->pStages[st].module);
        if (!sh) continue;
        for (int k = 0; k < 3; k++)
            if (sh->key == keys[k] && said[k]++ < 2)
                logf_("watch: BG3 made a %s pipeline %.2f s after it started (%llu graphics pipelines so far)", names[k], proc_age_s(),
                      (unsigned long long)g_pipe_count);
    }
    if (g_pipe_count == 1 || (g_pipe_count >= 256 && !(g_pipe_count & (g_pipe_count - 1))))
        logf_("watch: %llu graphics pipelines %.2f s after BG3 started", (unsigned long long)g_pipe_count, proc_age_s());
}
#include "wowbg3_pace.inc.c"
#include "wowbg3_ground.inc.c"
#include "wowbg3_shade.inc.c"
#include "wowbg3_surf.inc.c"
#include "wowbg3_preview.inc.c"
__attribute__((visibility("hidden"))) int g_lk_sent_;
__attribute__((visibility("hidden"))) uint32_t g_lk_seq_;
__attribute__((visibility("hidden"))) int g_wdg_pending_;
__attribute__((visibility("hidden"))) uint8_t g_wdg_cam_[96];
static struct { GBuf b; __typeof__(g_sf.wcm[0]) m; float tx, ty; int have; } g_lk;
#include "wowbg3_fxbg.inc.c"
#include "wowbg3_wd.inc.c"

// A G-buffer draw from an index buffer that several planned meshes share (tavd_note;
// recording thread, no lock): marks the one nearest to where BG3 places the draw, and any within 0.3 m
// of that (one figure's two copies of a part); all of them when the draw is not placed within 3 m of one.
static void tavd_note_shared(VkCommandBuffer cb, uint32_t first, uint32_t n, uint32_t fi) {
    if (first >= TAV_MAX || n > TAV_MAX - first) return;
    const uint16_t* list = g_tavd_list[t_tavd_tab] + first;
    RecInfo* ri = (g_rec && t_bind_cb == cb && t_pipe) ? TABLE_FIND(g_rec, REC_CAP, t_pipe, 0) : NULL;
    float at[3] = {0, 0, 0}, d2[TAV_MAX];
    int how = ri && ri->r ? fxc_place(ri->r, fi, at, NULL) : 0;
    int placed = how == 1 || how == 2;
    float best = 3.4e38f;
    for (uint32_t k = 0; placed && k < n; k++) {
        const float* q = g_tavd_pos[list[k]];
        float dx = at[0] - q[0], dy = at[1] - q[1], dz = at[2] - q[2];
        d2[k] = dx * dx + dy * dy + dz * dz;
        if (d2[k] < best) best = d2[k];
    }
    if (placed && best > 9.0f) placed = 0;
    float lim = placed ? (sqrtf(best) + 0.3f) * (sqrtf(best) + 0.3f) : 0.0f;
    for (uint32_t k = 0; k < n; k++) {
        if (placed && d2[k] > lim) continue;
        uint32_t s = list[k];
        if (s < TAV_MAX) t_tavd_bits[s >> 6] |= 1ull << (s & 63);
    }
    __atomic_add_fetch(placed ? &g_tavd_placed : &g_tavd_unplaced, 1, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------- intercepts
static VKAPI_ATTR VkResult VKAPI_CALL layer_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    uint64_t t_in = now_ns();
    pthread_mutex_lock(&g_lock);
    uint64_t t_locked = now_ns(), t_tav = 0, t_fxc = 0, t_comp = 0, t;
    QueueData* q = queue_data(queue);
    DeviceData* d = q ? q->dev : device_for(queue);
    PFN_vkQueuePresentKHR next = d ? d->QueuePresentKHR : NULL;
    VkPresentInfoKHR copy = *info;
    VkSemaphore wait = VK_NULL_HANDLE;
    if (q && d) {
        g_req.presents++;
        if (info->swapchainCount >= 1) trace_present(chain_data(info->pSwapchains[0]), info->pImageIndices[0]);
        if (g_req.presents % 15 == 0) { check_request(); compose_config(); }
        if (g_req.presents % 5 == 0 && g_cc.on) screen_check();
        buffer_research(q);
        t = now_ns();
        tav_present(q);
        t_tav = now_ns() - t;
        fxl_present();
        t = now_ns();
        fxc_present(q);
        ground_present();
        shade_present();
        amb_present();
        fb_retire_sweep(d);
        surf_present();
        pv_present(q);
        t_fxc = now_ns() - t;
        if (g_dump.armed) {
            pthread_mutex_lock(&g_trace_lock);
            dump_image(q, info);
            pthread_mutex_unlock(&g_trace_lock);
        }
        bg3_state_check();
        if (g_cc.on && g_bg3_running && info->swapchainCount >= 1) {
            t = now_ns();
            SwapchainData* c = chain_data(info->pSwapchains[0]);
            if (g_img) { role_present(c); fx_present(); }
            // On a BG3 screen with its own 3D picture the compositor still runs when it is to paint
            // the dark backdrop ("screenvoid"), but without WoW's frame, so the pacing stays out.
            const int scr_dark = g_screen && g_cc.screenvoid;
            if (c && info->pImageIndices[0] < c->count && !g_cc.raw && (!g_screen || scr_dark || g_screen_dialog)) {
                const int paced = !scr_dark;
                if (paced) pace_before_compose();  // which WoW frame, and a bounded wait for it (no trace lock held)
                pthread_mutex_lock(&g_trace_lock);
                VkSemaphore w = compose_frame(q, c, info->pImageIndices[0], info);
                pthread_mutex_unlock(&g_trace_lock);
                if (paced) pace_after_compose(t_in);
                if (w != VK_NULL_HANDLE) {
                    wait = w;
                    copy.waitSemaphoreCount = 1;
                    copy.pWaitSemaphores = &wait;
                }
            }
            t_comp = now_ns() - t;
        }
        if (g_req.active && info->swapchainCount >= 1) {
            if (g_req.index % g_req.every == 0) {
                SwapchainData* c = chain_data(info->pSwapchains[0]);
                if (c && info->pImageIndices[0] < c->count) {
                    VkSemaphore cw = capture_image(q, c, info->pImageIndices[0], &copy, NULL);
                    if (cw != VK_NULL_HANDLE) {
                        wait = cw;
                        copy.waitSemaphoreCount = 1;
                        copy.pWaitSemaphores = &wait;
                    }
                    if (--g_req.remaining <= 0) {
                        g_req.active = 0;
                        logf_("request done: %d frames, tag %s", g_req.frames_written, g_req.tag);
                    }
                }
            }
            g_req.index++;
        }
        perf_present(now_ns() - t_in, t_locked - t_in, t_tav, t_fxc, t_comp);
    }
    pthread_mutex_unlock(&g_lock);
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    t = now_ns();
    VkResult r = next(queue, &copy);
    perf_next(now_ns() - t);
    return r;
}

// BG3's picture size, for the WoW side (link/bg3size.txt, "W H"): benilla renders at it (patch 0143), so a resolution
// or aspect ratio changed in BG3's options while playing is followed at once. Before, WoW kept the size it started
// with and its picture was stretched over BG3's (lines, a wrong aspect).
static void bg3_size_note(uint32_t w, uint32_t h) {
    static uint32_t last_w, last_h;
    if (!w || !h || (w == last_w && h == last_h)) return;
    char path[600], tmp[620];
    snprintf(path, sizeof path, "%s/bg3size.txt", link_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%u %u\n", w, h);
    fclose(f);
    if (rename(tmp, path) == 0) { last_w = w; last_h = h; logf_("BG3's picture is %ux%u: told the WoW side (%s)", w, h, path); }
}
static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                               const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    DeviceData* d = device_for(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR copy = *info;
    copy.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkResult r = d->CreateSwapchainKHR(device, &copy, alloc, out);
    if (r != VK_SUCCESS) {
        // Fall back to the game's own request if the surface refuses copy usage.
        logf_("swapchain with TRANSFER_SRC refused (%d); retrying unchanged", (int)r);
        return d->CreateSwapchainKHR(device, info, alloc, out);
    }
    pthread_mutex_lock(&g_lock);
    SwapchainData* c = NULL;
    for (int i = 0; i < MAX_ENTRIES; i++) if (!g_chains[i].chain) { c = &g_chains[i]; break; }
    if (c) {
        memset(c, 0, sizeof *c);
        c->chain = *out;
        c->dev = d;
        c->format = info->imageFormat;
        c->extent = info->imageExtent;
        uint32_t n = 16;
        d->GetSwapchainImagesKHR(device, *out, &n, c->images);
        c->count = n;
        logf_("swapchain %ux%u format %d, %u images", c->extent.width, c->extent.height, (int)c->format, n);
    }
    pthread_mutex_unlock(&g_lock);
    bg3_size_note(info->imageExtent.width, info->imageExtent.height);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR chain,
                                                            const VkAllocationCallbacks* alloc) {
    DeviceData* d = device_for(device);
    pthread_mutex_lock(&g_lock);
    SwapchainData* c = chain_data(chain);
    if (c) memset(c, 0, sizeof *c);
    pthread_mutex_unlock(&g_lock);
    if (d) d->DestroySwapchainKHR(device, chain, alloc);
}

static void remember_queue(DeviceData* d, VkQueue q, uint32_t family) {
    pthread_mutex_lock(&g_lock);
    if (!queue_data(q)) {
        for (int i = 0; i < MAX_ENTRIES; i++) {
            if (!g_queues[i].queue) { g_queues[i].queue = q; g_queues[i].dev = d; g_queues[i].family = family; break; }
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* out) {
    DeviceData* d = device_for(device);
    if (!d) return;
    d->GetDeviceQueue(device, family, index, out);
    if (*out) remember_queue(d, *out, family);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* out) {
    DeviceData* d = device_for(device);
    if (!d || !d->GetDeviceQueue2) return;
    d->GetDeviceQueue2(device, info, out);
    if (*out) remember_queue(d, *out, info->queueFamilyIndex);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo* info,
                                                           const VkAllocationCallbacks* alloc, VkInstance* out) {
    VkLayerInstanceCreateInfo* chain = (VkLayerInstanceCreateInfo*)info->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo*)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(info, alloc, out);
    if (r != VK_SUCCESS) return r;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!g_instances[i].key) {
            InstanceData* d = &g_instances[i];
            d->key = key_of(*out);
            d->instance = *out;
            d->gipa = gipa;
            d->DestroyInstance = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
            d->GetPhysicalDeviceMemoryProperties =
                (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(*out, "vkGetPhysicalDeviceMemoryProperties");
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    (void)out_dir();  // reads the paths, creates the capture folder
#ifndef WOWBG3_PLUGIN
    logf_("instance created%s", layer_passive() ? "; passive (WOW_BG3_RENDER / render file: the render plugin draws)" : "");
#endif
    return r;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    InstanceData* d = instance_for(instance);
    if (!d) return;
    PFN_vkDestroyInstance next = d->DestroyInstance;
    pthread_mutex_lock(&g_lock);
    memset(d, 0, sizeof *d);
    pthread_mutex_unlock(&g_lock);
    next(instance, alloc);
}

#define DEV_FN(name) d->name = (PFN_vk##name)gdpa(dev, "vk" #name)
// Every device function the layer calls, from the next layer (or, in the render plugin, from BG3's
// own device table: wowbg3_render.c).
static void device_fns(DeviceData* d, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    DEV_FN(DestroyDevice); DEV_FN(GetDeviceQueue); DEV_FN(GetDeviceQueue2);
    DEV_FN(CreateSwapchainKHR); DEV_FN(DestroySwapchainKHR); DEV_FN(GetSwapchainImagesKHR);
    DEV_FN(QueuePresentKHR); DEV_FN(CreateCommandPool); DEV_FN(DestroyCommandPool);
    DEV_FN(AllocateCommandBuffers); DEV_FN(BeginCommandBuffer); DEV_FN(EndCommandBuffer);
    DEV_FN(ResetCommandBuffer); DEV_FN(CmdPipelineBarrier); DEV_FN(CmdCopyImageToBuffer);
    DEV_FN(QueueSubmit); DEV_FN(CreateFence); DEV_FN(WaitForFences); DEV_FN(ResetFences);
    DEV_FN(CreateSemaphore); DEV_FN(CreateBuffer); DEV_FN(GetBufferMemoryRequirements);
    DEV_FN(AllocateMemory); DEV_FN(BindBufferMemory); DEV_FN(MapMemory); DEV_FN(FreeMemory);
    DEV_FN(DestroyBuffer);
    d->SetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)gdpa(dev, "vkSetDebugUtilsObjectNameEXT");
    DEV_FN(CreateImage); DEV_FN(CreateImageView); DEV_FN(CreateFramebuffer); DEV_FN(CreateRenderPass);
    DEV_FN(CreateRenderPass2); DEV_FN(CmdBeginRenderPass); DEV_FN(CmdBeginRenderPass2);
    DEV_FN(CmdEndRenderPass); DEV_FN(CmdEndRenderPass2); DEV_FN(CmdBeginRendering); DEV_FN(CmdEndRendering);
    DEV_FN(CmdDraw); DEV_FN(CmdDrawIndexed); DEV_FN(CmdDrawIndirect); DEV_FN(CmdDrawIndexedIndirect);
    DEV_FN(CmdDispatch); DEV_FN(CmdDispatchIndirect); DEV_FN(CmdCopyImage); DEV_FN(CmdBlitImage); DEV_FN(CmdClearColorImage); DEV_FN(CmdResolveImage);
    DEV_FN(CmdExecuteCommands); DEV_FN(QueueSubmit2); DEV_FN(CmdPipelineBarrier2);
    DEV_FN(DestroyImage); DEV_FN(CmdCopyBufferToImage);
    DEV_FN(GetImageMemoryRequirements); DEV_FN(BindImageMemory);
    DEV_FN(BindBufferMemory2); DEV_FN(CmdCopyBuffer); DEV_FN(CmdBindIndexBuffer);
    DEV_FN(CreateGraphicsPipelines); DEV_FN(CreateComputePipelines); DEV_FN(CmdBindPipeline); DEV_FN(CmdBindVertexBuffers);
    DEV_FN(CmdBindDescriptorSets); DEV_FN(CmdSetBlendConstants); DEV_FN(CmdPushConstants); DEV_FN(UnmapMemory); DEV_FN(UpdateDescriptorSets); DEV_FN(UpdateDescriptorSetWithTemplate);
    DEV_FN(CreateDescriptorUpdateTemplate); DEV_FN(CreateShaderModule); DEV_FN(CreateSampler);
    DEV_FN(CreateDescriptorSetLayout); DEV_FN(CreatePipelineLayout); DEV_FN(CmdSetViewport);
    DEV_FN(CreateDescriptorPool); DEV_FN(AllocateDescriptorSets); DEV_FN(FreeDescriptorSets); DEV_FN(ResetDescriptorPool); DEV_FN(DestroyDescriptorPool); DEV_FN(GetFenceStatus); DEV_FN(DestroyShaderModule);
    DEV_FN(CreateQueryPool); DEV_FN(CmdResetQueryPool); DEV_FN(CmdWriteTimestamp); DEV_FN(GetQueryPoolResults);
    DEV_FN(CmdSetScissor); DEV_FN(DestroyImageView); DEV_FN(DestroyFramebuffer);
    d->DestroyBufferNext = d->DestroyBuffer;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                                         const VkAllocationCallbacks* alloc, VkDevice* out) {
    VkLayerDeviceCreateInfo* link = (VkLayerDeviceCreateInfo*)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo*)link->pNext;
    VkLayerDeviceCreateInfo* data = (VkLayerDeviceCreateInfo*)info->pNext;
    while (data && !(data->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && data->function == VK_LOADER_DATA_CALLBACK))
        data = (VkLayerDeviceCreateInfo*)data->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    InstanceData* inst = instance_for(physical);
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(inst ? inst->instance : VK_NULL_HANDLE, "vkCreateDevice");
    VkResult r = create(physical, info, alloc, out);
    if (r != VK_SUCCESS) return r;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!g_devices[i].key) {
            DeviceData* d = &g_devices[i];
            memset(d, 0, sizeof *d);
            d->key = key_of(*out);
            d->device = *out;
            d->physical = physical;
            d->inst = inst;
            d->gdpa = gdpa;
            d->setLoaderData = data ? data->u.pfnSetDeviceLoaderData : NULL;
            device_fns(d, *out, gdpa);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    logf_("device created; loader data callback %s", data ? "present" : "missing");
    return r;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    DeviceData* d = device_for(device);
    if (!d) return;
    PFN_vkDestroyDevice next = d->DestroyDevice;
    gc_device_gone(d);   // stops the WoW frame reader
    fxj_device_gone(d);  // finishes the effects readbacks
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (g_staging[i].dev == d) {
            if (g_staging[i].buffer) d->DestroyBuffer(device, g_staging[i].buffer, NULL);
            if (g_staging[i].memory) d->FreeMemory(device, g_staging[i].memory, NULL);
            if (g_staging[i].pool) d->DestroyCommandPool(device, g_staging[i].pool, NULL);
            memset(&g_staging[i], 0, sizeof g_staging[i]);
        }
        if (g_queues[i].dev == d) memset(&g_queues[i], 0, sizeof g_queues[i]);
        if (g_chains[i].dev == d) memset(&g_chains[i], 0, sizeof g_chains[i]);
    }
    memset(d, 0, sizeof *d);
    pthread_mutex_unlock(&g_lock);
    next(device, alloc);
}

// ---------------------------------------------------------------- proc addresses
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wowbg3_GetDeviceProcAddr(VkDevice device, const char* name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wowbg3_GetInstanceProcAddr(VkInstance instance, const char* name) {
#define HOOK(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)layer_##n
    HOOK(CreateInstance); HOOK(DestroyInstance); HOOK(CreateDevice);
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)wowbg3_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)wowbg3_GetDeviceProcAddr;
    HOOK(DestroyDevice);
#ifndef WOWBG3_PLUGIN
    if (layer_passive()) {  // the render plugin draws: nothing of ours in the device's calls
        if (!instance) return NULL;
        InstanceData* pd = instance_for(instance);
        return pd ? pd->gipa(instance, name) : NULL;
    }
#endif
    HOOK(GetDeviceQueue); HOOK(GetDeviceQueue2); HOOK(CreateSwapchainKHR);
    HOOK(DestroySwapchainKHR); HOOK(QueuePresentKHR);
    HOOK(CreateImage); HOOK(CreateImageView); HOOK(CreateFramebuffer); HOOK(CreateRenderPass);
    HOOK(CreateRenderPass2); HOOK(CreateRenderPass2KHR); HOOK(BeginCommandBuffer); HOOK(EndCommandBuffer);
    HOOK(CmdBeginRenderPass); HOOK(CmdBeginRenderPass2); HOOK(CmdEndRenderPass); HOOK(CmdEndRenderPass2);
    HOOK(CmdBeginRendering); HOOK(CmdBeginRenderingKHR); HOOK(CmdEndRendering); HOOK(CmdEndRenderingKHR);
    HOOK(CmdDraw); HOOK(CmdDrawIndexed); HOOK(CmdDrawIndirect); HOOK(CmdDrawIndexedIndirect); HOOK(CmdDispatch); HOOK(CmdDispatchIndirect);
    HOOK(CmdCopyImage); HOOK(CmdBlitImage); HOOK(CmdResolveImage); HOOK(CmdExecuteCommands);
    HOOK(QueueSubmit); HOOK(QueueSubmit2); HOOK(QueueSubmit2KHR);
    HOOK(CmdPipelineBarrier); HOOK(CmdPipelineBarrier2); HOOK(CmdPipelineBarrier2KHR); HOOK(DestroyImage);
    HOOK(CreateBuffer); HOOK(BindBufferMemory); HOOK(BindBufferMemory2); HOOK(BindBufferMemory2KHR); HOOK(DestroyBuffer);
    HOOK(CmdBindIndexBuffer); HOOK(CreateGraphicsPipelines); HOOK(CreateComputePipelines); HOOK(CmdBindPipeline); HOOK(CmdBindVertexBuffers);
    HOOK(CmdBindDescriptorSets); HOOK(CmdPushConstants); HOOK(MapMemory); HOOK(UnmapMemory); HOOK(UpdateDescriptorSets);
    HOOK(UpdateDescriptorSetWithTemplate); HOOK(CreateDescriptorUpdateTemplate); HOOK(CreateShaderModule);
    HOOK(CreateSampler); HOOK(CreateDescriptorSetLayout); HOOK(CreatePipelineLayout); HOOK(CmdSetViewport); HOOK(CmdSetScissor);
    HOOK(AllocateDescriptorSets); HOOK(FreeDescriptorSets); HOOK(ResetDescriptorPool); HOOK(DestroyDescriptorPool);
    if (!instance) return NULL;
    InstanceData* d = instance_for(instance);
    return d ? d->gipa(instance, name) : NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wowbg3_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)wowbg3_GetDeviceProcAddr;
    HOOK(DestroyDevice);
#ifndef WOWBG3_PLUGIN
    if (layer_passive()) {
        DeviceData* pd = device_for(device);
        return pd ? pd->gdpa(device, name) : NULL;
    }
#endif
    HOOK(GetDeviceQueue); HOOK(GetDeviceQueue2); HOOK(CreateSwapchainKHR);
    HOOK(DestroySwapchainKHR); HOOK(QueuePresentKHR);
    HOOK(CreateImage); HOOK(CreateImageView); HOOK(CreateFramebuffer); HOOK(CreateRenderPass);
    HOOK(CreateRenderPass2); HOOK(CreateRenderPass2KHR); HOOK(BeginCommandBuffer); HOOK(EndCommandBuffer);
    HOOK(CmdBeginRenderPass); HOOK(CmdBeginRenderPass2); HOOK(CmdEndRenderPass); HOOK(CmdEndRenderPass2);
    HOOK(CmdBeginRendering); HOOK(CmdBeginRenderingKHR); HOOK(CmdEndRendering); HOOK(CmdEndRenderingKHR);
    HOOK(CmdDraw); HOOK(CmdDrawIndexed); HOOK(CmdDrawIndirect); HOOK(CmdDrawIndexedIndirect); HOOK(CmdDispatch); HOOK(CmdDispatchIndirect);
    HOOK(CmdCopyImage); HOOK(CmdBlitImage); HOOK(CmdResolveImage); HOOK(CmdExecuteCommands);
    HOOK(QueueSubmit); HOOK(QueueSubmit2); HOOK(QueueSubmit2KHR);
    HOOK(CmdPipelineBarrier); HOOK(CmdPipelineBarrier2); HOOK(CmdPipelineBarrier2KHR); HOOK(DestroyImage);
    HOOK(CreateBuffer); HOOK(BindBufferMemory); HOOK(BindBufferMemory2); HOOK(BindBufferMemory2KHR); HOOK(DestroyBuffer);
    HOOK(CmdBindIndexBuffer); HOOK(CreateGraphicsPipelines); HOOK(CreateComputePipelines); HOOK(CmdBindPipeline); HOOK(CmdBindVertexBuffers);
    HOOK(CmdBindDescriptorSets); HOOK(CmdPushConstants); HOOK(MapMemory); HOOK(UnmapMemory); HOOK(UpdateDescriptorSets);
    HOOK(UpdateDescriptorSetWithTemplate); HOOK(CreateDescriptorUpdateTemplate); HOOK(CreateShaderModule);
    HOOK(CreateSampler); HOOK(CreateDescriptorSetLayout); HOOK(CreatePipelineLayout); HOOK(CmdSetViewport); HOOK(CmdSetScissor);
    HOOK(AllocateDescriptorSets); HOOK(FreeDescriptorSets); HOOK(ResetDescriptorPool); HOOK(DestroyDescriptorPool);
#undef HOOK
    DeviceData* d = device_for(device);
    return d ? d->gdpa(device, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL wowbg3_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = wowbg3_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = wowbg3_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
