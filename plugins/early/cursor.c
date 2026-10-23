/* wowbg3-early, part 2: WoW's quest cursors when BG3 built its cursors before the plugin loaded
 * (the interaction notes "Cursors").
 *
 * BG3 imports SDL_SetCursor and SDL_CreateColorCursor from libSDL2 through lazily bound PLT slots
 * in its own writable .got.plt (no BIND_NOW; checked with readelf), so a plugin can take them over
 * by rewriting the slot. Part 1 (wowbg3_early.c) re-points the Listen cursor files at WoW's art; that
 * only counts when BG3 creates its cursors after the plugin loads. This part covers the other case:
 *  - it reports at load whether SDL_CreateColorCursor was already bound (already called);
 *  - SDL_SetCursor is hooked: the first time BG3 sets a cursor it has not set before, the hook asks
 *    the X server for the cursor image now shown (XFixesGetCursorImage, only when the X cursor serial
 *    changed, so the pointer was over BG3's window) and hashes it (FNV-1a over the 32-bit ARGB
 *    pixels). If it is BG3's own Listen, Listen pressed, ListenWarning or ListenWarning pressed
 *    (hashes taken from the X server in the harness, tmp/interaction/xcursor.py), that SDL cursor is
 *    swapped for ours from now on: SDL color cursors made from wow-bg3/cursors/quest-*.cur.
 *    BG3's OpenDoor cursors (no BG3 door in world mode) the same way for WoW's merchant cursor,
 *    merchant*.cur (benilla patch 0064): their hashes computed from Game.pak's Cursor_OpenDoor_*.cur
 *    as the X server shows a cursor (premultiplied ARGB; the method gives the four Listen hashes above).
 * If the X server is not reachable (Wayland, no XFixes) or no hash matches, nothing changes. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include "bg3le_plugin.h"
#include "cursor.h"

static const bg3le_host* g_host;
static bg3le_plugin* g_self;

/* ---- BG3's PLT slots ---- */
struct slot_query { const char* name; uintptr_t slot; uintptr_t lo, hi; int done; };

static int find_slot(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    struct slot_query* q = data;
    if (q->done) return 1;
    q->done = 1; /* the first object is the main program (BG3) */
    uintptr_t base = info->dlpi_addr;
    ElfW(Dyn)* dyn = NULL;
    q->lo = UINTPTR_MAX; q->hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_DYNAMIC) dyn = (ElfW(Dyn)*)(base + ph->p_vaddr);
        if (ph->p_type == PT_LOAD) {
            if (base + ph->p_vaddr < q->lo) q->lo = base + ph->p_vaddr;
            if (base + ph->p_vaddr + ph->p_memsz > q->hi) q->hi = base + ph->p_vaddr + ph->p_memsz;
        }
    }
    if (dyn == NULL) return 1;
    uintptr_t strtab = 0, symtab = 0, jmprel = 0, pltrelsz = 0;
    for (ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB: strtab = d->d_un.d_ptr; break;
        case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        default: break;
        }
    }
    /* glibc relocates these in place for most objects; add the base when it did not. */
    if (strtab && strtab < base) strtab += base;
    if (symtab && symtab < base) symtab += base;
    if (jmprel && jmprel < base) jmprel += base;
    if (!strtab || !symtab || !jmprel) return 1;
    const ElfW(Rela)* r = (const ElfW(Rela)*)jmprel;
    for (size_t i = 0; i < pltrelsz / sizeof(ElfW(Rela)); i++) {
        if (ELF64_R_TYPE(r[i].r_info) != R_X86_64_JUMP_SLOT) continue;
        const ElfW(Sym)* s = (const ElfW(Sym)*)symtab + ELF64_R_SYM(r[i].r_info);
        if (strcmp((const char*)strtab + s->st_name, q->name) == 0) { q->slot = base + r[i].r_offset; break; }
    }
    return 1;
}

/* ---- our cursors ---- */
typedef SDL_Cursor* (*create_fn)(SDL_Surface*, int, int);
typedef void (*set_fn)(SDL_Cursor*);
static set_fn real_set;
static create_fn real_create;
static unsigned created_after_load;

