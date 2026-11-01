#!/usr/bin/env python3
"""wowbg3-coord.py: the release coordinator.

Run by the wowbg3-launcher plugin as BG3's child inside Steam's runtime container (launcher.conf
[coord], start = enabled), with the container's python3 (3.9, standard library only). It does in the
release what the PC launchers (link/tools/world-headless.sh, play-world.sh) and their systemd units do
in the harness:

  * first start: waits for the portable server (launcher.conf [server], wowbg3-server.sh run), makes
    the default WoW account (account.env, mode 600) and the fight bridge's creature numbers;
    wow-bg3/host.json (the host level) and window.json (BG3's window size, for the mod's cursor);
  * runs link/tools/lifecycle.py's watcher (Life) in this process: every BG3 load, save, new game and
    death, exactly as in the harness, with the `lc` actions done here instead of by a shell launcher:
      down               world mode's tools and benilla stopped (benilla logs its character out)
      benilla CHAR [ACC] benilla started offscreen and logged in as CHAR (account ACC or the default)
      create SPEC [ACC]  one benilla start that makes NAME,RACE,CLASS,GENDER if missing and logs it in
      world              compose words, the world bridge, the fight bridge (once BG3's client runs),
                         the inventory bridge and the chat command handler
  * stops everything cleanly on SIGTERM (BG3 closing): benilla first (its logout), then the rest.

Folders come from the launcher's environment (relenv.py). Logs: one line per step on stdout, which the
launcher writes to ~/.local/share/wow-bg3/logs/coord.log; every child logs to logs/NAME.log.
Settings: WOW-BG3/settings.conf (`key = value`, see the shipped file).
"""
import argparse, ctypes, json, os, re, secrets, signal, struct, subprocess, sys, threading, time
from pathlib import Path

HOME = Path(os.environ.get('WOWBG3_HOME') or Path(__file__).resolve().parents[1])
TOOLS = HOME / 'tools/link/tools'
sys.path.insert(0, str(TOOLS))
import relenv  # noqa: E402

P = relenv.configure()
import lifecycle  # noqa: E402
import wowsave  # noqa: E402
import wowserver  # noqa: E402
try:
    import wowaccount  # noqa: E402  one hidden WoW account per playthrough
except ImportError:
    wowaccount = None
sys.path.insert(0, str(HOME / 'tools/server/tools'))
WC = __import__('world-console')  # the release's console words (tools/server/tools/world-console.py)

DATA, IO, LINK, SHM, WOW = P['data'], P['io'], P['link'], P['shm'], P['wow']
LOGS = DATA / 'logs'
RUN = DATA / 'run'
CAPTURE = Path(os.environ.get('WOWBG3_CAPTURE_DIR') or DATA / 'capture')
VAR = Path(os.environ['WOWBG3_SERVER_VAR'])
DEFAULT_ACCOUNT = 'WOWBG3'
# The fight bridge's stand-in defaults, as link/tools/bridge.sh.
TEMPLATE = '41da0eff-b533-43cc-a014-218f7d23fa11'
FACTION = 'ACT0a_TUT_LowerDeckDevil_cb3a8427-8890-430c-9e9e-70db3a9de7a8'
CREATURES = """SELECT t.entry, t.name, t.level_min, t.level_max, t.type, t.pet_family, t.`rank`,
 t.unit_class, t.health_multiplier, t.damage_multiplier, t.damage_variance, t.base_attack_time, t.faction,
 t.flags_extra
 FROM creature_template t JOIN (SELECT entry, MAX(patch) AS p FROM creature_template GROUP BY entry) m
 ON m.entry = t.entry AND m.p = t.patch;"""
LEVELS = "SELECT class, level, melee_damage, health FROM creature_classlevelstats;"


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


lifecycle.log = log  # the watcher's lines in the same log


def settings():
    """WOW-BG3/settings.conf: `key = value` lines, # comments."""
    s = {'debug_commands': '1', 'host': 'BGO_Main_A', 'wow_sound': '1', 'view_distance': '',
         'compose': 'auto fxcopy void wowbar ground fxsight surf fxprops fxsolid fxglow', 'inventory': '1', 'mob_abilities': '1', 'companions': '1', 'camp': '1', 'waypoints': '1', 'scaling': '0', 'difficulty': 'balanced', 'wow_spells': '0'}
    try:
        for line in (HOME / 'settings.conf').read_text().splitlines():
            line = line.split('#', 1)[0].strip()
            if '=' in line:
                k, v = line.split('=', 1)
                s[k.strip()] = v.strip()
    except OSError:
        pass
    return s


