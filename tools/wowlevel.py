#!/usr/bin/env python3
"""wowlevel.py: the hero's WoW level for BG3's progression.

Party design (the progression notes): WoW levels 1 to 4 each open a party slot, every WoW level
from 5 on gives one BG3 level-up. The BG3 side (Progression.lua) only reads wow-bg3/wow-level.json
{Character, Level, Online, Xp, XpNext, Percent, Seq}; this tool is the WoW backend's half of that
channel. Percent (0 to 99.9) is how far WoW's own XP stands into the next level: BG3 shows it in its
own XP bar (never a full level, so BG3's Level Up button is never opened by it).

  wowlevel.py get [NAME]          the WoW character's level now (online: the server writes it to the
                                  database first, world-console.py saveall)
  wowlevel.py once [NAME]         get, then write wow-bg3/wow-level.json (party.sh does this first)
  wowlevel.py watch [NAME] [S]    once every S seconds (default 10) while it runs; when the level
                                  changes, the party's bot characters follow it (bots below)
  wowlevel.py set NAME N          test helper: GM console `character level NAME N` on the local
                                  server (any level 1 to 60, also down); WOW-BG3's own characters
                                  only (the default account or a playthrough account)

NAME: the WoW character; default $WOW_CHAR, else the one online character on a WOW-BG3 account (the
default account, or the hidden account a playthrough made; wowaccount.py).
Bots (watch): the bot characters of the other party members, wow-bg3/party-bots.json {"Bots": ["Name",
...]} (written by whoever logs them in), follow the hero's level: every round, a bot whose level is
not the hero's gets it through the server's verb (BOT_LEVEL below, world-console.py); until that
verb exists the difference is only logged.
"""
import json, os, re, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import wowserver  # noqa: E402  (the portable server WOW_BG3_SERVER names)

ROOT = Path(__file__).resolve().parents[2]  # the development workspace; relenv.py sets it in the release
IO = ROOT / "bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3"
DB = 'wow-bg3-database-1'
CONSOLE = 'wow-bg3-mangosd-1'
TEST_ACCOUNT = 'WOWBG3TEST'
# The server's verb that sets a bot character's level, as world-console.py arguments
# ( `world-console.py bot level NAME N`). None until it exists.
BOT_LEVEL = ['bot', 'level', '{name}', '{level}']  # server patch 0010, live from m5-p11


def sql(q, db='characters'):
    r = wowserver.db_sh('mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" -N %s' % db, q, DB)
    if r.returncode:
        raise SystemExit('database: %s' % (r.stderr.strip() or r.returncode))
    return r.stdout


def name_ok(name):
    if not re.fullmatch(r'[A-Za-z]{2,12}', name or ''):
        raise SystemExit('wowlevel: a WoW character name, letters only')
    return name


def default_name():
    n = os.environ.get('WOW_CHAR')
    if n:
        return name_ok(n)
    import wowbg3cmd  # the same rule as the commands: a character on one of WOW-BG3's accounts
    rows = [l.split('\t') for l in sql("select c.name, a.username from characters c join realmd.account a "
                                       "on a.id = c.account where c.online = 1;").splitlines()]
    ours = [r[0] for r in rows if len(r) == 2 and wowbg3cmd.is_wow_bg3_account(r[1])]
    if len(ours) != 1:
        raise SystemExit('wowlevel: name the WoW character (online on a WOW-BG3 account: %s)' % (', '.join(ours) or 'none'))
    return ours[0]


def console(command):
    """One GM console line on the WoW server (wowserver.py)."""
    return wowserver.console(command, 2.5, CONSOLE)


def get(name):
    row = sql("select level, online from characters where name = '%s';" % name).split()
    if not row:
        raise SystemExit('wowlevel: no WoW character %s' % name)
    if row[1] == '1':
        # The server keeps an online character's level in memory; write it out first.
        subprocess.run(['python3', str(ROOT / 'server/tools/world-console.py'), 'saveall'], capture_output=True)
        time.sleep(0.5)
        row = sql("select level, online from characters where name = '%s';" % name).split()
    return int(row[0]), row[1] == '1'


