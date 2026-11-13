#!/usr/bin/env python3
"""wowbg3cmd.py: the WOW-BG3 test and party commands, one code for the PC tools and for WoW's chat.
party.sh, tele.sh, faction.sh and server/tools/xprate.sh run this file;
link/tools/wowbg3-chatcmd.py runs the same functions for `/wowbg3 ...` typed in WoW's chat (benilla 0053).

One verb each, no domain word in front:
  wowbg3cmd.py list | add NAME | remove NAME                     the party
  wowbg3cmd.py pool | respec NAME              BG3 levels from the WoW levels
  wowbg3cmd.py tp PLACE | tp ZONE X Y | tp X Y | where | find TEXT
  wowbg3cmd.py faction [horde|alliance|own]
  wowbg3cmd.py refill           HP, spell slots, rage... back to full (test helper)
  wowbg3cmd.py level N          the hero's WoW level (test helper, 1 to 60, also down)
  wowbg3cmd.py xprate [N]       the server's XP rate (kill, elite kill, quest, exploration); no N: show
  wowbg3cmd.py learn SPELL | unlearn SPELL   give the hero a BG3 spell or take it back (test helper)
  wowbg3cmd.py waypoint [list] | waypoint go NAME   fast travel between known flight points (wow_waypoints.py)
  wowbg3cmd.py cross [list | NAME]   test only: at a ship's or zeppelin's dock, over to the dock at its other end
                                     (wow_docks.py). A player asks the guardian at the dock (DockGuide.lua).
  wowbg3cmd.py help
  --json on any party verb: the mod's whole answer

Where things are (each from the environment, else harness A's place on the PC):
  WOWBG3_IO      the mod's IO folder (release: Mods/WOW-BG3/data/io; A: bg3/private/home/.../Script Extender/wow-bg3)
  WOWBG3_LINK    the link folder benilla writes             (WOWBG3_IO/link)
  WOW_CHAR       the hero's WoW character                   (else the one online on a WOW-BG3 account)
  WOW_BG3_SERVER a portable server folder (harness B, the release), its var/ in WOWBG3_SERVER_VAR
                 (else FOLDER/var); unset: the Docker test server (wow-bg3-mangosd-1, wow-bg3-database-1)
Python 3.9, standard library only (it runs in Steam's sniper runtime in the release).
"""
import json
import os
import re
import shlex
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import wowserver  # noqa: E402

ROOT = HERE.parent.parent  # the repo on the PC; WOW-BG3/tools in the release
A_IO = (str(ROOT) + "/bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/"
        "Script Extender/wow-bg3")
DOCKER_CONF = (os.environ.get('WOWBG3_TEST_STACK') or '/opt/stacks/wow-bg3') + '/config/mangosd.conf'
DOCKER_CONF_INSIDE = '/opt/vmangos/config/mangosd.conf'
DOCKER_DBC = (os.environ.get('WOWBG3_TEST_STACK') or '/opt/stacks/wow-bg3') + '/storage/mangosd/extracted-data/5875/dbc'
DOCKER_DB = 'wow-bg3-database-1'
DOCKER_WORLD = 'wow-bg3-mangosd-1'
TEST_ACCOUNT = 'WOWBG3TEST'
PLAYTHROUGH = re.compile(r'WB3P[A-Z0-9]{8}')  # one hidden WoW account per playthrough (wowaccount.py)
XP_KEYS = r'Rate\.XP\.(Kill|Kill\.Elite|Quest|Explore)'  # also an ERE for grep
SETTINGS_HINT = 'setting debug_commands = 1 in WOW-BG3/settings.conf'


class CmdError(Exception):
    """A message for the user; the PC tools exit 1 with it."""


class Usage(CmdError):
    """Wrong words; the PC tools exit 2."""


class Ctx:
    """Where a command runs and where its lines go. out(line, error) prints one answer line."""

    def __init__(self, io=None, link=None, char=None, debug=True, out=None, chat=False):
        self.io = Path(io or os.environ.get('WOWBG3_IO') or A_IO)
        self.link = Path(link or os.environ.get('WOWBG3_LINK') or self.io / 'link')
        self.char = char or os.environ.get('WOW_CHAR') or None
        self.debug = debug
        self.chat = chat  # WoW's chat: shorter lists
        self.out = out or (lambda line, error=False: print(line, file=sys.stderr if error else sys.stdout, flush=True))

    def say(self, line=''):
        self.out(line, False)


def atomic_write(path, text):
    tmp = Path(str(path) + '.tmp')
    tmp.write_text(text)
    os.replace(tmp, path)


# ---------------------------------------------------------------- the WoW server (test or portable)

def server_dir():
    p = os.environ.get('WOW_BG3_SERVER')
    return Path(p) if p else None


def server_var():
    p = server_dir()
    if p is None:
        return None
    v = os.environ.get('WOWBG3_SERVER_VAR')
    return Path(v) if v else p / 'var'


def sql(query, db=''):
    """One query, tab separated rows without a header."""
    p = server_dir()
    if p is None:
        cmd = ['sudo', '-n', 'docker', 'exec', '-i', DOCKER_DB, 'sh', '-c',
               'mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" -N -B %s' % db]
        env = None
    else:
        v = server_var()
        sec = dict(l.split('=', 1) for l in (v / 'secrets.env').read_text().split() if '=' in l)
        cmd = [str(p / 'mariadb/bin/mariadb'), '--socket=%s' % wowserver.db_socket(v), '-uroot', '-N', '-B'] + ([db] if db else [])
        env = dict(os.environ, LD_LIBRARY_PATH=str(p / 'lib'), MYSQL_PWD=sec['ROOT_PASS'])
    r = subprocess.run(cmd, input=query, capture_output=True, text=True, env=env)
    if r.returncode:
        raise CmdError('database: %s' % (r.stderr.strip()[-200:] or r.returncode))
    return r.stdout


def console(line, seconds=2.5):
    """One line to mangosd's console (wowserver.py: Docker attach, or the portable server's pipe)."""
    return wowserver.console(line, seconds)


def dbc_dir():
    v = server_var()
    return str(v / 'data/5875/dbc') if v else DOCKER_DBC


def name_ok(name):
    if not re.fullmatch(r'[A-Za-z]{2,12}', name or ''):
        raise CmdError('a WoW character name, letters only')
    return name


def wow_bg3_accounts():
    """The WoW accounts WOW-BG3 makes its characters on: the default account (the test account on the
    harnesses) and every playthrough account wowaccount.py made, one per New Game (its accounts.json)."""
    out = {TEST_ACCOUNT}
    try:
        import wowaccount
        d = str(wowaccount.default_user() or '')
        if re.fullmatch(r'[A-Za-z0-9_]{2,32}', d):
            out.add(d.upper())
        out.update(str(u).upper() for u in (wowaccount.load().get('Accounts') or {}))
    except Exception:  # noqa: BLE001  (no registry reachable here: the names below still hold)
        pass
    return out


