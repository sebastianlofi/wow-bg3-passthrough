// wowbg3-launcher: the bg3le native plugin that runs the WoW side of WOW-BG3
// (plug-and-play plan "Design": everything starts from our plugin).
//
//   1. Finds the WOW-BG3 folder (plugin, benilla, portable server, tools, the player's WoW/):
//      $WOWBG3_HOME; the plugin's own folder if it holds launcher.conf (loaded from Mods/WOW-BG3/
//      by bg3le's mod-folder plugins, patch 0003); <plugin folder>/WOW-BG3/; <Mods>/WOW-BG3/.
//   2. Puts WOW-BG3.pak into BG3's Mods folder when the folder has one and Mods has none or an
//      older copy (at plugin load, 1.7 s, well before BG3 reads its load order at ~9.4 s).
//   3. Starts and stops the WoW side as child processes (launcher.conf: the portable server
//      and the coordinator, which runs the offscreen benilla), with the data folder
//      ~/.local/share/wow-bg3. They die with BG3 (PR_SET_PDEATHSIG from a supervisor thread that
//      lives as long as the process) and get SIGTERM at a clean exit.
//   4. Holds BG3's loading screen at every level load of a world-mode game until WoW's ground is
//      placed under the hero. With bg3le patch 0004 the client is held in
//      PrepareRunning: BG3's server goes Running and runs the level (the hero, the lifecycle's WoW
//      bring-up, world mode's Start with its ground) while BG3's own loading screen stays up; released
//      when world mode reports its Start done (wow-bg3/world-status.json Last "Start", OK) in this
//      session, by Lua (setting "ready" = 1, e.g. character creation, which needs no WoW), by the file
//      <data>/run/ready, by lifecycle Phase "failed", or after hold_timeout seconds (logged as a
//      failure, never a hang). Without 0004 (patch 0003 only) the queued StopLoading is held as
//      before, released at lifecycle Phase benilla or later (BG3's server waits in Sync meanwhile, so
//      the ground follows after the release). Input to the game is held with it.
//   5. Writes wow-bg3/bg3-state.json the frame BG3's client or server game state changes, so the WoW
//      side sees a load start at once (UnloadLevel/UnloadSession).
//   6. The release's environment (0.3.0), set in BG3's own process at plugin
//      init (before the mod's Lua and the render plugin start) and given to every child:
//      WOW_BG3_RUN_ID (a fresh id when BG3 has none: the mod's files carry it), WOWBG3_SHM (a tmpfs
//      folder for WoW's frames, /dev/shm/wow-bg3-UID), WOWBG3_TAV_DIR and WOW_BG3_TAV_DIR ($WOWBG3_SHM/tav),
//      WOWBG3_CAPTURE_DIR (<data>/capture: the render plugin's compose words and log). The harness's
//      sandbox mounts (/wowshm, /logs/capture) win where they exist, so a dev run stays as it was.
//      The coordinator child (launcher.conf [coord], tools/wowbg3-coord.py) does the rest: lifecycle,
//      benilla, the bridges. While it reports Busy (run/coord.json, the server's first start building
//      its data) a held loading screen waits past hold_timeout, at most 15 minutes more.
//
// Lua's view (Ext.Plugins.Get/Set("wowbg3-launcher", id)):
//   world      -1 unknown, 0 vanilla, 1 world mode (WorldFlag.lua sets it every state change)
//   ready      0/1: the WoW side is in the world and the ground is placed (releases the hold)
//   <child>    -1 automatic (launcher.conf "start"), 0 stop, 1 run   e.g. "benilla", "server"
//   <child>_pid   read only: the child's pid, 0 when not running
//   holding    read only: 1 while the loading screen is held
//   mod_enabled   read only: WOW-BG3's UUID is enabled in modsettings.lsx
//   pak        read only: 0 no pak known, 1 copied this start, 2 already in Mods, 3 Mods unwritable
//   hold       1: hold the loading screen (default), 0: never        (kept across starts)
//   hold_timeout  seconds before a hold gives up (default 120)          (kept across starts)
// Status for tools: <data>/run/launcher.json; per child log <data>/logs/<child>.log.

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "bg3le_plugin.h"

#define LAUNCHER_VERSION "0.3.0"
#define MOD_UUID "a97c1258-37c3-434c-9309-c982f6cea252"
#define MAX_CHILDREN 8
#define MAX_ARGS 48
#define MAX_ENV 16

// bg3le's host table with patch 0003's additions; used only when host->size covers them.
typedef int (*state_gate_fn)(void* user, const char* from, const char* to);
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
    const char* (*game_state)(int server);
    int (*state_gate_can_hold)(const char* from, const char* to);  // patch 0004
} host_v2;

static const bg3le_host* g_host;
static const host_v2* g_host2;  // NULL on a bg3le without patch 0003
static int g_host4;             // the host table has patch 0004's state_gate_can_hold
static int g_gate_on;           // the loading-screen gate is registered
// The client's and server's game state names: patch 0003's game_state(), else bg3le's exported
// functions (default visibility), else nothing.
static const char* (*g_client_state)(void);
static const char* (*g_server_state)(void);
static const char* state_via_host_client(void) { return g_host2->game_state(0); }
static const char* state_via_host_server(void) { return g_host2->game_state(1); }
static bg3le_plugin* g_self;

#define LOG(...) g_host->log(g_self, __VA_ARGS__)
#define WARN(...) g_host->warn(g_self, __VA_ARGS__)

static char g_home[PATH_MAX];   // the WOW-BG3 folder, "" if not found
static char g_data[PATH_MAX];   // ~/.local/share/wow-bg3
static char g_mods[PATH_MAX];   // BG3's Mods folder
static char g_io[PATH_MAX];     // Script Extender/wow-bg3 (the SE mod's IO folder)
static char g_modsettings[PATH_MAX];
static char g_shm[PATH_MAX];     // WOWBG3_SHM (frames, Tav's geometry): a tmpfs folder
static char g_capture[PATH_MAX]; // WOWBG3_CAPTURE_DIR: the render plugin's compose words and log

