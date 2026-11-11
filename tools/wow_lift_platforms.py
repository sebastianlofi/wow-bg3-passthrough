#!/usr/bin/env python3
"""wow_lift_platforms.py: the platforms BG3 keeps for WoW's lifts.

A WoW lift (an elevator, a tram car) carries BG3's characters on one of BG3's own moving platforms: a small level
of its own placed in ours as a level template and moved by BG3's PlatformMoveTo (tools/wowbg3_level.py). A
platform's floor is baked into its level, and a car's size is only known from the player's own WoW data, so the
level keeps a few round floors of several sizes, EACH of every size, parked off every WoW map's place. While the
game runs the mod takes the smallest one a car's floor fits in (mod Lifts.lua; the table there is lua()).

  specs()        the platform levels' specs (wowbg3_level.build)
  instances()    what the level places: [{Root, MapKey, Name, Pos}] (a level spec's "LevelTemplates")
  pick(radius)   the floor radius for a car's (metres), or None when no platform is that large
  lua()          the same family as Lifts.lua's table

  wow_lift_platforms.py lua

The ids are in every save: RADII, EACH and the names must not change once released (more may be added).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wowbg3_level  # noqa: E402

LEVEL = 'WOWBG3_Azeroth_A'
RADII = (3.0, 5.0, 6.5, 8.0, 10.0, 12.5)   # metres
EACH = 3                                    # Undercity has three cars in sight of each other
SLACK = 0.25                                # a car's floor may be this much wider than its platform's
# Parked in the level's low corner, beside its start square and off every WoW map's place (test_wow_lift_platforms.py).
PARK = (-15700.0, -60.0, -15500.0)
STEP = 40.0
FLOOR_SHAPE = '1645d06c-0a6f-4656-9fc7-0dc8e174e655'   # a stock BG3 shape (by id) the platform's grid belongs to


def name(radius):
    return 'WOWBG3_Lift_%03d' % round(radius * 10)


def root(radius):
    return wowbg3_level.uid(name(radius), 'root')


def key(radius, i):
    return wowbg3_level.uid(LEVEL, 'lift', name(radius), i)


def park(radius, i):
    return (PARK[0] + STEP * i, PARK[1], PARK[2] + STEP * RADII.index(radius))


def specs():
    out = []
    for r in RADII:
        n = int(-(-(2 * r + 1) // 1)) * 2          # tiles a side: the disc and half a metre round it
        half = n * 0.25
        out.append({'Name': name(r), 'DisplayName': 'Azeroth', 'Platform': True, 'Root': root(r),
                    'Bounds': [-half - 8, -100, -half - 8, half + 8, 100, half + 8],
                    'Scenery': [{'Template': FLOOR_SHAPE, 'Name': name(r) + '_Floor', 'Pos': [0.0, -40.0, 0.0]}],
                    'Subgrids': [{'X': -half, 'Y': 0, 'Z': -half, 'W': n, 'H': n, 'Kind': 'disc', 'Radius': r, 'Owner': 0}]})
    return out


def instances():
    return [{'Root': root(r), 'MapKey': key(r, i), 'Name': '%s_%03d' % (name(r), i), 'Pos': list(park(r, i))}
            for r in RADII for i in range(EACH)]


def pick(radius):
    return next((r for r in RADII if r >= radius - SLACK), None)


def lua():
    rows = ['    {Radius = %s, Park = {%s}, Keys = {%s}},' % (
        ('%g' % r), ', '.join('{%g, %g, %g}' % park(r, i) for i in range(EACH)), ', '.join('"%s"' % key(r, i) for i in range(EACH)))
        for r in RADII]
    return 'M.SLACK = %g\nM.PLATFORMS = {\n%s\n}' % (SLACK, '\n'.join(rows))


if __name__ == '__main__':
    if sys.argv[1:] == ['lua']:
        print(lua())
    else:
        print(__doc__)
        sys.exit(2)