def is_wow_bg3_account(user):
    """Whether that account is one of WOW-BG3's. A playthrough account counts by its own name too, so a
    registry this tool cannot read does not lock the commands out."""
    u = str(user or '').upper()
    return bool(u) and (u in wow_bg3_accounts() or PLAYTHROUGH.fullmatch(u) is not None)


def online_characters():
    """[(character, account)] for every WoW character online now."""
    out = []
    for line in sql("select c.name, a.username from characters c join realmd.account a on a.id = c.account "
                    "where c.online = 1;", 'characters').splitlines():
        p = line.split('\t')
        if len(p) == 2:
            out.append((p[0], p[1]))
    return out


def own_character(name):
    """The account that owns NAME, refused when it is not one of WOW-BG3's: never change a WoW character
    that is not WOW-BG3's (every playthrough has a hidden account of its own, wowaccount.py)."""
    rows = sql("select a.username from characters c join realmd.account a on a.id = c.account "
               "where c.name = '%s';" % name_ok(name), 'characters').split()
    if not rows:
        raise CmdError('no WoW character %s' % name)
    if not is_wow_bg3_account(rows[0]):
        raise CmdError('%s is on account %s, which is not one of WOW-BG3\'s (the default account, or the '
                       'account made for a playthrough)' % (name, rows[0]))
    return rows[0]


def hero(ctx):
    """The hero's WoW character: WOW_CHAR (the chat helper passes the typing character), else the one
    online character on a WOW-BG3 account (wowlevel.py's rule)."""
    if ctx.char:
        return name_ok(ctx.char)
    ours = [c for c, a in online_characters() if is_wow_bg3_account(a)]
    if len(ours) != 1:
        raise CmdError('name the WoW character (WOW_CHAR; online on a WOW-BG3 account: %s)' % (', '.join(ours) or 'none'))
    return ours[0]


def wow_level(name):
    """(level, online); an online character is written to the database first (saveall)."""
    q = "select level, online from characters where name = '%s';" % name_ok(name)
    row = sql(q, 'characters').split()
    if not row:
        raise CmdError('no WoW character %s' % name)
    if row[1] == '1':
        console('saveall', 1.5)
        time.sleep(0.5)
        row = sql(q, 'characters').split()
    return int(row[0]), row[1] == '1'


def wow_progress(name):
    """(XP into this WoW level, XP the next level needs) from WoW's own numbers (characters.xp and the
    server's player_xp_for_level); (0, 0) at the last level or when the table is not there."""
    try:
        row = sql("select c.xp, coalesce((select p.xp_for_next_level from mangos.player_xp_for_level p "
                  "where p.lvl = c.level), 0) from characters.characters c where c.name = '%s';"
                  % name_ok(name)).split()
        xp, nxt = int(row[0]), int(row[1])
    except Exception:  # noqa: BLE001  (an old server without the table: no percent, the level still works)
        return 0, 0
    return (max(0, min(xp, nxt)), nxt) if nxt > 0 else (0, 0)


def write_level(ctx, name, level, online):
    """wow-bg3/wow-level.json, what Progression.lua reads (wowlevel.py once): the level, and how far into
    it WoW's XP stands (Percent, 0 to 99.9; BG3 shows it in its own XP bar)."""
    xp, nxt = wow_progress(name)
    body = {'Character': name, 'Level': level, 'Online': online, 'Xp': xp, 'XpNext': nxt,
            'Percent': min(round(100.0 * xp / nxt, 1), 99.9) if nxt > 0 else 0.0,
            'Seq': int(time.time() * 1000)}
    atomic_write(ctx.io / 'wow-level.json', json.dumps(body))
    return body


# ---------------------------------------------------------------- party (Party.lua through party-command.json)

PARTY_ACTIONS = {'list': 'List', 'add': 'Add', 'remove': 'Remove', 'refill': 'Refill', 'pool': 'Pool',
                 'respec': 'Reset'}
PARTY_HELP = [
    'list - the party and where they are',
    'add NAME - recruit (shadowheart, astarion, laezel, wyll, karlach, gale, jaheira, minsc, halsin, minthara)',
    'remove NAME - dismiss (it keeps its BG3 levels; they stay spent until a respec)',
    'pool - your WoW level, party slots, BG3 level-ups left',
    'respec NAME - Withers\' respec: back to BG3 level 1, its level-ups back to the pool',
]
# The two level words, said once and reused: nobody should have to guess which game a level belongs to.
LEVEL_HINT = ('"level" is your WoW level: each one from 5 on earns one BG3 level-up, which BG3\'s own Level Up '
              'button offers on every party member with room (level whichever you want); "pool" shows what is left')


def party_usage():
    return '\n'.join(PARTY_HELP + ['refill - HP, spell slots, rage... back to full (test command)'])


def live_run(ctx, action):
    """BG3's RunId, refused when no game with the mod would answer a command."""
    io = ctx.io
    try:
        run = json.load(open(io / 'm3-server.json'))['RunId']
    except (OSError, ValueError, KeyError):
        raise CmdError('%s: no BG3 run with the mod (wow-bg3/m3-server.json)' % action)
    # Party.lua rewrites party.json at least every 300 ticks while a game runs: without a fresh one for
    # this run, BG3 is not running (or sits in its menu, or is loading), and nothing would answer.
    try:
        live = json.load(open(io / 'party.json')).get('RunId') == run and time.time() - os.stat(io / 'party.json').st_mtime < 40
    except (OSError, ValueError):
        live = False
    if not live:
        raise CmdError('%s: BG3 is not running a game with the mod (no fresh wow-bg3/party.json)' % action)
    return run