// ---- settings (ints Lua reads and writes) ----
static int s_world = -1, s_ready = 0, s_holding = 0, s_mod_enabled = 0, s_pak = 0;
static int s_hold = 1, s_hold_timeout = 120;
// Where the loading screen is held: 0 automatic (4 where bg3le can hold it, else 1), 1 the queued
// StopLoading (patch 0003: measured, BG3's server waits in Sync meanwhile), 2 the queued
// PrepareRunning, 3 a queued Running, 4 the client in PrepareRunning (patch 0004: the engine leaves
// PrepareRunning for Running by itself, never queued; the server runs the level meanwhile).
static int s_hold_at = 0;
static int g_hold_mode = 1;     // the resolved hold point (1 to 4)
static const char* hold_target(void) {
    return g_hold_mode == 2 ? "PrepareRunning" : g_hold_mode == 3 || g_hold_mode == 4 ? "Running" : "StopLoading";
}

// ---- children ----
enum start_rule { START_ENABLED, START_REQUEST, START_ALWAYS, START_NEVER };
typedef struct {
    char name[32];
    char exec[PATH_MAX * 2];
    char* argv[MAX_ARGS + 2];
    int argc;
    char* env[MAX_ENV];
    int envc;
    enum start_rule start;
    int grace;        // seconds between SIGTERM and SIGKILL
    int restarts;     // automatic restarts allowed after an unexpected exit
    int once;         // a job: runs to the end once; exit 0 leaves {data}/run/NAME.done and it never runs again
    int done;
    // live state
    int want;         // setting: -1 auto, 0 stop, 1 run
    int pid_setting;  // setting: pid or 0
    pid_t pid;
    int stopping;
    double stop_at;   // when SIGTERM went out
    int used_restarts;
    double next_try;
    char pid_id[48];
} child_t;
static child_t g_child[MAX_CHILDREN];
static int g_nchild;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char** g_child_envp;     // the environment children get (built once)

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int is_file(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static void mkdirs(const char* path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    mkdir(tmp, mode);
}

// {home} {data} {io} {link} {mods} {wow} in a config value.
static void expand(const char* in, char* out, size_t cap) {
    size_t o = 0;
    while (*in && o + 1 < cap) {
        const char* sub = NULL;
        size_t len = 0;
        char linkdir[PATH_MAX + 8], wowdir[PATH_MAX + 8];
        if (*in == '{') {
            snprintf(linkdir, sizeof linkdir, "%s/link", g_io);
            snprintf(wowdir, sizeof wowdir, "%s/WoW", g_home);
            static const char* keys[] = {"{home}", "{data}", "{io}", "{link}", "{mods}", "{wow}"};
            const char* vals[] = {g_home, g_data, g_io, linkdir, g_mods, wowdir};
            for (int k = 0; k < 6; k++) {
                size_t kl = strlen(keys[k]);
                if (strncmp(in, keys[k], kl) == 0) {
                    sub = vals[k];
                    len = kl;
                    break;
                }
            }
        }
        if (sub) {
            for (const char* s = sub; *s && o + 1 < cap; s++) out[o++] = *s;
            in += len;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = 0;
}

// ---- paths ----
static void find_paths(void) {
    const char* home = getenv("HOME");
    const char* xdg = getenv("XDG_DATA_HOME");
    char share[PATH_MAX];
    if (xdg && *xdg) snprintf(share, sizeof share, "%s", xdg);
    else snprintf(share, sizeof share, "%s/.local/share", home ? home : "/tmp");
    snprintf(g_data, sizeof g_data, "%s/wow-bg3", share);
    // bg3le and BG3 itself use $HOME/.local/share for the profile.
    char profile[PATH_MAX];
    snprintf(profile, sizeof profile, "%s/.local/share/Larian Studios/Baldur's Gate 3", home ? home : "/tmp");
    snprintf(g_mods, sizeof g_mods, "%s/Mods", profile);
    snprintf(g_io, sizeof g_io, "%s/Script Extender/wow-bg3", profile);
    snprintf(g_modsettings, sizeof g_modsettings, "%s/PlayerProfiles/Public/modsettings.lsx", profile);

    g_home[0] = 0;
    char cand[PATH_MAX], conf[PATH_MAX + 32];
    const char* env = getenv("WOWBG3_HOME");
    if (env && *env) {
        snprintf(cand, sizeof cand, "%s", env);
        snprintf(conf, sizeof conf, "%s/launcher.conf", cand);
        if (is_file(conf)) {
            snprintf(g_home, sizeof g_home, "%s", cand);
            return;
        }
        WARN("WOWBG3_HOME=%s has no launcher.conf; looking elsewhere", env);
    }
    Dl_info info;
    if (dladdr((void*)&find_paths, &info) && info.dli_fname) {
        snprintf(cand, sizeof cand, "%s", info.dli_fname);
        char* slash = strrchr(cand, '/');
        if (slash) *slash = 0;
        snprintf(conf, sizeof conf, "%s/launcher.conf", cand);
        if (is_file(conf)) {
            snprintf(g_home, sizeof g_home, "%s", cand);
            return;
        }
        char sub[PATH_MAX];
        snprintf(sub, sizeof sub, "%s/WOW-BG3", cand);
        snprintf(conf, sizeof conf, "%s/launcher.conf", sub);
        if (is_file(conf)) {
            snprintf(g_home, sizeof g_home, "%s", sub);
            return;
        }
    }
    snprintf(cand, sizeof cand, "%s/WOW-BG3", g_mods);
    snprintf(conf, sizeof conf, "%s/launcher.conf", cand);
    if (is_file(conf)) snprintf(g_home, sizeof g_home, "%s", cand);
}

// ---- the .pak ----
static int same_file_content(const char* a, const char* b) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0 || sa.st_size != sb.st_size) return 0;
    FILE* fa = fopen(a, "rb");
    FILE* fb = fopen(b, "rb");
    int same = fa && fb;
    static char ba[65536], bb[65536];
    while (same) {
        size_t na = fread(ba, 1, sizeof ba, fa), nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(ba, bb, na) != 0) same = 0;
        if (na == 0) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

static int copy_file(const char* from, const char* to) {
    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof tmp, "%s.wowbg3-tmp", to);
    FILE* in = fopen(from, "rb");
    if (!in) return -1;
    FILE* out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    static char buf[1 << 16];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    ok = ok && !ferror(in);
    fclose(in);
    ok = (fflush(out) == 0) && ok;
    ok = (fsync(fileno(out)) == 0) && ok;
    ok = (fclose(out) == 0) && ok;
    if (!ok || rename(tmp, to) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void ensure_pak(void) {
    if (!g_home[0]) return;
    char src[PATH_MAX + 32], dst[PATH_MAX + 32];
    snprintf(src, sizeof src, "%s/WOW-BG3.pak", g_home);
    snprintf(dst, sizeof dst, "%s/WOW-BG3.pak", g_mods);
    if (!is_file(src)) {
        // Shape (b): the pak already sits beside the folder in Mods.
        s_pak = is_file(dst) ? 2 : 0;
        LOG("pak: %s", s_pak ? "WOW-BG3.pak is in Mods" : "no WOW-BG3.pak in the folder or in Mods");
        return;
    }
    if (strcmp(src, dst) == 0 || same_file_content(src, dst)) {
        s_pak = 2;
        LOG("pak: WOW-BG3.pak in Mods is current");
        return;
    }
    mkdirs(g_mods, 0755);
    int had = is_file(dst);
    if (copy_file(src, dst) == 0) {
        s_pak = 1;
        LOG("pak: %s WOW-BG3.pak in %s", had ? "updated" : "installed", g_mods);
    } else {
        s_pak = 3;
        WARN("pak: could not write %s (%s); copy WOW-BG3.pak into BG3's Mods folder by hand", dst, strerror(errno));
    }
}

static int mod_enabled_now(void) {
    FILE* f = fopen(g_modsettings, "rb");
    if (!f) return 0;
    static char buf[1 << 18];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, MOD_UUID) != NULL;
}

// ---- launcher.conf ----
static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void read_conf(void) {
    if (!g_home[0]) return;
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/launcher.conf", g_home);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    child_t* c = NULL;
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        char* s = trim(line);
        if (!*s) continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            if (!end || g_nchild >= MAX_CHILDREN) {
                WARN("launcher.conf:%d: bad section or too many children", lineno);
                c = NULL;
                continue;
            }
            *end = 0;
            c = &g_child[g_nchild++];
            memset(c, 0, sizeof *c);
            snprintf(c->name, sizeof c->name, "%s", trim(s + 1));
            c->start = START_REQUEST;
            c->grace = 15;
            c->restarts = 3;
            c->want = -1;
            continue;
        }
        char* eq = strchr(s, '=');
        if (!c || !eq) {
            WARN("launcher.conf:%d: ignored", lineno);
            continue;
        }
        *eq = 0;
        char* key = trim(s);
        char* val = trim(eq + 1);
        char x[PATH_MAX * 2];
        expand(val, x, sizeof x);
        if (!strcmp(key, "exec")) snprintf(c->exec, sizeof c->exec, "%s", x);
        else if (!strcmp(key, "arg") && c->argc < MAX_ARGS) c->argv[1 + c->argc++] = strdup(x);
        else if (!strcmp(key, "env") && c->envc < MAX_ENV && strchr(x, '=')) c->env[c->envc++] = strdup(x);
        else if (!strcmp(key, "start")) {
            c->start = !strcmp(x, "enabled") ? START_ENABLED : !strcmp(x, "always") ? START_ALWAYS
                     : !strcmp(x, "never") ? START_NEVER : START_REQUEST;
        } else if (!strcmp(key, "grace")) c->grace = atoi(x);
        else if (!strcmp(key, "restarts")) c->restarts = atoi(x);
        else if (!strcmp(key, "once")) c->once = atoi(x);
        else WARN("launcher.conf:%d: unknown key %s", lineno, key);
    }
    fclose(f);
    for (int i = 0; i < g_nchild; i++) {
        child_t* ch = &g_child[i];
        ch->argv[0] = ch->exec;
        ch->argv[1 + ch->argc] = NULL;
        snprintf(ch->pid_id, sizeof ch->pid_id, "%s_pid", ch->name);
        if (ch->once) {
            char marker[PATH_MAX + 64];
            snprintf(marker, sizeof marker, "%s/run/%s.done", g_data, ch->name);
            ch->done = is_file(marker);
        }
        static const char* rules[] = {"enabled", "request", "always", "never"};
        LOG("child %s: %s (%d args), start %s", ch->name, ch->exec, ch->argc, rules[ch->start]);
    }
}

