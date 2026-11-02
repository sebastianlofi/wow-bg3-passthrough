#!/usr/bin/env python3
"""A reinstall keeps BG3's saves and their snapshots but gets a new server database: a load puts the snapshot's
WoW character back on its own account (wowsave.py rehome), never a new level 1 one (lifecycle.py bring_up).
The database is a fake: sqlite with the tables' columns as vmangos has them (fewer of them in `characters`)."""
import argparse, json, os, re, sqlite3, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lifecycle  # noqa: E402
import wowaccount  # noqa: E402
import wowsave  # noqa: E402

TABLES = {
    'characters': 'guid, account, name, race, class, gender, level, online',
    'character_action': 'guid, button, action, type',
    'character_aura': 'guid, caster_guid, item_guid, spell, stacks',
    'character_inventory': 'guid, bag, slot, item_guid, item_id',
    'character_spell': 'guid, spell, active, disabled',
    'item_instance': 'guid, item_id, owner_guid, creator_guid, gift_creator_guid, count, enchantments, text',
    'item_loot': 'guid, owner_guid, item_id, amount, property',
    'character_pet': 'id, entry, owner_guid, name',
    'pet_spell': 'guid, spell, active',
    'corpse': 'guid, player_guid, map',
}


class FakeDB:
    """wowsave.sql, dump and columns on sqlite (the statements wowsave and wowaccount send, MariaDB's -N output)."""
    made = []

    def __init__(self):
        self.db = sqlite3.connect(':memory:')
        FakeDB.made.append(self.db)
        self.db.execute("attach database ':memory:' as realmd")
        for t, cols in TABLES.items():
            self.db.execute('create table %s (%s)' % (t, cols))
        self.db.execute('create table realmd.account (id integer primary key autoincrement, username text unique, v text, s text)')
        self.db.execute('create table realmd.realmcharacters (realmid, acctid, numchars, primary key (realmid, acctid))')
        self.db.execute('create table realmd.realmlist (id)')
        self.db.execute('insert into realmd.realmlist values (1)')
        self.fail_on = None

    def sql(self, q):
        out, cur = [], ''
        for line in q.splitlines():
            if line.startswith('INSERT INTO '):  # MariaDB's string escapes, sqlite's
                table, vals = wowsave.values(line)
                line = 'INSERT INTO `%s` VALUES (%s);' % (table, ','.join(
                    "'%s'" % re.sub(r'\\(.)', r'\1', v[1:-1]).replace("'", "''") if v.startswith("'") else v for v in vals))
            cur += line + '\n'
            if not sqlite3.complete_statement(cur):
                continue
            st, cur = cur.strip(), ''
            if st == 'show tables;':
                out += [r[0] for r in self.db.execute("select name from sqlite_master where type = 'table'")]
                continue
            if self.fail_on and self.fail_on in st:
                raise RuntimeError('database: a failure for the test')
            try:
                rows = self.db.execute(st).fetchall()
            except sqlite3.Error as e:
                raise RuntimeError('database: %s' % e)
            for r in rows:
                out.append('\t'.join('NULL' if v is None else str(v) for v in r))
        return ''.join(l + '\n' for l in out)

    def dump(self, guid):
        lines = []
        for t, w in wowsave.wheres(guid):
            for r in self.db.execute('select * from %s where %s order by rowid' % (t, w)):
                lines.append('INSERT INTO `%s` VALUES (%s);' % (t, ','.join(
                    'NULL' if v is None else "'%s'" % v.replace('\\', '\\\\').replace("'", "\\'")
                    if isinstance(v, str) else str(v) for v in r)))
        return lines

    def columns(self):
        return {t: [c.strip() for c in cols.split(',')] for t, cols in TABLES.items()}

    def one(self, q):
        return self.db.execute(q).fetchall()

    def account(self, user):
        self.db.execute("insert into realmd.account (username, v, s) values (?, 'v', 's')", (user,))
        return self.db.execute('select id from realmd.account where username = ?', (user,)).fetchone()[0]

    def tav(self, guid=15, account=16, level=60):
        """Tav: a bag (112) holding a sword (113) with a loot row, a crafted shirt (111), an aura it cast on itself,
        a pet with a spell, a corpse."""
        x = self.db.execute
        x("insert into characters values (?, ?, 'Tav', 2, 1, 1, ?, 1)", (guid, account, level))
        x('insert into character_action values (?, 72, 6603, 0)', (guid,))
        x('insert into character_aura values (?, ?, 113, 2457, 1)', (guid, guid))
        x('insert into character_spell values (?, 78, 1, 0)', (guid,))
        x('insert into character_inventory values (?, 0, 3, 111, 6125)', (guid,))
        x('insert into character_inventory values (?, 0, 19, 112, 4496)', (guid,))
        x('insert into character_inventory values (?, 112, 0, 113, 2488)', (guid,))
        x("insert into item_instance values (111, 6125, ?, ?, 0, 1, '0 0 0', 0)", (guid, guid))
        x("insert into item_instance values (112, 4496, ?, 0, 0, 1, '0 0 0', 0)", (guid,))
        x("insert into item_instance values (113, 2488, ?, 0, 0, 1, 'it''s, (sharp)', 0)", (guid,))
        x('insert into item_loot values (113, ?, 2589, 2, 0)', (guid,))
        x("insert into character_pet values (4, 3099, ?, 'Wolf')", (guid,))
        x('insert into pet_spell values (4, 17253, 1)')
        x('insert into corpse values (9, ?, 1)', (guid,))


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.saved = (wowsave.SNAP, wowsave.ROOT, wowsave.sql, wowsave.dump, wowsave.columns,
                      wowsave.wowserver.mangosd_running, wowaccount.console, dict(os.environ))
        wowsave.SNAP, wowsave.ROOT = root / 'snapshots', root
        os.environ['WOWBG3_ACCOUNTS_DIR'] = os.environ['WOWBG3_DATA'] = str(root)
        (root / 'account.env').write_text('WOW_USER=WOWBG3\nWOW_PASS=x\n')
        self.root = root
        self.use(FakeDB())
        wowsave.wowserver.mangosd_running = lambda: False
        self.console = []
        wowaccount.console = lambda line, seconds=3: self.console.append(line) or 'Password changed'
        self.registry({'WB3PTAVAAAAA': {'Pass': 'tavpass', 'Made': 1000.0, 'Hero': 'Tav'}})

    def tearDown(self):
        (wowsave.SNAP, wowsave.ROOT, wowsave.sql, wowsave.dump, wowsave.columns,
         wowsave.wowserver.mangosd_running, wowaccount.console, env) = self.saved
        os.environ.clear()
        os.environ.update(env)
        while FakeDB.made:
            FakeDB.made.pop().close()
        self.tmp.cleanup()

    def use(self, db):
        self.db = db
        wowsave.sql, wowsave.dump, wowsave.columns = db.sql, db.dump, db.columns

    def registry(self, accounts):
        (self.root / 'accounts.json').write_text(json.dumps({'Accounts': accounts}))

    def old_database(self):
        """The database before the reinstall: Tav (guid 15) on its playthrough account, two snapshots of it."""
        db = FakeDB()
        db.db.execute("insert into realmd.account (id, username, v, s) values (16, 'WB3PTAVAAAAA', 'v', 's')")
        db.tav()
        self.use(db)
        wowsave.snapshot('s1', ['Tav'])
        db.db.execute("update characters set level = 61 where guid = 15")
        wowsave.snapshot('s2', ['Tav'])
        return db

    def new_database(self):
        """A reinstall's database: the default account's Bob has guid 15 and item 111; a pet 4 and a corpse 9 are
        someone else's."""
        db = FakeDB()
        a = db.account('WOWBG3')
        db.db.execute("insert into characters values (15, ?, 'Bob', 1, 1, 0, 3, 0)", (a,))
        db.db.execute("insert into item_instance values (111, 25, 15, 0, 0, 1, '', 0)")
        db.db.execute("insert into character_inventory values (15, 0, 15, 111, 25)")
        db.db.execute("insert into character_pet values (4, 299, 15, 'Imp')")
        db.db.execute('insert into corpse values (9, 15, 0)')
        self.use(db)
        wowsave.guard_item_guids()
        return db