def party(ctx, args, as_json=False):
    action = args[0].lower() if args else ''
    if action not in PARTY_ACTIONS:
        raise Usage(party_usage())
    name = args[1] if len(args) > 1 else ''
    if action not in ('list', 'refill', 'pool') and not name:
        raise Usage('%s NAME (e.g. hero, shadowheart, astarion)' % action)
    count = 1
    if action in ('add', 'pool'):
        # The WoW level the mod counts with, fresh from the server first (quiet: the mod keeps the last
        # one it had when the server is not there).
        try:
            n = hero(ctx)
            write_level(ctx, n, *wow_level(n))
            time.sleep(0.6)
        except Exception:  # noqa: BLE001
            pass
    io = ctx.io
    run = live_run(ctx, action)
    path = io / 'party-command.json'
    try:
        old = json.load(open(path))
        seq = old.get('Sequence', 0) if old.get('RunId') == run else 0
    except (OSError, ValueError):
        seq = 0
    seq += 1
    atomic_write(path, json.dumps({'RunId': run, 'Sequence': seq, 'Action': PARTY_ACTIONS[action], 'Name': name,
                                   'Count': count}))
    res = None
    for _ in range(240):
        time.sleep(0.25)
        try:
            r = json.load(open(io / 'party-result.json'))
        except (OSError, ValueError):
            continue
        if r.get('RunId') == run and r.get('Sequence') == seq:
            res = r
            break
    if res is None:
        raise CmdError('%s: no answer from the mod in 60 s (is BG3 running a game with the mod?)' % action)
    if as_json:
        ctx.say(json.dumps(res, indent=1))
        if not res.get('OK'):
            raise CmdError('')
        return
    if not res.get('OK'):
        raise CmdError('%s: %s' % (action, res.get('Value')))
    v = res.get('Value') or {}
    say = ctx.say

    def members(party_):
        for m in party_ or []:
            state = 'dead' if m.get('Dead') else ('downed' if m.get('Downed') else '%s/%s HP' % (m.get('Hp'), m.get('MaxHp')))
            p = m.get('Pos') or []
            where = ' at %.0f %.0f %.0f' % tuple(p) if len(p) == 3 else ''
            say('  %s%s: %s, %s%s%s' % (m.get('Name'), ' (hero)' if m.get('Hero') else '', state, m.get('Region'),
                                        where, ', in combat' if m.get('InCombat') else ''))

    def show_pool(p):
        pct = p.get('WowPercent')
        say('WoW level %s%s%s: %s party slot%s (party of %s), level-ups earned %s, given %s, left %s' % (
            p.get('WowLevel'), ' of %s' % p['WowMax'] if p.get('WowMax') else '',
            '' if pct is None else ' (%g%% into it)' % pct,
            p.get('Slots'), '' if p.get('Slots') == 1 else 's', p.get('PartySize'), p.get('Earned'),
            p.get('Spent'), p.get('Available')))
        for m in p.get('Members') or []:
            up = (m.get('Available') or 0) - (m.get('Level') or 0)
            say('  %s%s: BG3 level %s, %s level-up%s given, XP %s%s' % (
                m.get('Name'), ' (hero)' if m.get('Hero') else '', m.get('Level'), m.get('LevelUps'),
                '' if m.get('LevelUps') == 1 else 's', m.get('Xp'),
                (', %d waiting: BG3\'s Level Up button' % up) if up > 0 else ''))
        for m in p.get('Others') or []:
            say('  %s (not in the party): %s level-up%s given' % (m.get('Name'), m.get('LevelUps'),
                                                                 '' if m.get('LevelUps') == 1 else 's'))
        say('  BG3 XP table ends at level %s; cap %s%s' % (
            p.get('MaxXpLevel'), p.get('GrantCap'),
            '' if p.get('XpOff') else '; BG3\'s own XP is NOT off yet'))

    if action == 'pool':
        show_pool(v)
    elif action == 'respec':
        say('%s: %s' % (v.get('Name'), v.get('Respec')))
        show_pool(v.get('Pool') or {})
    elif action == 'refill':
        for m in v.get('Refilled') or []:
            say('%s%s: HP %s -> %s/%s' % (m.get('Name'), ' (hero)' if m.get('Hero') else '', m.get('HpBefore'),
                                         m.get('Hp'), m.get('MaxHp')))
            say('  was under max: ' + (', '.join(m.get('LowBefore') or []) or 'nothing'))
            if m.get('TopUp'):
                say('  topped up after BG3\'s restore: ' + ', '.join(m['TopUp']))
            if m.get('LowAfter'):
                say('  still under max: ' + ', '.join(m['LowAfter']))
    elif action == 'list':
        say('Party (BG3 limit %s%s):' % (v.get('MaxPartySize'), ', full' if v.get('Full') else ''))
        members(v.get('Party'))
        say('Companions this tool knows:')
        for k in v.get('Known') or []:
            say('  %-11s %s, %s%s%s' % (k.get('Key'), k.get('Name'), k.get('Region'), ', in the party' if k.get('InParty') else '',
                                        ', dead' if k.get('Dead') else ''))
    else:
        say('%s %s' % (v.get('Added') or v.get('Removed'), 'joined the party' if action == 'add' else 'left the party'))
        for s in v.get('Steps') or []:
            say('  - ' + s)
        say('Party now:')
        members(v.get('Party'))


# -------------------------------------------------- tp, where, find (the world bridge's world-move.json)

CONT = {0: 'Eastern Kingdoms', 1: 'Kalimdor'}  # the continents world mode cuts ground from
TELE_USAGE = 'tp ZONE X Y | tp X Y | tp PLACE | where | find TEXT (X and Y from 0 to 100)'


_MAP_NAMES = None


def map_names():
    """WoW's own names of its maps, from the player's client data (Map.dbc, 1.12: id, internal name, type,
    battleground flag, then the name by language, English first): a dungeon, a raid, the tram."""
    global _MAP_NAMES
    if _MAP_NAMES is None:
        _MAP_NAMES = {}
        try:
            import wow_waypoints
            for i, _, text in wow_waypoints.read_dbc(Path(dbc_dir()) / 'Map.dbc'):
                _MAP_NAMES[i[0]] = text(i[4]) or text(i[1])
        except (OSError, ValueError, ImportError, IndexError):
            pass
    return _MAP_NAMES


def cont(m):
    return CONT.get(m) or map_names().get(m) or 'map %d' % m


def wdbc(name):
    b = open(os.path.join(dbc_dir(), name), 'rb').read()
    magic, n, f, rs, ss = struct.unpack_from('<4s4I', b, 0)
    assert magic == b'WDBC', name
    strs = b[20 + n * rs:]

    def text(o):
        return strs[o:strs.find(b'\0', o)].decode('utf-8', 'replace')
    return [(struct.unpack_from('<%dI' % f, b, 20 + i * rs), struct.unpack_from('<%df' % f, b, 20 + i * rs), text)
            for i in range(n)]


def zones():
    """AreaTable.dbc (1.12): id, map, parent zone, the area's value in the server's map files, enUS name at field 11."""
    return [{'id': i[0], 'map': i[1], 'parent': i[2], 'bit': i[3], 'name': t(i[11])} for i, _, t in wdbc('AreaTable.dbc')]


def maps():
    """WorldMapArea.dbc (1.12): id, map, area, name, then the map's edges in world yards: left (+y),
    right (-y), top (+x), bottom (-x)."""
    return [{'map': i[1], 'area': i[2], 'name': t(i[3]), 'left': f[4], 'right': f[5], 'top': f[6], 'bottom': f[7]}
            for i, f, t in wdbc('WorldMapArea.dbc') if i[2]]