/* BG3's own Listen cursors as the X server shows them (harness). */
#define NCUR 8
static const uint64_t BG3_HASH[NCUR] = {
    0xc94c0472578073c2ull, /* Listen */
    0x6e4b37b487d0375full, /* Listen pressed */
    0x0d3c1c21de925385ull, /* ListenWarning */
    0xb814203833a59dbaull, /* ListenWarning pressed */
    0x4393e5026819e117ull, /* OpenDoor */
    0x6be727f010d45158ull, /* OpenDoor pressed */
    0x92c3a8f6b5cac498ull, /* OpenDoorWarning */
    0xc490a0ae2265c299ull, /* OpenDoorWarning pressed */
};
static const char* const OUR_FILE[NCUR] = {"quest-available.cur", "quest-available-pressed.cur",
                                         "quest-turnin.cur", "quest-turnin-pressed.cur",
                                         "merchant.cur", "merchant-pressed.cur",
                                         "merchant-gray.cur", "merchant-gray-pressed.cur"};
static SDL_Cursor* ours[NCUR];
static char g_dir[1024];

static SDL_Cursor* load_cur(const char* name) {
    char path[1200];
    snprintf(path, sizeof path, "%s/cursors/%s", g_dir, name);
    FILE* f = fopen(path, "rb");
    if (f == NULL) return NULL;
    unsigned char b[4286];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    /* One 32 x 32 32-bit image, as link/tools/wowcursor.py writes and BG3's own files are. */
    if (n != sizeof b || b[2] != 2 || b[6] != 32 || b[7] != 32) return NULL;
    uint32_t off = (uint32_t)b[18] | (uint32_t)b[19] << 8 | (uint32_t)b[20] << 16 | (uint32_t)b[21] << 24;
    if (off + 40 + 4096 > n) return NULL;
    static uint32_t px[NCUR][32 * 32];
    static int used;
    if (used >= NCUR) return NULL;
    uint32_t* dst = px[used++];
    const unsigned char* src = b + off + 40;
    for (int y = 0; y < 32; y++) /* bottom-up BGRA rows -> top-down ARGB8888 */
        memcpy(dst + y * 32, src + (31 - y) * 128, 128);
    typedef SDL_Surface* (*surf_fn)(void*, int, int, int, int, Uint32);
    surf_fn mk = (surf_fn)g_host->sdl_function("SDL_CreateRGBSurfaceWithFormatFrom");
    if (mk == NULL || real_create == NULL) return NULL;
    SDL_Surface* s = mk(dst, 32, 32, 32, 128, SDL_PIXELFORMAT_ARGB8888);
    if (s == NULL) return NULL;
    return real_create(s, b[10] | b[11] << 8, b[12] | b[13] << 8); /* the surface stays (static pixels) */
}

/* ---- X cursor image ---- */
typedef struct { short x, y; unsigned short width, height, xhot, yhot; unsigned long serial; unsigned long* pixels; } XFixesImage;
typedef XFixesImage* (*xfixes_get_fn)(void*);
typedef int (*xfree_fn)(void*);
static xfixes_get_fn x_get;
static xfree_fn x_free;
static void* x_display;
static unsigned long last_serial;

static int x_ready(void) {
    if (x_get && x_display) return 1;
    void* fx = dlopen("libXfixes.so.3", RTLD_LAZY | RTLD_LOCAL);
    void* x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (fx == NULL || x11 == NULL) return 0;
    x_get = (xfixes_get_fn)dlsym(fx, "XFixesGetCursorImage");
    x_free = (xfree_fn)dlsym(x11, "XFree");
    typedef SDL_Window* (*focus_fn)(void);
    typedef SDL_bool (*wm_fn)(SDL_Window*, SDL_SysWMinfo*);
    focus_fn focus = (focus_fn)g_host->sdl_function("SDL_GetMouseFocus");
    focus_fn kfocus = (focus_fn)g_host->sdl_function("SDL_GetKeyboardFocus");
    wm_fn wm = (wm_fn)g_host->sdl_function("SDL_GetWindowWMInfo");
    SDL_Window* w = focus ? focus() : NULL;
    if (w == NULL && kfocus) w = kfocus();
    if (w == NULL || wm == NULL) return 0;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!wm(w, &info) || info.subsystem != SDL_SYSWM_X11) return 0;
    x_display = info.info.x11.display;
    return x_get != NULL && x_free != NULL && x_display != NULL;
}

