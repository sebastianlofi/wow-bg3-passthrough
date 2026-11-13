// A stand-in bg3le host for testing wowbg3-launcher without BG3.
// Loads the plugin with a host table that has patch 0003's add_state_gate and game_state, then
// runs the script given on stdin, one command per line:
//   set ID VALUE       write a setting (as Ext.Plugins.Set does)
//   get ID             print a setting
//   state NAME         the client game state game_state(0) reports from now on
//   frame [N]          N frame handler calls (default 1)
//   gate FROM TO       ask the plugin's gate; prints "gate FROM->TO: 1|0"
//   key                offer a key-down event; prints whether the plugin kept it
//   sleep SECONDS
//   exit               exit() (atexit handlers run)
//   die                SIGKILL ourselves (no atexit)
// Build: cc -std=gnu11 -o test_host test_host.c -ldl -lpthread -I../../bg3le/include
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bg3le_plugin.h"

typedef int (*state_gate_fn)(void*, const char*, const char*);
// The host table as wowbg3_launcher.c reads it (its own fixed layout: bg3le's header has since grown
// these fields into bg3le_host itself, so a struct that appends them to bg3le_host puts them too far).
typedef struct {
    uint32_t abi;
    uint32_t size;
    const char* version;
    void (*describe)(bg3le_plugin*, const char*, const char*);
    void (*log)(bg3le_plugin*, const char*, ...);
    void (*warn)(bg3le_plugin*, const char*, ...);
    int (*add_event_handler)(bg3le_plugin*, bg3le_event_handler, void*);
    void* (*sdl_function)(const char*);
    int (*add_setting)(bg3le_plugin*, const char*, bg3le_setting_type, void*, double, double);
    int (*add_frame_handler)(bg3le_plugin*, bg3le_frame_handler, void*);
    int (*add_state_gate)(bg3le_plugin*, state_gate_fn, void*);
    const char* (*game_state)(int);
    int (*state_gate_can_hold)(const char*, const char*);  // bg3le patch 0004 (TEST_HOLD4=1)
} host_v2;

struct setting { char id[64]; bg3le_setting_type type; void* value; };
static struct setting settings[64];
static int nsettings;
static bg3le_frame_handler frame_fn;
static bg3le_event_handler event_fn;
static state_gate_fn gate_fn;
static char state[64] = "Menu";

static void hlog(bg3le_plugin* s, const char* fmt, ...) {
    va_list a; va_start(a, fmt); printf("[log] "); vprintf(fmt, a); printf("\n"); va_end(a); fflush(stdout);
}
static void hwarn(bg3le_plugin* s, const char* fmt, ...) {
    va_list a; va_start(a, fmt); printf("[WARN] "); vprintf(fmt, a); printf("\n"); va_end(a); fflush(stdout);
}
static void describe(bg3le_plugin* s, const char* n, const char* v) { printf("[describe] %s %s\n", n, v); }
static int add_event(bg3le_plugin* s, bg3le_event_handler h, void* u) { event_fn = h; return 0; }
static void* sdl_fn(const char* n) { return NULL; }
static int add_setting(bg3le_plugin* s, const char* id, bg3le_setting_type t, void* v, double mn, double mx) {
    snprintf(settings[nsettings].id, 64, "%s", id); settings[nsettings].type = t; settings[nsettings].value = v;
    nsettings++; return 0;
}
static int add_frame(bg3le_plugin* s, bg3le_frame_handler h, void* u) { frame_fn = h; return 0; }
static int add_gate(bg3le_plugin* s, state_gate_fn g, void* u) { gate_fn = g; return 0; }
static int can_hold(const char* from, const char* to) { return 1; }
static const char* game_state(int server) {
    // Stable pointers per name, as bg3le's names table gives.
    static char names[16][64]; static int n;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], state)) return names[i];
    snprintf(names[n], 64, "%s", state); return names[n++];
}
static int* find(const char* id) {
    for (int i = 0; i < nsettings; i++) if (!strcmp(settings[i].id, id)) return (int*)settings[i].value;
    return NULL;
}

// bg3le patch 0012's mod IO folders, found by the plugin with dlsym (link this host with -rdynamic); a stock bg3le
// (TEST_OLD_BG3LE) has none.
__attribute__((visibility("default"))) _Bool bg3le_io_root_add(const char* name, const char* dir) {
    if (getenv("TEST_OLD_BG3LE")) return 0;
    printf("[io root] %s/ is %s\n", name, dir);
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: test_host PLUGIN.so < script\n"); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    int (*init)(const bg3le_host*, bg3le_plugin*) = (int (*)(const bg3le_host*, bg3le_plugin*))dlsym(h, "bg3le_plugin_init");
    static host_v2 host;
    host.abi = BG3LE_PLUGIN_ABI;
    host.size = getenv("TEST_OLD_BG3LE") ? offsetof(host_v2, add_state_gate)
              : getenv("TEST_HOLD4") ? sizeof(host_v2) : offsetof(host_v2, state_gate_can_hold);
    host.state_gate_can_hold = can_hold;
    host.version = "test"; host.describe = describe; host.log = hlog; host.warn = hwarn;
    host.add_event_handler = add_event; host.sdl_function = sdl_fn; host.add_setting = add_setting;
    host.add_frame_handler = add_frame; host.add_state_gate = add_gate; host.game_state = game_state;
    int rc = init((const bg3le_host*)&host, (bg3le_plugin*)&host);
    printf("init -> %d\n", rc);
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        char a[128] = "", b[128] = "", c[128] = "";
        int n = sscanf(line, "%127s %127s %127s", a, b, c);
        if (n < 1 || a[0] == '#') continue;
        if (!strcmp(a, "set")) { int* p = find(b); if (p) *p = atoi(c); printf("set %s %s -> %s\n", b, c, p ? "ok" : "no such setting"); }
        else if (!strcmp(a, "get")) { int* p = find(b); printf("get %s = %d\n", b, p ? *p : -999); }
        else if (!strcmp(a, "state")) { snprintf(state, sizeof state, "%s", b); printf("state %s\n", state); }
        else if (!strcmp(a, "frame")) { int k = n > 1 ? atoi(b) : 1; for (int i = 0; i < k; i++) if (frame_fn) frame_fn(NULL, 0.016); }
        else if (!strcmp(a, "gate")) printf("gate %s->%s: %d\n", b, c, gate_fn ? gate_fn(NULL, b, c) : -1);
        else if (!strcmp(a, "key")) { uint32_t ev[16] = {0x300}; printf("key kept by plugin: %d\n", event_fn ? event_fn(NULL, (SDL_Event*)ev) : -1); }
        else if (!strcmp(a, "sleep")) { struct timespec t = {atoi(b), 0}; nanosleep(&t, NULL); }
        else if (!strcmp(a, "exit")) { printf("exit\n"); exit(0); }
        else if (!strcmp(a, "die")) { printf("die\n"); kill(getpid(), SIGKILL); }
    }
    return 0;
}