def to_world(m, mx, my):
    # As the server's Zone2MapCoordinates: map X runs along world y (left to right), map Y along world x.
    return m['top'] + my * (m['bottom'] - m['top']) / 100.0, m['left'] + mx * (m['right'] - m['left']) / 100.0


def to_map(m, x, y):
    return (y - m['left']) * 100.0 / (m['right'] - m['left']), (x - m['top']) * 100.0 / (m['bottom'] - m['top'])


def find_zone(text):
    t = text.lower()
    zs = [z for z in zones() if z['name']]
    for test in (lambda z: z['name'].lower() == t, lambda z: z['name'].lower().startswith(t), lambda z: t in z['name'].lower()):
        hits = [z for z in zs if test(z)]
        if hits:
            hits.sort(key=lambda z: (z['parent'] != 0, z['map'] not in CONT, z['name']))  # zones before subzones
            return hits[0]
    return None


def map_of(zone):
    area = zone['parent'] or zone['id']  # a subzone uses its zone's map, as in game
    for m in maps():
        if m['area'] == area:
            return m
    return None


def at_camp(io):
    """Camp.lua's record of a trip (wow-bg3/camp-state.json) says this BG3 run is at camp. World.move refuses
    from there too: camp's teleport is BG3's own story, and a Stop there would lose the way back."""
    try:
        camp = json.load(open(io / 'camp-state.json'))
        return camp.get('InCamp') is True and camp.get('RunId') == json.load(open(io / 'm3-client.json')).get('RunId')
    except (OSError, ValueError):
        return False


def tele(ctx, args, at=None):
    """`at` (x, y, z, map, what): a move decided elsewhere (a waypoint), its height to be snapped to a floor."""
    if not args:
        raise Usage(TELE_USAGE)
    link, io = ctx.link, ctx.io

    def player():
        return struct.unpack_from('<3f', open(link / 'camera.bin', 'rb').read(), 72)

    def player_map():
        return struct.unpack_from('<I', open(link / 'camera.bin', 'rb').read(), 84)[0]

    def here():
        x, y, _ = player()
        pm = player_map()
        inside = [m for m in maps() if m['map'] == pm and m['bottom'] <= x <= m['top'] and m['right'] <= y <= m['left']]
        inside.sort(key=lambda m: (m['top'] - m['bottom']) * (m['left'] - m['right']))
        return (inside[0] if inside else None), x, y

    cache = {}

    def all_places():
        if 'p' not in cache:
            out = []
            rows = sql('SELECT name, map, position_x, position_y, position_z FROM game_tele WHERE map IN (0, 1) ORDER BY name;', 'mangos')
            for line in rows.splitlines():
                p = line.split('\t')
                if len(p) == 5:
                    out.append((p[0], int(p[1]), float(p[2]), float(p[3]), float(p[4])))
            cache['p'] = out
        return cache['p']

    def places(text=''):
        """Named places (game_tele) on both continents: the exact name, else names containing text."""
        t = text.lower()
        out = all_places()
        exact = [q for q in out if q[0].lower() == t]
        return exact or [q for q in out if t in q[0].lower()]

    def num(s):
        try:
            v = float(s)
        except ValueError:
            return None
        return v if 0.0 <= v <= 100.0 else None

    if args[0] == 'find':
        text = ' '.join(args[1:]).lower()
        cap = 10 if ctx.chat else None
        zs = [z for z in zones() if text in z['name'].lower()]
        ctx.say('Zones (tp ZONE X Y):')
        for z in zs[:cap]:
            ctx.say('  %-28s %s%s' % (z['name'], cont(z['map']), '' if not z['parent'] else ', in zone %d' % z['parent']))
        if cap and len(zs) > cap:
            ctx.say('  ... %d more' % (len(zs) - cap))
        cap = 15 if ctx.chat else 60
        ps = places(text)
        ctx.say('Named places (tp PLACE):')
        for name, m, *_ in ps[:cap]:
            ctx.say('  %-28s %s' % (name, cont(m)))
        if ctx.chat and len(ps) > cap:
            ctx.say('  ... %d more' % (len(ps) - cap))
        return
    if args[0] == 'where':
        try:
            m, x, y = here()
        except OSError:
            raise CmdError('where: no WoW position yet (link/camera.bin)')
        if not m:  # a dungeon, a raid: WoW has no zone map there, its map's own name says where
            ctx.say('%s (WoW %.1f %.1f)' % (cont(player_map()), x, y))
            return
        mx, my = to_map(m, x, y)
        ctx.say('%s %.1f %.1f (WoW %.1f %.1f, %s)' % (m['name'], mx, my, x, y, cont(m['map'])))
        return

    z = None
    if at:
        x, y, z, mapid, what = at
    elif len(args) == 1:
        hits = places(args[0])
        if not hits:
            raise CmdError('tp: no named place like %r (find TEXT lists them)' % args[0])
        name, mapid, x, y, z = hits[0]
        if mapid not in CONT:
            raise CmdError('tp: %s is on %s; world mode has Eastern Kingdoms and Kalimdor' % (name, cont(mapid)))
        what = name
    elif len(args) == 2 and num(args[0]) is not None and num(args[1]) is not None:
        m, _, _ = here()
        if not m:
            raise CmdError('tp: you are on no zone map; name the zone (tp ZONE X Y)')
        x, y = to_world(m, num(args[0]), num(args[1]))
        mapid = m['map']
        what = '%s %s %s' % (m['name'], args[0], args[1])
    elif len(args) >= 3 and num(args[-2]) is not None and num(args[-1]) is not None:
        zone = find_zone(' '.join(args[:-2]))
        if not zone:
            raise CmdError('tp: no zone like %r (find TEXT lists them)' % ' '.join(args[:-2]))
        if zone['map'] not in CONT:
            raise CmdError('tp: %s is on %s; world mode has Eastern Kingdoms and Kalimdor' % (zone['name'], cont(zone['map'])))
        mapid = zone['map']
        m = map_of(zone)
        if not m:
            raise CmdError('tp: %s has no map in WoW' % zone['name'])
        x, y = to_world(m, num(args[-2]), num(args[-1]))
        what = '%s %s %s' % (zone['name'], args[-2], args[-1])
    else:
        raise Usage(TELE_USAGE)

    # World mode must run, and not in a fight or at camp (asked first: Camp.lua freezes tav.bin there).
    if at_camp(io):
        raise CmdError('tp: not from camp: leave camp first')
    # Right after a load world mode is still coming up: wait for it (bridge start and WoW login).
    for wait in range(61):
        try:
            run = json.load(open(io / 'm3-client.json'))['RunId']
            w = json.load(open(link / 'world.json'))
            active = w.get('Active') and time.time() - os.stat(link / 'tav.bin').st_mtime < 5
        except (OSError, ValueError, KeyError):
            active = False
        if active:
            break
        if wait == 60:
            raise CmdError('tp: world mode is not running')
        time.sleep(1)
    raw = open(link / 'tav.bin', 'rb').read()
    if len(raw) >= 44 and struct.unpack_from('<I', raw, 36)[0] & 1:
        raise CmdError('tp: not during a BG3 fight')

    path = io / 'world-move.json'
    try:
        old = json.load(open(path))
        seq = old.get('Seq', 0) if old.get('RunId') == run else 0
    except (OSError, ValueError):
        seq = 0
    seq += 1
    body = {'RunId': run, 'Seq': seq, 'Wow': [x, y, z], 'What': what, 'Map': mapid}
    if at:
        body['Snap'] = True
    cross = mapid != player_map()
    if cross:
        # The other continent: WoW's character goes first to the named place there nearest the point
        # (a server teleport; world-console.py takes names of letters and digits only).
        # An offline name would "work": the console only rewrites that character's saved position.
        char = hero(ctx)
        if char not in [c for c, _ in online_characters()]:
            raise CmdError('tp: WoW character %s is not online (for the server teleport to %s)' % (char, cont(mapid)))
        via = [q for q in places() if q[1] == mapid and q[0].isalnum()]
        if not via:
            raise CmdError('tp: no named place on %s to go through' % cont(mapid))
        via.sort(key=lambda q: (q[2] - x) ** 2 + (q[3] - y) ** 2)
        body.update(Char=char, Via=via[0][0])
    atomic_write(path, json.dumps(body))
    # The screen answers the click at once; the world bridge takes the cover off when it is done.
    flag = ctx.link / 'wowmove.flag'  # MoveLoading.lua: BG3's screen fade
    try:
        flag.write_text('%.3f\n' % time.time())
    except OSError:
        pass
    ctx.say('moving to %s (WoW %.1f %.1f, %s%s)...' % (what, x, y, cont(mapid), ', through %s' % body['Via'] if cross else ''))
    res = None
    for _ in range(600 if cross else 240):
        time.sleep(0.5)
        try:
            r = json.load(open(io / 'world-move-result.json'))
        except (OSError, ValueError):
            continue
        if r.get('RunId') == run and r.get('Seq') == seq:
            res = r
            break
    if not res:
        flag.unlink(missing_ok=True)
        raise CmdError('tp: no answer from the world bridge in time (see its log)')
    if not res.get('OK'):
        raise CmdError('tp: ' + str(res.get('Value')))
    # benilla puts WoW's character on the hero within a few seconds.
    for _ in range(30):
        time.sleep(0.5)
        px, py, pz = player()
        if player_map() == mapid and ((px - x) ** 2 + (py - y) ** 2) ** 0.5 < 15.0:
            ctx.say('there: WoW %.1f %.1f %.1f' % (px, py, pz))
            return
    px, py, _ = player()
    raise CmdError('tp: world mode moved, but WoW\'s character is still %.0f yd away (dead in WoW? a dead '
                   'character is not moved: revive it, then try again)' % (((px - x) ** 2 + (py - y) ** 2) ** 0.5))