/* The shown cursor's hash, or 0 when the X cursor did not change (pointer not over BG3). */
static uint64_t shown_hash(void) {
    if (!x_ready()) return 0;
    XFixesImage* im = x_get(x_display);
    if (im == NULL) return 0;
    uint64_t h = 0;
    if (im->serial != last_serial) {
        last_serial = im->serial;
        h = 0xcbf29ce484222325ull;
        for (int k = 0; k < im->width * im->height; k++) {
            uint32_t v = (uint32_t)im->pixels[k];
            for (int sh = 0; sh < 32; sh += 8) { h ^= (v >> sh) & 255u; h *= 0x100000001b3ull; }
        }
    }
    x_free(im);
    return h;
}

/* ---- the hooks ---- */
#define SEEN_MAX 256
static SDL_Cursor* seen[SEEN_MAX];
static int nseen;
static SDL_Cursor* from[NCUR];
static unsigned swaps;

static int was_seen(SDL_Cursor* c) {
    for (int i = 0; i < nseen; i++) if (seen[i] == c) return 1;
    return 0;
}

static unsigned set_calls;
static void hook_set(SDL_Cursor* c) {
    if (++set_calls == 20)
        g_host->log(g_self, "wowbg3-early: %u cursor(s) created after the plugin loaded", created_after_load);
    for (int k = 0; k < NCUR; k++) {
        if (c != NULL && c == from[k] && ours[k] != NULL) { swaps++; real_set(ours[k]); return; }
    }
    real_set(c);
    if (c == NULL || was_seen(c)) return;
    for (int k = 0; k < NCUR; k++) if (c == ours[k]) return;
    uint64_t h = shown_hash();
    if (h == 0) return; /* not judged yet: the next set of this cursor tries again */
    if (nseen < SEEN_MAX) seen[nseen++] = c;
    for (int k = 0; k < NCUR; k++) {
        if (h != BG3_HASH[k]) continue;
        if (ours[k] == NULL) ours[k] = load_cur(OUR_FILE[k]);
        if (ours[k] == NULL) return;
        from[k] = c;
        g_host->log(g_self, "wowbg3-early: BG3's cursor %d (X hash %016llx) shows WoW's %s from now on", k,
                    (unsigned long long)h, OUR_FILE[k]);
        swaps++;
        real_set(ours[k]);
        return;
    }
}

static SDL_Cursor* hook_create(SDL_Surface* s, int hx, int hy) {
    created_after_load++;
    return real_create(s, hx, hy);
}

static int take_slot(const char* name, void* hook, void** original, int* was_bound) {
    struct slot_query q = {name, 0, 0, 0, 0};
    dl_iterate_phdr(find_slot, &q);
    if (q.slot == 0) return 0;
    void** p = (void**)q.slot;
    void* now = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    /* Lazily bound: the slot points back into BG3's own PLT until the first call. */
    *was_bound = !((uintptr_t)now >= q.lo && (uintptr_t)now < q.hi);
    *original = g_host->sdl_function(name);
    if (*original == NULL) return 0;
    __atomic_store_n(p, hook, __ATOMIC_RELEASE);
    return 1;
}

void cursor_init(const bg3le_host* host, bg3le_plugin* self, const char* dir) {
    g_host = host; g_self = self;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    int bound_create = 0, bound_set = 0;
    int okc = take_slot("SDL_CreateColorCursor", (void*)hook_create, (void**)&real_create, &bound_create);
    int oks = take_slot("SDL_SetCursor", (void*)hook_set, (void**)&real_set, &bound_set);
    host->log(self, "wowbg3-early: BG3's SDL_CreateColorCursor %s (%s), SDL_SetCursor %s (%s)",
              okc ? "hooked" : "not found", bound_create ? "already called: cursors exist" : "not called yet",
              oks ? "hooked" : "not found", bound_set ? "already called" : "not called yet");
}

unsigned cursor_created_after_load(void) { return created_after_load; }
unsigned cursor_swaps(void) { return swaps; }
