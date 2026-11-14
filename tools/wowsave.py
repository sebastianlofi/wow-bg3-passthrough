#!/usr/bin/env python3
"""wowsave.py: the WoW characters' state per BG3 save (save sync; the save-sync notes).

  wowsave.py snapshot ID NAME [NAME...]  every row of these WoW characters -> server/private/snapshots/ID.sql
                                         (+ ID.json: names, guids, time)
  wowsave.py restore ID [NAME...]        put them back (every one offline; a failure puts the rows back);
                                         without NAMEs every character the snapshot holds
  wowsave.py index ID SAVE_DIR           note which BG3 save folder holds snapshot ID (ID.json)
  wowsave.py prune [--dry]               snapshots whose BG3 save folder is gone (or now holds a newer
                                         snapshot) move to #recycle_bin/wowsave-pruned-YYYYMMDD/; in the release
                                         such a folder is removed after 30 days (BIN_DAYS)
  wowsave.py list                        the snapshots, their characters and saves
  wowsave.py rehome                      every snapshot character the database lacks put back from its newest
                                         snapshot, on its own account (mangosd stopped: wowbg3-server.sh start
                                         runs it before mangosd starts)

ID: letters, digits, '-' and '_' (BG3 side: a fresh id per save, kept in the save, WowSave.lua).
Rows per character (guid G): every characters-database table keyed by G (characters, inventory, spells,
quests, reputation, auras, action bars, skills, homebind, ...), its items (item_instance and item_loot
by owner), its corpse (corpse.player_guid), its pets (character_pet by owner) and their spells and
auras (pet_* by pet id). World tables (respawns), GM, guild, group, mail and petition tables are left
alone. A snapshot file holds INSERT statements only. A restore deletes every row the snapshot covers
(plus any item row with a snapshot item's guid), then inserts the snapshot's; the database's tables are
mostly MyISAM (no transactions), so it checks every line first and puts the rows back as they were if
a statement fails half-way. ID.json also names each character's account (Accounts: {name: username}).
A new database with BG3's saves and snapshots kept (a reinstall): `rehome`, before mangosd starts (it reads its
guid counters and its name cache only then), inserts each missing character from its newest snapshot on its own
account (made if missing, without a password until wowaccount.ensure or the coordinator sets one). Guids the
new database already uses (the character's, its items', pets', corpse's) get new numbers, and every snapshot of
that character is rewritten to the new numbers and account (the old files go to #recycle_bin/wowsave-rehomed-
YYYYMMDD/), so its later loads are plain restores. A load never makes such a character again (lifecycle.py).
Runs the dump and the load inside the private database container only (wow-bg3-database-1), or in
harness B's own server when WOW_BG3_SERVER names it (link/tools/wowserver.py, the harness notes).
"""
import datetime, json, re, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import wowserver  # noqa: E402  (the portable server WOW_BG3_SERVER names)

ROOT = Path(__file__).resolve().parents[2]  # the development workspace; relenv.py sets it in the release
SNAP = ROOT / 'server/private/snapshots'
# What prune and rehome set aside (ROOT/#recycle_bin/wowsave-pruned-DATE, wowsave-rehomed-DATE) is kept this many days
# and then removed. None in the source tree: nothing set aside there is ever removed. The release sets it
# (relenv.py): its ROOT is the player's data folder, and a snapshot goes there at every save BG3 overwrites.
BIN_DAYS = None
DB = 'wow-bg3-database-1'
BY_GUID = ['characters', 'character_account_data', 'character_action', 'character_aura',
           'character_battleground_data', 'character_forgotten_skills', 'character_gifts',
           'character_homebind', 'character_honor_cp', 'character_instance', 'character_inventory',
           'character_queststatus', 'character_reputation', 'character_skills', 'character_social',
           'character_spell', 'character_spell_cooldown', 'character_stats']
BY_OWNER = ['item_instance', 'item_loot', 'character_pet']
BY_PLAYER = ['corpse']                       # column player_guid
BY_PET = ['pet_aura', 'pet_spell', 'pet_spell_cooldown']  # column guid = character_pet.id
NAME = re.compile(r'[A-Za-z]{2,12}')
SID = re.compile(r'[A-Za-z0-9_-]{1,64}')
GUARD_MARGIN = 1000