# ---------------------------------------------------------------- waypoints (wow_waypoints.py: the rules)

WAYPOINT_USAGE = 'waypoint [list] | waypoint go NAME (a name or its number from the list)'


def waypoints_on():
    try:
        import wow_waypoints
    except ImportError:  # a build without it: the other commands must still work
        return False
    return wow_waypoints.enabled()


def taxi_state(name):
    """(known-taxi mask text, race, on a WoW flight now, dead), an online character written to the database first.
    The columns by what they hold: the name differs between server versions."""
    wow_level(name)
    cols = [l.split('\t')[0] for l in sql("show columns from characters like '%taxi%';", 'characters').splitlines()]
    mask = next((c for c in cols if 'mask' in c), None)
    path = next((c for c in cols if 'path' in c), None)
    if not mask:
        raise CmdError('waypoint: this server keeps no flight points in its characters table')
    row = sql("select race, health, `%s`, %s from characters where name = '%s';"
              % (mask, '`%s`' % path if path else "''", name_ok(name)), 'characters').rstrip('\n').split('\t')
    if len(row) != 4 or not row[0].isdigit():
        raise CmdError('no WoW character %s' % name)
    return row[2], int(row[0]), row[3].strip() not in ('', 'NULL'), row[1] == '0'


def waypoint(ctx, args):
    if not waypoints_on():
        raise CmdError('waypoints are off (waypoints = 1 in WOW-BG3/settings.conf turns them on)')
    import wow_waypoints
    action = args[0].lower() if args else 'list'
    if action not in ('list', 'go', 'pins') or (action == 'go') != (len(args) > 1):
        raise Usage(WAYPOINT_USAGE)
    mask, race, flying, dead = taxi_state(hero(ctx))
    team = wow_waypoints.team_of(race)
    if not team:
        raise CmdError('waypoint: no team for race %d' % race)
    every = wow_waypoints.nodes(dbc_dir())
    points = wow_waypoints.waypoints(every, mask, team)
    if action == 'pins':  # for the WoW layout addon, which keeps these lines out of the chat
        rows = wow_waypoints.pins(points, wow_waypoints.map_rects(dbc_dir()))
        for f, i, x, y, name in rows:
            ctx.say('WBWP;pin;%s;%d;%.2f;%.2f;%s' % (f, i, x, y, name))
        ctx.say('WBWP;end;%d' % len(rows))
        # The map just opened: BG3's own panel beside it lists what WoW knows right now.
        atomic_write(ctx.link / 'waypoint-panel.json',
                     json.dumps({'Points': wow_waypoints.panel(every, mask, team)}))
    elif action == 'list':
        ctx.say('Waypoints you have discovered (%s; a flight master\'s window adds its own):' % team)
        for p in points:
            ctx.say('  %d %s (%s)' % (p['id'], p['name'], cont(p['map'])))
        if not points:
            ctx.say('  none yet')
    else:
        text = ' '.join(args[1:])
        hits = wow_waypoints.choose(points, text)
        if not hits:
            raise CmdError('waypoint: you have discovered no flight point like %r ("waypoint" lists them)' % text)
        if len(hits) > 1:
            raise CmdError('waypoint: which one? ' + '; '.join(p['name'] for p in hits[:6]))
        if flying:
            raise CmdError('waypoint: not on a WoW flight')
        if dead:
            raise CmdError('waypoint: not while dead in WoW')
        p = hits[0]
        try:
            tele(ctx, ['waypoint'], at=(p['x'], p['y'], p['z'], p['map'], p['name']))
        except CmdError as e:
            raise CmdError(re.sub(r'^tp: ', 'waypoint: ', str(e)))


CROSS_USAGE = 'cross | cross NAME (where two vessels stop at one dock) | cross list'


