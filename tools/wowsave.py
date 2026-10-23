#!/usr/bin/env python3
"""wowsave.py: the WoW characters' state per BG3 save (save sync; the save-sync notes).

  wowsave.py snapshot ID NAME [NAME...]  every row of these WoW characters -> server/private/snapshots/ID.sql
                                         (+ ID.json: names, guids, time)
  wowsave.py restore ID [NAME...]        put them back (every one offline; a failure puts the rows back);
                                         without NAMEs every character the snapshot holds
  wowsave.py index ID SAVE_DIR           note which BG3 save folder holds snapshot ID (ID.json)
  wowsave.py prune [--dry]               snapshots whose BG3 save folder is gone (or now holds a newer
                                         snapshot) move to #recycle_bin/wowsave-pruned-YYYYMMDD/
  wowsave.py list                        the snapshots, their characters and saves

ID: letters, digits, '-' and '_' (BG3 side: a fresh id per save, kept in the save, WowSave.lua).
Rows per character (guid G): every characters-database table keyed by G (characters, inventory, spells,
quests, reputation, auras, action bars, skills, homebind, ...), its items (item_instance and item_loot
by owner), its corpse (corpse.player_guid), its pets (character_pet by owner) and their spells and
auras (pet_* by pet id). World tables (respawns), GM, guild, group, mail and petition tables are left
alone. A snapshot file holds INSERT statements only. A restore deletes every row the snapshot covers
(plus any item row with a snapshot item's guid), then inserts the snapshot's; the database's tables are
mostly MyISAM (no transactions), so it checks every line first and puts the rows back as they were if
a statement fails half-way.
Runs the dump and the load inside the private database container only (wow-bg3-database-1), or in
harness B's own server when WOW_BG3_SERVER names it (link/tools/wowserver.py, the harness notes).
"""
import datetime, json, re, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import wowserver  # noqa: E402  (harness A's Docker test server, or B's own: WOW_BG3_SERVER)

ROOT = Path(__file__).resolve().parents[2]  # the development workspace; relenv.py sets it in the release
SNAP = ROOT / 'server/private/snapshots'
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
    m = read_meta(sid)
    m.update(Id=sid, Time=time.time(), Chars=chars, Rows=rows)
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


def apply(secs, names):
    """Deletes every row the sections cover, then their inserts (statements in one client call)."""
    stmts = []
    for name in names:
        guid, lines = secs[name]
        stmts += ['delete from %s where %s;' % (t, w) for t, w in wheres(guid)]
        items = [m.group(1) for m in map(ITEM_GUID.match, lines) if m]
        if items:  # an item of the snapshot that has another owner now (traded, mailed)
            stmts.append('delete from item_instance where guid in (%s);' % ','.join(items))
        stmts += lines
    sql('\n'.join(stmts) + '\n')


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
    print('restored %s: %s' % (sid, ', '.join('%s (guid %d, %d rows)' % (n, secs[n][0], len(secs[n][1])) for n in names)))


def index(sid, save_dir):
    assert (SNAP / ('%s.sql' % sid)).is_file(), 'no snapshot %s' % sid
    m = read_meta(sid)
    m.update(Id=sid, Save=str(save_dir), SaveTime=time.time())
    write_meta(sid, m)
    print('snapshot %s is in save %s' % (sid, save_dir))


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


def main(a):
    if a[:1] == ['list']:
        for p in sorted(SNAP.glob('*.sql'), key=lambda p: p.stat().st_mtime):
            m = read_meta(p.stem)
            print(p.stem, ', '.join(sections(p.stem)), m.get('Save', '-'))
        return
    if a[:1] == ['prune']:
        return prune('--dry' in a)
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
    main(sys.argv[1:])
