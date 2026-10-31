#!/usr/bin/env python3
"""inventory_bridge.py: one inventory for WoW and BG3.

  inventory_bridge.py [--fake] [--char NAME] [--member UUID=NAME ...] [--once]

Carries BG3's inventory requests to the WoW server and the WoW characters' inventories back:
  BG3 (Inventory.lua) -> wow-bg3/link/inventory-req.json  {RunId, Reqs: [{Seq, Op, Member, ...}]}
  bridge -> BG3          wow-bg3/link/inventory.json      {Seq, Time, Members: {UUID: {Char, Money,
                         Items: [{Guid, Entry, Count, Bag, Slot, Rand}]}}, Vendors: {GUID: [{Idx, Entry, Price,
                         Stock}]}, Loot: {GUID: {Money, Items: [{Idx, Entry, Count}]}}, Replies: [...]}
inventory.json's For is the RunId of the requests read: Inventory.lua takes only a file meant for it.
Ops: list, equip, unequip, move, give, add, del, money, vendor (list wares), buy, sell, loot (list),
take. Every reply is {Seq, Op, Ok, Why, ...}; the inventory itself always comes from the server
(WoW is the truth: a refused BG3 change is undone by Inventory.lua's next reconcile).

Speed (a 0.1.0 playtest report: loot and wares took seconds): a vendor or loot answer is written at
once, before the members' inventories are listed again (one server call each), and in idle steps the
wares of every friendly NPC with a stand-in (wow-bg3/link/mirror.json) are listed ahead, one NPC a step
(server patch 0018 lists a vendor from afar), so a vendor's stand-in holds its wares before its trade
screen opens. An NPC that is not a vendor is remembered and not asked again.

Members: the hero (wow-bg3/hero.json Uuid) is --char (default env WOW_CHAR, else Test); companions
from wow-bg3/link/party-chars.json {UUID: NAME} once lifecycle.py writes it, or --member.

Backends:
  --fake     a WoW stand-in kept in wow-bg3/link/inventory-fake.json (WoW's slot rules, prices from
             items.json), for building and testing the BG3 side before the server verbs exist.
  default    the server's `wowbg3 inv|loot|vendor` verbs (server patches 0011 and
             0013) through INVENTORY_CONSOLE (a command that takes
             one console line on stdin and prints the reply lines; default server/tools/world-console.py
             wowbg3 ARGS).
"""
import argparse, json, os, shlex, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IO = Path(os.environ.get('WOW_BG3_IO') or ROOT / "bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3")
LINK = IO / 'link'

# WoW equipment slots by inventory type (the client's own choice; a second ring/trinket/weapon slot
# when the first is taken).
EQUIP = {1: [0], 2: [1], 3: [2], 4: [3], 5: [4], 20: [4], 6: [5], 7: [6], 8: [7], 9: [8], 10: [9],
         11: [10, 11], 12: [12, 13], 16: [14], 13: [15, 16], 17: [15], 21: [15], 14: [16], 22: [16], 23: [16],
         15: [17], 25: [17], 26: [17], 28: [17], 19: [18]}
BACKPACK = list(range(23, 39))
# Pockets (switch `camp`, Bridge.pocket): [below this NPC level, bread, water].
POCKET_TIERS = [(10, 4540, 159), (20, 4541, 1179), (30, 4542, 1205), (40, 4544, 1708), (50, 4601, 1645), (10 ** 6, 8950, 8766)]


def jload(p, default=None):
    try:
        return json.loads(Path(p).read_text())
    except Exception:  # noqa: BLE001
        return default


def jsave(p, v):
    p = Path(p)
    tmp = p.with_suffix(p.suffix + '.tmp')
    tmp.write_text(json.dumps(v, separators=(',', ':')))
    tmp.replace(p)