def progress(name):
    """(XP into this WoW level, XP the next level needs): WoW's own numbers (characters.xp and the
    server's player_xp_for_level); (0, 0) at the last level or without that table."""
    try:
        row = sql("select c.xp, coalesce((select p.xp_for_next_level from mangos.player_xp_for_level p "
                  "where p.lvl = c.level), 0) from characters c where c.name = '%s';" % name_ok(name)).split()
        xp, nxt = int(row[0]), int(row[1])
    except (SystemExit, OSError, ValueError, IndexError):
        return 0, 0
    return (max(0, min(xp, nxt)), nxt) if nxt > 0 else (0, 0)


def write(name, level, online):
    xp, nxt = progress(name)
    body = {'Character': name, 'Level': level, 'Online': online, 'Xp': xp, 'XpNext': nxt,
            'Percent': min(round(100.0 * xp / nxt, 1), 99.9) if nxt > 0 else 0.0,
            'Seq': int(time.time() * 1000)}
    tmp = IO / 'wow-level.json.tmp'
    tmp.write_text(json.dumps(body))
    os.replace(tmp, IO / 'wow-level.json')
    return body


said = {}
def follow_bots(level):
    """Every bot at the hero's level (its database level: watch's saveall wrote the online ones)."""
    try:
        bots = json.loads((IO / 'party-bots.json').read_text()).get('Bots') or []
    except (OSError, ValueError):
        bots = []
    for b in bots:
        if not re.fullmatch(r'[A-Za-z]{2,12}', str(b)):
            continue
        row = sql("select level from characters where name = '%s';" % b).split()
        if not row or int(row[0]) == level:
            continue
        if BOT_LEVEL:
            args = [a.format(name=b, level=level) for a in BOT_LEVEL]
            out = subprocess.run(['python3', str(ROOT / 'server/tools/world-console.py')] + args, capture_output=True, text=True)
            print(time.strftime('%H:%M:%S'), 'bot %s: level %s -> %d: %s' % (b, row[0], level, (out.stdout or out.stderr).strip()[-160:]), flush=True)
        elif said.get(b) != (row[0], level):
            said[b] = (row[0], level)
            print(time.strftime('%H:%M:%S'), 'bot %s is level %s, the hero %d (no server bot level verb yet)' % (b, row[0], level), flush=True)


def main(a):
    if not a or a[0] not in ('get', 'once', 'watch', 'set'):
        raise SystemExit(__doc__)
    if a[0] == 'set':
        if len(a) != 3 or not a[2].isdigit() or not 1 <= int(a[2]) <= 60:
            raise SystemExit('wowlevel.py set NAME N (1 to 60)')
        # The same code as /wowbg3 level in WoW's chat (link/tools/wowbg3cmd.py).
        import wowbg3cmd
        try:
            wowbg3cmd.level(wowbg3cmd.Ctx(io=IO, char=name_ok(a[1])), [a[2]], wow_bg3_only=True)
        except wowbg3cmd.CmdError as e:
            raise SystemExit('wowlevel: %s' % e)
        return
    name = name_ok(a[1]) if len(a) > 1 and not a[1].isdigit() else default_name()
    if a[0] == 'get':
        lvl, online = get(name)
        print('%s %d%s' % (name, lvl, '' if online else ' (offline)'))
    elif a[0] == 'once':
        print(json.dumps(write(name, *get(name))))
    else:
        every = float(a[-1]) if a[-1].replace('.', '', 1).isdigit() else 10.0
        last = None
        while True:
            try:
                lvl, online = get(name)
                write(name, lvl, online)
                if last is not None and lvl != last:
                    print(time.strftime('%H:%M:%S'), '%s: WoW level %d -> %d' % (name, last, lvl), flush=True)
                follow_bots(lvl)
                last = lvl
            except (SystemExit, OSError, AssertionError) as e:
                print(time.strftime('%H:%M:%S'), 'wowlevel: %s' % e, flush=True)
            time.sleep(every)


if __name__ == '__main__':
    main(sys.argv[1:])
