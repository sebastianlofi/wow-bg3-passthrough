"""relenv.py: the harness tools, run from the release.

The tools in link/tools name the development folders (the workspace, the private BG3
profile, the Docker test server). In the release they ship unchanged in WOW-BG3/tools/link/tools/ and
this module points them at the player's folders, from the environment the launcher plugin
(wowbg3-launcher, plugins/launcher) gives every child:

  WOWBG3_HOME   the WOW-BG3 folder (Mods/WOW-BG3)       WOWBG3_DATA   Mods/WOW-BG3/data
  WOWBG3_IO     the mod's IO folder, Mods/WOW-BG3/data/io (Ext.IO's wow-bg3/, bg3le patch 0012)
  WOWBG3_LINK   its link/ folder                         WOWBG3_MODS   BG3's Mods folder (its parent: BG3's profile)
  WOWBG3_SHM    a tmpfs folder (/dev/shm/wow-bg3-UID)    WOWBG3_WOW    the player's WoW 1.12.1 files

  configure()           sets the derived environment (WOW_BG3_SERVER, WOWBG3_SERVER_VAR, WOW_BG3_IO,
                        WOW_VMAP_CACHE, WOW_BG3_MAPS...) and imports and points every tool module
  python3 relenv.py MODULE ARGS...   runs MODULE's main() configured (world_bridge, inventory_bridge,
                        wowlevel, wow_items, ...), as `python3 MODULE.py ARGS...` would on the PC

Only module constants are set here (ROOT, IO, LINK, CELLS, SAVES, SNAP, HERO); the tools' code is the
same in the harness and in the release.
"""
import importlib, inspect, os, sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent            # WOW-BG3/tools/link/tools
ROOT = TOOLS.parents[1]                             # WOW-BG3/tools: ROOT/'link/tools', ROOT/'server/tools'
MODULES = ['wowserver', 'wowsave', 'wowchar', 'wowaccount', 'lifecycle', 'wow_vmap', 'wow_arena',
           'world_bridge', 'inventory_bridge', 'wowlevel', 'wow_mpq', 'wow_items', 'wow_icons', 'wowcursor']


def env(name, default=None):
    v = os.environ.get(name)
    if v:
        return v
    if default is None:
        raise SystemExit('relenv: %s is not set (run by the wowbg3-launcher plugin)' % name)
    return default


def paths():
    home = Path(env('WOWBG3_HOME'))
    data = Path(env('WOWBG3_DATA'))
    io = Path(env('WOWBG3_IO'))
    shm = Path(env('WOWBG3_SHM', str(data / 'shm')))
    wow = Path(env('WOWBG3_WOW', str(home / 'WoW')))
    profile = Path(env('WOWBG3_MODS', str(home.parent))).parent    # BG3's profile: Mods/.., the saves, graphicSettings.lsx
    return {'home': home, 'data': data, 'io': io, 'link': io / 'link', 'shm': shm, 'wow': wow, 'profile': profile,
            'server': home / 'server', 'var': data / 'server', 'snap': data / 'snapshots',
            'saves': profile / 'PlayerProfiles/Public/Savegames/Story', 'cells': shm / 'cells',
            'maps': data / 'server/data/maps', 'vmaps': data / 'server/data/vmaps',
            'mmaps': data / 'server/data/mmaps', 'cache': data / 'cache'}


def configure():
    p = paths()
    e = os.environ
    e.setdefault('WOW_BG3_SERVER', str(p['server']))
    e.setdefault('WOWBG3_SERVER_VAR', str(p['var']))
    e.setdefault('WOW_BG3_IO', str(p['io']))
    e.setdefault('WOW_VMAP_CACHE', str(p['cache'] / 'vmap'))
    e.setdefault('WOW_CUT_CACHE', str(p['cache'] / 'cuts'))
    e.setdefault('WOW_BG3_MAPS', str(p['maps']))
    e.setdefault('WOW_BG3_VMAPS', str(p['vmaps']))
    e.setdefault('WOW_BG3_MMAPS', str(p['mmaps']))
    e.setdefault('WOWBG3_ACCOUNTS_DIR', str(p['data']))
    for d in ('cells', 'snap', 'cache'):
        p[d].mkdir(parents=True, exist_ok=True)
    if str(TOOLS) not in sys.path:
        sys.path.insert(0, str(TOOLS))
    for name in MODULES:
        try:
            m = importlib.import_module(name)
        except ImportError:
            continue
        point(m, p)
    return p


def point(m, p):
    """A tool module's folder constants, the release's way."""
    sets = {'ROOT': ROOT, 'IO': p['io'], 'LINK': p['link'], 'CELLS': p['cells'], 'SAVES': p['saves'],
            'SNAP': p['snap'], 'HERO': p['io'] / 'hero.json', 'MOVE': p['io'] / 'world-move.json',
            'MOVE_RESULT': p['io'] / 'world-move-result.json', 'DATA': p['wow'] / 'Data', 'SE': p['io']}
    if m.__name__ == 'wowsave':
        sets['ROOT'] = p['data']   # its #recycle_bin (pruned snapshots) goes to the data folder
    for k, v in sets.items():
        if hasattr(m, k):
            setattr(m, k, v)


def run(name, args):
    configure()
    m = importlib.import_module(name)
    sys.argv = [str(TOOLS / (name + '.py'))] + list(args)
    main = m.main
    return main(list(args)) if inspect.signature(main).parameters else main()


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    sys.exit(run(sys.argv[1], sys.argv[2:]) or 0)