class Fake:
    """WoW's inventory rules, small: 16-slot backpack, no bags, equipment by inventory type."""

    def __init__(self, items):
        self.items = items
        self.path = LINK / 'inventory-fake.json'
        self.s = jload(self.path, None) or {'next': 1000, 'chars': {}, 'vendors': {}, 'loot': {}}

    def save(self):
        jsave(self.path, self.s)

    def char(self, name):
        c = self.s['chars'].setdefault(name, {'money': 0, 'items': {}})
        return c

    def _free(self, c):
        used = {(i['bag'], i['slot']) for i in c['items'].values()}
        for s in BACKPACK:
            if (255, s) not in used:
                return s
        return None

    def _new(self, c, entry, count):
        slot = self._free(c)
        if slot is None:
            return None
        self.s['next'] += 1
        g = str(self.s['next'])
        c['items'][g] = {'entry': entry, 'count': count, 'bag': 255, 'slot': slot}
        return g

    def list(self, name):
        c = self.char(name)
        return c['money'], [{'Guid': g, 'Entry': i['entry'], 'Count': i['count'], 'Bag': i['bag'], 'Slot': i['slot']}
                            for g, i in sorted(c['items'].items())]

    def add(self, name, entry, count):
        c = self.char(name)
        row = self.items.get(entry)
        if not row:
            return False, 'unknown', {}
        g = self._new(c, entry, count)
        return (True, '', {'Guid': g}) if g else (False, 'full', {})

    def delete(self, name, guid, count=None):
        c = self.char(name)
        it = c['items'].get(guid)
        if not it:
            return False, 'gone', {}
        if count and count < it['count']:
            it['count'] -= count
        else:
            del c['items'][guid]
        return True, '', {}

    def money(self, name, delta):
        c = self.char(name)
        if c['money'] + delta < 0:
            return False, 'nomoney', {}
        c['money'] += delta
        return True, '', {'Money': c['money']}

    def equip(self, name, guid, slot=None):
        c = self.char(name)
        it = c['items'].get(guid)
        if not it:
            return False, 'gone', {}
        row = self.items.get(it['entry'], {})
        slots = EQUIP.get(row.get('t'), [])
        if not slots:
            return False, 'cant_equip', {}
        if row.get('rl', 0) > c.get('level', 60):
            return False, 'cant_equip_level', {}
        taken = {i['slot']: g for g, i in c['items'].items() if i['bag'] == 255 and i['slot'] < 19}
        target = slot if slot in slots else next((s for s in slots if s not in taken), slots[0])
        if row.get('t') == 17 and 16 in taken:  # two-hand: the off hand goes to the bag
            off = c['items'][taken[16]]
            fs = self._free(c)
            if fs is None:
                return False, 'full', {}
            off['slot'] = fs
        old = taken.get(target)
        if old and old != guid:
            c['items'][old]['bag'], c['items'][old]['slot'] = it['bag'], it['slot']
        it['bag'], it['slot'] = 255, target
        return True, '', {'Slot': target}

    def unequip(self, name, guid):
        c = self.char(name)
        it = c['items'].get(guid)
        if not it:
            return False, 'gone', {}
        if not (it['bag'] == 255 and it['slot'] < 19):
            return True, 'not_equipped', {}
        fs = self._free(c)
        if fs is None:
            return False, 'full', {}
        it['slot'] = fs
        return True, '', {'Slot': fs}

    def give(self, name, guid, to):
        a, b = self.char(name), self.char(to)
        it = a['items'].get(guid)
        if not it:
            return False, 'gone', {}
        fs = self._free(b)
        if fs is None:
            return False, 'full', {}
        del a['items'][guid]
        it['bag'], it['slot'] = 255, fs
        b['items'][guid] = it  # WoW keeps the item guid on a trade
        return True, '', {'Guid': guid}

    def vendor(self, npc):
        v = self.s['vendors'].get(npc)
        if v is None:  # a few wares from the table for a vendor the fake has not seen
            picks = [e for e in (2488, 2490, 2491, 159, 4540, 117, 118, 2589) if e in self.items]
            v = [{'Idx': i, 'Entry': e, 'Price': self.items[e]['bp'], 'Stock': -1} for i, e in enumerate(picks)]
            self.s['vendors'][npc] = v
        return v

    def buy(self, name, npc, entry, count):
        ware = next((w for w in self.vendor(npc) if w['Entry'] == entry), None)
        if not ware:
            return False, 'not_sold', {}
        c = self.char(name)
        cost = ware['Price'] * count
        if c['money'] < cost:
            return False, 'nomoney', {}
        g = self._new(c, entry, count)
        if not g:
            return False, 'full', {}
        c['money'] -= cost
        return True, '', {'Guid': g, 'Money': c['money']}

    def sell(self, name, npc, guid):
        c = self.char(name)
        it = c['items'].get(guid)
        if not it:
            return False, 'gone', {}
        if it['bag'] == 255 and it['slot'] < 19:
            return False, 'equipped', {}
        price = self.items.get(it['entry'], {}).get('sp', 0) * it['count']
        if price <= 0:
            return False, 'no_price', {}
        del c['items'][guid]
        c['money'] += price
        return True, '', {'Money': c['money']}

    def loot(self, npc):
        lt = self.s['loot'].get(npc)
        if lt is None:
            picks = [e for e in (2589, 117, 769, 2672) if e in self.items]
            lt = {'Money': 37, 'Items': [{'Idx': i, 'Entry': e, 'Count': 1} for i, e in enumerate(picks[:2])]}
            self.s['loot'][npc] = lt
        return lt

    def take(self, name, npc, idx):
        lt = self.loot(npc)
        c = self.char(name)
        if idx == 'money':
            c['money'] += lt['Money']
            got, lt['Money'] = lt['Money'], 0
            return True, '', {'Money': c['money'], 'Got': got}
        row = next((r for r in lt['Items'] if r['Idx'] == int(idx)), None)
        if not row:
            return False, 'gone', {}
        g = self._new(c, row['Entry'], row['Count'])
        if not g:
            return False, 'full', {}
        lt['Items'].remove(row)
        return True, '', {'Guid': g}