// The release's environment (6. above), in BG3's process: the render plugin and the mod's Lua read it.
static int is_dir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static void release_env(void) {
    if (!getenv("WOW_BG3_RUN_ID") || !*getenv("WOW_BG3_RUN_ID")) {
        unsigned char r[16] = {0};
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (read(fd, r, sizeof r) != (ssize_t)sizeof r) memset(r, 0, sizeof r);
            close(fd);
        }
        char id[40];
        for (int i = 0; i < 16; i++) snprintf(id + 2 * i, 3, "%02x", r[i] ^ (unsigned char)(i == 0 ? getpid() : 0));
        setenv("WOW_BG3_RUN_ID", id, 1);
        LOG("run id %s (BG3 had none)", id);
    }
    const char* e = getenv("WOWBG3_SHM");
    if (e && *e) snprintf(g_shm, sizeof g_shm, "%s", e);
    else if (is_dir("/wowshm")) snprintf(g_shm, sizeof g_shm, "/wowshm");  // the harness sandbox's mount
    else if (is_dir("/dev/shm") && access("/dev/shm", W_OK) == 0) snprintf(g_shm, sizeof g_shm, "/dev/shm/wow-bg3-%d", (int)getuid());
    else snprintf(g_shm, sizeof g_shm, "%s/shm", g_data);
    char tav[PATH_MAX + 8];
    snprintf(tav, sizeof tav, "%s/tav", g_shm);
    if (strcmp(g_shm, "/wowshm") != 0) {
        mkdirs(g_shm, 0700);
        mkdirs(tav, 0700);
    } else if (is_dir("/wowtav")) {
        snprintf(tav, sizeof tav, "/wowtav");
    }
    e = getenv("WOWBG3_CAPTURE_DIR");
    if (e && *e) snprintf(g_capture, sizeof g_capture, "%s", e);
    else if (is_dir("/logs/capture")) snprintf(g_capture, sizeof g_capture, "/logs/capture");
    else snprintf(g_capture, sizeof g_capture, "%s/capture", g_data);
    mkdirs(g_capture, 0700);
    setenv("WOWBG3_SHM", g_shm, 1);
    setenv("WOWBG3_CAPTURE_DIR", g_capture, 1);
    if (!getenv("WOWBG3_TAV_DIR")) setenv("WOWBG3_TAV_DIR", tav, 1);
    if (!getenv("WOW_BG3_TAV_DIR")) setenv("WOW_BG3_TAV_DIR", tav, 1);
    LOG("frames %s, Tav %s, capture %s", g_shm, getenv("WOWBG3_TAV_DIR"), g_capture);
}