def sh(cmd, stdin=None):
    """A shell command inside the database container with the root password from its environment."""
    r = wowserver.db_sh(cmd, stdin, DB)
    if r.returncode:
        raise RuntimeError('database: %s' % (r.stderr.strip() or r.returncode))
    return r.stdout


def sql(q):
    return sh('mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" -N characters', stdin=q)


def char(name):
    assert NAME.fullmatch(name), 'a WoW character name'
    row = sql("select guid, online from characters where name = '%s';" % name).split()
    assert row, 'no character %s' % name
    return int(row[0]), int(row[1])


def online(names):
    """{name: online} for the names that exist."""
    ok = [n for n in names if NAME.fullmatch(n)]
    if not ok:
        return {}
    out = sql("select name, online from characters where name in (%s);" % ','.join("'%s'" % n for n in ok))
    return {r.split('\t')[0]: int(r.split('\t')[1]) for r in out.splitlines() if '\t' in r}


def exists(name):
    return bool(online([name]))


def all_names():
    return sql("select name from characters;").split()


def accounts(names):
    """{name: username of the account that owns it} (a snapshot's Accounts: where `rehome` puts it back)."""
    ok = [n for n in names if NAME.fullmatch(n)]
    if not ok:
        return {}
    out = sql("select c.name, a.username from characters c join realmd.account a on a.id = c.account "
              "where c.name in (%s);" % ','.join("'%s'" % n for n in ok))
    return {r.split('\t')[0]: r.split('\t')[1].upper() for r in out.splitlines() if r.count('\t') == 1}


def tables_present():
    have = set(sql("show tables;").split())
    pick = lambda ts: [t for t in ts if t in have]  # noqa: E731
    return pick(BY_GUID), pick(BY_OWNER), pick(BY_PLAYER), pick(BY_PET)


def wheres(guid):
    by_guid, by_owner, by_player, by_pet = tables_present()
    pets = 'guid in (select id from character_pet where owner_guid=%d)' % guid
    return ([(t, 'guid=%d' % guid) for t in by_guid] + [(t, 'owner_guid=%d' % guid) for t in by_owner]
            + [(t, 'player_guid=%d' % guid) for t in by_player] + [(t, pets) for t in by_pet])


def dump(guid):
    """INSERT lines for every row of character guid (one container call for all tables)."""
    script = ''.join('mariadb-dump -uroot -p"$MARIADB_ROOT_PASSWORD" --no-create-info --skip-extended-insert '
                     '--order-by-primary --skip-comments --compact --skip-lock-tables --single-transaction '
                     '--where="%s" characters %s || exit 1\n' % (w, t) for t, w in wheres(guid))
    return [line for line in sh(script).splitlines() if line.startswith('INSERT INTO ')]


def meta_path(sid):
    return SNAP / ('%s.json' % sid)


def read_meta(sid):
    try:
        return json.loads(meta_path(sid).read_text())
    except (OSError, ValueError):
        return {}


def write_meta(sid, m):
    tmp = meta_path(sid).with_suffix('.json.tmp')
    tmp.write_text(json.dumps(m, indent=1))
    tmp.replace(meta_path(sid))


def snapshot(sid, names):
    parts, chars, rows = [], {}, 0
    for name in names:
        guid, _ = char(name)
        lines = dump(guid)
        parts += ['-- wowsave %s %s guid %d' % (sid, name, guid)] + lines
        chars[name] = guid
        rows += len(lines)
    SNAP.mkdir(parents=True, exist_ok=True)
    out = SNAP / ('%s.sql' % sid)
    tmp = out.with_suffix('.sql.tmp')
    tmp.write_text('\n'.join(parts) + '\n')
    tmp.replace(out)
    guard_item_guids()
    m = read_meta(sid)
    m.update(Id=sid, Time=time.time(), Chars=chars, Rows=rows, Accounts=accounts(list(chars)))
    write_meta(sid, m)
    print('snapshot %s: %s, %d rows -> %s' % (sid, ', '.join('%s (guid %d)' % kv for kv in chars.items()), rows, out))


