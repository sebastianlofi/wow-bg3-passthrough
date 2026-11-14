// wowbg3_shm.h: the folder WoW's frames go through, found the same way by whoever asks first: the launcher plugin
// (release_env) and the render code (path_init), which bg3le may start before the launcher.
//
// Its usual place is /dev/shm/wow-bg3-<uid>/<install>, <install> a key of the mod's own data folder: the uid folder is
// shared by every install of one user id (a Wolf container's beside the desktop's, both 1000), and a leftover of one
// install there (the fight bridge's tools link of another) broke the other's fights. Each install keeps to its own.
// The uid folder's name can be guessed and /dev/shm is everyone's, so a folder of
// that name is used only when it is ours: a real directory (not a link), owned by this user, closed to everyone
// else (one of our own with a looser mode is closed here). A folder somebody else made first is left alone, with
// whatever it holds, and the frames go to a folder of our own in the user's runtime folder instead
// ($XDG_RUNTIME_DIR/wow-bg3: memory too, and nobody else's) or, where there is none, to <data>/shm on disk.
// The same holds where /dev/shm cannot be written at all.
//
//   wowbg3_shm_dir(out, cap, "/dev/shm", getenv("XDG_RUNTIME_DIR"), data, getuid())
//   returns 0: <shm_root>/wow-bg3-<uid>/<key>, 1: <runtime>/wow-bg3/<key>, 2: <data>/shm (not made here: the caller makes it)
//   <key>: 12 hex digits of the data folder's path, its parent resolved (the same for the launcher and the render code
//   however each reached it).
#ifndef WOWBG3_SHM_H
#define WOWBG3_SHM_H

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Makes `path` if it is not there; 1 when it is then a directory of user `me` that nobody else can enter.
static int wowbg3_dir_is_ours(const char* path, uid_t me) {
    struct stat st;
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return 0;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != me) return 0;
    if ((st.st_mode & 077) != 0 && chmod(path, 0700) != 0) return 0;
    return 1;
}

// The install's key: FNV-1a 64 of <realpath of data's parent>/<data's last name>, 12 hex digits.
static void wowbg3_install_key(char out[13], const char* data) {
    char parent[PATH_MAX], res[PATH_MAX], full[PATH_MAX * 2];
    snprintf(parent, sizeof parent, "%s", data);
    size_t n = strlen(parent);
    while (n > 1 && parent[n - 1] == '/') parent[--n] = 0;
    char* sl = strrchr(parent, '/');
    const char* base = sl ? sl + 1 : parent;
    if (sl && sl != parent) {
        *sl = 0;
        snprintf(full, sizeof full, "%s/%s", realpath(parent, res) ? res : parent, base);
    } else snprintf(full, sizeof full, "%s", data);
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char* c = (const unsigned char*)full; *c; c++) { h ^= *c; h *= 0x100000001b3ull; }
    snprintf(out, 13, "%012llx", (unsigned long long)(h & 0xffffffffffffull));
}

static int wowbg3_shm_dir(char* out, size_t cap, const char* shm_root, const char* runtime, const char* data, uid_t me) {
    char p[700], q[720], key[13];
    struct stat st;
    wowbg3_install_key(key, data);
    if (shm_root && stat(shm_root, &st) == 0 && S_ISDIR(st.st_mode) && access(shm_root, W_OK) == 0) {
        snprintf(p, sizeof p, "%s/wow-bg3-%d", shm_root, (int)me);
        snprintf(q, sizeof q, "%s/%s", p, key);
        if (wowbg3_dir_is_ours(p, me) && wowbg3_dir_is_ours(q, me)) {
            snprintf(out, cap, "%s", q);
            return 0;
        }
    }
    if (runtime && *runtime && stat(runtime, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == me) {
        snprintf(p, sizeof p, "%s/wow-bg3", runtime);
        snprintf(q, sizeof q, "%s/%s", p, key);
        if (wowbg3_dir_is_ours(p, me) && wowbg3_dir_is_ours(q, me)) {
            snprintf(out, cap, "%s", q);
            return 1;
        }
    }
    snprintf(out, cap, "%s/shm", data);
    return 2;
}

#endif