static void json_field(const char* path, const char* key, char* out, size_t cap, double* mtime);

// The coordinator's run/coord.json says Busy (true) and was written in the last 10 s.
static int coord_busy(void) {
    static double next = 0;
    static int last = 0;
    double t = now_s();
    if (t < next) return last;
    next = t + 1.0;
    char path[PATH_MAX + 32], busy[16];
    double m = 0;
    snprintf(path, sizeof path, "%s/run/coord.json", g_data);
    json_field(path, "Busy", busy, sizeof busy, &m);
    struct timespec w;
    clock_gettime(CLOCK_REALTIME, &w);
    last = !strcmp(busy, "true") && (w.tv_sec + w.tv_nsec / 1e9) - m < 10.0;
    return last;
}

// The environment children get: BG3's, minus what belongs to BG3 alone (bg3le's preload, its
// settings, our Vulkan layer), plus where things are.
static void build_env(void) {
    extern char** environ;
    int n = 0;
    for (char** e = environ; *e; e++) n++;
    g_child_envp = calloc(n + 16, sizeof(char*));
    int o = 0;
    static const char* drop[] = {"LD_PRELOAD=", "BG3LE_", "VK_ADD_LAYER_PATH=", "VK_INSTANCE_LAYERS=",
                                 "VK_LOADER_LAYERS_ENABLE=", "WOWBG3_", NULL};
    for (char** e = environ; *e; e++) {
        int skip = 0;
        for (int k = 0; drop[k]; k++)
            if (!strncmp(*e, drop[k], strlen(drop[k]))) skip = 1;
        if (!skip) g_child_envp[o++] = *e;
    }
    char buf[PATH_MAX + 32];
    snprintf(buf, sizeof buf, "WOWBG3_HOME=%s", g_home);
    g_child_envp[o++] = strdup(buf);
    snprintf(buf, sizeof buf, "WOWBG3_DATA=%s", g_data);
    g_child_envp[o++] = strdup(buf);
    snprintf(buf, sizeof buf, "WOWBG3_IO=%s", g_io);
    g_child_envp[o++] = strdup(buf);
    snprintf(buf, sizeof buf, "WOWBG3_LINK=%s/link", g_io);
    g_child_envp[o++] = strdup(buf);
    snprintf(buf, sizeof buf, "WOWBG3_MODS=%s", g_mods);
    g_child_envp[o++] = strdup(buf);
    snprintf(buf, sizeof buf, "WOWBG3_WOW=%s/WoW", g_home);
    g_child_envp[o++] = strdup(buf);
    snprintf(buf, sizeof buf, "WOWBG3_BG3_PID=%d", (int)getpid());
    g_child_envp[o++] = strdup(buf);
    snprintf(buf, sizeof buf, "WOWBG3_SHM=%s", g_shm);
    g_child_envp[o++] = strdup(buf);
    snprintf(buf, sizeof buf, "WOWBG3_CAPTURE_DIR=%s", g_capture);
    g_child_envp[o++] = strdup(buf);
    snprintf(buf, sizeof buf, "WOWBG3_TAV_DIR=%s", getenv("WOWBG3_TAV_DIR") ? getenv("WOWBG3_TAV_DIR") : "");
    g_child_envp[o++] = strdup(buf);
    g_child_envp[o] = NULL;
}

// ---- spawning: clone(CLONE_VM | CLONE_VFORK) so BG3's many gigabytes are not copied ----
typedef struct {
    const char* exec;
    char* const* argv;
    char* const* envp;
    const char* log;
    const char* cwd;
    pid_t parent;
    sigset_t mask;
    int err;
} spawn_t;

static int spawn_child_main(void* arg) {
    spawn_t* s = arg;
    // Own signal table (no CLONE_SIGHAND): defaults, so no BG3 handler runs here.
    for (int i = 1; i < 65; i++) {
        if (i == SIGKILL || i == SIGSTOP) continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_DFL;
        sigaction(i, &sa, NULL);
    }
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != s->parent) _exit(127);
    setpgid(0, 0);
    // Nothing of BG3's open files (GPU, sockets, saves) goes to the WoW side.
    if (syscall(436 /* close_range */, 3, ~0U, 0) != 0) {
        for (int fd = 3; fd < 4096; fd++) close(fd);
    }
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0 && nul != 0) {
        dup2(nul, 0);
        close(nul);
    }
    int fd = open(s->log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
    }
    if (s->cwd && chdir(s->cwd) != 0) {
        s->err = errno;
        _exit(127);
    }
    sigprocmask(SIG_SETMASK, &s->mask, NULL);
    execve(s->exec, s->argv, s->envp);
    s->err = errno;
    _exit(127);
}

static pid_t spawn(child_t* c) {
    char logdir[PATH_MAX + 16], log[PATH_MAX + 64];
    snprintf(logdir, sizeof logdir, "%s/logs", g_data);
    mkdirs(logdir, 0700);
    snprintf(log, sizeof log, "%s/%s.log", logdir, c->name);
    // The child's own env lines first: getenv() returns the first of a name.
    char* envp[512];
    int o = 0;
    for (int i = 0; i < c->envc && o < 500; i++) envp[o++] = c->env[i];
    for (char** e = g_child_envp; *e && o < 510; e++) envp[o++] = *e;
    envp[o] = NULL;
    spawn_t s;
    memset(&s, 0, sizeof s);
    s.exec = c->exec;
    s.argv = c->argv;
    s.envp = envp;
    s.log = log;
    s.cwd = g_home[0] ? g_home : NULL;
    s.parent = getpid();
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &s.mask);
    const size_t stack_size = 256 * 1024;
    char* stack = malloc(stack_size);
    pid_t pid = -1;
    if (stack) pid = clone(spawn_child_main, stack + stack_size, CLONE_VM | CLONE_VFORK | SIGCHLD, &s);
    int clone_errno = errno;
    pthread_sigmask(SIG_SETMASK, &s.mask, NULL);
    free(stack);
    if (pid < 0) {
        WARN("child %s: clone failed: %s", c->name, strerror(clone_errno));
        return -1;
    }
    if (s.err != 0) {
        waitpid(pid, NULL, 0);
        WARN("child %s: cannot run %s: %s", c->name, c->exec, strerror(s.err));
        return -1;
    }
    LOG("child %s: started pid %d (log %s)", c->name, (int)pid, log);
    return pid;
}