def sections(sid):
    """{name: (guid, [INSERT lines])} of a snapshot (old one-character files too)."""
    src = SNAP / ('%s.sql' % sid)
    assert src.is_file(), 'no snapshot %s' % sid
    out, cur = {}, None
    for line in src.read_text().splitlines():
        m = re.match(r'-- wowsave \S+ ([A-Za-z]+) guid (\d+)$', line)
        if m:
            cur = m.group(1)
            out[cur] = (int(m.group(2)), [])
        elif cur and line.startswith('INSERT INTO '):
            out[cur][1].append(line)
    return out


ITEM_GUID = re.compile(r"^INSERT INTO `item_instance` VALUES \((\d+),")
INSERT_TABLE = re.compile(r"^INSERT INTO `([a-z_]+)` VALUES ")


def guard_item_guids(floor=0):
    """The server takes its next item guid from the highest in item_instance when it starts. A restore can put back
    items above that (a load of a later snapshot after a restart that followed a load of an earlier one), and a new
    item would get the guid of a restored one. A row of nobody's (owner 0, no item) keeps the number above everything
    a snapshot can hold; the margin is for the new items of a session that never reach a snapshot. The server's reuse
    of gaps is off for the same reason (wowbg3-server.sh). The row never moves down, and `floor` lifts it: the
    snapshots of a character `rehome` put back can hold items above anything in the new database."""
    out = sql('select ifnull(max(guid), 0) from item_instance where owner_guid = 0 and item_id = 0;\n'
              'select ifnull(max(guid), 0) from item_instance where not (owner_guid = 0 and item_id = 0);\n').split()
    guard = max(int(out[1]) + GUARD_MARGIN, int(out[0]), floor)
    sql('delete from item_instance where owner_guid = 0 and item_id = 0;\n'
        "insert into item_instance (guid, item_id, owner_guid, enchantments) values (%d, 0, 0, '');\n" % guard)


def apply(secs, names):
    """Deletes every row the sections cover, then their inserts (statements in one client call). Every delete comes
    before any insert: an item can move between the characters restored together (Shadowheart's water given to Gale
    after the save), and restoring them one by one put Shadowheart's row back while Gale's still held the item, which
    character_inventory's key (the item) refuses (a play report: an older save loaded on WoW's latest state)."""
    deletes, inserts = [], []
    for name in names:
        guid, lines = secs[name]
        deletes += ['delete from %s where %s;' % (t, w) for t, w in wheres(guid)]
        items = [m.group(1) for m in map(ITEM_GUID.match, lines) if m]
        if items:  # an item of the snapshot that has another owner now (traded, mailed, given), whoever holds it
            deletes.append('delete from item_instance where guid in (%s);' % ','.join(items))
            deletes.append('delete from character_inventory where item_guid in (%s);' % ','.join(items))
            deletes.append('delete from item_loot where guid in (%s);' % ','.join(items))
        inserts += lines
    sql('\n'.join(deletes + inserts) + '\n')


def restore(sid, names=None):
    """The characters database is mostly MyISAM (no transactions, checked): every line is
    checked first (an INSERT into one of the snapshot's own tables, the character offline and the same
    guid), the characters' rows as they are now are kept aside, and a failure half-way puts those back."""
    secs = sections(sid)
    names = names or list(secs)
    missing = [n for n in names if n not in secs]
    assert not missing, 'snapshot %s holds no %s' % (sid, ', '.join(missing))
    known = set(BY_GUID + BY_OWNER + BY_PLAYER + BY_PET)
    for name in names:
        guid, lines = secs[name]
        now, on = char(name)
        assert now == guid, 'snapshot %s: %s was guid %d, is %d now' % (sid, name, guid, now)
        assert on == 0, '%s is online: log it out first' % name
        bad = [l[:80] for l in lines if (INSERT_TABLE.match(l) or [None, None])[1] not in known]
        assert not bad, 'snapshot %s: unexpected lines for %s: %s' % (sid, name, bad[:2])
        assert any(l.startswith('INSERT INTO `characters` ') for l in lines), 'snapshot %s: no characters row for %s' % (sid, name)
    before = {name: (secs[name][0], dump(secs[name][0])) for name in names}
    try:
        apply(secs, names)
    except RuntimeError as e:
        apply(before, names)
        raise RuntimeError('restore %s failed, the characters were put back as they were: %s' % (sid, e))
    guard_item_guids()
    print('restored %s: %s' % (sid, ', '.join('%s (guid %d, %d rows)' % (n, secs[n][0], len(secs[n][1])) for n in names)))


