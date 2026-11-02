#!/usr/bin/env python3
"""WOW-BG3's bg3le: installs the Script Extender build WOW-BG3 needs.

Shipped as Mods/WOW-BG3/bg3le/install.py with bg3le's own installer (bg3le_install.py, MIT, unchanged), its
launch wrapper (bg3le-launch) and our libbg3le.so: bg3le (github.com/lenonk/bg3le) with WOW-BG3's patches
0003 (plugins from a mod's folder), 0004 (the held loading screen) and those after them (patches/bg3le/).
It does what bg3le's installer does, with our library and without bg3le's console client:

  * copies libbg3le.so and bg3le-launch into ~/.local/share/bg3le (lib/, bin/), as bg3le's installer;
  * puts the wrapper in front of %command% in BG3's Steam launch options (Steam must be closed; it asks
    before stopping Steam, as bg3le's installer does).

    python3 install.py                 install (or update an older bg3le, the same place)
    python3 install.py --uninstall     launch option out, ~/.local/share/bg3le removed (bg3le's own uninstall)
    python3 install.py --dry-run       show what would change
    python3 install.py --files-only    only the files, no Steam change (tests; a launch option set by hand)
"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bg3le_install as b  # noqa: E402  bg3le's own install.py


def main():
    ap = argparse.ArgumentParser(description="Install WOW-BG3's bg3le for Steam's Baldur's Gate 3.")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--files-only", action="store_true")
    args = ap.parse_args()
    target = os.path.join(b.data_home(), "bg3le")
    wrapper = os.path.join(target, "bin", b.WRAPPER_NAME)
    if args.uninstall:
        sys.argv = [sys.argv[0], "--uninstall"] + (["--dry-run"] if args.dry_run else [])
        return b.main()
    files = [(os.path.join(HERE, "libbg3le.so"), os.path.join(target, "lib", "libbg3le.so"), 0o755),
             (os.path.join(HERE, b.WRAPPER_NAME), wrapper, 0o755)]
    for src, _, _ in files:
        if not os.path.isfile(src):
            sys.exit("install: missing %s (a broken WOW-BG3 download?)" % src)
    configs, stopped = [], False
    if not args.files_only:
        roots = b.steam_roots()
        if not roots:
            if b.flatpak_steam():
                sys.exit("install: Flatpak Steam is not supported yet: its sandbox cannot see %s" % target)
            sys.exit("install: no Steam installation found")
        configs = [c for r in roots for c in b.local_configs(r)]
        if not configs:
            sys.exit("install: no Steam user has a localconfig.vdf yet; start Steam once and log in")
        transform = lambda old: b.add_wrapper(old, wrapper)  # noqa: E731
        if not args.dry_run and b.configs_need_edit(configs, transform) and b.steam_running():
            stopped = b.ensure_steam_stopped(roots)
    print("Files, into %s:" % target)
    for src, dst, mode in files:
        print("  %s" % os.path.relpath(dst, target))
        if not args.dry_run:
            b.copy_atomic(src, dst, mode)
    ver = os.path.join(target, "version")
    if not args.dry_run:
        with open(ver, "w") as f:
            f.write("WOW-BG3 build of bg3le (see Mods/WOW-BG3/bg3le/)\n")
    if args.files_only:
        print("Done (files only; the Steam launch option is up to you: \"%s\" %%command%%)" % wrapper)
        return
    print("Steam launch options:")
    b.edit_configs(configs, transform, args.dry_run)
    restart = "\nSteam was stopped; start it again." if stopped else ""
    print(("Done: the next launch of Baldur's Gate 3 from Steam loads bg3le." + restart) if not args.dry_run
          else "Dry run: nothing changed.")


if __name__ == "__main__":
    main()
