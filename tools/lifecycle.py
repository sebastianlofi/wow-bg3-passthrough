#!/usr/bin/env python3
"""lifecycle.py: the WoW side follows BG3's sessions (the save-sync notes).

  lifecycle.py --launcher link/tools/world-headless.sh --tag TAG [--char NAME]

Runs beside the private BG3 for a harness or desktop session (the launchers start it as a unit before
BG3). It reads what the mod writes (wow-bg3/lifecycle.json and hero.json from WowSave.lua and
Hero.lua, wowsave-request.json, death.json) and drives the WoW side through the launcher's
`lc` actions (down, benilla CHAR, create SPEC, world) and the server tools:

  * Any load (menu or quickload) or leaving the game: world mode's tools and benilla stop at once
    (benilla's WoW character logs out; bots log out through the server, when its verbs exist).
  * A new BG3 session running with its hero ready: every party member's WoW character offline, the
    loaded save's snapshot restored (link/tools/wowsave.py), the names handed to the mod
    (wow-bg3/wowchars.json; the save keeps them), then benilla started and logged in with the hero's
    character. A missing hero character (a new game; link/tools/wowchar.py rules) is made by that same
    benilla start (`lc create`: benilla patch 0048 WOW_BG3_CREATE, in the player build we ship; result
    in wow-bg3/link/charcreate.json); a name the server refuses gets the next free name. Then the
    companions log in as its bots (server `bot add`, which makes a missing companion character), then
    world mode (compose, the world and fight bridges). benilla never runs before the restore.
  * A save: `saveall`, a snapshot of every member's WoW character under the save's id, the save
    folder noted next to it, snapshots of saves that no longer exist pruned.
  * Death (Death.lua): a member down or dead in BG3 dies in WoW in place, a member back up is
    resurrected in place (server verbs; `world-console.py revive` until the server's own verb lands).
  * Per session, with the WoW side up: link/tools/wowlevel.py watch HERO (the WoW
    level for Progression.lua) runs and stops with it; wow-bg3/party-bots.json {"Bots": [...]} lists the
    bot characters logged in (wowlevel.py keeps them at the hero's level).
--char NAME: the hero's WoW character for a save that names none (older saves).
Log: one line per step on stdout (the launcher's unit sends it to tmp/m4-slice/lifecycle-TAG.log).
"""
import argparse, glob, json, os, re, signal, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # the development workspace; relenv.py sets it in the release
IO = ROOT / "bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3"
SAVES = ROOT / "bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/PlayerProfiles/Public/Savegames/Story"
sys.path.insert(0, str(ROOT / 'link/tools'))
import wowchar  # noqa: E402
import wowsave  # noqa: E402
import wowaccount  # noqa: E402  (one hidden WoW account per BG3 playthrough)
import crashreport  # noqa: E402

# Server verbs for party characters (server patch 0010; the m5-server-handoff notes "Patch 0010"):
# the console's `wowbg3 HERO VERB ...` runs a `.wowbg3` verb as the online hero HERO, through
# `server/tools/world-console.py wowbg3 HERO ...` (the allowlisted tool
# passes `wowbg3` lines). Until the test server has 0010 and that tool has the verb, `verbs_ok()` is
# False and the lifecycle falls back: companions get a name (kept in the save) but no character and
# no login, a BG3 death is not sent (in a fight the bridge's `hero` still kills the hero's character)
# and a BG3 revive uses the console's GM `revive NAME` (in place, 50 %).
VERBS = {
    'bot_add': ['wowbg3', '{hero}', 'bot', 'add', '{name}', '{race}', '{cls}', '{gender}'],  # makes it if missing, logs in, groups
    'bot_remove': ['wowbg3', '{hero}', 'bot', 'remove', '{name}'],  # saved and logged out (the hero's logout does it too)
    'bot_list': ['wowbg3', '{hero}', 'bot', 'list'],
    'kill': ['wowbg3', '{hero}', 'health', '{name}', '0', '{max}'],  # dead in place, no release
    'revive': ['wowbg3', '{hero}', 'revive', '{name}', '{hp}', '{max}'],  # alive in place at HP/MAX
    'health': ['wowbg3', '{hero}', 'health', '{name}', '{hp}', '{max}'],  # HP to HP/MAX; `dead` if dead
}


# SMSG_CHAR_CREATE codes that refuse the name itself (benilla patch 0048 writes them to
# wow-bg3/link/charcreate.json): 0x31 in use (on another account too), 0x45..0x50 empty, short, long,
# invalid, profane, reserved... The lifecycle then tries the next free name (wowchar.free_name).
NAME_REFUSED = {0x31} | set(range(0x45, 0x51))
CREATE_TRIES = 4
SIDES = ('horde', 'alliance', 'own')


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def read(name):
    try:
        return json.loads((IO / name).read_text())
    except (OSError, ValueError):
        return None