# ---- rehome: a snapshot's character a new database lacks (a reinstall kept BG3's saves and the snapshots) ----
VALUES = re.compile(r"^INSERT INTO `([a-z_]+)` VALUES \((.*)\);$")
INT = re.compile(r'-?\d+')
USERNAME = re.compile(r'[A-Z0-9]{1,32}')
# Which columns hold which numbers (vmangos' characters database; aura casters are raw guids, a player's is its low guid).
PLAYER_COLS = ('owner_guid', 'player_guid', 'creator_guid', 'gift_creator_guid', 'caster_guid')  # and `guid` in BY_GUID
ITEM_COLS = {'item_instance': ('guid',), 'item_loot': ('guid',), 'character_inventory': ('bag', 'item_guid'),
             'character_gifts': ('item_guid',), 'character_aura': ('item_guid',), 'pet_aura': ('item_guid',)}
PET_COLS = {'character_pet': ('id',), 'pet_aura': ('guid',), 'pet_spell': ('guid',), 'pet_spell_cooldown': ('guid',)}
CORPSE_COLS = {'corpse': ('guid',)}
KINDS = (('item', ITEM_COLS, 'item_instance', 'guid'), ('pet', PET_COLS, 'character_pet', 'id'),
         ('corpse', CORPSE_COLS, 'corpse', 'guid'))
DEFAULT_USER = 'WOWBG3'  # the release's default account (wowbg3-coord.py DEFAULT_ACCOUNT)


def values(line):
    """(table, [each value as dumped]) of one INSERT line (mariadb-dump: one row, no column list)."""
    m = VALUES.match(line)
    assert m, 'not an INSERT line: %s' % line[:60]
    s, out, cur, i, quoted = m.group(2), [], [], 0, False
    while i < len(s):
        c = s[i]
        cur.append(c)
        if quoted and c == '\\':
            cur.append(s[i + 1:i + 2])
            i += 1
        elif c == "'":
            quoted = not quoted
        elif c == ',' and not quoted:
            out.append(''.join(cur[:-1]))
            cur = []
        i += 1
    assert not quoted, 'an unfinished string: %s' % line[:60]
    out.append(''.join(cur))
    return m.group(1), out


def columns():
    """{table: [its columns in order]} of the characters database."""
    out = sql("select table_name, column_name from information_schema.columns where table_schema = 'characters' "
              "order by table_name, ordinal_position;")
    cols = {}
    for r in out.splitlines():
        t, _, c = r.partition('\t')
        cols.setdefault(t, []).append(c)
    return cols


def renumber(lines, cols, m):
    """The lines with a character's numbers changed, m = {'char': {old: new}, 'item': {..}, 'pet': {..},
    'corpse': {..}, 'account': its account id}; a line that changes in nothing stays as it was."""
    out = []
    for line in lines:
        table, vals = values(line)
        names = cols.get(table) or []
        assert len(names) == len(vals), 'snapshot rows of %s have %d values, the database %d columns' % (
            table, len(vals), len(names))
        changed = False
        for i, c in enumerate(names):
            if not INT.fullmatch(vals[i]):
                continue
            n = was = int(vals[i])
            if (c == 'guid' and table in BY_GUID) or c in PLAYER_COLS:
                n = m['char'].get(n, n)
            for kind, ref, _, _ in KINDS:
                if c in ref.get(table, ()):
                    n = m[kind].get(n, n)
            if table == 'characters' and c == 'account':
                n = m['account']
            if n != was:
                vals[i], changed = str(n), True
        out.append('INSERT INTO `%s` VALUES (%s);' % (table, ','.join(vals)) if changed else line)
    return out


