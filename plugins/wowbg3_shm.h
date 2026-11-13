// wowbg3_shm.h: the folder WoW's frames go through, found the same way by whoever asks first: the launcher plugin
// (release_env) and the render code (path_init), which bg3le may start before the launcher.
//
// Its usual place is /dev/shm/wow-bg3-<uid>. That name can be guessed and /dev/shm is everyone's, so a folder of
// that name is used only when it is ours: a real directory (not a link), owned by this user, closed to everyone
// else (one of our own with a looser mode is closed here). A folder somebody else made first is left alone, with
// whatever it holds, and the frames go to a folder of our own in the user's runtime folder instead
// ($XDG_RUNTIME_DIR/wow-bg3: memory too, and nobody else's) or, where there is none, to <data>/shm on disk.
// The same holds where /dev/shm cannot be written at all.
//
//   wowbg3_shm_dir(out, cap, "/dev/shm", getenv("XDG_RUNTIME_DIR"), data, getuid())
//   returns 0: <shm_root>/wow-bg3-<uid>, 1: <runtime>/wow-bg3, 2: <data>/shm (not made here: the caller makes it)
#ifndef WOWBG3_SHM_H
#define WOWBG3_SHM_H

#include <errno.h>
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

static int wowbg3_shm_dir(char* out, size_t cap, const char* shm_root, const char* runtime, const char* data, uid_t me) {
    char p[700];
    struct stat st;
    if (shm_root && stat(shm_root, &st) == 0 && S_ISDIR(st.st_mode) && access(shm_root, W_OK) == 0) {
        snprintf(p, sizeof p, "%s/wow-bg3-%d", shm_root, (int)me);
        if (wowbg3_dir_is_ours(p, me)) {
            snprintf(out, cap, "%s", p);
            return 0;
        }
    }
    if (runtime && *runtime && stat(runtime, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == me) {
        snprintf(p, sizeof p, "%s/wow-bg3", runtime);
        if (wowbg3_dir_is_ours(p, me)) {
            snprintf(out, cap, "%s", p);
            return 1;
        }
    }
    snprintf(out, cap, "%s/shm", data);
    return 2;
}

#endif
