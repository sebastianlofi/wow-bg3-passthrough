#!/usr/bin/env python3
"""wow_lifts.py: WoW's lifts (elevators, tram cars) from the client's own table, for BG3's side of a ride.

A lift is a WoW game object of type 11. The server never sends where its car is, only a clock; the client
moves the car itself along TransportAnimation.dbc: keyframes (time, offset from the spawn point) per
game object entry, looped, the offset turned by the spawn's rotation. This module reads that table and
answers with the client's own rule (benilla-formats `elevators.rs`, the 1.12 client's 0x5f6280):

  read_paths(DBC_DIR)                    {entry: [(ms, (x, y, z))]} in time order
  period(frames)                         the cycle in ms
  sample(frames, spawn, quat, ms)        ((x, y, z) in WoW, moving) at a cycle time
  stops(frames)                          [(from ms, to ms, offset)]: where the car rests and a rider steps on or off
  at_stop(frames, ms)                    the stop the car rests at, or None while it travels
  legs(frames)                           [{From, To, Start, End, Spans}]: each journey between two stops
  speed(span)                            yards a second on one span of a leg
  to_bg3(wow, centre, origin)            world mode's mapping (world_bridge.py World.to_bg3)

  wow_lifts.py table DBC_DIR             every path: period, stops, legs, top speed

Nothing here moves anything: it is the timetable the BG3 side needs to move its platform in step with
WoW's car.
"""
import struct
import sys
from pathlib import Path

YD = 0.9144


def read_paths(dbc_dir):
    """TransportAnimation.dbc (1.12): id, game object entry, time ms, x, y, z, sequence. A path that cannot
    run a cycle (under two frames, no frame at 0, a period of 0) is left out, as the client leaves its car
    at the spawn point."""
    path = Path(dbc_dir) / 'TransportAnimation.dbc'
    b = path.read_bytes()
    magic, n, _, size, _ = struct.unpack_from('<4s4I', b, 0)
    if magic != b'WDBC':
        raise ValueError('%s: not a DBC file' % path)
    out = {}
    for i in range(n):
        _, entry, ms, x, y, z = struct.unpack_from('<3I3f', b, 20 + i * size)
        out.setdefault(entry, []).append((ms, (x, y, z)))
    for frames in out.values():
        frames.sort(key=lambda f: f[0])
    return {e: f for e, f in out.items() if len(f) >= 2 and f[0][0] == 0 and f[-1][0] > 0}


def period(frames):
    return max(frames[-1][0], 1)


def turn(v, q):
    """R(q) v for a unit quaternion (x, y, z, w)."""
    x, y, z, w = q
    t = (2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0]))
    return (v[0] + w * t[0] + (y * t[2] - z * t[1]),
            v[1] + w * t[1] + (z * t[0] - x * t[2]),
            v[2] + w * t[2] + (x * t[1] - y * t[0]))


def sample(frames, spawn, quat, ms):
    """Where the car is at a cycle time, and whether the span it is on moves."""
    t = ms % period(frames)
    hi = next((i for i, f in enumerate(frames) if f[0] > t), len(frames))
    (t0, p0), (t1, p1) = frames[hi - 1], frames[min(hi, len(frames) - 1)]
    if p0 == p1 or t1 == t0:
        local = p0
    else:
        k = (t - t0) / (t1 - t0)
        local = tuple(a + (b - a) * k for a, b in zip(p0, p1))
    r = turn(local, quat)
    return (spawn[0] + r[0], spawn[1] + r[1], spawn[2] + r[2]), p0 != p1


def stops(frames):
    """[(from ms, to ms, offset)]: every rest of the car, in time order from the one at the cycle's start.
    A rest over the cycle's seam is one stop, its `from` later than its `to`."""
    runs = []
    for (t0, p0), (t1, p1) in zip(frames, frames[1:]):
        if p0 != p1 or t1 == t0:
            continue
        if runs and runs[-1][1] == t0 and runs[-1][2] == p0:
            runs[-1] = (runs[-1][0], t1, p0)
        else:
            runs.append((t0, t1, p0))
    T = period(frames)
    if len(runs) > 1 and runs[0][0] == 0 and runs[-1][1] == T and runs[0][2] == runs[-1][2]:
        runs[0] = (runs[-1][0], runs[0][1], runs[0][2])
        runs.pop()
    return runs


def at_stop(frames, ms):
    """The index in stops() of the stop the car rests at, or None while it travels."""
    t = ms % period(frames)
    for i, (a, b, _) in enumerate(stops(frames)):
        if (a <= t < b) if a < b else (t >= a or t < b):
            return i
    return None


def legs(frames):
    """Each journey from one stop to the next: {From, To (indices in stops()), Start, End (ms; End runs past
    the period when the journey crosses the cycle's seam), Spans [(from ms, to ms, from offset, to offset)]}.
    The spans are the path's own, so a platform moved along them keeps the car's speeds."""
    rests = stops(frames)
    if not rests:
        return []
    T = period(frames)
    moving = [(t0, t1, p0, p1) for (t0, p0), (t1, p1) in zip(frames, frames[1:]) if p0 != p1 and t1 > t0]
    moving += [(t0 + T, t1 + T, p0, p1) for t0, t1, p0, p1 in moving]
    out = []
    for i, (_, start, _) in enumerate(rests):
        j = (i + 1) % len(rests)
        end = rests[j][0]
        if end < start or (end == start and len(rests) == 1):
            end += T
        if end == start:
            continue
        out.append({'From': i, 'To': j, 'Start': start, 'End': end,
                    'Spans': [s for s in moving if start <= s[0] and s[1] <= end]})
    return out


def speed(span):
    """Yards a second on one span."""
    t0, t1, p0, p1 = span
    return sum((a - b) ** 2 for a, b in zip(p0, p1)) ** 0.5 / ((t1 - t0) / 1000.0)


def to_bg3(w, centre, origin):
    """World mode's mapping of a WoW point (world_bridge.py World.to_bg3)."""
    return (centre[0] + (w[1] - origin[1]) * YD, centre[1] + (w[2] - origin[2]) * YD, centre[2] - (w[0] - origin[0]) * YD)


def main(argv):
    if len(argv) == 3 and argv[1] == 'table':
        for entry, frames in sorted(read_paths(argv[2]).items()):
            rests, journeys = stops(frames), legs(frames)
            top = max((speed(s) for g in journeys for s in g['Spans']), default=0.0)
            print('%7d  period %6.1f s  frames %3d  stops %d  legs %d  top %5.1f yd/s  %s' % (
                entry, period(frames) / 1000.0, len(frames), len(rests), len(journeys), top,
                '  '.join('(%.1f %.1f %.1f)' % r[2] for r in rests)))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