def held(lines, cols):
    """{'item': {guids}, 'pet': {ids}, 'corpse': {guids}} the lines hold."""
    out = {kind: set() for kind, _, _, _ in KINDS}
    for line in lines:
        table, vals = values(line)
        for c, v in zip(cols.get(table) or [], vals):
            for kind, ref, _, _ in KINDS:
                if c in ref.get(table, ()) and INT.fullmatch(v) and int(v):
                    out[kind].add(int(v))
    return out


def in_use(table, col, nums):
    """Which of nums the database's table already uses in col."""
    nums, out = sorted(nums), set()
    for k in range(0, len(nums), 500):
        out |= {int(x) for x in sql('select %s from %s where %s in (%s);' % (
            col, table, col, ','.join(map(str, nums[k:k + 500])))).split()}
    return out


def highest(table, col):
    return int((sql('select ifnull(max(%s), 0) from %s;' % (col, table)).split() or ['0'])[0])


def owner(name, secs, meta):
    """The username of the account `name` goes back to: the snapshot's Accounts; for an older snapshot the playthrough
    account the registry made for that hero before the snapshot (wowaccount.py), else the default account for the
    snapshot's first character (the hero: a save request lists it first) and the bot account (WB3B + NAME, vmangos
    patch 0010) for a companion."""
    acc = (meta.get('Accounts') or {}).get(name)
    if acc:
        return acc.upper()
    import wowaccount  # noqa: E402  (it imports this module)
    made = sorted((v.get('Made') or 0, u) for u, v in wowaccount.load()['Accounts'].items()
                  if v.get('Hero') == name and (v.get('Made') or 0) <= (meta.get('Time') or time.time()))
    if made:
        return made[-1][1]
    if list(secs)[:1] == [name]:
        d = wowaccount.default_user().upper()
        return d if USERNAME.fullmatch(d) else DEFAULT_USER
    return 'WB3B' + name.upper()


def account_id(user):
    """(realmd id of account `user`, made now). A row made here has no password (v, s empty) until
    wowaccount.ensure or the coordinator's account_env sets one through the console; a bot account needs none."""
    assert USERNAME.fullmatch(user), 'an account name: %r' % user
    q = "select id from realmd.account where username = '%s';" % user
    row = sql(q).split()
    if row:
        return int(row[0]), False
    sql("insert into realmd.account (username) values ('%s');" % user)
    row = sql(q).split()
    assert row, 'account %s not made' % user
    return int(row[0]), True