def vessel(r):
    """What a route's vessel is. The server's own names for them are its makers' ("TEST Ship"): what it is says more."""
    import wow_docks
    return 'ship' if r['Stops'][0]['Z'] < wow_docks.SEA else 'zeppelin'


def dock_key(r, s):
    """A crossing's name between this helper and BG3's side: the vessel's taxi path and the stop it goes to."""
    return '%d:%d' % (r['Path'], r['Stops'].index(s))


def wow_here(ctx):
    """(map, x, y) of WoW's character, as the client last wrote it (link/camera.bin)."""
    try:
        raw = open(ctx.link / 'camera.bin', 'rb').read()
        x, y, _ = struct.unpack_from('<3f', raw, 72)
        return struct.unpack_from('<I', raw, 84)[0], x, y
    except (OSError, struct.error):
        raise CmdError('cross: no WoW position yet (link/camera.bin)')


_DOCKS = {'routes': None, 'stand': {}, 'name': {}}


def dock_stand(r, i):
    """Where a player stands at a route's stop (wow_docks.landing), worked out once; None where nothing is found."""
    import wow_docks
    if (r['Path'], i) not in _DOCKS['stand']:
        s = r['Stops'][i]
        _DOCKS['stand'][(r['Path'], i)] = wow_docks.landing(s['Map'], s['X'], s['Y'], s['Z'])
    return _DOCKS['stand'][(r['Path'], i)]


def dock_name(r, s):
    """A stop's place as the server names it where a player stands at it: the pier ("Auberdine, Darkshore"), not
    the water the vessel lies in beside it ("The Long Wash, Darkshore")."""
    key = dock_key(r, s)
    if key not in _DOCKS['name']:
        at = dock_stand(r, r['Stops'].index(s))
        _DOCKS['name'][key] = zone_at(s['Map'], at[0], at[1]) if at else zone_at(s['Map'], s['X'], s['Y'])
    return _DOCKS['name'][key]


def dock_panel(ctx):
    """dock-panel.json for the guardian at a dock (DockGuide.lua): the crossings from the docks within reach of
    WoW's character, each with the place a player stands at this end's pier and the place at the other end by the
    server's own name. The routes, the standing places and the names never change: each is worked out once."""
    import wow_docks
    try:
        here_map, x, y = wow_here(ctx)
    except CmdError:
        return
    if _DOCKS['routes'] is None:
        _DOCKS['routes'] = crossings()
    docks = []
    for r, i, _ in wow_docks.near(_DOCKS['routes'], here_map, x, y):
        at = dock_stand(r, i)
        if not at:
            continue
        for s in wow_docks.other(r, i):
            # Only a crossing that can be made: no offer to a stop where no place to stand is found.
            if not dock_stand(r, r['Stops'].index(s)):
                continue
            docks.append({'Key': dock_key(r, s), 'Name': dock_name(r, s), 'Kind': vessel(r), 'Wow': [round(v, 2) for v in at[:3]]})
    atomic_write(ctx.link / 'dock-panel.json', json.dumps({'Map': here_map, 'Docks': docks}))


def crossings():
    """WoW's ships and zeppelins as routes between docks (wow_docks.py): the server's type 15 game objects and the
    client's taxi paths."""
    import wow_docks
    rows = [l.split('\t') for l in sql('SELECT entry, name, data0 FROM gameobject_template WHERE type=15;', 'mangos').splitlines() if l.count('\t') == 2]
    return wow_docks.routes(rows, wow_docks.read_nodes(dbc_dir()))


def zone_at(map_id, x, y):
    """A point's place as the server names it: "Ratchet, The Barrens", or the zone alone, or the continent where
    the map files name no area (wow_docks.area_bit)."""
    import wow_docks
    import wow_stand
    bit = wow_docks.area_bit(wow_stand.data_dir('WOW_BG3_MAPS', 'maps'), map_id, x, y)
    zs = zones()
    area = next((z for z in zs if z['map'] == map_id and z['bit'] == bit and z['name']), None)
    if not area:
        return cont(map_id)
    zone = next((z for z in zs if z['id'] == area['parent'] and z['name']), None)
    return '%s, %s' % (area['name'], zone['name']) if zone else area['name']


def cross(ctx, args):
    """BG3 has no ship to ride in our level, so a crossing is a move: standing at a ship's or a zeppelin's dock,
    the party goes to the dock at the vessel's other end, as it does to a flight point."""
    import wow_docks
    kind = vessel
    if len(args) > 1 and args[0].lower() == 'list':
        raise Usage(CROSS_USAGE)
    routes = crossings()
    if args and args[0].lower() == 'list':
        ctx.say('Ships and zeppelins (the guardian at a dock takes you across):')
        for r in routes:
            ctx.say('  %s (%s)' % (' - '.join(dock_name(r, s) for s in r['Stops']), kind(r)))
        return
    here_map, x, y = wow_here(ctx)
    ways = [(r, s) for r, i, _ in wow_docks.near(routes, here_map, x, y) for s in wow_docks.other(r, i)]
    if not ways:
        raise CmdError('cross: no ship or zeppelin stops within %d yards of you ("cross list" names them all)' % wow_docks.REACH)
    names = ['%s (%s)' % (dock_name(r, s), kind(r)) for r, s in ways]
    if args and args[0].startswith('#'):
        # The guardian's "yes" (dock-click.json): the crossing by its key, never by a name that two might share.
        pick = [k for k, (r, s) in enumerate(ways) if '#' + dock_key(r, s) == args[0]]
        if len(pick) != 1:
            raise CmdError('cross: that vessel does not stop here')
        k = pick[0]
    elif args:
        text = ' '.join(args).lower()
        pick = [k for k, n in enumerate(names) if text in n.lower()]
        if len(pick) != 1:
            raise CmdError('cross: %s here: %s' % ('no crossing like %r' % text if not pick else 'which one?', '; '.join(names)))
        k = pick[0]
    elif len(ways) > 1:
        raise CmdError('cross: where to? ' + '; '.join(names) + ' (cross NAME)')
    else:
        k = 0
    r, s = ways[k]
    at = dock_stand(r, r['Stops'].index(s))
    if not at:
        raise CmdError('cross: no place to stand found at the dock at %s' % zone_at(s['Map'], s['X'], s['Y']))
    try:
        tele(ctx, ['cross'], at=(at[0], at[1], at[2], s['Map'], 'the dock at ' + names[k]))
    except CmdError as e:
        raise CmdError(re.sub(r'^tp: ', 'cross: ', str(e)))


def waypoint_panel(ctx):
    """waypoint-panel.json for BG3's own panel (Waypoints.lua). Nothing written when waypoints are off."""
    if not waypoints_on():
        return
    import wow_waypoints
    mask, race, _, _ = taxi_state(hero(ctx))
    team = wow_waypoints.team_of(race)
    if team:
        rows = wow_waypoints.panel(wow_waypoints.nodes(dbc_dir()), mask, team)
        atomic_write(ctx.link / 'waypoint-panel.json', json.dumps({'Points': rows}))


