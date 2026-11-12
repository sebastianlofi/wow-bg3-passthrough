#!/usr/bin/env python3
"""wow_lift_platforms.py: the platforms BG3 keeps for WoW's lifts.

A WoW lift (an elevator, a tram car) carries BG3's characters on one of BG3's own moving platforms: a small level
of its own placed in ours as a level template and moved by BG3's PlatformMoveTo (tools/wowbg3_level.py). A
platform's floor is baked into its level, and a car's size is only known from the player's own WoW data, so the
level keeps a few round floors of several sizes, EACH of every size, parked off every WoW map's place. While the
game runs the mod takes the smallest one a car's floor fits in (mod Lifts.lua; the table there is lua()).

  specs()        the platform levels' specs (wowbg3_level.build): the clock's first, then the floors'
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
RADII = (3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.5)   # metres
EACH = 3                                    # Undercity has three cars in sight of each other
# A platform's floor reaches this far past the car's own: the walkable ground of a landing ends short of the shaft's
# edge (WoW's movement map keeps a walker's width from every edge), and BG3's path finder needs the platform's rim
# and the landing's ground to be neighbouring tiles. At a stop the platform takes the place of the ground it lies
# over (world_bridge.py carve_lifts). With 25 cm the hero could not step off Undercity's car at the top (harness).
MARGIN = 1.0
# Parked in the level's low corner, beside its start square and off every WoW map's place (test_wow_lift_platforms.py).
PARK = (-15700.0, -60.0, -15500.0)
STEP = 40.0
FLOOR_SHAPE = '1645d06c-0a6f-4656-9fc7-0dc8e174e655'   # a stock BG3 shape (by id) the platform's grid belongs to
# Clocks: platforms with no floor at all. In turn-based mode BG3 holds a platform near a character who is in the mode
# and moves it on the environment's turn only, six seconds' worth a round (stock, measured: the Emerald Grove's
# elevator and the creche's floating bed; a platform 32 m from the party runs on in real time). A car has to keep
# BG3's time, at rest too, and none of the extender's clocks does (Ext.Utils.GameTime runs on through the mode). So
# a clock platform beside each car near the party is kept moving at a known speed, and how far BG3 moved it says how
# much of BG3's time passed there (mod Lifts.lua).
CLOCKS = 6
CLOCK = 'WOWBG3_Lift_Clock'


def name(radius):
    return 'WOWBG3_Lift_%03d' % round(radius * 10)


def root(radius):
    return wowbg3_level.uid(name(radius), 'root')


def key(radius, i):
    return wowbg3_level.uid(LEVEL, 'lift', name(radius), i)


def park(radius, i):
    return (PARK[0] + STEP * i, PARK[1], PARK[2] + STEP * RADII.index(radius))


def clock_key(i):
    return wowbg3_level.uid(LEVEL, 'lift', CLOCK, i)


def clock_park(i):
    return (PARK[0] + STEP * i, PARK[1], PARK[2] + STEP * (len(RADII) + 1))


def clock_spec():
    return {'Name': CLOCK, 'DisplayName': 'Azeroth', 'Platform': True, 'Root': wowbg3_level.uid(CLOCK, 'root'),
            'Bounds': [-9, -100, -9, 9, 100, 9],
            'Scenery': [{'Template': FLOOR_SHAPE, 'Name': CLOCK + '_Floor', 'Pos': [0.0, -40.0, 0.0]}],
            'Subgrids': [{'X': -1.0, 'Y': 0, 'Z': -1.0, 'W': 4, 'H': 4, 'Kind': 'blank', 'Owner': 0}]}


def specs():
    out = [clock_spec()]
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
            for r in RADII for i in range(EACH)] + [
        {'Root': wowbg3_level.uid(CLOCK, 'root'), 'MapKey': clock_key(i), 'Name': '%s_%03d' % (CLOCK, i), 'Pos': list(clock_park(i))}
        for i in range(CLOCKS)]


def pick(radius):
    return next((r for r in RADII if r >= radius + MARGIN), None)


def lua():
    rows = ['    {Radius = %s, Park = {%s}, Keys = {%s}},' % (
        ('%g' % r), ', '.join('{%g, %g, %g}' % park(r, i) for i in range(EACH)), ', '.join('"%s"' % key(r, i) for i in range(EACH)))
        for r in RADII]
    clocks = 'M.CLOCKS = {Park = {%s}, Keys = {%s}}' % (
        ', '.join('{%g, %g, %g}' % clock_park(i) for i in range(CLOCKS)), ', '.join('"%s"' % clock_key(i) for i in range(CLOCKS)))
    return 'M.MARGIN = %g\nM.PLATFORMS = {\n%s\n}\n%s' % (MARGIN, '\n'.join(rows), clocks)


if __name__ == '__main__':
    if sys.argv[1:] == ['lua']:
        print(lua())
    else:
        print(__doc__)
        sys.exit(2)
