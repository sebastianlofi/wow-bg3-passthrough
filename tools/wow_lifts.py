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

  deck(model groups)                     a car's floor from its collision model: height, area, radius, box
  models(VMAPS)  decks(templates, VMAPS, read_model)   every rideable lift's floor in metres, by template entry
  stop_points(frames, spawn, quat)       where a car rests, in WoW
  carve(section, rec, minx, minz, spots) a square's tiles without the static ground a car rests on
  read_live(bytes)                       benilla's link/wowlifts.bin: every lift on the map this frame

  wow_lifts.py table DBC_DIR             every path: period, stops, legs, top speed
  wow_lifts.py live FILE                 wowlifts.bin as text

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


def deck(groups):
    """A lift car's floor from its collision model (wow_vmap.read_model: [(lo, hi, verts, idx)], model space, WoW
    axes, yards): the level where most of its upward-facing area lies (its top side, not its underside or a roof's). {Floor (z of it), Area (square yards),
    Radius (the farthest floor point from the model's axis), Box (x0, y0, x1, y1)}, or None for a model with no
    floor to stand on (a door, a plunger)."""
    faces = []
    for _, _, verts, idx in groups:
        for i in range(0, len(idx), 3):
            a, b, c = (verts[3 * k:3 * k + 3] for k in idx[i:i + 3])
            u, v = [b[j] - a[j] for j in range(3)], [c[j] - a[j] for j in range(3)]
            n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            size = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5
            # The server's collision models wind a face clockwise seen from its front: a floor's normal by this
            # cross product points down. (Counting both ways took the underside of Undercity's car for its floor,
            # 0.29 yd low: the static floor round the shaft then lay above the platform and BG3 stood the rider on
            # that, harness.)
            if size > 1e-9 and -n[2] / size > 0.94:
                faces.append((size / 2.0, (a[2] + b[2] + c[2]) / 3.0, (a, b, c)))
    if not faces:
        return None
    levels = {}
    for area, z, _ in faces:
        levels[round(z * 2)] = levels.get(round(z * 2), 0.0) + area
    floor = max(levels, key=lambda k: levels[k]) / 2.0
    on = [(area, z, tri) for area, z, tri in faces if abs(z - floor) <= 0.5]
    area = sum(a for a, _, _ in on)
    if area < 4.0:
        return None
    pts = [p for _, _, tri in on for p in tri]
    return {'Floor': sum(a * z for a, z, _ in on) / area, 'Area': area,
            'Radius': max((p[0] ** 2 + p[1] ** 2) ** 0.5 for p in pts),
            'Box': (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))}


def models(vmaps):
    """{display id: model file name} from the server's collision data (vmaps/temp_gameobject_models: display id,
    name length, name, the model's bounds as six floats)."""
    raw = (Path(vmaps) / 'temp_gameobject_models').read_bytes()
    out, o = {}, 0
    while o + 8 <= len(raw):
        display, n = struct.unpack_from('<II', raw, o)
        if o + 8 + n + 24 > len(raw):
            break
        out[display] = raw[o + 8:o + 8 + n].decode('utf-8', 'replace')
        o += 8 + n + 24
    return out


def decks(templates, vmaps, read_model):
    """{entry: {Radius, Floor}} in metres for the lifts a character can ride: `templates` [(entry, display id)]
    of the server's type 11 game objects, each car's floor from its own collision model (deck). A model with no
    floor (a door) or no file is left out."""
    names, out, seen = models(vmaps), {}, {}
    for entry, display in templates:
        if display not in seen:
            seen[display] = None
            path = Path(vmaps) / (names.get(display, '') + '.vmo')
            if display in names and path.is_file():
                try:
                    seen[display] = deck(read_model(path))
                except (ValueError, struct.error, OSError):
                    pass
        d = seen[display]
        if d:
            out[int(entry)] = {'Radius': round(d['Radius'] * YD, 3), 'Floor': round(d['Floor'] * YD, 3)}
    return out


