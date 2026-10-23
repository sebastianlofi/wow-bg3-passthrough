/* wowbg3-early: a bg3le plugin for things that must be done before the mod's Lua runs
 * (the interaction notes "Cursors").
 *
 * Part 2 (cursor.c) is what it is for: WoW's quest cursors. BG3 builds its SDL cursors before bg3le
 * loads any plugin (this one's log: "SDL_CreateColorCursor ... already called"), so cursor.c swaps
 * BG3's Listen cursors for WoW's in a hooked SDL_SetCursor.
 *
 * Part 1 (here): engine file overrides added at plugin load, about 10 s before the mod's Lua can add
 * them (Ext.IO.AddPathOverride), for files BG3 reads after that point. List:
 *   <Script Extender>/wow-bg3/early-overrides.txt, one per line: GAME_PATH <TAB> FILE
 *   (FILE relative to <Script Extender>/wow-bg3/; a line whose file is missing is skipped).
 * Only paths under Cursors/ and Mods/MainUI/GUI/ are accepted. Measured: BG3's cursors
 * and its GUI pages (Mods/MainUI/GUI/Pages/Examine.xaml) are both read before plugins load, so no
 * mod file uses this list today (kept for files read later). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bg3le_plugin.h"
#include "cursor.h"

#define VERSION "0.1.0"

/* bool in bg3le (C++): only the low byte of the return register is set. */
typedef bool (*add_fn)(const char*, const char*);
typedef const char* (*get_fn)(const char*);

static int allowed(const char* game) {
    return strncmp(game, "Cursors/", 8) == 0 || strncmp(game, "Mods/MainUI/GUI/", 16) == 0;
}

/* The engine's Data root as bg3le makes paths absolute: add a probe, read it back. */
static int data_root(add_fn add, get_fn get, char* out, size_t n) {
    if (!add("WowBg3/early-root-probe", "x")) return 0;
    const char* abs = get("WowBg3/early-root-probe");
    if (abs == NULL) return 0;
    size_t len = strlen(abs);
    if (len < 2 || strcmp(abs + len - 2, "/x") != 0 || len - 2 >= n) return 0;
    memcpy(out, abs, len - 2);
    out[len - 2] = 0;
    return 1;
}

BG3LE_PLUGIN_EXPORT int bg3le_plugin_init(const bg3le_host* host, bg3le_plugin* self) {
    host->describe(self, "wowbg3-early", VERSION);
    add_fn add = (add_fn)dlsym(RTLD_DEFAULT, "bg3le_path_override_add");
    get_fn get = (get_fn)dlsym(RTLD_DEFAULT, "bg3le_path_override_get");
    if (add == NULL || get == NULL) {
        host->warn(self, "wowbg3-early: bg3le has no path overrides; nothing done");
        return 0;
    }
    char se[1024];
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (xdg && *xdg) snprintf(se, sizeof se, "%s/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3", xdg);
    else if (home && *home) snprintf(se, sizeof se, "%s/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3", home);
    else { host->warn(self, "wowbg3-early: no HOME"); return 0; }
    /* Part 2 (cursor.c): WoW's quest cursors when BG3's cursors already exist. */
    cursor_init(host, self, se);
    char list[1200];
    snprintf(list, sizeof list, "%s/early-overrides.txt", se);
    FILE* f = fopen(list, "r");
    if (f == NULL) { host->log(self, "wowbg3-early %s: no %s (no early overrides)", VERSION, list); return 0; }
    char root[1024];
    if (!data_root(add, get, root, sizeof root)) { fclose(f); host->warn(self, "wowbg3-early: no Data root"); return 0; }
    int depth = 0;
    for (const char* p = root; *p; p++) if (*p == '/' && p[1] && p[1] != '/') depth++;
    char line[1024];
    int added = 0, skipped = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* tab = strchr(line, '\t');
        if (tab == NULL) continue;
        *tab = 0;
        const char* game = line;
        const char* file = tab + 1;
        if (!allowed(game) || strstr(file, "..") != NULL || *file == '/') { skipped++; continue; }
        char abs[2048];
        snprintf(abs, sizeof abs, "%s/%s", se, file);
        struct stat st;
        if (stat(abs, &st) != 0 || st.st_size <= 0) { skipped++; continue; }
        /* The override is joined to the Data root: climb out of it to our absolute file. */
        char rel[4096];
        size_t at = 0;
        for (int i = 0; i < depth && at + 3 < sizeof rel; i++) { memcpy(rel + at, "../", 3); at += 3; }
        snprintf(rel + at, sizeof rel - at, "%s", abs[0] == '/' ? abs + 1 : abs);
        if (add(game, rel)) { added++; host->log(self, "wowbg3-early: %s -> %s", game, get(game)); }
        else skipped++;
    }
    fclose(f);
    host->log(self, "wowbg3-early %s: %d override(s) in, %d skipped (root %s)", VERSION, added, skipped, root);
    return 0;
}
