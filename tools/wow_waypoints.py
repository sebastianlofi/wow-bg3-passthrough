#!/usr/bin/env python3
"""wow_waypoints.py: fast travel between WoW's flight points (`waypoints` in settings.conf).

The rules, one place:
  Which points  TaxiNodes.dbc on the two continents world mode has ground for (0 and 1), less what no
                flight master stands at: nodes no TaxiPath.dbc path uses, the "Generic" flight target, the
                capture towers (NO_MASTER). A node with no mount for either side (ferries, ships) is never one.
                Ironforge and Undercity lie under their terrain: world_bridge.py cuts those sessions with --under.
  Discovered    WoW's own: the character's known-taxi mask in the characters table, bit (node - 1). WoW sets
                it when the character opens a flight master's window, and a new character starts with its
                race's capital. Nothing here writes it, so WoW's window and these waypoints always agree.
  Faction       The character's race's team, as the server's taxi code: a node needs a mount for that team
                (MountCreatureID, horde first). Each side's twin in a neutral town (Booty Bay, Gadgetzan)
                therefore shows once. `/wowbg3 faction` does not change it: it forces reactions and leaves
                the server's team alone (vmangos 0009).
  From where    Anywhere, as BG3's waypoints: no flight master needed, and any known point on either
                continent (a far one by the same server teleport as `tp`). Never in a BG3 fight, on a WoW
                flight, or dead in WoW.
  Cost          Free, as BG3's waypoints; WoW's flight prices are not charged.
  BG3's panel   The discovered points in BG3's own waypoint panel (panel(), Waypoints.lua); a click is `waypoint go`.
  The move      The party goes as for `tp`: wow-bg3/world-move.json (wowbg3cmd.py tele, World.move in
                world_bridge.py). A flight point's height is a little off the ground, and the terrain is
                wrong under a city, so Snap asks the bridge for the walkable floor nearest it (stand_z).

  wow_waypoints.py table DBC_DIR                    every point by team (DBC_DIR holds TaxiNodes.dbc)
  wow_waypoints.py stand DBC_DIR                    each point's height and the floor it lands on
                                                    (needs the server's map data: WOW_BG3_MAPS, _MMAPS, _VMAPS)
Python 3.9, standard library only, except `stand` (wow_stand.py's needs).
"""
import os
import struct
import sys
from pathlib import Path

CONTINENTS = {0: 'Eastern Kingdoms', 1: 'Kalimdor'}
ALLIANCE, HORDE = 'alliance', 'horde'
RACE_TEAM = {1: ALLIANCE, 3: ALLIANCE, 4: ALLIANCE, 7: ALLIANCE, 2: HORDE, 5: HORDE, 6: HORDE, 8: HORDE}
# Nodes with a mount and no flight master (checked against TaxiNodes.dbc and TaxiPath.dbc of 5875): no path
# uses 1, 3, 9, 15 and 57; 36 is a "Generic" flight target; 84 to 87 are the capture towers' own flights.
NO_MASTER = frozenset((1, 3, 9, 15, 36, 57, 84, 85, 86, 87))
FLOOR_REACH = 15.0  # yards: a floor farther than this from a flight point's height is another storey


def enabled():
    """The switch: WOWBG3_WAYPOINTS (the coordinator sets it from settings.conf), else `waypoints` in
    $WOWBG3_HOME/settings.conf. Off without either."""
    v = os.environ.get('WOWBG3_WAYPOINTS')
    if v is not None and v.strip() != '':
        return v.strip() == '1'
    home = os.environ.get('WOWBG3_HOME')
    if home:
        try:
            for line in (Path(home) / 'settings.conf').read_text().splitlines():
                line = line.split('#', 1)[0]
                if '=' in line and line.split('=', 1)[0].strip() == 'waypoints':
                    return line.split('=', 1)[1].strip() == '1'
        except OSError:
            pass
    return False


def read_dbc(path):
    """[(ints, floats, text)] per row, as wowbg3cmd.py's wdbc."""
    b = Path(path).read_bytes()
    magic, n, f, rs, _ = struct.unpack_from('<4s4I', b, 0)
    if magic != b'WDBC':
        raise ValueError('%s: not a DBC file' % path)
    strs = b[20 + n * rs:]

    def text(o):
        return strs[o:strs.find(b'\0', o)].decode('utf-8', 'replace')
    return [(struct.unpack_from('<%dI' % f, b, 20 + i * rs), struct.unpack_from('<%df' % f, b, 20 + i * rs), text)
            for i in range(n)]


def nodes(dbc_dir):
    """TaxiNodes.dbc (1.12): id, map, x, y, z, enUS name at 5, mount creature at 14 (horde) and 15 (alliance)."""
    return [{'id': i[0], 'map': i[1], 'x': f[2], 'y': f[3], 'z': f[4], 'name': t(i[5]), HORDE: i[14], ALLIANCE: i[15]}
            for i, f, t in read_dbc(Path(dbc_dir) / 'TaxiNodes.dbc')]


def map_rects(dbc_dir):
    """WorldMapArea.dbc (1.12), continents too (area 0): map, the art's name (WoW's GetMapInfo()), edges in world yards."""
    return [{'map': i[1], 'area': i[2], 'file': t(i[3]), 'left': f[4], 'right': f[5], 'top': f[6], 'bottom': f[7]}
            for i, f, t in read_dbc(Path(dbc_dir) / 'WorldMapArea.dbc')]


def team_of(race):
    return RACE_TEAM.get(race)