class Server:
    """The server's verbs. One console line per call; reply lines start WOWBG3."""

    def __init__(self, items):
        self.items = items
        cmd = os.environ.get('INVENTORY_CONSOLE')
        self.cmd = shlex.split(cmd) if cmd else [sys.executable, str(ROOT / 'server/tools/world-console.py'), 'wowbg3']

    def save(self):
        pass

    def call(self, *words):
        r = subprocess.run(self.cmd + [str(w) for w in words], capture_output=True, text=True, timeout=20)
        return [l.split() for l in r.stdout.splitlines() if l.startswith('WOWBG3 ')]

    @staticmethod
    def verdict(lines, *head):
        for w in lines:
            if 'refused' in w:
                return False, ' '.join(w[w.index('refused') + 1:]) or 'refused', w
            if 'ok' in w:
                return True, '', w
        return False, 'no_reply', []

    def list(self, name):
        money, items, end = 0, [], None
        for w in self.call('inv', 'list', name):
            if len(w) >= 5 and w[1] == 'inv' and w[3] == 'begin':
                money = int(w[4])
            elif len(w) >= 9 and w[1] == 'inv' and w[3] == 'item':
                items.append({'Guid': w[6], 'Entry': int(w[7]), 'Count': int(w[8]), 'Bag': int(w[4]), 'Slot': int(w[5])})
                if len(w) >= 12 and w[11] != '0':
                    items[-1]['Rand'] = int(w[11])  # the random suffix: Durability, MaxDurability, Rand after Count
            elif len(w) >= 5 and w[1] == 'inv' and w[3] == 'end':
                end = int(w[4])
            elif 'refused' in w:
                raise RuntimeError('list %s: %s' % (name, ' '.join(w)))
        # The console is shared: a far move's teleport can read this list's lines as its own reply. A list
        # with lines missing is no word on what WoW holds (Inventory.lua would delete the rest).
        if end != len(items):
            raise RuntimeError('list %s: %d item line(s), end says %s' % (name, len(items), end))
        return money, items

    def _after_ok(self, lines, key):
        ok, why, w = self.verdict(lines)
        extra = {}
        if ok and key and w:
            i = w.index('ok')
            if len(w) > i + 1:
                extra[key] = w[i + 1]
        return ok, why, extra

    def add(self, name, entry, count):
        return self._after_ok(self.call('inv', 'add', name, entry, count), 'Guid')

    def delete(self, name, guid, count=None):
        return self._after_ok(self.call('inv', 'del', name, guid, *([count] if count else [])), None)

    def money(self, name, delta):
        return self._after_ok(self.call('inv', 'money', name, delta), 'Money')

    def equip(self, name, guid, slot=None):
        if slot is not None:
            return self._after_ok(self.call('inv', 'move', name, guid, 255, slot), None)
        return self._after_ok(self.call('inv', 'equip', name, guid), None)

    def unequip(self, name, guid):
        return self._after_ok(self.call('inv', 'unequip', name, guid), None)

    def give(self, name, guid, to):
        ok, why, w = self.verdict(self.call('inv', 'give', name, guid, to))
        return ok, why, ({'Guid': w[-1]} if ok and w else {})

    def vendor(self, npc, name=None):
        out = []
        for w in self.call('vendor', 'list', name, npc):
            if len(w) >= 9 and w[1] == 'vendor' and w[4] == 'item':
                out.append({'Idx': int(w[5]), 'Entry': int(w[6]), 'Price': int(w[7]), 'Stock': int(w[8])})
        return out

    def buy(self, name, npc, entry, count):
        ok, why, w = self.verdict(self.call('vendor', 'buy', name, npc, entry, count))
        extra = {}
        if ok and w:
            i = w.index('ok')
            extra = {'Guid': w[i + 1] if len(w) > i + 1 else None, 'Money': int(w[i + 2]) if len(w) > i + 2 else None}
        return ok, why, extra

    def sell(self, name, npc, guid):
        return self._after_ok(self.call('vendor', 'sell', name, npc, guid, 0), 'Money')

    def loot(self, npc, name=None):
        out = {'Money': 0, 'Items': []}
        for w in self.call('loot', 'list', name, npc):
            if len(w) >= 6 and w[1] == 'loot' and w[4] == 'begin':
                out['Money'] = int(w[5])
            elif len(w) >= 8 and w[1] == 'loot' and w[4] == 'item':
                out['Items'].append({'Idx': int(w[5]), 'Entry': int(w[6]), 'Count': int(w[7])})
        return out

    def take(self, name, npc, idx):
        return self._after_ok(self.call('loot', 'take', name, npc, idx), 'Guid')