# ---------------------------------------------------------------- faction (the fight bridge's faction.txt)

def faction(ctx, args):
    f = ctx.link / 'faction.txt'
    if not args:
        try:
            parts = f.read_text().split()
            ctx.say(parts[1] if len(parts) > 1 else 'own')
        except OSError:
            ctx.say('own')
        return
    side = args[0].lower()
    if side not in ('horde', 'alliance', 'own') or len(args) > 1:
        raise Usage('faction horde | alliance | own (own: back to your race\'s side); faction: what is set now')
    ctx.link.mkdir(parents=True, exist_ok=True)
    cmd = ctx.link / 'server-cmd.txt'

    def sent():
        n = 0
        try:
            for line in cmd.read_text(errors='replace').splitlines():
                if '.wowbg3 faction ' in line:
                    try:
                        n = int(line.split()[0])
                    except (ValueError, IndexError):
                        pass
        except OSError:
            pass
        return n
    before = sent()
    try:
        seq = int(f.read_text().split()[0])
    except (OSError, ValueError, IndexError):
        seq = 0
    atomic_write(f, '%d %s\n' % (seq + 1, side))
    if os.environ.get('FACTION_NOWAIT'):
        ctx.say('saved (%s)' % side)
        return
    # The bridge passes it on within a second while it runs (its line in server-cmd.txt).
    for _ in range(20):
        if sent() > before:
            ctx.say('sent: your WoW character now counts as %s' % side)
            return
        time.sleep(0.5)
    ctx.say('saved (%s): the bridge sends it when world mode runs' % side)


# ---------------------------------------------------------------- time (for recording)

DAYTIMES = {'day': 720, 'noon': 720, 'dawn': 360, 'dusk': 1140, 'night': 0, 'midnight': 0}