def known_ids(mask_text):
    """The node ids set in a saved taxi mask: uint32s in text, bit (node - 1)."""
    out = set()
    for field, word in enumerate(str(mask_text or '').split()):
        if word.isdigit():
            v = int(word)
            out.update(field * 32 + bit + 1 for bit in range(32) if v >> bit & 1)
    return out


def can_be_waypoint(node, team):
    return node['map'] in CONTINENTS and node['id'] not in NO_MASTER and node[team] != 0


def waypoints(all_nodes, mask_text, team):
    """The waypoints of a character: the points of its team that it has discovered, by continent and name."""
    known = known_ids(mask_text)
    out = [n for n in all_nodes if n['id'] in known and can_be_waypoint(n, team)]
    out.sort(key=lambda n: (n['map'], n['name'], n['id']))
    return out


def choose(points, text):
    """The waypoints `text` names: by id, else an exact name, the start of one, or part of one (the first of
    those that matches anything)."""
    t = text.strip().lower()
    if t.isdigit():
        return [p for p in points if p['id'] == int(t)]
    for test in (lambda n: n == t, lambda n: n.startswith(t), lambda n: t in n):
        hits = [p for p in points if test(p['name'].lower())]
        if hits:
            return hits
    return []


# TaxiNodes' place names against WorldMapArea's art names, where they differ more than by spaces and case.
MAP_OF_PLACE = {'orgrimmar': 'ogrimmar', 'hillsbrad': 'hilsbrad', 'dustwallowmarsh': 'dustwallow',
                'silverpineforest': 'silverpine', 'azshara': 'aszhara', 'thebarrens': 'barrens',
                'thehinterlands': 'hinterlands'}


def map_key(place):
    k = ''.join(c for c in place.lower() if c.isalnum())
    return MAP_OF_PLACE.get(k, k)


def pins(points, rects):
    """(map file, id, x, y, name) of each point on its own zone's map and its city's, x and y from 0 to 100. Not on
    a continent's map; not on a neighbour's, whose art's edges also hold it. The node's name says where it is:
    "Crossroads, The Barrens", "Orgrimmar, Durotar", "Moonglade"."""
    out = []
    for p in points:
        own = {map_key(part) for part in p['name'].split(',')}
        for r in rects:
            if r['area'] and map_key(r['file']) in own and r['map'] == p['map']:
                out.append((r['file'], p['id'], (p['y'] - r['left']) * 100.0 / (r['right'] - r['left']),
                            (p['x'] - r['top']) * 100.0 / (r['bottom'] - r['top']), p['name']))
    return out


def panel(all_nodes, mask_text, team):
    """BG3's own waypoint panel: every point of the team with its slot by node id, so a point keeps its slot as more
    are discovered (Waypoints.lua gives each slot a trigger of the host level), and whether it is discovered: a save
    keeps the panel's entries, also those of a character that does not know them."""
    known = known_ids(mask_text)
    every = sorted((n for n in all_nodes if can_be_waypoint(n, team)), key=lambda n: n['id'])
    return [{'Slot': i, 'Id': n['id'], 'Name': n['name'], 'Known': n['id'] in known} for i, n in enumerate(every)]


def nearest_floor(z, floors, reach=FLOOR_REACH):
    """The floor nearest z within reach, else z (no floor in the movement map here, or another storey's)."""
    near = [h for h in floors if abs(h - z) <= reach]
    return min(near, key=lambda h: abs(h - z)) if near else z


def stand_z(map_id, x, y, z, terrain=None):
    """The floor a flight point (x, y, z) lands on: the walkable movement-map polygon nearest its height, moved
    onto the surface WoW draws, as wow_stand.height (which knows no height to stay near, so a point in Undercity
    would land on the ruins above it)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import wow_arena
    import wow_stand
    import wow_vmap
    maps = wow_stand.data_dir('WOW_BG3_MAPS', 'maps')
    mmaps = wow_stand.data_dir('WOW_BG3_MMAPS', 'mmaps')
    vmaps = wow_stand.data_dir('WOW_BG3_VMAPS', 'vmaps')
    terrain = terrain or wow_arena.Terrain(maps, map_id)
    wow_arena.ensure_mmtiles(mmaps, maps, map_id, x, y, 0.0)
    col, _ = wow_stand.column(mmaps, map_id, x, y)
    dry = [c for c in col if not wow_arena.Poly([], c[2]).water]
    best = nearest_floor(z, [c[0] for c in (dry or col)])
    tris, _ = wow_vmap.VMap(vmaps, map_id).triangles(x - 3, x + 3, y - 3, y + 3, m2=False)
    drawn = [f[0] for f in wow_arena.Surfaces(tris).floors(x, y)]
    if not terrain.hole(x, y):
        drawn.append(terrain.height(x, y))
    return nearest_floor(best, drawn, wow_stand.SNAP_YD)


def main(argv):
    if len(argv) != 2 or argv[0] not in ('table', 'stand'):
        print(__doc__)
        return 2
    every = nodes(argv[1])
    for team in (ALLIANCE, HORDE):
        points = [n for n in every if can_be_waypoint(n, team)]
        print('%s: %d' % (team, len(points)))
        for n in sorted(points, key=lambda n: (n['map'], n['name'])):
            line = '  %3d %-42s %-16s %9.1f %9.1f %7.1f' % (n['id'], n['name'], CONTINENTS[n['map']], n['x'], n['y'], n['z'])
            if argv[0] == 'stand':
                try:
                    line += '  -> %7.1f' % stand_z(n['map'], n['x'], n['y'], n['z'])
                except Exception as e:  # noqa: BLE001  one bad column does not end the survey
                    line += '  -> %s' % e
            print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