def stop_points(frames, spawn, quat):
    """Where a car rests, in WoW: [(x, y, z)] of its stops (the spawn point plus each stop's turned offset)."""
    out = []
    for _, _, off in stops(frames):
        r = turn(off, quat)
        p = (spawn[0] + r[0], spawn[1] + r[1], spawn[2] + r[2])
        if p not in out:
            out.append(p)
    return out


AWAY_M = 300.0   # a tile under a resting car is moved this far up, out of BG3's reach from the car's floor


def carve(section, rec, minx, minz, spots, reach=2.0, top=None):
    """A square's tile records without the ground a lift car rests on. Where a car stops, WoW's own floor under it
    is the car's; BG3 stands a walker on the tile nearest its height, so with the static tile left at its height
    (12 cm from the platform's in Undercity) the rider stood on that tile and stayed behind when the car left
    (harness). `section`: n x n records of `rec` bytes, the first a flags byte (1 walkable), then one float
    floor (rec 5) or two, low and high (rec 9), in BG3 m; (minx, minz) the square's corner; `spots` [(x, y, z,
    radius)] in BG3 metres, y the car's floor. Tiles within the radius whose floor lies within `reach` of it lose
    the walkable bit and go AWAY_M up (not past `top`, the highest a tile of this section may lie; then they
    only lose the bit). (bytes, how many)."""
    n = int(round((len(section) // rec) ** 0.5))
    out, cut = None, 0
    for bx, by, bz, radius in spots:
        c0, c1 = int((bx - radius - minx) // 0.5), int((bx + radius - minx) // 0.5)
        r0, r1 = int((bz - radius - minz) // 0.5), int((bz + radius - minz) // 0.5)
        for r in range(max(r0, 0), min(r1, n - 1) + 1):
            for c in range(max(c0, 0), min(c1, n - 1) + 1):
                k = (r * n + c) * rec
                x, z = minx + (c + 0.5) * 0.5, minz + (r + 0.5) * 0.5
                if (x - bx) ** 2 + (z - bz) ** 2 > radius * radius:
                    continue
                floor = struct.unpack_from('<f', section, k + 1)[0]
                if abs(floor - by) > reach:
                    continue
                if out is None:
                    out = bytearray(section)
                out[k] &= 0xFE
                up = floor + AWAY_M
                if top is None or up <= top:
                    for o in range(1, rec, 4):
                        struct.pack_into('<f', out, k + o, up)
                cut += 1
    return (bytes(out) if out is not None else section), cut


def read_live(raw):
    """benilla's link/wowlifts.bin (patch 0135): WBL1, seq, count, count x (guid u64, entry u32, flags u32 (1 moving),
    period ms, cycle ms, WoW xyz now, WoW xyz of the end of the span it is on, ms to that end), seq again.
    [{Guid, Entry, Moving, Period, Cycle, Pos, To, ToMs}], or None for a file that is not whole."""
    if len(raw) < 16 or raw[:4] != b'WBL1':
        return None
    seq, n = struct.unpack_from('<II', raw, 4)
    if len(raw) != 16 + 52 * n or struct.unpack_from('<I', raw, 12 + 52 * n)[0] != seq:
        return None
    out = []
    for i in range(n):
        guid, entry, flags, period, cycle, x, y, z, tx, ty, tz, to_ms = struct.unpack_from('<Q4I6fI', raw, 12 + 52 * i)
        out.append({'Guid': guid, 'Entry': entry, 'Moving': bool(flags & 1), 'Period': period, 'Cycle': cycle,
                    'Pos': (x, y, z), 'To': (tx, ty, tz), 'ToMs': to_ms})
    return out


def main(argv):
    if len(argv) == 3 and argv[1] == 'live':
        lifts = read_live(Path(argv[2]).read_bytes())
        if lifts is None:
            print('not a whole wowlifts.bin')
            return 1
        for l in sorted(lifts, key=lambda l: l['Entry']):
            print('%7d %016x  %s cycle %5d of %5d ms  at %9.1f %9.1f %7.1f  to %9.1f %9.1f %7.1f in %5d ms' % (
                l['Entry'], l['Guid'], 'moving' if l['Moving'] else 'rests ', l['Cycle'], l['Period'], *l['Pos'], *l['To'], l['ToMs']))
        return 0
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