def rehome_one(name, newest, snaps, cols, bin_, reserved):
    """`name` (missing from the database) inserted from its newest snapshot; returns the highest item guid its
    snapshots hold now (the item guard goes above it). New numbers come above `reserved` too: what the snapshots of
    the characters still to come hold, so a free number of theirs stays theirs."""
    assert NAME.fullmatch(name), 'a WoW character name: %r' % name
    _, sid, secs, meta = newest
    old, lines = secs[name]
    same = [s for s in snaps if name in s[2] and s[2][name][0] == old]       # every snapshot of this character
    others = {s[2][name][0] for s in snaps if name in s[2]} - {old}          # another one of that name (an older database's)
    user = owner(name, secs, meta)
    acct, made = account_id(user)
    holds = {kind: set() for kind, _, _, _ in KINDS}
    for s in same:
        for kind, nums in held(s[2][name][1], cols).items():
            holds[kind] |= nums
    # Numbers another character, item, pet or corpse of this database has get the next free ones; the rest stay.
    m = {'account': acct, 'char': {}}
    if in_use('characters', 'guid', [old]):
        m['char'][old] = max([highest('characters', 'guid')] + list(others) + list(reserved['char'])) + 1
    for kind, _, table, col in KINDS:
        m[kind] = {}
        taken = in_use(table, col, holds[kind]) if holds[kind] and table in cols else set()
        nxt = max([highest(table, col)] + list(holds[kind]) + list(reserved[kind])) if taken else 0
        for n in sorted(taken):
            nxt += 1
            m[kind][n] = nxt
    guid = m['char'].get(old, old)
    rows = {name: (guid, renumber(lines, cols, m))}
    before = {name: (guid, dump(guid))}  # rows a deleted character left under that guid, put back on a failure
    try:
        apply(rows, [name])
    except RuntimeError as e:
        apply(before, [name])
        raise RuntimeError('not put back, the database is as it was: %s' % e)
    sql('update characters set online = 0 where guid = %d;\n'
        'replace into realmd.realmcharacters (realmid, acctid, numchars) select id, %d, '
        '(select count(*) from characters where account = %d) from realmd.realmlist;\n' % (guid, acct, acct))
    # Every snapshot of it in the new numbers and account: its later loads are plain restores.
    for _, sid2, secs2, meta2 in same:
        secs2 = dict(secs2, **{name: (guid, renumber(secs2[name][1], cols, m))})
        rewrite(sid2, secs2, bin_)
        meta2 = read_meta(sid2)
        keep(meta_path(sid2), bin_)
        meta2.setdefault('Chars', {})[name] = guid
        meta2.setdefault('Accounts', {})[name] = user
        meta2.setdefault('Rehomed', {})[name] = {'From': old, 'Guid': guid, 'Account': user, 'Time': time.time(),
                                                 'Renumbered': {k: len(m[k]) for k, _, _, _ in KINDS}}
        write_meta(sid2, meta2)
    print('rehomed %s from snapshot %s: guid %d%s, account %s%s, %d rows; renumbered %s; %d snapshot(s) rewritten' % (
        name, sid, old, ' -> %d' % guid if guid != old else '', user, ' (made, no password yet)' if made else '',
        len(lines), ', '.join('%d %s' % (len(m[k]), k) for k, _, _, _ in KINDS), len(same)))
    items = [m['item'].get(n, n) for n in holds['item']]
    return max(items) if items else 0


def keep(path, bin_):
    """A copy of a file about to change, in bin_ (never overwriting an earlier copy)."""
    if not path.exists():
        return
    bin_.mkdir(parents=True, exist_ok=True)
    dst, k = bin_ / path.name, 1
    while dst.exists():
        dst, k = bin_ / ('%s.%d' % (path.name, k)), k + 1
    shutil.copy2(str(path), str(dst))


def rewrite(sid, secs, bin_):
    src = SNAP / ('%s.sql' % sid)
    text = ''.join('-- wowsave %s %s guid %d\n' % (sid, n, g) + ''.join(l + '\n' for l in ls) for n, (g, ls) in secs.items())
    if src.is_file() and src.read_text() == text:
        return
    keep(src, bin_)
    tmp = src.with_suffix('.sql.tmp')
    tmp.write_text(text)
    tmp.replace(src)


def rehome():
    """Every character a snapshot holds and the database lacks, put back from its newest snapshot. Only while mangosd
    is stopped: it reads the highest guids and its name cache when it starts (ObjectMgr::SetHighestGuids,
    LoadPlayerCacheData), so a character inserted while it runs would get its guid handed out again."""
    assert not wowserver.mangosd_running(), 'mangosd is running: rehome only runs before it starts'
    snaps = []
    for p in SNAP.glob('*.sql'):
        secs = sections(p.stem)
        meta = read_meta(p.stem)
        snaps.append((meta.get('Time') or p.stat().st_mtime, p.stem, secs, meta))
    snaps.sort(key=lambda s: (s[0], s[1]))
    newest = {}
    for s in snaps:
        for name in s[2]:
            newest[name] = s
    have = set(all_names())
    missing = sorted((n for n in newest if n not in have), key=lambda n: newest[n][:2])
    if not missing:
        print('rehome: the database has every character of the %d snapshot(s)' % len(snaps))
        return 0
    cols = columns()
    reserved = dict({kind: set() for kind, _, _, _ in KINDS}, char=set())
    for name in missing:
        old = newest[name][2][name][0]
        reserved['char'].add(old)
        for s in snaps:
            if s[2].get(name, (None,))[0] == old:
                for kind, nums in held(s[2][name][1], cols).items():
                    reserved[kind] |= nums
    bin_ = ROOT / '#recycle_bin' / ('wowsave-rehomed-%s' % datetime.date.today().strftime('%Y%m%d'))
    failed, floor = [], 0
    for name in missing:
        try:
            floor = max(floor, rehome_one(name, newest[name], snaps, cols, bin_, reserved))
        except (RuntimeError, AssertionError, OSError) as e:
            failed.append(name)
            print('rehome %s FAILED: %s' % (name, e))
        snaps = [(t, sid, sections(sid), read_meta(sid)) for t, sid, _, _ in snaps]  # the rewritten ones
    guard_item_guids(floor + GUARD_MARGIN if floor else 0)
    return 1 if failed else 0