class Values(unittest.TestCase):
    def test_a_line_splits_into_its_values_and_back(self):
        line = "INSERT INTO `item_instance` VALUES (113,2488,15,0,NULL,1,'it\\'s, (sharp)','a\\\\b,c',-2.5);"
        table, vals = wowsave.values(line)
        self.assertEqual(table, 'item_instance')
        self.assertEqual(vals, ['113', '2488', '15', '0', 'NULL', '1', "'it\\'s, (sharp)'", "'a\\\\b,c'", '-2.5'])
        self.assertEqual('INSERT INTO `%s` VALUES (%s);' % (table, ','.join(vals)), line)

    def test_an_unchanged_line_stays_as_it_was(self):
        line = "INSERT INTO `character_spell` VALUES (15,78,1,0);"
        m = {'char': {}, 'item': {}, 'pet': {}, 'corpse': {}, 'account': 3}
        self.assertEqual(wowsave.renumber([line], {'character_spell': ['guid', 'spell', 'active', 'disabled']}, m), [line])


class Rehome(Base):
    def test_a_snapshot_records_each_characters_account(self):
        self.old_database()
        self.assertEqual(wowsave.read_meta('s1')['Accounts'], {'Tav': 'WB3PTAVAAAAA'})

    def test_a_missing_character_comes_back_from_its_newest_snapshot_with_its_guids_renumbered(self):
        self.old_database()
        db = self.new_database()
        self.assertEqual(wowsave.rehome(), 0)
        (guid, acct, level, online), = db.one("select guid, account, level, online from characters where name = 'Tav'")
        self.assertEqual((guid, level, online), (16, 61, 0))  # Bob has 15; the newest snapshot (level 61)
        self.assertEqual(db.one("select username, v from realmd.account where id = %d" % acct), [('WB3PTAVAAAAA', None)])
        self.assertEqual(db.one('select numchars from realmd.realmcharacters where acctid = %d' % acct), [(1,)])
        # Bob's item 111 and pet 4 stay his; Tav's shirt, pet and corpse got new numbers, the bag and sword kept theirs.
        self.assertEqual(db.one('select owner_guid, item_id from item_instance where guid = 111'), [(15, 25)])
        inv = db.one('select bag, slot, item_guid, item_id from character_inventory where guid = 16 order by slot, bag')
        shirt = [r for r in inv if r[3] == 6125][0][2]
        self.assertGreater(shirt, 113)
        self.assertEqual(sorted(inv), sorted([(0, 3, shirt, 6125), (0, 19, 112, 4496), (112, 0, 113, 2488)]))
        self.assertEqual(db.one('select owner_guid, creator_guid from item_instance where guid = %d' % shirt), [(16, 16)])
        self.assertEqual(db.one('select enchantments from item_instance where guid = 113'), [("it's, (sharp)",)])
        self.assertEqual(db.one('select owner_guid from item_loot where guid = 113'), [(16,)])
        self.assertEqual(db.one('select guid, caster_guid, item_guid from character_aura'), [(16, 16, 113)])
        (pet,), = db.one('select id from character_pet where owner_guid = 16')
        self.assertNotEqual(pet, 4)
        self.assertEqual(db.one('select guid from pet_spell'), [(pet,)])
        self.assertEqual(db.one('select guid, player_guid from corpse order by guid'), [(9, 15), (10, 16)])
        self.assertEqual(db.one("select count(*) from characters where name = 'Bob' and guid = 15"), [(1,)])
        # The guard row: above every item any snapshot of Tav holds.
        (guard,), = db.one('select guid from item_instance where owner_guid = 0 and item_id = 0')
        self.assertGreaterEqual(guard, shirt + wowsave.GUARD_MARGIN)
        # Both snapshots in the new numbers (the old files kept in the recycle bin), so a load is a plain restore.
        for sid in ('s1', 's2'):
            secs = wowsave.sections(sid)
            self.assertEqual(secs['Tav'][0], 16)
            self.assertEqual(wowsave.read_meta(sid)['Chars'], {'Tav': 16})
            self.assertIn('%s.sql' % sid, os.listdir(next(self.root.glob('#recycle_bin/wowsave-rehomed-*'))))
        wowsave.restore('s1')
        self.assertEqual(db.one("select guid, account, level from characters where name = 'Tav'"), [(16, acct, 60)])
        self.assertEqual(db.one('select owner_guid, item_id from item_instance where guid = 111'), [(15, 25)])
        self.assertEqual(db.one('select count(*) from character_inventory where guid = 15'), [(1,)])

    def test_its_account_gets_the_registrys_password_once_the_server_runs(self):
        self.old_database()
        db = self.new_database()
        wowsave.rehome()
        self.assertEqual(wowaccount.account_for('Tav'), 'WB3PTAVAAAAA')
        self.assertEqual(self.console, ['account set password WB3PTAVAAAAA tavpass tavpass'])
        db.db.execute("update realmd.account set v = 'v', s = 's' where username = 'WB3PTAVAAAAA'")
        self.assertEqual(wowaccount.account_for('Tav'), 'WB3PTAVAAAAA')
        self.assertEqual(len(self.console), 1)

    def test_free_guids_stay_as_they_were(self):
        self.old_database()
        db = FakeDB()
        self.use(db)
        self.assertEqual(wowsave.rehome(), 0)
        self.assertEqual(db.one("select guid from characters where name = 'Tav'"), [(15,)])
        self.assertEqual(sorted(r[0] for r in db.one('select guid from item_instance where owner_guid = 15')), [111, 112, 113])
        self.assertEqual(db.one('select id from character_pet'), [(4,)])

    def test_an_older_snapshot_finds_its_account_in_the_registry(self):
        self.old_database()
        for sid in ('s1', 's2'):
            m = wowsave.read_meta(sid)
            del m['Accounts']
            wowsave.write_meta(sid, m)
        t = wowsave.read_meta('s2')['Time']
        # Another playthrough's Tav made after these saves (the bug made one): not this character's account.
        self.registry({'WB3PTAVAAAAA': {'Pass': 'tavpass', 'Made': t - 100, 'Hero': 'Tav'},
                       'WB3PTAVLATER': {'Pass': 'other', 'Made': t + 100, 'Hero': 'Tav'}})
        db = self.new_database()
        wowsave.rehome()
        self.assertEqual(db.one("select a.username from characters c join realmd.account a on a.id = c.account "
                                "where c.name = 'Tav'"), [('WB3PTAVAAAAA',)])

    def test_an_older_snapshots_companion_goes_to_its_bot_account_and_a_hero_without_one_to_the_default(self):
        db = FakeDB()
        self.use(db)
        a = db.account('WOWBG3')
        b = db.account('WB3BSHADOWHEART')
        db.db.execute("insert into characters values (3, ?, 'Hero', 1, 1, 0, 5, 1)", (a,))
        db.db.execute("insert into characters values (4, ?, 'Shadowheart', 1, 5, 1, 5, 1)", (b,))
        wowsave.snapshot('old', ['Hero', 'Shadowheart'])
        m = wowsave.read_meta('old')
        del m['Accounts']
        wowsave.write_meta('old', m)
        db = FakeDB()
        self.use(db)
        wowsave.rehome()
        self.assertEqual(db.one("select c.name, a.username from characters c join realmd.account a on a.id = c.account "
                                "order by c.guid"), [('Hero', 'WOWBG3'), ('Shadowheart', 'WB3BSHADOWHEART')])

    def test_a_character_the_database_has_is_left_alone(self):
        self.old_database()
        before = {p.name: p.read_text() for p in wowsave.SNAP.iterdir()}
        self.assertEqual(wowsave.rehome(), 0)
        self.assertEqual({p.name: p.read_text() for p in wowsave.SNAP.iterdir()}, before)
        self.assertFalse((self.root / '#recycle_bin').exists())

    def test_an_existing_characters_restore_is_as_before(self):
        db = self.old_database()
        db.db.execute('update characters set online = 0')
        wowsave.rehome()
        wowsave.restore('s1')
        self.assertEqual(db.one("select guid, account, level from characters where name = 'Tav'"), [(15, 16, 60)])
        db.db.execute("delete from characters where guid = 15")
        db.db.execute("insert into characters values (1, 16, 'Tav', 2, 1, 1, 1, 0)")
        with self.assertRaisesRegex(AssertionError, 'Tav was guid 15, is 1 now'):
            wowsave.restore('s1')

    def test_only_while_mangosd_is_stopped(self):
        self.old_database()
        self.use(FakeDB())
        wowsave.wowserver.mangosd_running = lambda: True
        with self.assertRaisesRegex(AssertionError, 'mangosd is running'):
            wowsave.rehome()
        self.assertEqual(self.db.one('select count(*) from characters'), [(0,)])

    def test_a_failed_insert_leaves_the_database_and_the_snapshots_as_they_were(self):
        self.old_database()
        db = self.new_database()
        before = {p.name: p.read_text() for p in wowsave.SNAP.iterdir()}
        db.fail_on = 'INSERT INTO `character_pet`'
        self.assertEqual(wowsave.rehome(), 1)
        self.assertEqual(db.one("select count(*) from characters where name = 'Tav'"), [(0,)])
        self.assertEqual(db.one('select count(*) from item_instance where owner_guid = 16'), [(0,)])
        self.assertEqual({p.name: p.read_text() for p in wowsave.SNAP.iterdir()}, before)