def daytime(ctx, args):
    """link/daytime.txt: benilla's world light holds at this minute of the day (patch 0099); `now` lets go."""
    f = ctx.link / 'daytime.txt'
    if not args:
        try:
            m = int(f.read_text().strip())
            ctx.say('held at %02d:%02d' % (m // 60, m % 60))
        except (OSError, ValueError):
            ctx.say('now: the time of this device')
        return
    word = args[0].lower()
    hhmm = re.fullmatch(r'(\d{1,2})(?::(\d\d))?', word)
    if word == 'now':
        text = 'now'
    elif word in DAYTIMES:
        text = str(DAYTIMES[word])
    elif hhmm and int(hhmm.group(1)) < 24 and int(hhmm.group(2) or 0) < 60:
        text = str(int(hhmm.group(1)) * 60 + int(hhmm.group(2) or 0))
    else:
        raise Usage('time day | night | dawn | dusk | HH:MM | now (now: back to the time of this device); time: what is set')
    ctx.link.mkdir(parents=True, exist_ok=True)
    atomic_write(f, text + '\n')
    ctx.say('back to the time of this device' if text == 'now'
            else 'the world holds at %02d:%02d, for you only' % (int(text) // 60, int(text) % 60))


# ---------------------------------------------------------------- level (test helper)

def level(ctx, args, wow_bg3_only=False):
    if len(args) != 1 or not args[0].isdigit() or not 1 <= int(args[0]) <= 60:
        raise Usage('level N (1 to 60, also down): your WoW level, not a BG3 one; ' + LEVEL_HINT)
    name = hero(ctx)
    if wow_bg3_only:
        own_character(name)  # a character of this playthrough's own account, or of the default account
    out = console('character level %s %d' % (name, int(args[0])))
    lvl, online = wow_level(name)
    ctx.say('%s: WoW level %d%s' % (name, lvl, '' if lvl == int(args[0]) else ' (asked %s; console said: %s)' % (args[0], out.strip()[-200:])))
    if (ctx.io / 'wow-level.json').exists():
        write_level(ctx, name, lvl, online)


# ---------------------------------------------------------------- xp rate (test helper)

def xp_conf():
    v = server_var()
    return v / 'etc/mangosd.conf' if v else Path(DOCKER_CONF)


def xp_seen():
    """The rate lines the server reads."""
    if server_dir() is None:
        r = subprocess.run(['sudo', '-n', 'docker', 'exec', DOCKER_WORLD, 'grep', '-E', '^%s *=' % XP_KEYS, DOCKER_CONF_INSIDE],
                           capture_output=True, text=True)
        text = r.stdout
    else:
        text = xp_conf().read_text()
    return [m.group(0) for m in re.finditer(r'^%s *=.*$' % XP_KEYS, text, re.M)]


def xp(ctx, args):
    """Kill, elite kill, quest and exploration XP times N, in the server's own config, then `reload config`
    (no restart, nobody kicked). The Docker test server's file is a single-file bind mount: it is written in
    place (same inode), a copy first to #recycle_bin/xprate-TIME/. The portable server's var/etc/mangosd.conf
    is the player's own; its first state is kept once as mangosd.conf.before-xp."""
    if not args:
        ctx.say('server sees:')
        for line in xp_seen():
            ctx.say(line)
        return
    n = args[0]
    if len(args) > 1 or not re.fullmatch(r'[0-9]{1,3}(\.[0-9]+)?', n) or float(n) <= 0:
        raise Usage('xprate N: a number, e.g. 20 or 1 (1 = normal); xprate: show')
    conf = xp_conf()
    if server_dir() is None:
        old = subprocess.run(['sudo', '-n', 'cat', str(conf)], capture_output=True, text=True, check=True).stdout
        new = re.sub(r'^(%s) *=.*$' % XP_KEYS, lambda m: 'Rate.XP.%s = %s' % (m.group(2), n), old, flags=re.M)
        keep = ROOT / '#recycle_bin' / ('xprate-' + time.strftime('%Y%m%d-%H%M%S'))
        keep.mkdir(parents=True, exist_ok=True)
        (keep / 'mangosd.conf').write_text(old)
        (keep / 'mangosd.conf.new').write_text(new)
        subprocess.run(['sudo', '-n', 'tee', str(conf)], input=new, capture_output=True, text=True, check=True)
    else:
        old = conf.read_text()
        new = re.sub(r'^(%s) *=.*$' % XP_KEYS, lambda m: 'Rate.XP.%s = %s' % (m.group(2), n), old, flags=re.M)
        first = conf.with_name('mangosd.conf.before-xp')
        if not first.exists():
            first.write_text(old)
        with open(conf, 'r+') as fh:  # in place, as the Docker one
            fh.write(new)
            fh.truncate()
    seen = xp_seen()
    ctx.say('server sees:')
    for line in seen:
        ctx.say(line)
    if len(seen) != 4 or any(not re.search(r'= *%s$' % re.escape(n), l) for l in seen):
        raise CmdError('the server still sees other values: not reloaded')
    out = console('reload config', 2)
    ok = [l for l in out.splitlines() if re.search(r'reloaded|Re-Loading', l)]
    ctx.say(ok[-1].strip() if ok else 'reload config sent')
    ctx.say('XP rate now %sx (kill, elite kill, quest, exploration)' % n)


# ---------------------------------------------------------------- learn, unlearn (test helper, Learn.lua)

LEARN_ACTIONS = {'learn': 'Learn', 'unlearn': 'Unlearn'}


def learn(ctx, verb, args):
    """Through learn-command.json, its own files: party-command and stand-in-command are one job at a time and
    the fight sends its own through them, so this one cannot disturb a fight."""
    if len(args) != 1 or not re.fullmatch(r'\w{1,128}', args[0], re.A):
        raise Usage('%s SPELL: the name in BG3\'s stats, e.g. %s Target_Grease' % (verb, verb))
    spell = args[0]
    run = live_run(ctx, verb)
    path = ctx.io / 'learn-command.json'
    try:
        old = json.load(open(path))
        seq = old.get('Sequence', 0) if old.get('RunId') == run else 0
    except (OSError, ValueError):
        seq = 0
    seq += 1
    atomic_write(path, json.dumps({'RunId': run, 'Sequence': seq, 'Action': LEARN_ACTIONS[verb], 'Spell': spell}))
    for _ in range(80):
        time.sleep(0.25)
        try:
            r = json.load(open(ctx.io / 'learn-result.json'))
        except (OSError, ValueError):
            continue
        if r.get('RunId') == run and r.get('Sequence') == seq:
            break
    else:
        raise CmdError('%s: no answer from the mod in 20 s (is BG3 running a game with the mod?)' % verb)
    if not r.get('OK'):
        raise CmdError('%s: %s' % (verb, r.get('Value')))
    if verb == 'unlearn':
        ctx.say('the hero no longer has %s' % spell)
    elif (r.get('Value') or {}).get('Already'):
        ctx.say('the hero already has %s' % spell)
    else:
        ctx.say('the hero now has %s; saving the game keeps it' % spell)


# ---------------------------------------------------------------- chat dispatch

# One verb each, no domain word in front and no second name for anything.
VERBS = tuple(PARTY_ACTIONS) + tuple(LEARN_ACTIONS) + ('tp', 'where', 'find', 'faction', 'time', 'level', 'xprate',
                                                       'waypoint', 'cross', 'report')
# cross: a player crosses by the guardian at the dock (DockGuide.lua); the typed command is for tests.
TEST_ONLY = ('tp', 'where', 'find', 'faction', 'time', 'refill', 'level', 'xprate', 'cross') + tuple(LEARN_ACTIONS)


def verb_of(words):
    """One command line, split into words, as (verb, arguments)."""
    if not words:
        return 'help', []
    verb, rest = words[0].lower(), words[1:]
    if verb == 'help':
        return 'help', rest
    if verb not in VERBS:
        raise Usage('unknown command %r; "help" lists them' % words[0])
    return verb, rest


def do(ctx, verb, args, as_json=False, wow_bg3_only=False):
    """One command whose verb is known and whose gate has passed."""
    if verb in PARTY_ACTIONS:
        party(ctx, [verb] + args, as_json)
    elif verb == 'tp':
        tele(ctx, args)
    elif verb in ('where', 'find'):
        tele(ctx, [verb] + args)
    elif verb == 'faction':
        faction(ctx, args)
    elif verb == 'time':
        daytime(ctx, args)
    elif verb == 'report':
        import crashreport
        ctx.say('Send this one file: %s' % str(crashreport.make(' '.join(args) or 'asked')).replace(str(Path.home()), '~'))
    elif verb == 'waypoint':
        waypoint(ctx, args)
    elif verb == 'cross':
        cross(ctx, args)
    elif verb == 'level':
        level(ctx, args, wow_bg3_only=wow_bg3_only)
    elif verb in LEARN_ACTIONS:
        learn(ctx, verb, args)
    else:
        xp(ctx, args)


def help_lines(debug):
    out = ['commands (type them in WoW\'s chat), one word each:']
    out += ['/wowbg3 ' + l for l in PARTY_HELP]
    if waypoints_on():
        out.append('/wowbg3 waypoint | waypoint go NAME - the flight points you have discovered; fast travel to one')
    out.append('/wowbg3 help - this list')
    if debug:
        out += ['test commands:',
                '/wowbg3 refill - HP, spell slots, rage... back to full',
                '/wowbg3 tp PLACE | tp ZONE X Y | tp X Y - move on the WoW map (X Y from 0 to 100)',
                '/wowbg3 where | find TEXT - where you are; search places',
                '/wowbg3 cross | cross NAME | cross list - at a ship\'s or zeppelin\'s dock: across without the guardian there',
                '/wowbg3 faction horde | alliance | own - your WoW side (faction: what is set)',
                '/wowbg3 report - one file with the logs, to send when something is wrong',
                '/wowbg3 time day | night | dawn | dusk | HH:MM | now - hold the light, for recording (now: this device\'s time)',
                '/wowbg3 level N - set your WoW level, 1 to 60 (WoW, not BG3: it fills the pool)',
                '/wowbg3 xprate N - the server\'s XP rate (1 normal); xprate: show',
                '/wowbg3 learn SPELL | unlearn SPELL - give the hero a BG3 spell by its stats name (Target_Grease), or take it back']
    else:
        out.append('test commands (tp, where, find, faction, time, refill, level, xprate, learn, unlearn) are off: %s turns '
                   'them on' % SETTINGS_HINT)
    out.append(LEVEL_HINT)
    return out


def run(ctx, text):
    """One chat line (the words after /wowbg3)."""
    try:
        words = shlex.split(text)
    except ValueError:
        words = text.split()
    verb, args = verb_of(words)
    if verb == 'help':
        for l in help_lines(ctx.debug):
            ctx.say(l)
        return
    if verb in TEST_ONLY and not ctx.debug:
        raise CmdError('"%s" is a test command and test commands are off (%s)' % (verb, SETTINGS_HINT))
    do(ctx, verb, args)


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__)
        return 2
    ctx = Ctx()
    as_json = '--json' in argv
    try:
        verb, args = verb_of([a for a in argv if a != '--json'])
        if verb == 'help':
            for l in help_lines(True):
                print(l)
        else:
            do(ctx, verb, args, as_json, wow_bg3_only=True)
    except Usage as e:
        print(e, file=sys.stderr)
        return 2
    except CmdError as e:
        if str(e):
            print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