// ---- status file ----
static void write_status(const char* why) {
    char run[PATH_MAX + 8], path[PATH_MAX + 32], tmp[PATH_MAX + 40];
    snprintf(run, sizeof run, "%s/run", g_data);
    mkdirs(run, 0700);
    snprintf(path, sizeof path, "%s/launcher.json", run);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\"Version\": \"%s\", \"Format\": 1, \"Why\": \"%s\", \"Home\": \"%s\", \"Data\": \"%s\", "
               "\"ModEnabled\": %d, \"Pak\": %d, \"World\": %d, \"Ready\": %d, \"Holding\": %d, "
               "\"Gate\": %d, \"HoldAt\": %d, \"Children\": [",
            LAUNCHER_VERSION, why, g_home, g_data, s_mod_enabled, s_pak, s_world, s_ready, s_holding,
            g_gate_on, g_hold_mode);
    for (int i = 0; i < g_nchild; i++) {
        child_t* c = &g_child[i];
        fprintf(f, "%s{\"Name\": \"%s\", \"Pid\": %d, \"Want\": %d, \"Stopping\": %d, \"Restarts\": %d, \"Done\": %d}",
                i ? ", " : "", c->name, (int)c->pid, c->want, c->stopping, c->used_restarts, c->done);
    }
    fprintf(f, "]}\n");
    fclose(f);
    rename(tmp, path);
}

// ---- the supervisor thread ----
static atomic_int g_quit;
// <data>/run/stop written since this BG3 started: every child is stopped and stays stopped (a tool's clean
// stop of the WoW side before it ends BG3 from outside, e.g. a test script: a SIGTERM to
// BG3 itself skips exit() and with it the children's clean shutdown).
static int s_stop_all = 0;
static double g_start_wall = 0;
static int desired(child_t* c) {
    if (c->done || s_stop_all) return 0;
    if (c->want == 0) return 0;
    if (c->want == 1) return 1;
    switch (c->start) {
        case START_ALWAYS: return 1;
        case START_ENABLED: return s_mod_enabled;
        default: return 0;
    }
}

static void stop_child(child_t* c, const char* why) {
    if (c->pid <= 0 || c->stopping) return;
    LOG("child %s: stopping pid %d (%s)", c->name, (int)c->pid, why);
    kill(-c->pid, SIGTERM);
    kill(c->pid, SIGTERM);
    c->stopping = 1;
    c->stop_at = now_s();
}

static void* supervisor(void* unused) {
    (void)unused;
    double next_conf_check = 0;
    struct timespec seen = {0, 0};
    for (;;) {
        double t = now_s();
        int changed = 0;
        pthread_mutex_lock(&g_lock);
        if (t >= next_conf_check) {
            next_conf_check = t + 1.0;
            char stopf[PATH_MAX + 16];
            snprintf(stopf, sizeof stopf, "%s/run/stop", g_data);
            struct stat sst;
            if (!s_stop_all && stat(stopf, &sst) == 0 && sst.st_mtim.tv_sec + sst.st_mtim.tv_nsec / 1e9 >= g_start_wall) {
                s_stop_all = 1;
                LOG("run/stop: stopping every child");
                changed = 1;
            }
            struct stat st;
            if (stat(g_modsettings, &st) == 0
                && (st.st_mtim.tv_sec != seen.tv_sec || st.st_mtim.tv_nsec != seen.tv_nsec)) {
                seen = st.st_mtim;
                int en = mod_enabled_now();
                if (en != s_mod_enabled) {
                    LOG("WOW-BG3 is %s in the mod manager", en ? "enabled" : "not enabled");
                    s_mod_enabled = en;
                    changed = 1;
                }
            }
        }
        for (int i = 0; i < g_nchild; i++) {
            child_t* c = &g_child[i];
            if (c->pid > 0) {
                int status = 0;
                pid_t r = waitpid(c->pid, &status, WNOHANG);
                int gone = r == c->pid || (r < 0 && errno == ECHILD && kill(c->pid, 0) != 0);
                if (gone) {
                    if (r == c->pid && WIFEXITED(status)) LOG("child %s: pid %d exited %d", c->name, (int)c->pid, WEXITSTATUS(status));
                    else if (r == c->pid && WIFSIGNALED(status)) LOG("child %s: pid %d killed by signal %d", c->name, (int)c->pid, WTERMSIG(status));
                    else LOG("child %s: pid %d is gone", c->name, (int)c->pid);
                    if (c->once && r == c->pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                        char marker[PATH_MAX + 64];
                        snprintf(marker, sizeof marker, "%s/run/%s.done", g_data, c->name);
                        FILE* mf = fopen(marker, "w");
                        if (mf) fclose(mf);
                        c->done = 1;
                        LOG("child %s: job done (%s)", c->name, marker);
                    }
                    if (!c->stopping && desired(c)) {
                        if (c->used_restarts < c->restarts) {
                            c->used_restarts++;
                            c->next_try = t + 2.0 * c->used_restarts;
                            WARN("child %s exited unexpectedly; restart %d of %d", c->name, c->used_restarts, c->restarts);
                        } else {
                            WARN("child %s exited unexpectedly %d times; not restarting (set %s to 1 to try again)",
                                 c->name, c->used_restarts + 1, c->name);
                            c->want = 0;
                        }
                    }
                    // The rest of its process group, if any, goes with it.
                    kill(-c->pid, SIGTERM);
                    c->pid = 0;
                    c->stopping = 0;
                    changed = 1;
                } else if (c->stopping && t - c->stop_at > c->grace) {
                    WARN("child %s: pid %d ignored SIGTERM for %d s; killing it", c->name, (int)c->pid, c->grace);
                    kill(-c->pid, SIGKILL);
                    kill(c->pid, SIGKILL);
                    c->stop_at = t + 1e9;
                }
            }
            int want = desired(c) && !atomic_load(&g_quit);
            if (want && c->pid == 0 && t >= c->next_try && c->exec[0]) {
                pid_t pid = spawn(c);
                if (pid > 0) c->pid = pid;
                else {
                    c->used_restarts++;
                    c->next_try = t + 5.0;
                    if (c->used_restarts > c->restarts) c->want = 0;
                }
                changed = 1;
            } else if (!want && c->pid > 0 && !c->stopping) {
                stop_child(c, atomic_load(&g_quit) ? "BG3 is closing" : "no longer wanted");
                changed = 1;
            }
            c->pid_setting = (int)c->pid;
        }
        if (changed) write_status("children");
        pthread_mutex_unlock(&g_lock);
        struct timespec nap = {0, 100 * 1000 * 1000};
        nanosleep(&nap, NULL);
    }
    return NULL;
}