class Life(Base):
    """bring_up of a save whose snapshot holds a character the database lacks."""
    def setUp(self):
        super().setUp()
        self.io_was = lifecycle.IO
        lifecycle.IO = self.root / 'io'
        (lifecycle.IO / 'link').mkdir(parents=True)
        self.life = lifecycle.Life(argparse.Namespace(launcher='x', tag='t', char=''))
        self.calls = []
        self.life.lc = lambda action, *a, **k: self.calls.append((action,) + a) or True
        self.life.create = lambda m, name: self.calls.append(('create', name)) or name
        self.logs = []
        self.log_was = lifecycle.log
        lifecycle.log = lambda *a: self.logs.append(' '.join(map(str, a)))

    def tearDown(self):
        lifecycle.IO, lifecycle.log = self.io_was, self.log_was
        super().tearDown()

    def load(self):
        return self.life.bring_up({'Session': 'S1', 'RunId': 'R', 'SaveId': 's1', 'Members': [
            {'Uuid': 'u1', 'Name': 'Tav', 'Char': 'Tav', 'Hero': True, 'Race': 'Human', 'Class': 'Fighter'}]},
            {'Name': 'Tav', 'Region': 'BGO_Main_A'})

    def test_a_lost_hero_is_never_made_again(self):
        self.old_database()
        self.new_database()
        self.assertFalse(self.load())
        self.assertEqual(self.calls, [])
        self.assertRegex(self.life.why, 'Tav is in save s1.s snapshot but not in the server.s database')

    def test_once_put_back_the_load_restores_and_logs_it_in(self):
        self.old_database()
        db = self.new_database()
        wowsave.rehome()
        lifecycle.verbs_ok = lambda hero: False
        self.life.match_life = lambda *a: None
        self.life.level_watch = lambda hero: None
        self.assertTrue(self.load())
        self.assertEqual(self.calls[0], ('benilla', 'Tav', 'WB3PTAVAAAAA'))
        self.assertTrue(any(re.match('restored snapshot s1', l) for l in self.logs), self.logs)
        self.assertEqual(db.one("select guid, level from characters where name = 'Tav'"), [(16, 60)])


if __name__ == '__main__':
    unittest.main()