def write(name, body):
    p = IO / name
    tmp = p.with_name(p.name + '.tmp')
    tmp.write_text(json.dumps(body))
    tmp.replace(p)


def side_now():
    """The side link/faction.txt names (`SEQ SIDE`, wowbg3cmd.py faction); own without one."""
    try:
        side = (IO / 'link/faction.txt').read_text().split()[1]
    except (OSError, IndexError):
        return 'own'
    return side if side in SIDES else 'own'


def side_of_save(sid):
    """The side a save was made on (ID.json Side). A new game, or a save from before the side was kept: own."""
    side = wowsave.read_meta(sid).get('Side') if sid and wowsave.SID.fullmatch(sid) else None
    return side if side in SIDES else 'own'


def set_side(side):
    """link/faction.txt names SIDE; the fight bridge sends it at its start and benilla reads it (a new SEQ
    only when the side changes)."""
    if side == side_now():
        return
    f = IO / 'link/faction.txt'
    try:
        seq = int(f.read_text().split()[0])
    except (OSError, ValueError, IndexError):
        seq = 0
    f.parent.mkdir(parents=True, exist_ok=True)
    tmp = f.with_name(f.name + '.tmp')
    tmp.write_text('%d %s\n' % (seq + 1, side))
    tmp.replace(f)
    log('side: %s' % side)


def benilla_runs():
    """A live process named benilla, on this machine (or in this runtime's view of it). One that crashed stays
    in the list as a zombie until the coordinator waits for it (seen in a test), and counts as gone."""
    for c in glob.glob('/proc/[0-9]*/stat'):
        try:
            head, _, rest = Path(c).read_text().rpartition(')')
            if head.partition('(')[2] == 'benilla' and rest.split()[0] != 'Z':
                return True
        except OSError:
            pass
    return False


def bg3_run():
    """The running BG3's run id (WOW_BG3_RUN_ID in its environment, as the mod's files carry it), or None.
    Files from an earlier BG3 (lifecycle.json, hero.json) carry another id and are ignored (harness
    tests: a new BG3 at its menu with the last run's files brought WoW up for the old session)."""
    r = subprocess.run(['pgrep', '-x', 'bg3'], capture_output=True, text=True)
    # Only this harness's BG3: its launch or setup unit, named with the
    # instance's prefix (WOW_BG3_UNIT: A wow-bg3-m4, B wow-bg3-m22; the harness notes).
    unit = re.compile(r'/%s-(launch|setup)-' % re.escape(os.environ.get('WOW_BG3_UNIT', 'wow-bg3-m4')))
    for pid in r.stdout.split():
        try:
            if not unit.search(Path('/proc/%s/cgroup' % pid).read_text()):
                continue
            env = Path('/proc/%s/environ' % pid).read_bytes().split(b'\0')
        except OSError:
            continue
        for kv in env:
            if kv.startswith(b'WOW_BG3_RUN_ID='):
                return kv.split(b'=', 1)[1].decode()
    return None


def console(args, timeout=30):
    r = subprocess.run([sys.executable, str(ROOT / 'server/tools/world-console.py')] + args,
                       capture_output=True, text=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).strip()


def verb(kind, **kw):
    """One patch 0010 verb through the console: (ok, this verb's reply lines starting WOWBG3).
    The console's stream also carries other callers' replies in the same seconds (the inventory
    bridge's `inv list` every 2 s, feeds, another bot's unasked lines: testing, a `bot add
    Gale` read as failed), so only lines of this verb (and of this NAME, when it names one) count."""
    args = [a.format(**kw) for a in VERBS[kind]]
    rc, out = console(args)
    word, name = args[2], kw.get('name')

    def mine(line):
        w = line.split()
        if len(w) < 2 or w[1] not in (word, word + 's'):
            return False
        return not name or w[1] == word + 's' or (len(w) > 2 and w[2] == name)
    lines = [l[l.index('WOWBG3'):].strip() for l in out.splitlines() if 'WOWBG3' in l]
    lines = [l for l in lines if mine(l)]
    return rc == 0 and bool(lines) and not any(' refused' in l or ' failed' in l for l in lines), ' | '.join(lines)[-300:] or out[-200:]


_verbs = {}


def wow_dead(name):
    """Whether WoW character `name` is dead in the database (0 HP, or a ghost: a dead character's logout
    makes it one at the graveyard, vmangos WorldSession::LogoutPlayer)."""
    if not wowsave.NAME.fullmatch(name):
        return False
    out = wowsave.sql("select c.health = 0 or exists(select 1 from character_aura a where a.guid = c.guid and a.spell = 8326) "
                      "from characters c where c.name = '%s';" % name).split()
    return out[:1] == ['1']