// At a clean exit: everything gets SIGTERM, and BG3's exit waits (up to each child's grace, at most
// 15 s) for them to finish their own shutdown (the server flushes its database). Measured in the
// harness: when BG3's process tree goes away, children still running are SIGKILLed with
// it (a pid namespace), so waiting here is what makes a clean quit clean. A crash or SIGKILL of BG3
// ends the supervisor thread and PR_SET_PDEATHSIG sends the same SIGTERM, without the wait.
static void at_exit(void) {
    atomic_store(&g_quit, 1);
    pthread_mutex_lock(&g_lock);  // the supervisor stops reaping; we do it here
    int grace = 0;
    for (int i = 0; i < g_nchild; i++) {
        if (g_child[i].pid > 0) {
            kill(-g_child[i].pid, SIGTERM);
            kill(g_child[i].pid, SIGTERM);
            if (g_child[i].grace > grace) grace = g_child[i].grace;
        }
    }
    if (grace > 15) grace = 15;
    double until = now_s() + grace;
    for (;;) {
        int left = 0;
        for (int i = 0; i < g_nchild; i++) {
            child_t* c = &g_child[i];
            if (c->pid <= 0) continue;
            pid_t r = waitpid(c->pid, NULL, WNOHANG);
            if (r == c->pid || (r < 0 && errno == ECHILD && kill(c->pid, 0) != 0)) c->pid = 0;
            else left++;
        }
        if (!left || now_s() > until) break;
        struct timespec nap = {0, 50 * 1000 * 1000};
        nanosleep(&nap, NULL);
    }
    // Whatever is still there when the grace runs out is killed, group first. BG3 runs inside a container
    // whose own init waits for every process in it, so one survivor keeps the game marked as running in
    // Steam after the player has quit, however the quit came (menu, window close or a crash).
    for (int i = 0; i < g_nchild; i++) {
        child_t* c = &g_child[i];
        if (c->pid <= 0) continue;
        WARN("child %s: pid %d is still there at the exit; killing it", c->name, (int)c->pid);
        kill(-c->pid, SIGKILL);
        kill(c->pid, SIGKILL);
        waitpid(c->pid, NULL, WNOHANG);
        c->pid = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

// ---- the loading-screen hold ----
static double g_hold_since = 0;     // 0: not holding
static int g_hold_gave_up = 0;      // this session's hold timed out
static int g_hold_done = 0;         // this session's hold ended (patch 0004: never held twice in one load)
static double g_session_wall = 0;   // wall clock at the last session boundary (or plugin start)

static double wall_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int release_file_seen(void) {
    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/run/ready", g_data);
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    double m = st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
    // Written in this session (since the last unload or the menu), even before the hold began.
    return m >= g_session_wall;
}

// A small JSON file's raw value for "key" (a string without its quotes, or a number), or "".
static void json_field(const char* path, const char* key, char* out, size_t cap, double* mtime) {
    out[0] = 0;
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (mtime) *mtime = st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    char* at = strstr(buf, pat);
    if (!at) return;
    at = strchr(at + strlen(pat), ':');
    if (!at) return;
    at++;
    while (*at == ' ' || *at == '\t') at++;
    int quoted = *at == '"';
    if (quoted) at++;
    size_t o = 0;
    while (*at && o + 1 < cap && (quoted ? *at != '"' : (*at != ',' && *at != '}' && *at != ' ' && *at != '\n'))) out[o++] = *at++;
    out[o] = 0;
}

// The lifecycle's progress (wow-bg3/lifecycle-state.json {Session, Phase}: down, restoring,
// characters, benilla, world, up, failed), written for this session: since the last unload, and
// for the session hero.json names when both say one. 1 benilla or world, 2 up, -1 failed, 0 not yet.
static int lifecycle_phase(void) {
    static double next = 0;
    static int last = 0;
    double t = now_s();
    if (t < next) return last;
    next = t + 0.25;
    char path[PATH_MAX + 32], phase[32], session[96], hero_session[96];
    double m = 0, hm = 0;
    snprintf(path, sizeof path, "%s/lifecycle-state.json", g_io);
    json_field(path, "Phase", phase, sizeof phase, &m);
    last = 0;
    if (!phase[0] || m < g_session_wall) return last;
    json_field(path, "Session", session, sizeof session, NULL);
    snprintf(path, sizeof path, "%s/hero.json", g_io);
    json_field(path, "Session", hero_session, sizeof hero_session, &hm);
    if (session[0] && hero_session[0] && hm >= g_session_wall && strcmp(session, hero_session) != 0) return last;
    // BG3's server waits in Sync while the client is held (measured), so world mode's
    // ground can only follow after the release: benilla in the world is enough to let BG3 in.
    if (!strcmp(phase, "benilla") || !strcmp(phase, "world")) last = 1;
    else if (!strcmp(phase, "up")) last = 2;
    else if (!strcmp(phase, "failed")) last = -1;
    return last;
}

// World mode's Start done in this session (WorldMode.lua's wow-bg3/world-status.json: Last "Start",
// OK true, written since the last unload): the hero stands on WoW's ground in the void. 1 placed,
// -1 the Start failed (the hold ends at once instead of at its timeout), 0 not yet.
static int ground_placed(void) {
    static double next = 0;
    static int last = 0;
    double t = now_s();
    if (t < next) return last;
    next = t + 0.25;
    char path[PATH_MAX + 32], what[32], ok[16];
    double m = 0;
    snprintf(path, sizeof path, "%s/world-status.json", g_io);
    json_field(path, "Last", what, sizeof what, &m);
    json_field(path, "OK", ok, sizeof ok, NULL);
    last = 0;
    if (m >= g_session_wall && !strcmp(what, "Start")) last = !strcmp(ok, "true") ? 1 : -1;
    return last;
}

static void hold_end(const char* why) {
    double took = now_s() - g_hold_since;
    if (!strcmp(why, "timeout")) {
        WARN("loading screen: held %.1f s and the WoW side is still not ready; letting BG3 in anyway "
             "(hold_timeout %d s)", took, s_hold_timeout);
        g_hold_gave_up = 1;
    } else {
        LOG("loading screen: released after %.1f s (%s)", took, why);
    }
    g_hold_since = 0;
    s_holding = 0;
    g_hold_done = 1;
    write_status(why);
}

// The hold point setting hold_at resolved against what this bg3le can hold.
static int hold_mode(void) {
    int can4 = g_host4 && g_host2->state_gate_can_hold("PrepareRunning", "Running") == 1;
    if (s_hold_at == 0 || s_hold_at == 4) return can4 ? 4 : 1;
    return s_hold_at;
}

static int gate(void* user, const char* from, const char* to) {
    (void)user;
    int holding = g_hold_since > 0;
    if (!holding) g_hold_mode = hold_mode();  // kept for the whole of one hold
    if (strcmp(to, hold_target()) != 0) return 0;
    // Patch 0004: only PrepareRunning -> Running (asked every frame once the client could leave it),
    // never Save -> Running or the like. Else only a level load's queued switch, never the menu's own.
    if (g_hold_mode == 4 && strcmp(from, "PrepareRunning") != 0) return 0;
    // The engine asks again in the frames before it switches: a released hold stays released.
    if (g_hold_mode == 4 && !holding && g_hold_done) return 0;
    if (g_hold_mode != 4 && !holding && strcmp(from, "LoadLevel") != 0 && strcmp(from, "SwapLevel") != 0) return 0;
    double waited = holding ? now_s() - g_hold_since : 0;
    const char* why = NULL;
    // The switch is queued before the mod's Lua knows the save (measured: StopLoading
    // queued ~0.7 s before the session's scripts run), so an unknown session is held too, until
    // WorldFlag says (server, at SessionLoaded) or 15 s pass.
    if (!s_hold) why = "hold off";
    else if (s_world == 0) why = "vanilla game";
    else if (s_world == -1 && !s_mod_enabled) why = "WOW-BG3 not enabled";
    else if (s_world == -1 && waited > (g_hold_mode == 4 ? 5 : 15)) why = "the mod never said whether this is a world-mode game";
    else if (s_ready) why = "ready";
    else if (g_hold_gave_up) why = "gave up earlier this session";
    if (why) {
        if (holding) hold_end(why);
        return 0;
    }
    if (!holding) {
        g_hold_since = now_s();
        s_holding = 1;
        LOG("loading screen: holding %s (world %s)", to, s_world == 1 ? "mode game" : "unknown yet");
        write_status("hold");
        return 1;
    }
    if (s_world != 1) return 1;
    if (release_file_seen()) {
        hold_end("ready file");
        return 0;
    }
    int lc = lifecycle_phase();
    int ground = g_hold_mode == 4 ? ground_placed() : 0;
    if (ground == 1) {
        hold_end("world mode: WoW's ground is placed");
        return 0;
    }
    if (ground == -1) {
        WARN("loading screen: world mode's Start failed (wow-bg3/world-status.json); letting BG3 in");
        hold_end("world mode Start failed");
        return 0;
    }
    if (g_hold_mode != 4 && lc >= 1) {
        hold_end("lifecycle: WoW is in");
        return 0;
    }
    if (lc == -1) {
        WARN("loading screen: the lifecycle reports failed; letting BG3 in");
        hold_end("lifecycle failed");
        return 0;
    }
    if (waited > s_hold_timeout && !(coord_busy() && waited < s_hold_timeout + 900)) {
        hold_end("timeout");
        return 0;
    }
    return 1;
}

// SDL event types (SDL2): keyboard 0x300-0x3ff, mouse 0x400-0x4ff, joystick/controller
// 0x600-0x6ff, touch 0x700-0x7ff. Held from the game while its loading screen is held.
static int on_event(void* user, SDL_Event* event) {
    (void)user;
    if (!s_holding || event == NULL) return 0;
    uint32_t type = *(uint32_t*)event;
    return (type >= 0x300 && type < 0x500) || (type >= 0x600 && type < 0x800);
}

// wow-bg3/bg3-state.json: BG3's client and server game states the moment they change, so the WoW
// side sees a load start at once (UnloadLevel, UnloadSession), not when the new
// session's Lua first runs. Loads counts load starts since BG3 started.
static void write_bg3_state(const char* client, const char* server, int loads, int seq) {
    char path[PATH_MAX + 32], tmp[PATH_MAX + 40];
    snprintf(path, sizeof path, "%s/bg3-state.json", g_io);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\"Format\": 1, \"Client\": \"%s\", \"Server\": \"%s\", \"Loads\": %d, \"Seq\": %d, "
               "\"Holding\": %d, \"World\": %d, \"Time\": %.3f, \"Pid\": %d}\n",
            client ? client : "", server ? server : "", loads, seq, s_holding, s_world, wall_s(), (int)getpid());
    fclose(f);
    rename(tmp, path);
}

// Per client frame: a new session starts unknown; state changes go to bg3-state.json.
static void on_frame(void* user, double dt) {
    (void)user;
    (void)dt;
    static const char* last = NULL;
    static const char* last_server = NULL;
    static int last_holding = 0, loads = 0, seq = 0;
    const char* st = g_client_state ? g_client_state() : NULL;
    const char* sv = g_server_state ? g_server_state() : NULL;
    if (st != last || sv != last_server || s_holding != last_holding) {
        if (st && st != last && (!strcmp(st, "UnloadLevel") || !strcmp(st, "UnloadSession"))
            && !(last && (!strcmp(last, "UnloadLevel") || !strcmp(last, "UnloadSession") || !strcmp(last, "Idle")))) {
            loads++;
        }
        write_bg3_state(st, sv, loads, ++seq);
        last_server = sv;
        last_holding = s_holding;
    }
    if (st && st != last) {
        if (!strcmp(st, "UnloadSession") || !strcmp(st, "UnloadLevel") || !strcmp(st, "Menu")
            || !strcmp(st, "Disconnect")) {
            if (s_world != -1 || s_ready) LOG("session: %s; world and ready reset", st);
            s_world = -1;
            s_ready = 0;
            g_hold_gave_up = 0;
            g_hold_done = 0;
            g_session_wall = wall_s();
        }
        last = st;
    }
}

BG3LE_PLUGIN_EXPORT int bg3le_plugin_init(const bg3le_host* host, bg3le_plugin* self) {
    g_host = host;
    g_self = self;
    if (host->abi != BG3LE_PLUGIN_ABI || host->size < offsetof(host_v2, add_frame_handler) + sizeof(void*)) {
        host->warn(self, "wowbg3-launcher: bg3le's plugin API is too old (needs add_frame_handler)");
        return 1;
    }
    host->describe(self, "wowbg3-launcher", LAUNCHER_VERSION);
    // Only in BG3 itself: bg3le is preloaded into every program BG3 starts, and BG3's crash reporter
    // (bin/LinuxCrashReporter) loaded this plugin and started a second WoW side
    // beside the first (its server fought the first one's for the ports). Test hosts set WOWBG3_ANY_EXE.
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    const char* base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    if (strcmp(base, "bg3") != 0 && !getenv("WOWBG3_ANY_EXE")) {
        host->log(self, "wowbg3-launcher: not BG3 (%s): staying out", exe);
        return 0;
    }
    if (host->size >= offsetof(host_v2, game_state) + sizeof(void*)) g_host2 = (const host_v2*)host;
    g_host4 = host->size >= sizeof(host_v2);

    find_paths();
    mkdirs(g_data, 0700);
    // One WoW side per data folder: a second BG3 (or anything else with this plugin) stays out.
    char lockp[PATH_MAX + 32];
    snprintf(lockp, sizeof lockp, "%s/run", g_data);
    mkdirs(lockp, 0700);
    snprintf(lockp, sizeof lockp, "%s/run/launcher.lock", g_data);
    int lfd = open(lockp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB) != 0) {
        WARN("another BG3 runs the WoW side of %s already: this one stays out", g_data);
        if (lfd >= 0) close(lfd);
        return 0;
    }
    g_session_wall = wall_s();
    g_start_wall = g_session_wall;
    LOG("WOW-BG3 folder: %s; data: %s", g_home[0] ? g_home : "(not found)", g_data);
    release_env();
    ensure_pak();
    s_mod_enabled = mod_enabled_now();
    LOG("WOW-BG3 is %s in modsettings.lsx", s_mod_enabled ? "enabled" : "not enabled");
    read_conf();
    build_env();

    int rc = 0;
    rc |= host->add_setting(self, "world", BG3LE_SETTING_INT, &s_world, -1, 1);
    rc |= host->add_setting(self, "ready", BG3LE_SETTING_INT, &s_ready, 0, 1);
    rc |= host->add_setting(self, "holding", BG3LE_SETTING_INT, &s_holding, 0, 0);
    rc |= host->add_setting(self, "mod_enabled", BG3LE_SETTING_INT, &s_mod_enabled, 0, 0);
    rc |= host->add_setting(self, "pak", BG3LE_SETTING_INT, &s_pak, 0, 0);
    rc |= host->add_setting(self, "hold", BG3LE_SETTING_INT, &s_hold, 0, 1);
    rc |= host->add_setting(self, "hold_timeout", BG3LE_SETTING_INT, &s_hold_timeout, 5, 3600);
    rc |= host->add_setting(self, "hold_at", BG3LE_SETTING_INT, &s_hold_at, 0, 4);
    for (int i = 0; i < g_nchild; i++) {
        rc |= host->add_setting(self, g_child[i].name, BG3LE_SETTING_INT, &g_child[i].want, -1, 1);
        rc |= host->add_setting(self, g_child[i].pid_id, BG3LE_SETTING_INT, &g_child[i].pid_setting, 0, 0);
    }
    // Saved values come back at registration; only hold and hold_timeout are meant to persist.
    int pak = s_pak, en = s_mod_enabled;
    s_world = -1;
    s_ready = 0;
    s_holding = 0;
    s_hold_at = 0;
    s_mod_enabled = en;
    s_pak = pak;
    for (int i = 0; i < g_nchild; i++) {
        g_child[i].want = -1;
        g_child[i].pid_setting = 0;
    }
    rc |= host->add_frame_handler(self, on_frame, NULL);
    rc |= host->add_event_handler(self, on_event, NULL);
    if (rc != 0) return 2;
    if (g_host2) {
        g_client_state = state_via_host_client;
        g_server_state = state_via_host_server;
    } else {
        g_client_state = (const char* (*)(void))dlsym(RTLD_DEFAULT, "_ZN5bg3le17client_game_stateEv");
        g_server_state = (const char* (*)(void))dlsym(RTLD_DEFAULT, "_ZN5bg3le17server_game_stateEv");
    }
    mkdirs(g_io, 0755);
    if (g_host2) {
        if (g_host2->add_state_gate(self, gate, NULL) != 0) {
            WARN("wowbg3-launcher: bg3le could not hook the game-state switch; the loading screen is not held");
        } else {
            g_gate_on = 1;
            g_hold_mode = hold_mode();
            LOG("loading screen hold at %s", g_hold_mode == 4 ? "PrepareRunning -> Running (bg3le patch 0004)"
                                             : hold_target());
        }
    } else {
        WARN("wowbg3-launcher: this bg3le has no state gates (WOW-BG3 patch 0003); the loading screen is not held");
    }
    atexit(at_exit);
    write_status("start");
    pthread_t th;
    if (pthread_create(&th, NULL, supervisor, NULL) != 0) {
        WARN("wowbg3-launcher: no supervisor thread; the WoW side will not start");
        return 3;
    }
    pthread_detach(th);
    LOG("wowbg3-launcher %s ready: %d child(ren), gate %s, states %s", LAUNCHER_VERSION, g_nchild,
        g_gate_on ? "on" : "off", g_client_state ? "seen" : "unknown");
    return 0;
}