S = settings()


def write_json(path, body):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(json.dumps(body))
    tmp.replace(path)


def status(phase, **extra):
    """run/coord.json: what the coordinator is doing (Busy holds BG3's loading screen past its timeout
    while the server's first start builds its data, wowbg3-launcher)."""
    write_json(RUN / 'coord.json', dict(extra, Format=1, Phase=phase, Pid=os.getpid(), Time=time.time()))


# ---- children ----
_libc = ctypes.CDLL(None)


def _child_setup():
    os.setsid()
    _libc.prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG: they go with the coordinator


class Procs:
    def __init__(self):
        self.p = {}
        self.lock = threading.Lock()

    def start(self, name, argv, env=None, cwd=None):
        self.stop(name)
        LOGS.mkdir(parents=True, exist_ok=True)
        with open(str(LOGS / ('%s.log' % name)), 'a') as f:
            f.write('---- %s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), ' '.join(map(str, argv))))
            f.flush()
            proc = subprocess.Popen([str(a) for a in argv], stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT,
                                    env=env or os.environ.copy(), cwd=str(cwd or DATA), preexec_fn=_child_setup)
        with self.lock:
            self.p[name] = proc
        log('%s: started pid %d (log logs/%s.log)' % (name, proc.pid, name))
        return proc

    def alive(self, name):
        with self.lock:
            proc = self.p.get(name)
        return proc is not None and proc.poll() is None

    def stop(self, name, grace=8.0, sig=signal.SIGTERM):
        with self.lock:
            proc = self.p.pop(name, None)
        if proc is None or proc.poll() is not None:
            return False
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            return False
        try:
            proc.wait(grace)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(5)
        log('%s: stopped (exit %s)' % (name, proc.returncode))
        return True

    def names(self):
        with self.lock:
            return list(self.p)


PROCS = Procs()


def py(module, *args):
    """argv running a link/tools module configured for the release (relenv.py)."""
    return [sys.executable, '-u', str(TOOLS / 'relenv.py'), module] + [str(a) for a in args]


# ---- the server and the account ----
def server_state():
    """wowbg3-server.sh run's state (var/run/state); "running" only while its mangosd lives (a file left
    by a BG3 that crashed says running too)."""
    try:
        st = (VAR / 'run/state').read_text().strip()
    except OSError:
        return 'none'
    if st == 'running':
        try:
            os.kill(int((VAR / 'run/mangosd.pid').read_text().strip()), 0)
        except (OSError, ValueError):
            return 'starting'
    return st


def wait_server(stop):
    last = None
    while not stop['now']:
        st = server_state()
        if st != last:
            log('server: %s' % st)
            last = st
        if st == 'running':
            return True
        status('server-' + st, Busy=st in ('none', 'init', 'starting'))
        time.sleep(1.0)
    return False


def ports():
    out = {'WORLD_PORT': '8105', 'AUTH_PORT': '3744'}
    try:
        for line in (VAR / 'ports.env').read_text().split():
            k, _, v = line.partition('=')
            out[k] = v
    except OSError:
        pass
    return out


def account_env():
    f = DATA / 'account.env'
    vals = {}
    try:
        vals = dict(l.split('=', 1) for l in f.read_text().split() if '=' in l)
    except OSError:
        pass
    user, pw = vals.get('WOW_USER'), vals.get('WOW_PASS')
    row = wowsave.sh('mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" -N realmd',
                     "select id from account where username = '%s';" % (user or DEFAULT_ACCOUNT)).split()
    if user and pw and row:
        return user, pw
    user, pw = DEFAULT_ACCOUNT, secrets.token_hex(8)
    if row:
        out = wowserver.console('account set password %s %s %s' % (user, pw, pw), 2)
    else:
        out = wowserver.console('account create %s %s' % (user, pw), 2)
    log('default WoW account %s: %s' % (user, 'password renewed' if row else 'made'), out.replace(pw, '<redacted>').strip()[-120:])
    old = os.umask(0o077)
    try:
        f.write_text('WOW_USER=%s\nWOW_PASS=%s\n' % (user, pw))
    finally:
        os.umask(old)
    os.chmod(str(f), 0o600)
    return user, pw


def credentials(account):
    if account and account != DEFAULT_ACCOUNT and wowaccount is not None:
        wowaccount.ensure(account)
        return account, wowaccount.password(account)
    return account_env()


def creature_stats():
    out = DATA / 'creature-stats.json'
    if out.is_file():
        return out

    def q(sql):
        txt = wowsave.sh('mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" --batch --skip-column-names mangos', sql)
        return [l.split('\t') for l in txt.splitlines() if l]
    entries = {}
    for r in q(CREATURES):
        entries[r[0]] = {'name': r[1].replace('\\t', ' ').replace('\\n', ' '), 'level_min': int(r[2]),
                         'level_max': int(r[3]), 'type': int(r[4]), 'family': int(r[5]), 'rank': int(r[6]),
                         'class': int(r[7]), 'health_mult': float(r[8]), 'damage_mult': float(r[9]),
                         'variance': float(r[10]), 'attack_ms': int(r[11]), 'faction': int(r[12]),
                         'flags_extra': int(r[13])}
    levels = {}
    for c, lv, dmg, hp in q(LEVELS):
        levels.setdefault(c, {})[lv] = [float(dmg), int(hp)]
    write_json(out, {'source': 'vmangos creature_template + creature_classlevelstats', 'entries': entries, 'levels': levels})
    log('creature numbers: %d creatures -> %s' % (len(entries), out))
    return out


# ---- BG3's side files ----
def host_json():
    host = S.get('host') or 'BGO_Main_A'
    f = IO / 'host.json'
    try:
        now = json.loads(f.read_text()).get('Region')
    except (OSError, ValueError):
        now = None
    if host == 'TUT_Avernus_C':
        if f.exists():
            f.rename(f.with_name('host.json.off'))
        return
    if now != host:
        write_json(f, {'Region': host})
        log('host level: %s (wow-bg3/host.json)' % host)


def window():
    """window.json {Width, Height} from BG3's own graphicSettings.lsx (the size it renders at)."""
    g = IO.parents[1] / 'graphicSettings.lsx'
    w, h = 1920, 1080
    try:
        t = g.read_text()
        def val(key):
            m = re.search(r'value="%s"/>\s*<attribute id="Type" type="int32" value="0"/>\s*<attribute id="Value" type="int32" value="(\d+)"' % key, t)
            return int(m.group(1)) if m else None
        w, h = val('ScreenWidth') or w, val('ScreenHeight') or h
    except OSError:
        pass
    write_json(IO / 'window.json', {'Width': w, 'Height': h, 'Mode': 'release'})
    return w, h


def bridge_project():
    """The fight bridge runs from PROJECT/link and writes PROJECT/tmp/bridge-arena-*: a tmpfs folder with
    link/tools pointing at the shipped tools."""
    proj = SHM / 'bridge'
    (proj / 'link').mkdir(parents=True, exist_ok=True)
    (proj / 'tmp').mkdir(exist_ok=True)
    t = proj / 'link/tools'
    if not t.is_symlink():
        t.symlink_to(TOOLS)
    return proj


def placed(since):
    """benilla's player is placed in the world (camera.bin flag), written after epoch `since`."""
    f = LINK / 'camera.bin'
    try:
        b = f.read_bytes()
        return len(b) >= 92 and b[:4] == b'WBC1' and struct.unpack_from('<I', b, 88)[0] & 1 and f.stat().st_mtime > since
    except OSError:
        return False


def charcreate(since):
    try:
        d = json.loads((LINK / 'charcreate.json').read_text())
    except (OSError, ValueError):
        return None
    return d if float(d.get('Time') or 0) >= since else None


def game_data():
    """BG3's Data folder: its executable is <game>/bin/bg3."""
    try:
        return Path(os.readlink('/proc/%s/exe' % os.environ.get('WOWBG3_BG3_PID'))).parents[1] / 'Data'
    except (OSError, ValueError):
        return None


def tav_ok():
    """Tav's textures need numpy, Pillow, lz4 and zstandard (the release's own python) and libfastlz.so."""
    if not (TOOLS / 'libfastlz.so').is_file() or not (TOOLS / 'tavtex.py').is_file():
        return False
    try:
        import numpy, PIL.Image, lz4.block, zstandard  # noqa: F401
        return True
    except ImportError:
        return False


def client_running():
    try:
        return json.loads((IO / 'm3-client.json').read_text()).get('GameState') == 'Running'
    except (OSError, ValueError):
        return False


# ---- the lc actions ----
class Release:
    def __init__(self):
        self.size = window()
        self.hero = None
        self.fight_pending = None
        self.tav_pending = False
        self.tav_index = DATA / 'cache/bg3index.json'

    def benilla_env(self, char=None, spec=None, account=None):
        user, pw = credentials(account)
        e = os.environ.copy()
        for k in ('WAYLAND_DISPLAY', 'DISPLAY', 'WOW_CHAR', 'WOW_BG3_CREATE'):
            e.pop(k, None)
        e.update({'WOW_BG3_OFFSCREEN': '1', 'WOW_BG3_LINK_DIR': str(LINK), 'WOW_BG3_LEAD': '1',
                  'WOW_BG3_FRAMES': str(SHM / 'wowframe.bin'),
                  'WOW_BG3_TAV_DIR': os.environ.get('WOWBG3_TAV_DIR') or str(SHM / 'tav'),
                  'WOW_BG3_FX_DIR': str(CAPTURE / 'fx'),  # BG3's effects library (render plugin)
                  'WOW_BG3_M5': '1', 'WOW_MSAA': 'off', 'WOW_WIN': '%dx%d' % self.size, 'WOW_NOVSYNC': '1',
                  'WOW_BG3_TAV_OUT': 'gamma', 'WOW_HOST': '127.0.0.1:%s' % ports()['AUTH_PORT'],
                  'WOW_DATA': str(WOW / 'Data'), 'BENILLA_HOME': str(DATA / 'benilla'), 'WOW_GM': 'off',
                  'WOW_GOD': 'off', 'WOW_USER': user, 'WOW_PASS': pw})
        rt = e.get('XDG_RUNTIME_DIR', '')
        if S.get('wow_sound') != '1' or not (e.get('PULSE_SERVER') or (rt and os.path.exists(rt + '/pulse/native'))):
            e['WOW_NOSOUND'] = '1'
        if S.get('view_distance'):
            e['WOW_FARCLIP'] = S['view_distance']
        if char:
            e['WOW_CHAR'] = char
        if spec:
            e['WOW_BG3_CREATE'] = spec
        return e

    def benilla_start(self, char=None, spec=None, account=None):
        (SHM / 'tav').mkdir(parents=True, exist_ok=True)
        addons = DATA / 'benilla/AddOns'
        addons.mkdir(parents=True, exist_ok=True)
        src = HOME / 'benilla/AddOns'
        if src.is_dir():  # WoW's world-mode HUD (chat, minimap, tracker clear of BG3's HUD)
            subprocess.run(['cp', '-rT', str(src), str(addons)], check=False)
            if S.get('waypoints') == '1':  # the map pins (wow_waypoints.py); the copy above is fresh each start
                with open(addons / 'WowBg3Layout/Settings.lua', 'a') as fh:
                    fh.write('\nWowBg3Layout_Waypoints = true\n')
        conf = DATA / 'benilla/config.toml'
        if not conf.exists():  # benilla picks the saved realm by itself (else it parks at the realm list)
            conf.write_text('# benilla local config (WOW-BG3 release: the local server\'s realm)\n[cvars]\nrealmName = "WOW-BG3"\n')
        return PROCS.start('benilla', [HOME / 'benilla/benilla'], self.benilla_env(char, spec, account), cwd=HOME / 'benilla')

    def world_down(self):
        self.fight_pending = None
        self.tav_pending = False
        for name in ('tavtex', 'chatcmd', 'inventory', 'fight', 'world'):
            PROCS.stop(name, grace=2.0, sig=signal.SIGKILL if name == 'world' else signal.SIGTERM)
        c = CAPTURE / 'compose'
        if c.exists():
            c.replace(CAPTURE / 'compose.off')

    def lc(self, action, *args):
        args = [a for a in args if a]
        if action == 'down':
            self.world_down()
            PROCS.stop('benilla', grace=10.0)
            return 0, 'WoW side down'
        if action == 'benilla':
            char, account = args[0], (args[1] if len(args) > 1 else None)
            if not re.fullmatch(r'[A-Za-z]{2,12}', char):
                return 1, 'lc benilla CHAR'
            PROCS.stop('benilla', grace=10.0)
            t0 = time.time()
            self.benilla_start(char=char, account=account)
            for i in range(90):
                if placed(t0):
                    self.hero = char
                    return 0, 'benilla in the world as %s (%d s)' % (char, i)
                if not PROCS.alive('benilla'):
                    return 1, 'benilla exited before placing %s (logs/benilla.log)' % char
                time.sleep(1)
            return 1, 'benilla did not place %s in 90 s' % char
        if action == 'create':
            spec, account = args[0], (args[1] if len(args) > 1 else None)
            if not re.fullmatch(r'[A-Za-z]{2,12},[0-9]+,[0-9]+,[01]', spec):
                return 1, 'lc create NAME,RACE,CLASS,GENDER'
            name = spec.split(',')[0]
            PROCS.stop('benilla', grace=10.0)
            t0 = time.time()
            self.benilla_start(spec=spec, account=account)
            for i in range(150):
                r = charcreate(t0)
                res = '%s %s %s' % (r.get('Result'), r.get('Code'), r.get('Reason')) if r else 'none'
                if placed(t0):
                    self.hero = name
                    return 0, 'benilla in the world as %s (%d s; %s)' % (name, i, res)
                if r and r.get('Result') in ('refused', 'timeout', 'login-refused', 'invalid'):
                    PROCS.stop('benilla')
                    return 2, 'create %s failed: %s' % (name, res)
                if not PROCS.alive('benilla'):
                    return 1, 'benilla exited (%s; logs/benilla.log)' % res
                time.sleep(1)
            PROCS.stop('benilla')
            return 1, 'benilla did not place %s in 150 s' % name
        if action == 'world':
            return self.world_up()
        return 1, 'lc: down, benilla CHAR [ACCOUNT], create SPEC [ACCOUNT], world'

    def world_up(self):
        CAPTURE.mkdir(parents=True, exist_ok=True)
        (CAPTURE / 'compose').write_text('compose %s\n' % S['compose'])
        for _ in range(60):
            if placed(0):
                break
            time.sleep(2)
        e = os.environ.copy()
        try:
            host = json.loads((IO / 'host.json').read_text()).get('Region')
        except (OSError, ValueError):
            host = None
        if host == 'BGO_Main_A':  # world-extras.sh wx_host_defaults
            e.setdefault('WOW_BG3_WORLD_X', '-2600')
            e.setdefault('WOW_BG3_WORLD_Z', '-1000')
            e.setdefault('WOW_BG3_POOL', '84')
            e.setdefault('WOW_BG3_LAYER', '-1.981,9.74')
        e.setdefault('WOW_BG3_WORLD_X', '-1400')
        e.setdefault('WOW_BG3_WORLD_Y', '120')
        e.setdefault('WOW_BG3_WORLD_Z', '-350')
        e['WOWBG3_CAPTURE_DIR'] = str(CAPTURE)  # Camp: the compose file aside during a trip
        PROCS.start('world', py('world_bridge', '--maps', P['maps']), e)
        self.tav_pending = tav_ok()  # Tav's textures for benilla (tick() starts the baker once its index exists)
        self.fight_pending = time.time()  # started by tick() once BG3's client runs the level
        char = self.hero or 'Test'
        if S.get('inventory') == '1':
            PROCS.start('inventory', py('inventory_bridge', '--char', char))
        chat = TOOLS / 'wowbg3-chatcmd.py'
        if chat.is_file():
            e2 = os.environ.copy()
            e2.update({'WOW_CHAR': char, 'WOWBG3_DEBUG_COMMANDS': '1' if S.get('debug_commands') == '1' else '0',
                       'WOWBG3_WAYPOINTS': '1' if S.get('waypoints') == '1' else '0'})
            PROCS.start('chatcmd', [sys.executable, '-u', chat], e2)
        time.sleep(1.0)
        if not PROCS.alive('world'):
            return 1, 'the world bridge exited at once (logs/world.log)'
        return 0, 'world mode up (fight bridge once BG3 runs the level)'

    def tav_prepare(self):
        """BG3's resource index for the texture baker, once (about a minute), while BG3 is at its menu."""
        if not tav_ok():
            log('Tav textures: off (no numpy/Pillow/lz4/zstandard or libfastlz.so in this python)')
            return
        if self.tav_index.is_file() or game_data() is None:
            return
        self.tav_index.parent.mkdir(parents=True, exist_ok=True)
        PROCS.start('tavindex', [sys.executable, '-u', TOOLS / 'bg3index.py', game_data(), self.tav_index])

    def tav_start(self):
        if PROCS.alive('tavindex') or not self.tav_index.is_file():
            return False
        (DATA / 'cache/tavtex').mkdir(parents=True, exist_ok=True)
        PROCS.start('tavtex', [sys.executable, '-u', TOOLS / 'tavtex.py', LINK / 'tavplan.txt', game_data(),
                               self.tav_index, os.environ.get('WOWBG3_TAV_DIR') or SHM / 'tav', DATA / 'cache/tavtex'])
        return True

    def tick(self):
        if self.tav_pending and PROCS.alive('world') and self.tav_start():
            self.tav_pending = False
        """From the main loop (children are started from the main thread only: PR_SET_PDEATHSIG fires
        when the starting thread ends). The fight bridge needs BG3's client Running (bridge.rs); under the
        held loading screen world mode starts in PrepareRunning (bg3le patch 0004)."""
        if not self.fight_pending:
            return
        if not PROCS.alive('world') or time.time() - self.fight_pending > 600:
            self.fight_pending = None
            return
        if not client_running():
            return
        self.fight_pending = None
        proj = bridge_project()
        PROCS.start('fight', [HOME / 'tools/bin/wow-bg3-link', 'bridge', LINK, IO, '--template', TEMPLATE,
                              '--faction', FACTION, '--stats', creature_stats()], cwd=proj / 'link')


REL = None


# ---- first-run data: the item table, the icon atlas, the quest cursors (from the player's own files) ----
def db_env():
    """The portable server's database for a tool that runs `mariadb` itself (as wowserver.db_sh does)."""
    srv, var = Path(os.environ['WOW_BG3_SERVER']), VAR
    sec = dict(l.split('=', 1) for l in (var / 'secrets.env').read_text().split() if '=' in l)
    e = os.environ.copy()
    e.update(PATH='%s:%s' % (srv / 'mariadb/bin', e.get('PATH', '/usr/bin:/bin')), LD_LIBRARY_PATH=str(srv / 'lib'),
             MYSQL_UNIX_PORT=str(var / 'run/mariadb.sock'), MARIADB_ROOT_PASSWORD=sec['ROOT_PASS'])
    return e


def items_current():
    """items.json is there and of this version's schema (its first key)."""
    try:
        with open(LINK / 'items.json') as f:
            m = re.match(r'\{"Schema":\s*(\d+)', f.read(40))
    except OSError:
        return False
    return bool(m) and int(m.group(1)) >= 5


def datagen_needed():
    return not (items_current() and (LINK / 'abilities.json').is_file() and (IO / 'gui/wow-icons.json').is_file()
                and (IO / 'cursors/quest-turnin.cur').is_file() and (LINK / 'see-noise.r8').is_file() and (LINK / 'spells.json').is_file())


def datagen():
    """`wowbg3-coord.py --datagen`: what the inventory and the quest cursors need, made once from the player's
    WoW files, BG3's paks and the server's database (link/tools wow_items.py, wow_icons.py, wowcursor.py; about
    a minute), and BG3's see-through noise for benilla (seenoise.py, from BG3's paks). BG3 reads the icon atlas
    when it starts, so the icons show from the next start on; benilla picks the noise up while it runs."""
    rc = 0
    steps = []
    if not items_current():
        steps.append(('items', py('wow_items', 'build', '--out', LINK / 'items.json', '--data', WOW / 'Data',
                                  '--db', 'mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" --batch mangos')))
    if not (LINK / 'abilities.json').is_file() and game_data():
        steps.append(('abilities', py('wow_abilities', 'make', LINK / 'abilities.json', '--data', WOW / 'Data', '--bg3', game_data(),
                                      '--db', 'mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" --batch mangos')))
    if not (LINK / 'spells.json').is_file():
        steps.append(('spells', py('wow_spells', '--data', WOW / 'Data', '--out', LINK / 'spells.json')))
    if not (IO / 'gui/wow-icons.json').is_file() and game_data():
        steps.append(('icons', py('wow_icons', 'build', '--data', WOW / 'Data', '--bg3', game_data(), '--out', IO / 'gui',
                                  '--items', LINK / 'items.json')))
    if not (IO / 'cursors/quest-turnin.cur').is_file():
        steps.append(('cursors', py('wowcursor', '--data', WOW / 'Data', '--out', IO / 'cursors')))
    if not (LINK / 'see-noise.r8').is_file() and game_data():
        steps.append(('seenoise', [sys.executable, '-u', TOOLS / 'seenoise.py', '--bg3', game_data(),
                                   '--out', LINK / 'see-noise.r8']))
    env = db_env()
    for name, argv in steps:
        t0 = time.time()
        r = subprocess.run([str(a) for a in argv], env=env, stdin=subprocess.DEVNULL)
        log('datagen %s: exit %d in %.1f s' % (name, r.returncode, time.time() - t0))
        rc |= r.returncode
        if name == 'items' and r.returncode:
            break
    return rc


# ---- movement tiles of the start zones, built ahead ----
# The portable server's first start makes no movement maps; wow_arena.py builds the tiles a ground square
# needs when it first needs them (6 to 10 s each, about a minute cold). The hero's start zones are built
# here once, in the background, right after the server's first start, so a new game's first squares do not
# wait for them under the loading screen. WoW start points (map, x, y) of every race's zone.
STARTS = {'Northshire': (0, -8949.95, -132.49), 'Coldridge': (0, -6240.32, 331.03), 'Deathknell': (0, 1676.35, 1677.45),
          'Shadowglen': (1, 10311.3, 832.46), 'ValleyOfTrials': (1, -618.52, -4251.67), 'Mulgore': (1, -2917.58, -257.98)}


def pretiles():
    """`wowbg3-coord.py --pretiles`: the 3 x 3 movement tiles round each start point (wowbg3-server.sh mmtiles,
    three zones at a time, low priority), then run/pretiles.done."""
    from concurrent.futures import ThreadPoolExecutor
    script = Path(os.environ['WOW_BG3_SERVER']) / 'wowbg3-server.sh'

    def zone(item):
        name, (m, x, y) = item
        tx, ty = int(32 - x / 533.33333), int(32 - y / 533.33333)
        t0 = time.time()
        # wowbg3-server.sh mmtile MAP X Y writes tile MAP Y X: its X is the grid's y index.
        r = subprocess.run(['nice', '-n', '10', 'bash', str(script), 'mmtiles', str(m), str(ty), str(tx), '1'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        log('start tiles %s (map %d, %d %d): exit %d in %.0f s' % (name, m, tx, ty, r.returncode, time.time() - t0))
        return r.returncode
    with ThreadPoolExecutor(3) as ex:
        rcs = list(ex.map(zone, STARTS.items()))
    (RUN / 'pretiles.done').write_text('%s\n' % time.strftime('%Y-%m-%d %H:%M:%S'))
    return 1 if any(rcs) else 0


# ---- the watcher, pointed at the release ----
def bg3_run():
    """The running BG3's run id (the launcher puts WOW_BG3_RUN_ID into BG3 and us), None once BG3 is gone."""
    try:
        os.kill(int(os.environ.get('WOWBG3_BG3_PID') or 0), 0)
    except (OSError, ValueError):
        return None
    return os.environ.get('WOW_BG3_RUN_ID')


def console(args, timeout=30):
    return WC.send(args)


def lc(self, action, *args, timeout=240):
    t0 = time.time()
    try:
        rc, tail = REL.lc(action, *args)
    except Exception as e:  # one failed action is logged; the watcher retries the session
        rc, tail = 1, 'error %r' % e
    log('lc %s %s -> %s in %.1f s: %s' % (action, ' '.join(a for a in args if a), rc, time.time() - t0, tail))
    return rc == 0


def level_watch(self, hero):
    self.level_stop()
    self.level = PROCS.start('wowlevel', py('wowlevel', 'watch', hero))


def level_stop(self):
    PROCS.stop('wowlevel', grace=5.0)
    self.level = None


lifecycle.bg3_run = bg3_run
lifecycle.console = console
lifecycle.Life.lc = lc
lifecycle.Life.level_watch = level_watch
lifecycle.Life.level_stop = level_stop


def main():
    global REL
    stop = {'now': False}
    signal.signal(signal.SIGTERM, lambda *_: stop.update(now=True))
    signal.signal(signal.SIGINT, lambda *_: stop.update(now=True))
    log('wowbg3-coord: WOW-BG3 %s, data %s, run %s, BG3 pid %s' % (HOME, DATA, os.environ.get('WOW_BG3_RUN_ID'),
                                                                   os.environ.get('WOWBG3_BG3_PID')))
    for d in (LOGS, RUN, LINK, CAPTURE, SHM / 'tav'):
        d.mkdir(parents=True, exist_ok=True)
    host_json()
    (IO / 'mob-abilities.json').write_text(json.dumps({'On': S.get('mob_abilities') == '1'}))
    (IO / 'companions.json').write_text(json.dumps({'On': S.get('companions') == '1'}))
    (IO / 'camp.json').write_text(json.dumps({'On': S.get('camp') == '1'}))
    (IO / 'wow-spells.json').write_text(json.dumps({'On': S.get('wow_spells') == '1'}))
    (IO / 'scaling.json').write_text(json.dumps({'On': S.get('scaling') == '1', 'Difficulty': S.get('difficulty')}))
    REL = Release()
    log('window %dx%d, settings %s' % (REL.size + (S,)))
    if not wait_server(stop):
        status('stopped')
        return
    status('accounts')
    account_env()
    REL.tav_prepare()
    if datagen_needed():
        PROCS.start('datagen', [sys.executable, '-u', Path(__file__).resolve(), '--datagen'])
    if not (RUN / 'pretiles.done').exists():
        PROCS.start('pretiles', [sys.executable, '-u', Path(__file__).resolve(), '--pretiles'])
    try:
        creature_stats()
    except Exception as e:
        log('creature numbers not made (%r): fights without WoW damage scaling' % e)
    life = lifecycle.Life(argparse.Namespace(launcher='release', tag='release', char=''))
    status('watching')

    def busy_writer():
        # While the start zones' movement tiles are still being built (the first start), a held loading screen
        # waits for world mode past hold_timeout (coord.json Busy, refreshed every 2 s: wowbg3-launcher wants it
        # under 10 s old; a thread, as a bring-up blocks the main loop for many seconds). Measured:
        # the first New Game of a fresh install needed about 110 s for its first squares' tiles.
        while not stop['now']:
            status('watching', Busy=PROCS.alive('pretiles'))
            time.sleep(2.0)
    threading.Thread(target=busy_writer, daemon=True).start()
    log('lifecycle: watching BG3 (run %s)' % os.environ.get('WOW_BG3_RUN_ID'))
    while not stop['now']:
        try:
            if life.step() == 'exit':
                log('BG3 stopped: lifecycle done')
                break
        except Exception as e:
            log('step error: %r' % e)
            time.sleep(5.0)
        REL.tick()
        if server_state() != 'running':
            log('server: %s; the WoW side waits for it' % server_state())
            life.down('the WoW server stopped')
            if not wait_server(stop):
                break
        time.sleep(0.5)
    stop['now'] = True   # ends the Busy writer
    time.sleep(0.1)
    status('stopping')
    REL.world_down()
    PROCS.stop('benilla', grace=10.0)
    for name in PROCS.names():
        PROCS.stop(name, grace=3.0)
    status('stopped')
    log('stopped')


if __name__ == '__main__':
    if sys.argv[1:] == ['--datagen']:
        sys.exit(datagen())
    if sys.argv[1:] == ['--pretiles']:
        sys.exit(pretiles())
    main()