class Bridge:
    def __init__(self, args):
        tbl = jload(LINK / 'items.json', {}) or {}
        self.items = {r['e']: r for r in tbl.get('Items', [])}
        self.be = Fake(self.items) if args.fake else Server(self.items)
        self.char = args.char
        self.extra = dict(m.split('=', 1) for m in args.member)
        self.done = {}  # RunId -> last Seq handled
        state = jload(LINK / 'inventory-bridge-state.json', {}) or {}
        self.done = state.get('Done', {})
        self.seq = state.get('Seq', 0)
        self.replies = []
        self.vendors, self.loot = {}, {}
        self.last_list = 0
        self.not_vendor = set()  # friendly NPCs a list was refused for (not vendors)
        self.pockets = set()     # of those, the ones with food on them to steal (pocket())
        self.camp_on = (jload(IO / 'camp.json', {}) or {}).get('On') is True

    def members(self):
        out = {}
        hero = jload(IO / 'hero.json', {}) or {}
        if hero.get('Uuid'):
            out[hero['Uuid']] = self.char
        for u, n in (jload(LINK / 'party-chars.json', {}) or {}).items():
            out[u] = n
        out.update(self.extra)
        return out

    def handle(self, req, members):
        op = req.get('Op')
        name = members.get(req.get('Member') or '', None)
        if op in ('vendor', 'loot'):
            name = name or self.char
        if not name and op not in ('vendor', 'loot'):
            return False, 'member', {}
        be = self.be
        if op == 'equip':
            return be.equip(name, str(req['Guid']), req.get('Slot'))
        if op == 'unequip':
            return be.unequip(name, str(req['Guid']))
        if op == 'give':
            to = members.get(req.get('To') or '')
            return be.give(name, str(req['Guid']), to) if to else (False, 'member', {})
        if op == 'add':
            npc = str(req.get('Npc') or '')
            if npc in self.pockets:  # stolen from a pocket (pocket()): gone from it
                self.vendors[npc] = [r for r in self.vendors.get(npc, []) if r['Idx'] != req.get('Idx')]
                self.urgent = True
            return be.add(name, int(req['Entry']), int(req.get('Count', 1)))
        if op == 'del':
            return be.delete(name, str(req['Guid']), req.get('Count'))
        if op == 'money':
            return be.money(name, int(req['Delta']))
        if op == 'vendor':
            npc = str(req['Npc'])
            self.vendors[npc] = be.vendor(npc) if isinstance(be, Fake) else be.vendor(npc, name)
            self.urgent = True
            return True, '', {'Npc': npc}
        if op == 'buy':
            return be.buy(name, str(req['Npc']), int(req['Entry']), int(req.get('Count', 1)))
        if op == 'sell':
            return be.sell(name, str(req['Npc']), str(req['Guid']))
        if op == 'loot':
            npc = str(req['Npc'])
            self.loot[npc] = be.loot(npc) if isinstance(be, Fake) else be.loot(npc, name)
            self.urgent = True
            return True, '', {'Npc': npc}
        if op == 'take':
            ok, why, extra = be.take(name, str(req['Npc']), req['Idx'])
            if ok:
                npc = str(req['Npc'])
                self.loot[npc] = be.loot(npc) if isinstance(be, Fake) else be.loot(npc, name)
            return ok, why, extra
        if op == 'list':
            return True, '', {}
        return False, 'unknown_op', {}

    def wow_side(self):
        """--fake only: a change made on the WoW side (as if in WoW's own client, a quest reward, ...),
        from wow-bg3/link/inventory-fake-cmd.json {Seq, Op, Char, Guid|Entry|Delta, Count}."""
        c = jload(LINK / 'inventory-fake-cmd.json', None)
        if not isinstance(self.be, Fake) or not c or c.get('Seq', 0) <= self.be.s.get('cmd_seq', 0):
            return False
        self.be.s['cmd_seq'] = c['Seq']
        op, name = c.get('Op'), c.get('Char', self.char)
        r = {'equip': lambda: self.be.equip(name, str(c['Guid'])), 'unequip': lambda: self.be.unequip(name, str(c['Guid'])),
             'add': lambda: self.be.add(name, int(c['Entry']), int(c.get('Count', 1))),
             'del': lambda: self.be.delete(name, str(c['Guid'])), 'money': lambda: self.be.money(name, int(c['Delta']))}.get(op)
        print('%s wow side %s -> %s' % (time.strftime('%H:%M:%S'), json.dumps(c), r() if r else 'unknown op'), flush=True)
        return True

    def prefetch(self):
        """One friendly NPC with a stand-in whose wares are not known yet: list them now."""
        if isinstance(self.be, Fake):
            return False
        mirror = jload(LINK / 'mirror.json', {}) or {}
        for m in mirror.get('Mobs') or []:
            g = str(m.get('Guid') or '')
            if not m.get('Friendly') or not g or g in self.vendors or g in self.not_vendor:
                continue
            wares = self.be.vendor(g, self.char)
            print('%s prefetch %s (%s): %d ware(s)' % (time.strftime('%H:%M:%S'), g, m.get('Name'), len(wares)), flush=True)
            if wares:
                self.vendors[g] = wares
                return True
            self.not_vendor.add(g)
            pocket = self.pocket(g, m)
            if pocket:
                self.vendors[g] = pocket
                self.pockets.add(g)
                return True
        return False

    def pocket(self, guid, mob):
        """Switch `camp`: what a friendly NPC that sells nothing has on it to steal, a little food and
        drink (WoW's vendor bread and water tiers). The same for the same NPC; gone once taken."""
        if not self.camp_on:
            return []
        # mirror.json carries no level for friendly NPCs: the hero's (wowlevel.py), what they can use anyway.
        level = int((jload(IO / 'wow-level.json', {}) or {}).get('Level') or 1)
        food, drink = next(t[1:] for t in POCKET_TIERS if level < t[0])
        seed = int(guid, 16) if all(c in '0123456789abcdefABCDEF' for c in guid) else sum(map(ord, guid))
        out = [{'Idx': 0, 'Entry': food, 'Price': 0, 'Stock': -1, 'Count': 1 + seed % 2}]
        if seed % 3 == 0:
            out.append({'Idx': 1, 'Entry': drink, 'Price': 0, 'Stock': -1, 'Count': 1})
        return [r for r in out if r['Entry'] in self.be.items]

    def publish(self, run, out):
        self.seq += 1
        jsave(LINK / 'inventory.json', {'Schema': 1, 'Seq': self.seq, 'For': run, 'Time': time.time(), 'Backend':
                                        'fake' if isinstance(self.be, Fake) else 'server', 'Members': out,
                                        'Vendors': self.vendors, 'Loot': self.loot, 'Replies': self.replies})
        jsave(LINK / 'inventory-bridge-state.json', {'Done': self.done, 'Seq': self.seq})

    def step(self):
        self.urgent = False
        members = self.members()
        wow_changed = self.wow_side()
        reqfile = jload(LINK / 'inventory-req.json', {}) or {}
        run = str(reqfile.get('RunId', ''))
        last = self.done.get(run, 0)
        changed = False
        for req in sorted(reqfile.get('Reqs', []), key=lambda r: r.get('Seq', 0)):
            if req.get('Seq', 0) <= last:
                continue
            try:
                ok, why, extra = self.handle(req, members)
            except Exception as e:  # noqa: BLE001
                ok, why, extra = False, 'error %s' % e, {}
            rep = dict(extra, Seq=req['Seq'], Op=req.get('Op'), Ok=ok, Why=why, RunId=run)
            self.replies = (self.replies + [rep])[-64:]
            print('%s req %s -> %s' % (time.strftime('%H:%M:%S'), json.dumps(req), json.dumps(rep)), flush=True)
            last = req['Seq']
            changed = True
        self.done[run] = last
        # A vendor's wares or a corpse's loot go out at once, with the inventories as last listed.
        if self.urgent and getattr(self, 'last_out', None) is not None:
            self.publish(run, self.last_out)
        # One friendly NPC's wares between the inventory lists (each list is a server call per member, about
        # a second each: counted from its end, or the next one was always due and nothing else ran).
        if not changed and not wow_changed and self.prefetch() and getattr(self, 'last_out', None) is not None:
            self.publish(run, self.last_out)
        now = time.monotonic()
        if changed or wow_changed or now - self.last_list >= 2.0:
            out = {}
            for uuid, name in members.items():
                try:
                    money, items = self.be.list(name)
                    out[uuid] = {'Char': name, 'Money': money, 'Items': items}
                except Exception as e:  # noqa: BLE001
                    out[uuid] = {'Char': name, 'Error': str(e)}
            self.be.save()
            self.last_out = out
            self.publish(run, out)
            self.last_list = time.monotonic()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fake', action='store_true')
    ap.add_argument('--char', default=os.environ.get('WOW_CHAR', 'Test'))
    ap.add_argument('--member', action='append', default=[])
    ap.add_argument('--once', action='store_true')
    ap.add_argument('--seed', action='append', default=[], help='fake only: CHAR:ENTRY[:COUNT] put in the bag at start')
    ap.add_argument('--money', type=int, default=None, help='fake only: set the --char money (copper)')
    a = ap.parse_args()
    b = Bridge(a)
    if isinstance(b.be, Fake):
        for s in a.seed:
            p = s.split(':')
            b.be.add(p[0], int(p[1]), int(p[2]) if len(p) > 2 else 1)
        if a.money is not None:
            b.be.char(a.char)['money'] = a.money
        b.be.save()
    while True:
        b.step()
        if a.once:
            break
        time.sleep(0.25)


if __name__ == '__main__':
    main()