def index(sid, save_dir):
    assert (SNAP / ('%s.sql' % sid)).is_file(), 'no snapshot %s' % sid
    m = read_meta(sid)
    m.update(Id=sid, Save=str(save_dir), SaveTime=time.time())
    write_meta(sid, m)
    print('snapshot %s is in save %s' % (sid, save_dir))


def empty_bin(now=None):
    """Our own dated folders in ROOT/#recycle_bin older than BIN_DAYS days go, by the date in their name; nothing
    else there is touched. Returns the names removed."""
    if not BIN_DAYS:
        return []
    today, gone = datetime.date.fromtimestamp(now or time.time()), []
    for d in sorted((ROOT / '#recycle_bin').glob('wowsave-*')):
        m = re.fullmatch(r'wowsave-(?:pruned|rehomed)-(\d{8})', d.name)
        if not m or d.is_symlink() or not d.is_dir():
            continue
        try:
            day = datetime.datetime.strptime(m.group(1), '%Y%m%d').date()
        except ValueError:
            continue
        if (today - day).days > BIN_DAYS:
            shutil.rmtree(str(d), ignore_errors=True)
            gone.append(d.name)
    return gone


def prune(dry=False):
    """Snapshots whose save folder is gone, or whose folder holds a newer snapshot (BG3 reused it)."""
    metas = [read_meta(p.stem) | {'Id': p.stem} for p in sorted(SNAP.glob('*.sql'))]
    newest = {}
    for m in metas:
        if m.get('Save'):
            s = m['Save']
            if s not in newest or m.get('SaveTime', 0) > newest[s].get('SaveTime', 0):
                newest[s] = m
    bin_ = ROOT / '#recycle_bin' / ('wowsave-pruned-%s' % datetime.date.today().strftime('%Y%m%d'))
    gone = []
    for m in metas:
        s = m.get('Save')
        if not s:
            continue  # never indexed (older snapshots): kept
        if not Path(s).is_dir() or newest[s]['Id'] != m['Id']:
            gone.append(m['Id'])
            if not dry:
                bin_.mkdir(parents=True, exist_ok=True)
                for p in (SNAP / ('%s.sql' % m['Id']), meta_path(m['Id'])):
                    if p.exists():
                        shutil.move(str(p), str(bin_ / p.name))
    print('pruned %d snapshot(s)%s: %s' % (len(gone), ' (dry run)' if dry else '', ' '.join(gone) or '-'))
    old = [] if dry else empty_bin()
    if old:
        print('removed %d set-aside folder(s) older than %d days: %s' % (len(old), BIN_DAYS, ' '.join(old)))


def main(a):
    if a[:1] == ['list']:
        for p in sorted(SNAP.glob('*.sql'), key=lambda p: p.stat().st_mtime):
            m = read_meta(p.stem)
            print(p.stem, ', '.join(sections(p.stem)), m.get('Save', '-'))
        return
    if a[:1] == ['prune']:
        return prune('--dry' in a)
    if a == ['rehome']:
        return rehome()
    assert len(a) >= 2 and a[0] in ('snapshot', 'restore', 'index'), __doc__
    assert SID.fullmatch(a[1]), 'ID: letters, digits, - and _'
    if a[0] == 'snapshot':
        assert len(a) >= 3, __doc__
        snapshot(a[1], a[2:])
    elif a[0] == 'restore':
        restore(a[1], a[2:] or None)
    else:
        assert len(a) == 3, __doc__
        index(a[1], a[2])


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]) or 0)