def verbs_ok(hero):
    """Whether the server answers patch 0010's verbs for the online hero (asked once per hero)."""
    if hero not in _verbs:
        ok, out = verb('bot_list', hero=hero)
        _verbs[hero] = ok and 'WOWBG3 bots' in out
        log('server party verbs for %s: %s' % (hero, 'yes' if _verbs[hero] else 'no (%s)' % out[:120]))
    return _verbs[hero]


class Life:
    def __init__(self, a):
        self.a = a
        self.started = time.time()
        self.benilla_seen = 0.0    # when benilla was last looked for
        self.benilla_back = 0      # times it was started again this session
        self.keep_chars = False    # the next bring_up is such a start: no restore, the WoW characters are current
        self.crash_kept = False    # its report is made
        # `/wowbg3 time` is for one sitting: a new start of the game shows the time of this device.
        try:
            (IO / 'link').mkdir(parents=True, exist_ok=True)
            (IO / 'link/daytime.txt').write_text('now\n')
        except OSError as e:
            log('daytime.txt: %s' % e)
        self.session = None        # the BG3 session (Hero.SESSION) the WoW side is up for
        self.down_for = None       # the session the WoW side was last taken down for
        self.up = {'benilla': None, 'world': False, 'bots': []}
        self.chars = {}            # member uuid -> WoW character name (this session)
        self.snap_done = None
        self.death_seen = {}       # session -> last event seq handled
        self.failed = {}           # session -> time of a failed bring-up (retried after 60 s)
        self.seen_bg3 = False
        self.waiting = set()       # members who joined without a WoW character (made at the next load)
        self.pending_index = None
        self.made_for = {}         # session -> the hero character a create made (a retry logs it in again)
        self.cur_session = None
        self.accounts = {}         # session -> the playthrough account its new hero is made on (wowaccount.py)
        self.account = None

    def phase(self, session, phase, **extra):
        """wow-bg3/lifecycle-state.json: where the WoW side is for which BG3 session (down, restoring,
        characters, benilla, world, up, failed): what a loading-screen hold waits on."""
        self.current = dict(extra, Session=session, Phase=phase, Time=time.time())
        write('lifecycle-state.json', self.current)

    # ---- the session's helpers: WoW level watch, bot list ----
    def bots_file(self):
        """wow-bg3/party-bots.json {"Bots": [...]}: the WoW bot characters logged in now (read by
        link/tools/wowlevel.py watch, which keeps them at the hero's WoW level)."""
        write('party-bots.json', {'Bots': list(self.up['bots']), 'Hero': self.up['benilla'], 'Time': time.time()})
        # link/party-chars.json {member uuid: bot name} (inventory_bridge.py: the companions'
        # WoW characters; testing). Only members whose bot is online.
        (IO / 'link').mkdir(parents=True, exist_ok=True)
        write('link/party-chars.json', {u: n for u, n in self.chars.items() if n in self.up['bots']})

    def level_watch(self, hero):
        """link/tools/wowlevel.py watch HERO (wow-bg3/wow-level.json for Progression.lua)
        for the session the WoW side is up for; stopped with it."""
        self.level_stop()
        logf = open(str(ROOT / ('tmp/m4-slice/wowlevel-%s.log' % self.a.tag)), 'a')
        self.level = subprocess.Popen([sys.executable, '-u', str(ROOT / 'link/tools/wowlevel.py'), 'watch', hero],
                                      stdout=logf, stderr=subprocess.STDOUT, cwd=str(ROOT), start_new_session=True)
        logf.close()
        log('wowlevel.py watch %s (pid %d, log tmp/m4-slice/wowlevel-%s.log)' % (hero, self.level.pid, self.a.tag))

    def level_stop(self):
        p = getattr(self, 'level', None)
        if p and p.poll() is None:
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
            log('wowlevel.py watch stopped')
        self.level = None

    # ---- the launcher's actions ----
    def lc(self, action, *args, timeout=240):
        t0 = time.time()
        r = subprocess.run(['bash', str(ROOT / self.a.launcher), 'lc', action, self.a.tag] + list(args),
                           capture_output=True, text=True, timeout=timeout, cwd=str(ROOT))
        # The action's own last line (stdout; systemd-run's "Running as unit" goes to stderr).
        tail = r.stdout.strip().splitlines()[-1:] or r.stderr.strip().splitlines()[-1:] or ['']
        log('lc %s %s -> %s in %.1f s: %s' % (action, ' '.join(args), r.returncode, time.time() - t0, tail[0]))
        return r.returncode == 0

    def down(self, why):
        if not (self.up['benilla'] or self.up['world'] or self.up['bots']):
            return
        log('WoW side down:', why)
        self.phase(None, 'down', Why=why)
        self.level_stop()
        hero = self.up['benilla']
        for name in self.up['bots']:  # saved and logged out while the hero is still online
            log('bot remove', name, verb('bot_remove', hero=hero, name=name))
        self.lc('down', timeout=120)
        self.up = {'benilla': None, 'world': False, 'bots': []}
        self.bots_file()

    # ---- characters ----
    def wait_offline(self, names, seconds=60):
        end = time.time() + seconds
        while True:
            on = {n: v for n, v in wowsave.online(names).items() if v}
            if not on:
                return True
            if time.time() > end:
                log('still online after %d s: %s' % (seconds, ', '.join(on)))
                return False
            time.sleep(1.0)

    def create(self, m, name):
        """The hero's WoW character `name` (wowchar.py rules) made and logged in by one benilla start (the
        launcher's `lc create`: benilla patch 0048 WOW_BG3_CREATE, in the player build;
        the dev-only capture probe never ran in the shipped build). benilla writes the
        server's answer to wow-bg3/link/charcreate.json. A refused name (NAME_REFUSED: taken on any
        account, reserved, invalid) is replaced by the next free one, up to CREATE_TRIES names; any
        other failure is logged and stops here. Returns the name benilla is in with, or None (self.why)."""
        tried = set()
        for attempt in range(CREATE_TRIES):
            spec = wowchar.mapping(dict(m, Name=name))
            what = '%s %s, %s' % (spec['Race'], spec['Class'], spec['Start'])
            line = '%s,%d,%d,%d' % (name, spec['RaceId'], spec['ClassId'], spec['Gender'])
            t0 = time.time()
            # The playthrough's own account as the last argument (none: the default account).
            log(wowaccount.describe(name, self.account))
            ok = self.lc('create', line, *([self.account] if self.account else []), timeout=240)
            res = read('link/charcreate.json') or {}
            if res.get('Name') != name or float(res.get('Time') or 0) < t0 - 2:
                res = {}  # an older run's file, or benilla never got that far
            code = res.get('Code')
            if res.get('Made'):
                self.made_for[self.cur_session] = name
            if ok:
                wowaccount.note_hero(self.account, name)
                log('WoW character %s (%s) for %s: %s; benilla in' % (
                    name, what, m.get('Name'), 'made' if res.get('Made') else 'already on the account'))
                return name
            tried.add(name)
            if res.get('Result') == 'refused' and code in NAME_REFUSED and attempt + 1 < CREATE_TRIES:
                new = wowchar.free_name(wowchar.wow_name(m.get('Name')),
                                        tried | set(wowsave.all_names()) | set(self.chars.values()))
                log('WoW character %s for %s: the server refused the name (%s, %#04x); next: %s' % (
                    name, m.get('Name'), res.get('Reason'), code, new))
                name = new
                continue
            if res.get('Result') == 'refused' and code == 0x34:
                why = 'the WoW account already has the 10 characters a realm allows (CHAR_CREATE_SERVER_LIMIT)'
            elif res:
                why = '%s%s: %s' % (res.get('Result'), ' %#04x' % code if isinstance(code, int) else '', res.get('Reason'))
            else:
                why = 'benilla wrote no answer (it did not reach character select)'
            self.why = 'WoW character %s (%s) for %s NOT made: %s' % (name, what, m.get('Name'), why)
            log(self.why)
            return None
        self.why = 'WoW character for %s NOT made: the server refused %d names (%s)' % (
            m.get('Name'), len(tried), ', '.join(sorted(tried)))
        log(self.why)
        return None

    def bot_in(self, hero, m, name):
        """A companion's character logged in as the hero's bot (patch 0010 `bot add`: made if missing)."""
        spec = wowchar.mapping(dict(m, Name=name))
        ok, out = verb('bot_add', hero=hero, name=name, race=spec['RaceId'], cls=spec['ClassId'], gender=spec['Gender'])
        log('bot add %s for %s: %s' % (name, m.get('Name'), out))
        if ok:
            self.up['bots'].append(name)
            if m.get('Uuid'):
                self.chars.setdefault(m['Uuid'], name)
            self.bots_file()
        return ok

    def resolve(self, members, new_game, snap=None):
        """(member uuid -> WoW character name, uuids whose character is still to be made). `snap`: the loaded
        save's snapshot {member uuid: character} (ID.json Members): a save made before its WoW characters
        were named (BG3's start-of-game autosave, written before WoW was up) takes the characters its
        snapshot holds, not the launcher's WOW_CHAR or a new one. Only the hero's
        is made, by the benilla start that logs it in (create()); a companion's is only named here: the
        server's `bot add` makes it on the bot's own account when it logs in (a character made on the
        player's account would be `taken`)."""
        out, taken, make_ = {}, set(), set()
        for m in members:
            u, have, hero = m['Uuid'], m.get('Char'), bool(m.get('Hero'))
            if have and (wowsave.exists(have) or not hero):
                out[u] = have
                taken.add(have)
                continue
            name, make = None, False
            got = (snap or {}).get(u)
            if not have and got and got not in taken and wowsave.exists(got):
                out[u] = got
                taken.add(got)
                continue
            if have:
                name, make = have, True  # the save names a hero character the database lost: made again
            elif hero and self.a.char and not new_game:
                name = self.a.char
                make = not wowsave.exists(name)
            else:
                base = wowchar.wow_name(m.get('Name'))
                if not new_game and base not in taken and wowsave.exists(base):
                    name = base  # an older save of this hero or companion: its character
                else:
                    name = wowchar.free_name(base, taken | set(wowsave.all_names()))
                    make = hero
            if make:
                make_.add(u)
            out[u] = name
            taken.add(name)
        return out, make_

    # ---- a session ----
    def bring_up(self, lc_, hero):
        session = lc_['Session']
        members = lc_.get('Members') or []
        new_game = bool(lc_.get('NewGame')) and not lc_.get('SaveId')
        log('session %s: %s in %s, save %s%s, party %s' % (session, hero.get('Name'), hero.get('Region'),
            lc_.get('SaveId') or '-', ' (new game)' if new_game else '',
            ', '.join('%s=%s' % (m.get('Name'), m.get('Char') or '?') for m in members)))
        again_now = self.keep_chars
        self.down('a new session')
        self.why = None
        self.cur_session = session
        self.phase(session, 'restoring', SaveId=lc_.get('SaveId'))
        self.waiting = set()
        known = [m['Char'] for m in members if m.get('Char')] + ([self.a.char] if self.a.char else [])
        self.wait_offline(known)
        sid = lc_.get('SaveId')
        lost = set()  # characters the loaded save's snapshot holds and the database lacks: never made again
        if self.keep_chars:
            self.keep_chars = False
            log('benilla again: WoW characters as they are')
        elif sid and (wowsave.SNAP / ('%s.sql' % sid)).is_file():
            secs = wowsave.sections(sid)
            # A new database (a reinstall kept the saves): the server's start put them back (wowsave.py rehome,
            # wowbg3-server.sh start); one still missing would come back as a new level 1 character.
            lost = {n for n in secs if not wowsave.exists(n)}
            if self.wait_offline(list(secs), 60):
                try:
                    wowsave.restore(sid)
                    log('restored snapshot %s: %s' % (sid, ', '.join(secs)))
                    # A snapshot is taken while its characters are logged in (online 1 in its characters row), and
                    # they were offline right before the restore: put that back (a
                    # restored character nobody logged in stayed "online" and every later load waited 60 s on it,
                    # then skipped its restore).
                    names = [n for n in secs if wowsave.NAME.fullmatch(n)]
                    if names:
                        wowsave.sql('update characters set online = 0 where name in (%s);' % ','.join("'%s'" % n for n in names))
                except (RuntimeError, AssertionError) as e:
                    log('restore %s FAILED: %s' % (sid, e))
        elif sid:
            log('no snapshot %s (made before save sync, or pruned): WoW characters as they are' % sid)
        # `/wowbg3 faction` is the save's too: link/faction.txt outlives a load, and the bridge would send the
        # last session's side to this one. Done before benilla and the bridge start.
        if not again_now:
            set_side(side_of_save(sid))
        self.phase(session, 'characters')
        snap = wowsave.read_meta(sid).get('Members') if sid and wowsave.SID.fullmatch(sid) else None
        if not isinstance(snap, dict) and members and sid and (wowsave.SNAP / ('%s.sql' % sid)).is_file():
            # A snapshot from before the Members map: its first character is the hero's (the
            # save request lists the hero first), e.g. a start-of-game autosave made by an older version.
            first = list(wowsave.sections(sid))[:1]
            snap = {members[0]['Uuid']: first[0]} if first else None
        self.chars, make = self.resolve(members, new_game, snap if isinstance(snap, dict) else None)
        again = self.made_for.get(session)
        if members and members[0]['Uuid'] in make and again and wowsave.exists(again):
            # A retry of this session: the character an earlier try made (its login failed) is the hero's.
            self.chars[members[0]['Uuid']] = again
            make.discard(members[0]['Uuid'])
        hero_char = self.chars.get(members[0]['Uuid']) if members else None
        if not hero_char:
            self.why = 'no WoW character for the hero'
            log('no WoW character for the hero: WoW stays down')
            return False
        was = (snap if isinstance(snap, dict) else {}).get(members[0]['Uuid'])
        if hero_char in lost or was in lost:
            self.why = ('WoW character %s is in save %s\'s snapshot but not in the server\'s database: the server puts '
                        'it back when it starts (start BG3 again; logs/server.log says why it did not)' % (
                            was if was in lost else hero_char, sid))
            log(self.why)
            return False
        self.phase(session, 'benilla', Char=hero_char)
        if members[0]['Uuid'] in make:
            # Made and logged in by one benilla start; the name may change if the server refuses it.
            # A new playthrough's hero gets a WoW account of its own (an account holds
            # 10 characters, so the 11th New Game was refused; wowaccount.py). Without one: the default account.
            self.account = self.accounts.get(session)
            if not self.account:
                try:
                    self.account = self.accounts[session] = wowaccount.new(members[0].get('Name') or '', session)
                    log('playthrough account %s made for %s' % (self.account, members[0].get('Name')))
                except Exception as e:  # noqa: BLE001
                    log('playthrough account NOT made (%s): the default account' % e)
            hero_char = self.create(members[0], hero_char)
            if not hero_char:
                log('no WoW character for the hero: WoW stays down')
                return False
            self.chars[members[0]['Uuid']] = hero_char
            write('wowchars.json', {'RunId': lc_['RunId'], 'Session': session, 'Chars': self.chars})
        else:
            write('wowchars.json', {'RunId': lc_['RunId'], 'Session': session, 'Chars': self.chars})
            # The account that owns it: its playthrough's (last argument), or none for the default.
            acct = wowaccount.account_for(hero_char)
            log(wowaccount.describe(hero_char, acct))
            if not self.lc('benilla', hero_char, *([acct] if acct else []), timeout=200):
                self.why = 'benilla did not log in %s' % hero_char
                log(self.why)
                self.up['benilla'] = hero_char
                return False
        self.up['benilla'] = hero_char
        self.bots_file()
        self.level_watch(hero_char)
        bots = verbs_ok(hero_char)
        for m in members[1:]:
            name = self.chars.get(m['Uuid'])
            if not name:
                continue
            if name in lost:  # `bot add` would make it again, at level 1
                log('bot %s stays offline: in the snapshot, not in the server\'s database' % name)
                continue
            if bots:
                self.bot_in(hero_char, m, name)
            else:
                log('bot %s stays offline (no server party verbs)' % name)
        self.match_life(hero_char, members, bots)
        self.phase(session, 'world', Char=hero_char)
        self.up['world'] = self.lc('world', timeout=300)
        self.session = session
        # The WoW side is up for this session now: its own later unload (the main menu: UnloadLevel with the
        # same session token, no new one) must take it down again (testing: after an F8 the
        # loading screen had already named this session, so the return to the main menu kept WoW running).
        self.down_for = None
        self.phase(session, 'up' if self.up['world'] else 'failed', Char=hero_char, Chars=self.chars)
        return self.up['world']

    def match_life(self, hero, members, verbs):
        """Every member's WoW character brought to BG3's state of the loaded game (
        BG3 decides who lives): alive in BG3 -> alive in WoW at BG3's HP fraction (a WoW character that died
        after the save, or one a save without a snapshot left dead, is revived in place); down or dead in
        BG3 -> dead in place in WoW. Runs with the hero and bots logged in, before world mode, so world
        mode never starts with a dead hero under a living Tav. `health NAME HP MAX` answers `dead` for a
        dead character, which then takes `revive`."""
        for m in members:
            name = self.chars.get(m.get('Uuid'))
            if not name or (m is not members[0] and name not in self.up['bots']):
                continue
            mx = max(1, int(m.get('MaxHp') or 1))
            hp = min(mx, max(0, int(m.get('Hp') if m.get('Hp') is not None else mx)))
            down = bool(m.get('Dead') or m.get('Down'))
            if not verbs:
                if not down and wow_dead(name):
                    log('life: %s alive in BG3, WoW %s dead -> GM revive: %s' % (m.get('Name'), name, console(['revive', name])))
                continue
            if down:
                ok, out = verb('kill', hero=hero, name=name, max=mx)
                log('life: %s %s in BG3 -> WoW %s: %s' % (m.get('Name'), 'dead' if m.get('Dead') else 'down', name, out))
                continue
            ok, out = verb('health', hero=hero, name=name, hp=max(1, hp), max=mx)
            if ' dead ' in out:
                ok, out = verb('revive', hero=hero, name=name, hp=max(1, hp), max=mx)
                log('life: %s alive in BG3 (%d/%d), WoW %s was dead -> revived in place: %s' % (m.get('Name'), hp, mx, name, out))
            else:
                log('life: %s alive in BG3 (%d/%d), WoW %s: %s' % (m.get('Name'), hp, mx, name, out))

    # ---- saves ----
    def snapshot(self, req):
        key = (req.get('RunId'), req.get('Session'), req.get('Seq'))
        if key == self.snap_done or req.get('Action') != 'snapshot':
            return
        self.snap_done = key
        t0 = time.time()
        names, by_member = [], {}
        for m in req.get('Members') or []:
            n = m.get('Char') or self.chars.get(m.get('Uuid'))
            if n and wowsave.exists(n):
                by_member[m.get('Uuid')] = n
                if n not in names:
                    names.append(n)
        if not names:
            log('save %s: no WoW character to snapshot' % req.get('Id'))
            return
        rc, out = console(['saveall'])
        time.sleep(2.0)
        try:
            wowsave.snapshot(req['Id'], names)
        except (RuntimeError, AssertionError) as e:
            log('snapshot %s FAILED: %s' % (req['Id'], e))
            return
        meta = wowsave.read_meta(req['Id'])
        meta['Members'] = by_member  # which member had which character (resolve(), a save naming none)
        meta['Side'] = side_now()  # `/wowbg3 faction` at this save (bring_up puts it back)
        wowsave.write_meta(req['Id'], meta)
        log('save %s: snapshot of %s in %.1f s' % (req['Id'], ', '.join(names), time.time() - t0))
        # The save folder is the newest one written since the request (BG3's start-of-game autosave is
        # written before the WoW side is up, so the snapshot comes later than the save).
        try:
            asked = (IO / 'wowsave-request.json').stat().st_mtime
        except OSError:
            asked = t0
        self.pending_index = (req['Id'], min(t0, asked) - 10.0)

    def index_save(self):
        """The save folder BG3 wrote for the last snapshot (the newest .lsv since the request)."""
        p = getattr(self, 'pending_index', None)
        if not p:
            return
        sid, since = p
        best = None
        for f in glob.glob(str(SAVES / '*' / '*.lsv')):
            t = os.stat(f).st_mtime
            if t >= since and (best is None or t > best[0]):
                best = (t, os.path.dirname(f))
        if best and time.time() - best[0] > 2.0:  # written and settled
            wowsave.index(sid, best[1])
            self.pending_index = None
            wowsave.prune()
        elif time.time() - since > 120:
            log('save %s: no save folder found' % sid)
            self.pending_index = None

    # ---- companions joining a running session ----
    def joined(self, lc_):
        """A member without a WoW character yet (party.sh add, a recruitment): made and logged in as the
        hero's bot (`bot add`); without the server's party verbs, at the next load."""
        # Only a running game lists the party (WowSave.lua): during a save (state Save) lifecycle.json has no
        # Members, which must not read as everyone leaving (testing: F5 logged both bots out).
        if lc_.get('State') != 'Running' or not isinstance(lc_.get('Members'), list):
            return
        hero = self.up['benilla']
        for m in lc_.get('Members') or []:
            u = m.get('Uuid')
            if not u or u in self.chars or u in self.waiting:
                continue
            if hero and verbs_ok(hero):
                got, _ = self.resolve([m], False)
                if got.get(u) and self.bot_in(hero, m, got[u]):
                    self.chars[u] = got[u]
                else:
                    self.waiting.add(u)
                    continue
            else:
                self.waiting.add(u)
                log('%s joined: WoW character made at the next load (no server party verbs)' % m.get('Name'))
                continue
            write('wowchars.json', {'RunId': lc_['RunId'], 'Session': lc_['Session'], 'Chars': self.chars})
        # A member who left the party: its bot logs out.
        now = {m.get('Uuid') for m in lc_.get('Members') or []}
        for u, name in list(self.chars.items()):
            if u not in now and name in self.up['bots']:
                log('%s left the party: bot remove %s' % (u, name), verb('bot_remove', hero=hero, name=name))
                self.up['bots'].remove(name)
                del self.chars[u]
                self.bots_file()

    # ---- death ----
    def death(self, session):
        d = read('death.json')
        if not d or d.get('Session') != session or d.get('RunId') != self.run:
            return
        last = self.death_seen.get(session)
        evs = d.get('Events') or []
        if last is None:  # the session's first look: nothing replayed
            self.death_seen[session] = max([e.get('Seq', 0) for e in evs] + [0])
            return
        for e in evs:
            if e.get('Seq', 0) <= last:
                continue
            self.death_seen[session] = e['Seq']
            name = e.get('Char') or self.chars.get(e.get('Uuid'))
            if not name:
                log('death: %s %s, no WoW character' % (e.get('Name'), e.get('What')))
                continue
            hero = self.up['benilla']
            hp, mx = int(e.get('Hp') or 0), max(1, int(e.get('MaxHp') or 1))
            party = bool(hero and verbs_ok(hero))
            if e.get('What') in ('down', 'dead'):
                if party:
                    r = verb('kill', hero=hero, name=name, max=mx)
                else:
                    r = '(no server party verbs: not sent; in a fight the bridge kills the hero)'
                log('death: %s %s in BG3 -> WoW %s dead in place: %s' % (e.get('Name'), e['What'], name, r))
            elif e.get('What') == 'up':
                if party:
                    r = verb('revive', hero=hero, name=name, hp=max(1, hp), max=mx)
                else:
                    r = console(['revive', name])  # GM revive: in place, 50 %
                log('death: %s up in BG3 (%d/%d) -> WoW %s alive in place: %s' % (e.get('Name'), hp, mx, name, r))

    # ---- the loop ----
    def step(self):
        run = bg3_run()
        if not run:
            if self.seen_bg3:
                self.down('BG3 stopped')
                return 'exit'
            return None
        self.seen_bg3 = True
        self.run = run
        lc_ = read('lifecycle.json')
        if not lc_ or lc_.get('RunId') != run:
            return None
        session = lc_.get('Session')
        # Any load, unload or the menu: the WoW side goes at once (benilla never runs across a load).
        if lc_.get('Loading') or (self.session and session != self.session):
            if self.down_for != session:
                # The last session's WoW names go with it: WowSave.lua stores
                # wowchars.json's names on the members of any session of this BG3 run, and an origin keeps its
                # uuid across playthroughs, so a load of another Gale playthrough took the last one's
                # character (Galeb) instead of its own (Galea). The new session gets its own file.
                write('wowchars.json', {'RunId': run, 'Session': None, 'Chars': {}})
                self.down('BG3 %s (session %s)' % (lc_.get('State'), session))
                self.down_for = session
                self.session = None
            if lc_.get('Loading'):
                return None
        req = read('wowsave-request.json')
        if req and self.session and req.get('RunId') == run and req.get('Session') == self.session:
            self.snapshot(req)
        self.index_save()
        if self.session == session:
            # benilla gone (a crash: the GPU lost at camp's door) and nothing brought it back: the
            # world stayed gone until a reload. Back here, at the WoW side's state as it is; never at camp,
            # where the world mode cannot start. Three times a session, then it stays down.
            if self.up['benilla'] and time.time() - self.benilla_seen > 5.0:
                self.benilla_seen = time.time()
                at_camp = (read('camp-state.json') or {}).get('InCamp')
                gone = not benilla_runs()
                if gone and not self.crash_kept:
                    self.crash_kept = True
                    # One file for the player to send, before a restart overwrites the logs or the kernel drops
                    # its GPU dump; WowSave.lua says where. Never in the way of the restart.
                    try:
                        report = crashreport.make('benilla crashed')
                        log('benilla is gone: report %s' % report)
                        write('crash-notice.json', {'RunId': run, 'Path': str(report).replace(str(Path.home()), '~')})
                    except Exception as e:  # noqa: BLE001
                        log('benilla is gone: no report: %s' % e)
                if gone and not at_camp and self.benilla_back < 3:
                    self.crash_kept = False
                    self.benilla_back += 1
                    log('benilla is gone: starting it again (%d of 3)' % self.benilla_back)
                    self.keep_chars = True
                    self.session = None
                    return None
            self.death(session)
            self.joined(lc_)
            return None
        hero = read('hero.json')
        ready = (hero and hero.get('RunId') == run and hero.get('Session') == session and hero.get('Ready')
                 and lc_.get('State') == 'Running' and lc_.get('Members'))
        if not ready or time.time() - self.failed.get(session, 0) < 60:
            return None
        if not self.bring_up(lc_, hero):
            self.failed[session] = time.time()
            self.phase(session, 'failed', Why=getattr(self, 'why', None) or 'world mode did not start')
            log('session %s: WoW side not up (%s); retried in 60 s' % (session, getattr(self, 'why', None) or 'world mode did not start'))
        else:
            log('session %s: WoW side up (%s)' % (session, self.up['benilla']))
            self.death(session)
        return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--launcher', required=True, choices=['link/tools/world-headless.sh', 'link/tools/play-world.sh'])
    p.add_argument('--tag', required=True)
    p.add_argument('--char', default='')
    a = p.parse_args()
    assert re.fullmatch(r'[0-9]{6}', a.tag) and (not a.char or re.fullmatch(r'[A-Za-z]{2,12}', a.char))
    life = Life(a)
    stop = {'now': False}
    signal.signal(signal.SIGTERM, lambda *_: stop.update(now=True))
    signal.signal(signal.SIGINT, lambda *_: stop.update(now=True))
    log('lifecycle for %s (tag %s, hero character for unnamed saves: %s)' % (a.launcher, a.tag, a.char or 'made from the hero'))
    while not stop['now']:
        try:
            if life.step() == 'exit':
                log('BG3 stopped: lifecycle done')
                life.level_stop()
                return
        except Exception as e:  # keep watching; one failed step is logged and retried
            log('step error: %r' % e)
            time.sleep(5.0)
        time.sleep(0.5)
    life.level_stop()
    log('stopped (the launcher takes the WoW side down)')


if __name__ == '__main__':
    main()
