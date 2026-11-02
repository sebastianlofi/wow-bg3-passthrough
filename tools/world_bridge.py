#!/usr/bin/env python3
"""World-mode bridge (BG3 window is the game): keeps WoW ground under Tav wherever she walks.

world_bridge.py [--centre X Y Z] [--pool 6]

At start it reads where benilla's player stands (link/camera.bin player record) and maps that WoW
point onto the void spot (--centre) for the whole session: BG3 = centre + (dy, dz, -dx) * 0.9144
from that WoW origin. The ground is a treadmill ("continue creating floor
plane as she walks and deleting the old one"): BG3's AI grid gets whole 64 m squares (pool
subgrids of the out-of-bounds layer, moved by the wowbg3 plugin) on a lattice whose node (0, 0) is
the centre, each stamped with the WoW ground cut there by wow_arena.py, and the plugin's ray hooks
get the same squares' WoW triangles. Four squares around the lattice node nearest Tav are always
placed (at least 24 m of ground on every side of her), and up to two more ahead of her along the
way she is heading. When she crosses 8 m past a square's middle the window moves on: squares behind
her are moved ahead and the triangles are swapped in one step (no moment without ground).
A fall alarm logs a member under every WoW face (it moves nobody: World.fall_alarm).
"Tav" here is the hero: whichever character the player controls (the custom Tav of
the test save, or a new game's character such as the origin Gale), as the mod's Hero.lua reports it
in wow-bg3/hero.json; the bridge waits for the hero to be ready (in the tutorial level, no dialog,
a new game's intro skipped) before it sends Start (--wait-hero, default 120 s).
Commands go to WorldMode.lua (wow-bg3/world-command.json, Start / Stream / Stop), one at a time:
each waits until the mod reports it done (world-status.json Seq). Ctrl-C (or SIGTERM) sends Stop:
Tav and the party back, every square home, camera restored.
link/world.json carries the session mapping while it runs, so a WoW pull (wow-bg3-link bridge)
cuts its fight arena with the same mapping and LinkLive.lua fights where Tav already stands.

A treadmill per party member (party-multiplayer-design.md "Ground"):
every party member in Party.lua's wow-bg3/party.json (the hero first, its position from tav.bin) has
its own window of squares (the same rules as the hero's: the 2 x 2 round its lattice node, the
squares ahead or the --floor block), all from one pool, so members can be any distance apart. When
the pool is short, squares go in tiers: first the square under every member, then every member's
2 x 2, then the rest of each window, members nearest BG3's camera (bg3cam.bin) first; so the member
farthest from the camera gets fewer squares, never none. The fall alarm runs per member.
link/party-wow.json (Format 1) says
where every member stands in WoW (for the bots that stand in for them on the server: the fight bridge,
link/src/mirror.rs PartyLink, sends patch 0010's `move`, `feed` and `view` verbs from it). The plugin
(0.2.9) builds the ray hooks' cell grids per cluster of squares, so the triangles of members far
apart need no huge grid, and takes the pool from several out-of-bounds layers at once (--layer
"-1.981,9.74").
"""
import argparse, concurrent.futures, hashlib, json, math, os, re, shutil, signal, struct, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # the development workspace; relenv.py sets it in the release
IO = ROOT / "bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3"
LINK = IO / 'link'
CELLS = Path('/tmp/wowbg3/cells')  # wow_arena.py output per square (tmpfs, overwritten)
sys.path.insert(0, str(ROOT / 'link/tools'))
import wow_arena  # noqa: E402
import wow_stand  # noqa: E402


YD = 0.9144
CELL = 64.0      # metres: one pool subgrid, 128 x 128 tiles of 0.5 m
CUT_JOBS = 4     # wow_arena.py cuts side by side
TILES = 128
HYST = 8.0       # metres past a square's middle before the window moves
# Moves (link/tools/tele.sh): wow-bg3/world-move.json {RunId, Seq, Wow: [x, y, z or
# null], What} starts world mode again at that WoW point (Stop, then Start with it as the origin); the hero
# stands there in BG3 and benilla puts WoW's player on her, as on any big gap ("placed on BG3's
# character"). A WoW-side teleport alone never sticks: benilla's movement sync puts WoW's player back
# on the hero within a second (tried with .tele). Answer: world-move-result.json.
# On the session's own continent the session stays (World.reorigin, WorldMode.lua Reorigin): the lattice and
# the pool subgrids stay where they are, only the WoW point the centre stands for changes. The squares placed
# then still hold the old place (stale): a wanted one is retiled where it lies, one nobody wants goes first when
# a square needs a subgrid, and a stale storey over wanted ground gets tiles all blocked, or BG3 would keep finding the
# old place's upper floor there; the plugin registers such a storey in no patch (cells_for). Only a refusal, or another
# continent, takes Stop and Start.
CUT_CACHE = None  # (folder, version) of the cuts made ahead, once asked for (cut_cache)
# Kalimdor too: Map in the request picks the continent. Another continent than the
# session's: Stop, a server teleport of the WoW character to the game_tele place Via names on that
# continent (server/tools/world-console.py tele CHAR VIA: a worldport, WoW's loading screen), wait for
# benilla to stand there, then Start at the requested point on the new continent.
# "Arrived": true (benilla 0041: WoW itself already moved the character, a hearthstone or a
# graveyard port): no Via/Char and no server teleport; wait for benilla to stand on Map, then Start.
CONTINENTS = {0: 'Eastern Kingdoms', 1: 'Kalimdor'}
MOVE = IO / 'world-move.json'
MOVE_RESULT = IO / 'world-move-result.json'
COVER_WAIT_S = 30.0  # a move's cover waits at most so long after the move for benilla's player at the new place
LANDED_YD = 100.0  # yards: benilla's player this near where it should be counts as having arrived (World.settle)
AHEAD = 20.0     # metres from the window's node before squares are placed ahead of her
# A placed square whose middle is this near a member keeps its WoW triangles (the cursor's ground and the
# camera's) even when no window wants it (the 0.1.0 playtest: back at the quest giver
# after a boar, the squares he had walked on were still placed but had lost their triangles, so a click
# there found no ground and BG3 found no path). Farther ones still lose them, so the trails members leave
# never join into one long cluster (the plugin refuses one over 1 km).
KEEP_RAYS = 100.0
SERIALS = 64     # section-<k>-<serial>.bin names in use (overwritten round robin)
# What a square's tiles say a WoW player may stand on: wow_arena.py keeps them to the 1.12 client's
# own rule (a surface within 50 degrees of up; steeper slides the player back), so BG3's movement
# circle is WoW's. WOW_BG3_SLOPE_LIMIT changes the angle and 0 turns it off (the earlier state:
# every movement-map polygon counted, and BG3 offered the NPC-only steep slopes too).
# How the squares' tiles reach BG3's grid (WOW_BG3_TILE_MODE): flat (one height per tile, BG3's
# MinHeight = MaxHeight = WoW's ground at the tile's middle), span (MinHeight and MaxHeight the
# ground's lowest and highest point in the tile, wow_arena.py spans.bin), slope (one height, BG3's
# Slope flag on tiles whose ground rises more than SLOPE_FLAG_M across them), span+slope (both).
# BG3's pathfinder treats all four alike (a step between neighbour
# tiles must stay under the path's StepHeight whatever the span or flag), so flat, whose MinHeight is
# WoW's ground itself, is the default. See the m4b-world-mode notes, Ground fit.
TILE_MODE = os.environ.get('WOW_BG3_TILE_MODE', 'flat')
SLOPE_FLAG_M = 0.25
# Upper storeys (WOW_BG3_STOREYS, 0 = off: one floor a tile, as before). A square whose cut has an
# upper storey (wow_arena.py storey.bin: upstairs in an inn, the far side of its stairs) gets a
# second pool subgrid stacked over it, one tile further +X (the plugin keeps one subgrid to a lattice
# slot, and the storeys of neighbouring squares then meet edge to edge like the ground's). Its key is
# (i, j, 1) beside the ground's (i, j); storeys take the pool subgrids the ground leaves (never one a
# square of ground needs), and the pool asks for STOREY_SPARE more than --pool for them. A storey
# with fewer than STOREY_MIN walkable tiles is left out.
STOREYS = os.environ.get('WOW_BG3_STOREYS', '1') != '0'
FALL_UNDER_M = 3.0   # fall alarm: within this far under a tile's floor a walker is on it, no faces read
FALL_HEAD_M = 1.5    # fall alarm: a face up to this far over a walker's feet still counts as under it
FALL_READ_S = 0.5    # fall alarm: at most so often per walker a square's faces are read (a cave is read all the time)
FALL_SAY_S = 10.0    # fall alarm: at most so often per walker it is said
STOREY_SPARE = 2
STOREY_MIN = 8
# The flags a seam tile (where a storey meets the ground) gets in both sections: 4 SubgridEdge, 8
# SubgridIntersection (the plugin's Place; BG3's pathfinder changes subgrids only on such tiles).
SEAM_BITS = int(os.environ.get('WOW_BG3_SEAM_BITS') or 12) & 12


def run_id():
    c = json.loads((IO / 'm3-client.json').read_text())
    assert client_live(c), 'BG3 is not in a running level'
    return c['RunId']


def player_wow():
    raw = (LINK / 'camera.bin').read_bytes()
    assert len(raw) >= 96 and raw[:4] == b'WBC1', 'no benilla camera stream'
    x, y, z = struct.unpack_from('<3f', raw, 72)
    flags, = struct.unpack_from('<I', raw, 88)
    assert flags & 1, 'benilla has no placed player yet'
    return x, y, z


def player_map():
    """The WoW map benilla's player is on (camera.bin, 0 Eastern Kingdoms, 1 Kalimdor), or None."""
    try:
        raw = (LINK / 'camera.bin').read_bytes()
    except FileNotFoundError:
        return None
    if len(raw) < 96 or raw[:4] != b'WBC1':
        return None
    m, flags = struct.unpack_from('<II', raw, 84)
    return m if flags & 1 and m != 0xFFFFFFFF else None


def tav(since=0.0):
    """Tav's (WoW xyz, BG3 xyz) from tav.bin, if written after `since` (a left-over file from an
    earlier BG3 run holds a position in that run's mapping)."""
    try:
        path = LINK / 'tav.bin'
        if path.stat().st_mtime < since:
            return None
        raw = path.read_bytes()
    except FileNotFoundError:
        return None
    if len(raw) < 44 or raw[:4] != b'TAV1':
        return None
    seq, = struct.unpack_from('<I', raw, 4)
    wx, wy, wz, bx, by, bz, yaw = struct.unpack_from('<7f', raw, 8)
    flags, seq2 = struct.unpack_from('<II', raw, 36)
    return ((wx, wy, wz), (bx, by, bz)) if seq == seq2 else None


def tav_flags(since=0.0):
    """tav.bin's flags (1 the hero in a BG3 fight, 2 BG3's sound muted), 0 without a fresh record."""
    try:
        path = LINK / 'tav.bin'
        if path.stat().st_mtime < since:
            return 0
        raw = path.read_bytes()
    except FileNotFoundError:
        return 0
    if len(raw) < 44 or raw[:4] != b'TAV1':
        return 0
    flags, = struct.unpack_from('<I', raw, 36)
    return flags


def camp_state(run):
    """Camp.lua's record of a trip (wow-bg3/camp-state.json) for this BG3 run: at camp or not."""
    try:
        s = json.loads((IO / 'camp-state.json').read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return False
    return s.get('RunId') == run and s.get('InCamp') is True


# The compositor's words for a trip to camp: BG3's own picture (no void, no ground, no effects copy), with
# Tav BG3's to draw, and WoW's interface over it. wowbar as the world's words have it: it is how the frame is read.
CAMP_WORDS = 'camp bg3tav'


COVER_BLACK_S = 1.0  # from the flag to a black screen: the mod looks every 5 ticks, then BG3 fades for 0.5 s


def move_cover(on):
    """A move: BG3's own screen fade covers the party's stay in BG3's level (MoveLoading.lua). On: the time the screen
    is black by, for cover_wait; the flag wowbg3cmd.py wrote at the player's command (a few seconds old) counts."""
    flag = LINK / 'wowmove.flag'
    now = time.time()
    try:
        if on:
            since = now
            try:
                t = float(flag.read_text().split()[0])
                if now - 30.0 < t <= now:
                    since = t
            except (OSError, ValueError, IndexError):
                pass
            flag.write_text('%.3f\n' % now)
            return since + COVER_BLACK_S
        flag.unlink(missing_ok=True)
    except OSError as e:
        print('move: cover file:', e, flush=True)
    return now


def cover_wait(black_at):
    """Nothing of the party moves before the cover is black (move_cover); the cuts before it take that time anyway."""
    if black_at:
        time.sleep(max(0.0, black_at - time.time()))


def cover_done(w, since):
    """After a move (ended at `since`): True once its cover is lifted, when benilla's player stands at the new place
    (World.arrived) or COVER_WAIT_S have passed without it."""
    got = w.arrived()
    if got is not None and got[0]:
        move_cover(False)
        return True
    if time.time() - since > COVER_WAIT_S:
        print('move: cover lifted after %.0f s without benilla\'s player at the new place' % COVER_WAIT_S, flush=True)
        move_cover(False)
        return True
    return False


def camp_compose(at_camp):
    """A trip to camp (Camp.lua): BG3's own picture with WoW's interface over it, and back. benilla draws
    its interface without the world while link/wowcamp.flag exists; the compositor takes the words at once
    and, back from camp, shows no WoW frame until one follows BG3's camera again. The coordinator writes
    `compose` new at each world-mode start."""
    d = os.environ.get('WOWBG3_CAPTURE_DIR')
    cap = Path(d) if d else ROOT / 'bg3/private/logs/capture'
    on, off, flag = cap / 'compose', cap / 'compose.camp', LINK / 'wowcamp.flag'
    try:
        if at_camp and on.exists():
            words = on.read_text()
            if not off.exists():
                off.write_text(words)
            tmp = cap / 'compose.tmp'
            tmp.write_text(' '.join([CAMP_WORDS] + (['wowbar'] if 'wowbar' in words.split() else [])) + '\n')
            tmp.replace(on)  # a new file, a new time: the layer reads it
            flag.touch()
        elif not at_camp:
            flag.unlink(missing_ok=True)
            if off.exists():
                off.replace(on)
                os.utime(on)  # the layer reads the file when its time changes
    except OSError as e:
        print('camp: compose file:', e, flush=True)


def move_request(run, done):
    """A new world-move.json for this BG3 run (seq above `done`), or None."""
    try:
        m = json.loads(MOVE.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None
    if m.get('RunId') != run or not isinstance(m.get('Seq'), int) or m['Seq'] <= done:
        return None
    return m


def moves_done(run):
    """The last world-move.json Seq this BG3 run has already had: a request on disk from this run is the world
    before this load's (one BG3 run keeps its RunId across loads, so a new bridge counting from 0 moved the party
    there again after every reload); a request from another run is none of this one's, and its Seq says nothing
    (a fresh BG3's first move is Seq 1 again: seeded from the old file it was dropped, 120 s of cover, no answer)."""
    try:
        m = json.loads(MOVE.read_text())
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return 0
    if not isinstance(m, dict) or m.get('RunId') != run or not isinstance(m.get('Seq'), int):
        return 0
    return m['Seq']


def client_live(c):
    """BG3's client runs the level (m3-client.json GameState): Running, or held in PrepareRunning under
    BG3's own loading screen by the launcher plugin (bg3le patch 0004, wow-bg3/bg3-state.json Holding 1):
    BG3's server runs the level then, and world mode places its ground before
    the player sees anything."""
    if c.get('GameState') == 'Running':
        return True
    if c.get('GameState') != 'PrepareRunning':
        return False
    try:
        s = json.loads((IO / 'bg3-state.json').read_text())
    except (OSError, ValueError):
        return False
    return s.get('Client') == 'PrepareRunning' and s.get('Holding') == 1


def hero(run=None):
    """The hero's record (wow-bg3/hero.json, Hero.lua) for this BG3 run, or None."""
    try:
        run = run or json.loads((IO / 'm3-client.json').read_text())['RunId']
        h = json.loads((IO / 'hero.json').read_text())
    except (FileNotFoundError, json.JSONDecodeError, KeyError):
        return None
    return h if h.get('RunId') == run and h.get('Uuid') else None


def wait_hero(seconds):
    """Waits until BG3 runs a level and the mod reports the hero ready for world mode."""
    end = time.time() + seconds
    while True:
        try:
            c = json.loads((IO / 'm3-client.json').read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            c = {}
        h = hero(c.get('RunId')) if client_live(c) else None
        if h and h.get('Ready'):
            print(time.strftime('%H:%M:%S'), 'hero', h.get('Uuid'), h.get('Name'), 'in', h.get('Region'), flush=True)
            return h
        assert time.time() < end, 'no hero ready for world mode (wow-bg3/hero.json: %s)' % (h,)
        time.sleep(1.0)


def atomic_write(path, data):
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_bytes(data)
    tmp.replace(path)


def camera_wow():
    """BG3's camera eye in WoW yards (bg3cam.bin, BGC1: magic, seq, eye[3], ...), or None."""
    try:
        raw = (LINK / 'bg3cam.bin').read_bytes()
    except FileNotFoundError:
        return None
    if len(raw) < 96 or raw[:4] != b'BGC1':
        return None
    seq, = struct.unpack_from('<I', raw, 4)
    eye = struct.unpack_from('<3f', raw, 8)
    return eye if all(math.isfinite(v) for v in eye) else None


def lattice_centre(c):
    """The lattice is in tiles: the centre goes on the 0.5 m grid."""
    return (round(c[0] * 2) / 2, c[1], round(c[2] * 2) / 2)


def cut_args(terrain, map_id, centre, origin, ij):
    """wow_arena.py's arguments (after the output folder) for square ij of a session: the same ones, the same cut."""
    W, C = origin, centre
    cx, cz = C[0] + CELL * ij[0] + CELL / 2, C[2] + CELL * ij[1] + CELL / 2
    wx, wy = W[0] - (cz - C[2]) / YD, W[1] + (cx - C[0]) / YD
    by = C[1] + (terrain.height(wx, wy) - W[2]) * YD
    args = ['--wow', str(wx), str(wy), '--bg3', str(cx), str(by), str(cz), '--size', str(CELL), '--islands', 'border',
            '--map', str(map_id), '--storeys', '1' if STOREYS else '0',
            '--models']  # WoW's collision models (wow_vmap.py): BG3's sight and casts see WoW's buildings, trees and fences
    if STOREYS and W[2] < terrain.height(W[0], W[1]) - wow_arena.UNDER_CLEAR:
        args += ['--under', str(W[0]), str(W[1]), str(W[2])]
    return args


# Cuts made ahead (wow_waypoints.py prewarm): a square of a session whose origin is a flight point comes out the same
# every time, as its arguments hold the map, the point and the square. On the player's machine only: they are
# Blizzard-derived, never shipped, never in git (WOW_CUT_CACHE; the release keeps them in its data folder's cache). A folder per
# version of the cutting code and of the settings it reads, so a new version cuts anew.
def cut_cache():
    root = Path(os.environ.get('WOW_CUT_CACHE') or Path(__file__).resolve().parents[2] / 'tmp' / 'cutcache')
    h = hashlib.sha1()
    for name in ('wow_arena.py', 'wow_stand.py', 'wow_vmap.py'):
        h.update(Path(wow_arena.__file__).with_name(name).read_bytes())
    for name in ('WOW_BG3_SLOPE_LIMIT', 'WOW_BG3_STEEP_POLYS', 'WOW_BG3_STOREY_LEVELS'):
        h.update(repr(os.environ.get(name)).encode())
    return root, h.hexdigest()[:16]


def cut_key(args):
    return hashlib.sha1(' '.join(args).encode()).hexdigest()


def cached_cut(args):
    """The folder of a cut made ahead with these arguments, or None. A cut goes there whole (store_cut)."""
    global CUT_CACHE
    if CUT_CACHE is None:
        CUT_CACHE = cut_cache()
    root, stamp = CUT_CACHE
    out = root / stamp / cut_key(args)
    return out if out.is_dir() else None


def store_cut(args):
    """Cuts a square ahead into the cache, unless it is there. False if wow_arena.py could not cut it."""
    if cached_cut(args):
        return True
    root, stamp = CUT_CACHE
    out = root / stamp / cut_key(args)
    tmp = out.with_name(out.name + '.tmp')
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    r = subprocess.run([sys.executable, wow_arena.__file__, str(tmp)] + args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        shutil.rmtree(tmp, ignore_errors=True)
        return False
    os.replace(tmp, out)
    return True


def prune_cuts():
    """An older version's cuts go: nothing reads them any more, and they are rebuilt by the same prewarm."""
    cached_cut([])
    root, stamp = CUT_CACHE
    if root.is_dir():
        for d in root.iterdir():
            if d.is_dir() and d.name != stamp and re.fullmatch('[0-9a-f]{16}', d.name):
                shutil.rmtree(d, ignore_errors=True)


def face_under(tris, x, y, z):
    """The height of the nearest WoW face under a walker's feet at BG3 (x, y, z), or None: nothing of WoW's lies
    under it, the only real fall. Anything else is standing, or in the air over what it will land on (a shove off
    a cliff, a drop into a cave). A square's tiles hold one floor, in a cave the hill over it; the faces hold all."""
    return max((h for h in faces_over(tris, x, z) if h < y + FALL_HEAD_M), default=None)


def faces_over(tris, x, z):
    """The height of every WoW face over BG3 (x, z), walls left out by having no area seen from above. Also a
    hair to each side: a point on the line between two triangles lies on neither."""
    points = ((x, z), (x + 0.15, z + 0.05), (x - 0.15, z - 0.05), (x + 0.05, z - 0.15), (x - 0.05, z + 0.15))
    x0, x1, z0, z1 = x - 0.15, x + 0.15, z - 0.15, z + 0.15
    out = []
    for ax, ay, az, bx, by, bz, cx, cy, cz in struct.iter_unpack('<9f', tris):
        if (x1 < ax and x1 < bx and x1 < cx) or (x0 > ax and x0 > bx and x0 > cx) \
                or (z1 < az and z1 < bz and z1 < cz) or (z0 > az and z0 > bz and z0 > cz):
            continue
        d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
        if abs(d) < 1e-6:
            continue
        for px, pz in points:
            u = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz)) / d
            v = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz)) / d
            if u >= 0.0 and v >= 0.0 and u + v <= 1.0:
                out.append(u * ay + v * by + (1.0 - u - v) * cy)
                break
    return out


class Walker:
    """One party member's window on the lattice (its own node, look-ahead axis and fall alarm)."""
    def __init__(self, uuid, name):
        self.uuid, self.name = uuid, name
        self.node = (0, 0)
        self.ahead_axis = None
        self.fall_said = 0.0
        self.faces_read = 0.0
        self.seen = 0.0


class World:
    def __init__(self, a, map_id=None):
        self.a = a
        self.run = run_id()
        self.seq = 0
        self.serial = 0
        self.ready = set()      # squares precut has cut and cut has not read yet
        # The WoW continent whose ground world mode cuts (Kalimdor too): the player's.
        self.map = map_id if map_id is not None else (player_map() or 0)
        assert self.map in CONTINENTS, 'world mode has ground for Eastern Kingdoms and Kalimdor only (map %s)' % self.map
        self.terrain = wow_arena.Terrain(Path(a.maps), self.map)
        self.centre = lattice_centre(a.centre)
        self.origin = None   # WoW point mapped onto self.centre for the whole session
        self.walkers = {}       # member uuid -> Walker (the hero's under its uuid, or 'hero' before hero.json)
        self.members = []       # [(Walker, (bx, by, bz), dead)] as last seen, nearest the camera first
        self.pinned = {}        # pool index -> until when it is not moved (something stood on it)
        self.placed = {}     # pool index -> (i, j)
        self.split = {}      # pool index -> squares it is at on one side or the other (sides disagree)
        self.cells = {}      # (i, j) -> cut square (cache, newest last)
        self.rays = None     # the squares whose triangles the plugin has (sorted), as last sent
        self.ground_pool = a.pool                                   # squares of ground at most
        self.pool = a.pool + (STOREY_SPARE if STOREYS else 0)    # pool subgrids, storeys included
        self.started = 0.0
        self.stale = set()      # pool indices whose square still holds the place before the last re-origin
        self.blocked = set()    # the stale storeys already given tiles all blocked (still stale: no new ground yet)
        self.to_block = set()   # those moves() has just asked cells_for to block
        self.landed = None      # when the last re-origin landed, until benilla's player has followed (settle)

    # ---- mapping ----
    def to_bg3(self, w):
        W, C = self.origin, self.centre
        return (C[0] + (w[1] - W[1]) * YD, C[1] + (w[2] - W[2]) * YD, C[2] - (w[0] - W[0]) * YD)

    def wow_xy(self, bx, bz):
        W, C = self.origin, self.centre
        return W[0] - (bz - C[2]) / YD, W[1] + (bx - C[0]) / YD

    def cell_min(self, ij):
        # An upper level (i, j, k) lies a tile off its square (see STOREYS).
        dc, dr = wow_arena.STOREY_OFFSETS[ij[2] - 1] if len(ij) == 3 else (0, 0)
        return self.centre[0] + CELL * ij[0] + 0.5 * dc, self.centre[2] + CELL * ij[1] + 0.5 * dr

    # ---- commands to WorldMode.lua ----
    def command(self, action, **fields):
        path = IO / 'world-command.json'
        try:
            old = json.loads(path.read_text())
            self.seq = max(self.seq, old.get('Sequence', 0) if old.get('RunId') == self.run else 0)
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        self.seq += 1
        body = dict(fields, RunId=self.run, Sequence=self.seq, Action=action)
        atomic_write(path, json.dumps(body).encode())
        shown = {k: v for k, v in fields.items() if k != 'Cells'}
        if 'Cells' in fields:
            shown['Cells'] = [[c[0], round(c[1], 1), round(c[2], 1), round(c[3], 2)] for c in fields['Cells']]
        print(time.strftime('%H:%M:%S'), action, json.dumps(shown), flush=True)
        return self.seq

    def wait_done(self, seq, timeout):
        """The mod's report for command `seq` (world-status.json): (ok, error)."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                s = json.loads((IO / 'world-status.json').read_text())
                if s.get('RunId') == self.run and s.get('Seq') == seq:
                    return s.get('OK') is True, s.get('Error')
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            time.sleep(0.2)
        return False, 'no report from the mod in %d s' % timeout

    # ---- squares ----
    def cut_cmd(self, ij):
        """The wow_arena.py command for square ij, and its output folder; no command for one made ahead."""
        args = cut_args(self.terrain, self.map, self.centre, self.origin, ij)
        ahead = cached_cut(args)
        if ahead:
            return None, ahead
        out = CELLS / ('%d_%d' % ij)
        out.mkdir(parents=True, exist_ok=True)
        return [sys.executable, str(ROOT / 'link/tools/wow_arena.py'), str(out)] + args, out

    def precut(self, squares):
        """The squares a round needs, cut side by side: a city's takes 3 s, and a move waited for each in turn.
        Their movement tiles first, one at a time, so no two cuts build the same tile."""
        todo = [ij for ij in dict.fromkeys(squares) if ij not in self.cells and ij not in self.ready]
        cmds = {ij: self.cut_cmd(ij) for ij in todo}
        todo = [ij for ij in todo if cmds[ij][0]]
        if len(todo) < 2:
            return
        maps, mmaps = wow_stand.data_dir('WOW_BG3_MAPS', 'maps'), wow_stand.data_dir('WOW_BG3_MMAPS', 'mmaps')
        for ij in todo:
            minx, minz = self.cell_min(ij)
            wx, wy = self.wow_xy(minx + CELL / 2, minz + CELL / 2)
            wow_arena.ensure_mmtiles(mmaps, maps, self.map, wx, wy, CELL / 2 / wow_arena.YD + 3)
        with concurrent.futures.ThreadPoolExecutor(CUT_JOBS) as pool:
            for ij, f in [(ij, pool.submit(subprocess.run, cmds[ij][0], check=True, stdout=subprocess.DEVNULL)) for ij in todo]:
                f.result()
                self.ready.add(ij)

    def cut(self, ij):
        """WoW ground for square ij: tiles (for the AI grid), triangles (for the ray hooks), base."""
        if ij in self.cells:
            self.cells[ij] = self.cells.pop(ij)  # newest last
            return self.cells[ij]
        cmd, out = self.cut_cmd(ij)
        if ij in self.ready:
            self.ready.discard(ij)
        elif cmd:
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        minx, minz = self.cell_min(ij)
        cx, cz = minx + CELL / 2, minz + CELL / 2
        by = self.centre[1] + (self.terrain.height(*self.wow_xy(cx, cz)) - self.origin[2]) * YD
        tiles = (out / 'tiles.bin').read_bytes()
        tris = (out / 'triangles.bin').read_bytes()
        kinds = (out / 'kinds.bin').read_bytes()
        assert len(tiles) == TILES * TILES * 5, 'wow_arena.py cut the wrong size'
        assert len(kinds) * 36 == len(tris), 'wow_arena.py wrote kinds for another triangle set'
        floors = [struct.unpack_from('<f', tiles, k * 5 + 1)[0] for k in range(TILES * TILES)]
        section = tiles
        if TILE_MODE != 'flat':
            # 9-byte records for the plugin's Place: flags (1 walkable, 2 Slope), MinHeight, MaxHeight.
            spans = (out / 'spans.bin').read_bytes()
            assert len(spans) == TILES * TILES * 8, 'wow_arena.py wrote no spans'
            recs = []
            for k in range(TILES * TILES):
                walk = tiles[k * 5]
                lo, hi = struct.unpack_from('<ff', spans, k * 8)
                slope = walk and 'slope' in TILE_MODE and hi - lo > SLOPE_FLAG_M
                if 'span' not in TILE_MODE:
                    lo = hi = floors[k]
                recs.append(struct.pack('<Bff', (1 if walk else 0) | (2 if slope else 0), lo, hi))
            section = b''.join(recs)
            floors = floors + [v for k in range(TILES * TILES) for v in struct.unpack_from('<ff', spans, k * 8)]
        base = math.floor((min(floors) - 0.5) * 50) / 50
        assert max(floors) - base < 1300, 'a square spans more height than a subgrid holds'
        cell = {'ij': ij, 'min': (minx, minz), 'base': base, 'tiles': tiles, 'section': section, 'tris': tris,
                'kinds': kinds, 'storey': None}
        if STOREYS:
            meta = json.loads((out / 'arena.json').read_text())
            ups = meta.get('storey_levels') or []
            if ups and ups[0]['tiles'] >= STOREY_MIN:
                # wow_arena.py's sections as they are, the seam bits as SEAM_BITS says; into each
                # section below a level (the ground's for the first) go the seams and the tiles
                # copied where the two meet (flags 1: walkable at that floor).
                def overlay(section, name):
                    seams = (out / name).read_bytes()
                    assert len(seams) == TILES * TILES * 5, 'wow_arena.py wrote no seams'
                    rec = len(section) // (TILES * TILES)
                    g = bytearray(section)
                    for k in range(TILES * TILES):
                        fl = seams[k * 5]
                        if fl & 1:
                            h = seams[k * 5 + 1:k * 5 + 5]
                            g[k * rec:k * rec + rec] = bytes([1 | SEAM_BITS]) + h + (h if rec == 9 else b'')
                        elif fl and (g[k * rec] & 1 or fl == 8):   # 8 alone: under a level, blocked or not
                            g[k * rec] |= fl & SEAM_BITS
                    return bytes(g)
                cell['section'] = overlay(section, 'seams.bin')
                cell['storeys'] = []
                for i, lv in enumerate(ups):
                    st = (out / ('storey.bin' if i == 0 else 'storey%d.bin' % (i + 1))).read_bytes()
                    assert len(st) == TILES * TILES * 5, 'wow_arena.py wrote no storey'
                    section_s = b''.join(bytes([st[k * 5] & (1 | SEAM_BITS)]) + st[k * 5 + 1:k * 5 + 5]
                                         for k in range(TILES * TILES))
                    if i + 1 < len(ups):
                        section_s = overlay(section_s, 'seams%d.bin' % (i + 1))
                    dc, dr = lv['off']
                    cell['storeys'].append({'min': (minx + 0.5 * dc, minz + 0.5 * dr), 'base': lv['base'],
                                            'section': section_s, 'tiles': lv['tiles'], 'floors': st})
                    assert max(struct.unpack_from('<f', st, k * 5 + 1)[0] for k in range(TILES * TILES)) - lv['base'] < 1300, \
                        'a storey spans more height than a subgrid holds'
                cell['storey'] = cell['storeys'][0]
        self.cells[ij] = cell
        # Every placed square stays cached (its triangles go to the plugin at every Stream, and the
        # fall alarm reads its floors and faces): a pool of up to 128 squares, plus room for squares coming in.
        keep = set(self.placed.values()) | {ij}
        while len(self.cells) > max(40, self.pool + 24):
            old = next((k for k in self.cells if k not in keep), None)
            if old is None:
                break
            self.cells.pop(old)
        return cell

    def window(self, w, bx, bz):
        """One member's squares as (core, rest): core the 2 x 2 round its lattice node (with
        hysteresis), rest the squares ahead of it (two once it is 20 m out) or, with --floor N > 2, the
        rest of an N x N block, nearest first."""
        ni, nj = w.node
        dx = bx - (self.centre[0] + CELL * ni)
        dz = bz - (self.centre[2] + CELL * nj)
        if abs(dx) > CELL / 2 + HYST:
            ni += int(math.floor(dx / CELL + 0.5))
        if abs(dz) > CELL / 2 + HYST:
            nj += int(math.floor(dz / CELL + 0.5))
        w.node = (ni, nj)
        dx = bx - (self.centre[0] + CELL * ni)
        dz = bz - (self.centre[2] + CELL * nj)
        want = [(ni - 1, nj - 1), (ni, nj - 1), (ni - 1, nj), (ni, nj)]
        # The axis to look ahead on: the one it is further out on, kept until the other one leads
        # by 6 m (walking diagonally would otherwise swap the squares ahead back and forth).
        axis = w.ahead_axis
        if axis == 'x' and abs(dz) > abs(dx) + 6 or axis == 'z' and abs(dx) > abs(dz) + 6 or axis is None:
            axis = 'x' if abs(dx) >= abs(dz) else 'z'
        extra = []
        if axis == 'x' and abs(dx) > AHEAD:
            col = ni + 1 if dx > 0 else ni - 2
            extra = [(col, nj - 1), (col, nj)]
            if dz > 0:
                extra.reverse()  # with one spare square, the one nearer comes first
        elif axis == 'z' and abs(dz) > AHEAD:
            row = nj + 1 if dz > 0 else nj - 2
            extra = [(ni - 1, row), (ni, row)]
            if dx > 0:
                extra.reverse()
        w.ahead_axis = axis
        if self.a.floor <= 2:
            return want, extra
        # A bigger floor (a host level with more spare squares): a floor x floor block
        # round the node, the middle 2 x 2 first, then by distance from the member.
        h = self.a.floor // 2
        block = [(i, j) for i in range(ni - h, ni + self.a.floor - h) for j in range(nj - h, nj + self.a.floor - h)
                 if (i, j) not in want]
        block.sort(key=lambda ij: (self.centre[0] + CELL * (ij[0] + 0.5) - bx) ** 2
                   + (self.centre[2] + CELL * (ij[1] + 0.5) - bz) ** 2)
        return want, block

    def under(self, bx, bz):
        """The lattice square a BG3 point lies in."""
        return (int(math.floor((bx - self.centre[0]) / CELL)), int(math.floor((bz - self.centre[2]) / CELL)))

    def walker(self, uuid, name):
        w = self.walkers.get(uuid)
        if w is None:
            w = self.walkers[uuid] = Walker(uuid, name)
            # A new member starts at the node nearest it (no hysteresis history).
            w.node = None
        w.seen = time.time()
        w.name = name or w.name
        return w

    def in_level(self, p):
        """Whether a BG3 point lies in the level's own AI grid (64 m round it), as WorldMode measured it
        at Start (world-status.json LevelBounds)."""
        if not hasattr(self, '_bounds'):
            self._bounds = None
        if self._bounds is None:
            try:
                st = json.loads((IO / 'world-status.json').read_text())
                b = st.get('LevelBounds') if st.get('RunId') == self.run else None
                if isinstance(b, list) and len(b) == 4:
                    self._bounds = [float(v) for v in b]
            except (FileNotFoundError, json.JSONDecodeError, TypeError, ValueError):
                pass
        b = self._bounds
        return bool(b) and b[0] - 64 <= p[0] <= b[2] + 64 and b[1] - 64 <= p[2] <= b[3] + 64

    def read_members(self, hero_pos):
        """[(Walker, BG3 position, dead)]: the hero (tav.bin's position) first, then every other
        party member on stage (party.json, Party.lua), sorted by distance from BG3's camera (the hero
        stays in the list even when the camera is elsewhere)."""
        h = hero(self.run)
        hero_uuid = (h or {}).get('Uuid') or 'hero'
        self.hero_walker = self.walker(hero_uuid, (h or {}).get('Name') or 'hero')
        out = [(self.hero_walker, hero_pos, False)]
        try:
            party = json.loads((IO / 'party.json').read_text())
            if party.get('RunId') == self.run:
                for m in party.get('Members', []):
                    p = m.get('Pos')
                    u = m.get('Uuid')
                    if m.get('Hero') or u == hero_uuid or not isinstance(u, str) or m.get('OnStage') is False:
                        continue
                    if not isinstance(p, list) or len(p) != 3 or not all(isinstance(v, (int, float)) for v in p):
                        continue
                    # Only members in the void (the treadmill's lattice), never one standing in the level
                    # itself (its own AI grid, world-status.json LevelBounds, 64 m round it).
                    if abs(p[0] - self.centre[0]) > 20000 or abs(p[2] - self.centre[2]) > 20000 or self.in_level(p):
                        continue
                    out.append((self.walker(u, m.get('Name') or u[:8]), tuple(p), bool(m.get('Dead'))))
        except (FileNotFoundError, json.JSONDecodeError, TypeError, ValueError):
            pass
        eye = camera_wow()
        cam = self.to_bg3(eye) if eye and self.origin else hero_pos
        out.sort(key=lambda e: (e[2], (e[1][0] - cam[0]) ** 2 + (e[1][2] - cam[2]) ** 2))
        self.members = out
        return out

    def wanted(self, members):
        """Every member's squares from one pool, in tiers (see the module notes): the square under
        each member, then each member's 2 x 2, then the rest of each window, members nearest the
        camera first; cut to the pool."""
        tiers = [[], [], []]
        for w, p, _ in members:
            if w.node is None:
                w.node = (int(math.floor((p[0] - self.centre[0]) / CELL + 0.5)),
                          int(math.floor((p[2] - self.centre[2]) / CELL + 0.5)))
            core, rest = self.window(w, p[0], p[2])
            tiers[0].append(self.under(p[0], p[2]))
            tiers[1].extend(core)
            tiers[2].extend(rest)
        out = []
        for t in tiers:
            for ij in t:
                if ij not in out:
                    out.append(ij)
        out = out[:self.ground_pool]
        self.write_floor(out)
        if STOREYS:
            # The storeys of the squares already cut (a new square's comes the round after its cut),
            # in the squares' order, in the pool subgrids the ground leaves.
            ups = [ij + (k + 1,) for k in range(len(wow_arena.STOREY_OFFSETS)) for ij in out
                   if k < len((self.cells.get(ij) or {}).get('storeys') or ())]
            out = out + ups[:max(0, self.pool - len(out))]
        return out

    def write_floor(self, squares):
        """wow-bg3/floor.json: the squares world mode keeps (BG3 min corners, 64 m), for Mirrors.lua (no
        stand-in spawned past the ground's edge)."""
        body = {'RunId': self.run, 'PartyFloor': True, 'Cell': CELL,
                'Squares': [[self.centre[0] + CELL * i, self.centre[2] + CELL * j] for i, j in squares]}
        try:
            atomic_write(IO / 'floor.json', json.dumps(body).encode())
        except OSError:
            pass

    def moves(self, want):
        """[(pool index, square)]: squares in `want` not placed yet, filled from pool subgrids not
        in `want` (stale ones first, then never placed ones, then the farthest from every member); the stale
        squares in `want` where they lie, nothing has to leave them for that (the plugin's pool_same_spot); and last
        the stale storeys over a wanted square that no cut wants, which only go blocked (self.to_block)."""
        at = {ij: k for k, ij in self.placed.items()}
        missing = [ij for ij in want if ij not in at]
        # A pool subgrid one side moved and the other did not (a refused Place) holds its square on
        # that side: the same subgrid goes there again (the plugin refuses any other: "another pool
        # subgrid is placed there"), and no other subgrid is sent to a square it holds.
        now = time.time()
        pairs, used = [], set()
        for ij in list(missing):
            for k, spots in sorted(self.split.items()):
                if ij in spots and k not in used and self.pinned.get(k, 0) < now:
                    pairs.append((k, ij))
                    used.add(k)
                    missing.remove(ij)
                    break
        held = set().union(*self.split.values()) if self.split else set()
        missing = [ij for ij in missing if ij not in held]
        spots = [p for _, p, _ in self.members] or [(self.centre[0], 0, self.centre[2])]
        def far(k):
            if k not in self.placed:
                return 1e12
            minx, minz = self.cell_min(self.placed[k])
            return min(math.hypot(minx + CELL / 2 - p[0], minz + CELL / 2 - p[2]) for p in spots)
        free = sorted((k for k in range(self.pool) if k not in self.split and self.placed.get(k) not in want
                       and self.pinned.get(k, 0) < now), key=lambda k: (k in self.stale, far(k)), reverse=True)
        # In `want` order, new and stale alike: the square under every member comes first.
        free, out = iter(free), []
        for ij in want:
            if ij in at:
                if at[ij] in self.stale:
                    out.append((at[ij], ij))
            elif ij in missing:
                k = next(free, None)
                if k is not None:
                    out.append((k, ij))
        pairs += out
        used = {k for k, _ in pairs}
        self.to_block = {k for k in self.stale - self.blocked - used
                         if len(self.placed.get(k, ())) == 3 and self.placed[k][:2] in want}
        return pairs + sorted((k, self.placed[k]) for k in self.to_block)

    def cells_for(self, pairs):
        """Writes each square's tiles for the plugin (section-<k>-<serial>.bin) -> Cells for the mod."""
        out = []
        self.precut([ij[:2] for k, ij in pairs if k not in self.to_block])
        for k, ij in pairs:
            if k in self.to_block:
                # No walkable tile: the plugin's Place takes a storey without a floor out of every patch, so BG3
                # finds nothing of it at any height (wowbg3_plugin.cpp pool_place). Registered with blocked tiles at
                # the centre's height it was the hero's own floor after a move's teleport, and BG3's server ran no
                # tick for 7 to 13.5 s after such re-origins (0.1.3013.59).
                self.serial = self.serial % SERIALS + 1
                atomic_write(IO / ('section-%d-%d.bin' % (k, self.serial)),
                             (b'\0' + struct.pack('<f', self.centre[1])) * (TILES * TILES))
                out.append([k, *self.cell_min(ij), self.centre[1], self.serial])
                continue
            cell = self.cut(ij[:2])
            if len(ij) == 3:
                cell = cell['storeys'][ij[2] - 1]
            self.serial = self.serial % SERIALS + 1
            atomic_write(IO / ('section-%d-%d.bin' % (k, self.serial)), cell['section'])
            out.append([k, cell['min'][0], cell['min'][1], cell['base'], self.serial])
        return out

    def write_triangles(self, squares):
        # What each triangle is (terrain, wall, WMO, M2, flat floor), for the plugin's camera clearance;
        # it reads both files at the next Rays command and ignores kinds of another count.
        cells = [self.cut(ij) for ij in squares if len(ij) == 2]   # storeys bring no triangles of their own
        atomic_write(IO / 'arena-triangles.kinds', b''.join(c['kinds'] for c in cells))
        atomic_write(IO / 'arena-triangles.bin', b''.join(c['tris'] for c in cells))

    def pool_files(self):
        """Where the plugin says the pool subgrids are, per side: {k: (i, j) or None}."""
        sides = []
        for side in ('server', 'client'):
            entries = json.loads((IO / ('native-%s-pool.json' % side)).read_text())
            got = {}
            for e in entries:
                ij = None
                if e.get('Moved'):
                    dx = e['At'][0] - self.centre[0]
                    ij = (int(round(dx / CELL)), int(round((e['At'][2] - self.centre[2]) / CELL)))
                    off = (round((dx - CELL * ij[0]) * 2), round((e['At'][2] - self.centre[2] - CELL * ij[1]) * 2))
                    if off != (0, 0):   # an upper level: a tile off its square
                        ij += (wow_arena.STOREY_OFFSETS.index(off) + 1,)
                got[e['K']] = ij
            sides.append(got)
        return sides

    def resync(self):
        """After a refused command: believe the plugin. A pool subgrid placed differently on the two
        sides is not counted as placed; its squares stay held for it (self.split, see moves)."""
        try:
            server, client = self.pool_files()
        except (FileNotFoundError, json.JSONDecodeError, KeyError) as e:
            print('resync failed:', e, flush=True)
            return
        self.pool = min(self.pool, len(server), len(client))
        self.ground_pool = min(self.ground_pool, self.pool)
        self.placed = {k: server[k] for k in server if k < self.pool and server[k] is not None and server[k] == client.get(k)}
        self.split = {k: {server.get(k), client.get(k)} - {None} for k in range(self.pool)
                      if server.get(k) != client.get(k) and ({server.get(k), client.get(k)} - {None})}
        self.stale &= set(self.placed)
        self.blocked &= self.stale
        print('resync:', self.placed, 'split:', self.split, flush=True)

    # ---- session ----
    def landing(self, at, login, want):
        """The ground a Start or a re-origin lands on: the squares `want` cut, and, at a point on a raised floor (a city
        street), its storeys, returned the level under the hero first; they go down with the ground, or the hero
        teleported there finds only the terrain under them and stays on it. May correct self.origin to the floor the
        cut has. `login`: the point is where WoW's player stands, the terrain will do."""
        def cut_all():
            self.precut(want)
            for ij in want:
                self.cut(ij)
        cut_all()
        if not at or abs(at[2] - self.terrain.height(at[0], at[1])) <= wow_arena.STOREY_CLEAR:
            return []
        # A cut holds one storey a tile: where it kept another floor of this column, that one.
        y = self.storey_floors(self.centre[0], self.centre[2])
        if y is None and login:
            self.origin = (at[0], at[1], self.terrain.height(at[0], at[1]))
            print('start: no raised floor under WoW\'s player, the terrain', flush=True)
            self.cells.clear()
            cut_all()
        elif y is not None and abs(y - self.centre[1]) > wow_arena.STOREY_CLEAR * YD:
            self.origin = (at[0], at[1], at[2] + (y - self.centre[1]) / YD)
            print('move: the cut has no floor at z %.1f here, z %.1f instead' % (at[2], self.origin[2]), flush=True)
            self.cells.clear()
            cut_all()
        # A Start holds 12 squares: the level under the hero first, the others stream in after it.
        def gap(u):
            st = self.cells[u[:2]]['storeys'][u[2] - 1]
            c0, r0 = int((self.centre[0] - st['min'][0]) // 0.5), int((self.centre[2] - st['min'][1]) // 0.5)
            if 0 <= c0 < TILES and 0 <= r0 < TILES and st['floors'][(r0 * TILES + c0) * 5] & 1:
                return abs(struct.unpack_from('<f', st['floors'], (r0 * TILES + c0) * 5 + 1)[0] - self.centre[1])
            return math.inf
        return sorted([ij + (k + 1,) for k in range(len(wow_arena.STOREY_OFFSETS)) for ij in want
                       if k < len(self.cells[ij].get('storeys') or ())], key=gap)

    def start(self, at=None):
        """World mode at WoW's player, or at the WoW point `at` (x, y, z): its origin for the session."""
        login = False
        if at:
            self.origin = tuple(at)
        else:
            wx, wy, wz = player_wow()
            self.origin = (wx, wy, self.terrain.height(wx, wy))
            # WoW's player on a raised floor (a city street) starts on it, not on the terrain under it.
            if STOREYS and abs(wz - self.origin[2]) > wow_arena.STOREY_CLEAR:
                at, login = (wx, wy, wz), True
                self.origin = tuple(at)
        w = self.walker((hero(self.run) or {}).get('Uuid') or 'hero', 'hero')
        w.node = (0, 0)
        want, _ = self.window(w, self.centre[0], self.centre[2])
        ups = self.landing(at, login, want)
        pairs = list(zip(range(4), want)) + list(zip(range(4, min(self.pool, 12)), ups))
        cells = self.cells_for(pairs)
        self.write_triangles(want)
        self.rays = sorted(want)
        seq = self.command('Start', Centre=list(self.centre), WowOrigin=list(self.origin), Cells=cells, PoolSize=self.pool,
                           Layer=self.a.layers[0], Layers=self.a.layers)
        ok, err = self.wait_done(seq, 90)
        assert ok, 'Start failed: %s' % err
        self.placed = dict(pairs)
        try:
            server, client = self.pool_files()
            self.pool = min(self.pool, len(server), len(client))
            self.ground_pool = min(self.ground_pool, self.pool)
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        print('pool of', self.pool, 'squares; placed', self.placed, flush=True)
        self.started = time.time()
        self.publish(True)

    def reorigin(self, at, covered=None):
        """This session at the WoW point `at` (see the notes at the top). None, or why not: then the caller goes on
        with Stop and Start, which take everything home whatever this left (the mod may have stopped publishing).
        `covered`: called once the cuts are made, before the mod's part (move: waits for the cover to be black)."""
        self.origin = tuple(at)
        self.cells.clear()
        self.ready.clear()
        self.walkers.clear()
        self.pinned.clear()
        self.stale = set(self.placed)
        self.blocked = set()
        w = self.walker((hero(self.run) or {}).get('Uuid') or 'hero', 'hero')
        w.node = (0, 0)
        want, _ = self.window(w, self.centre[0], self.centre[2])
        ups = self.landing(at, False, want)
        squares = want + ups[:max(0, min(self.pool, 12) - len(want))]
        why = None
        for _ in range(3):
            pairs = self.moves(squares)
            short = set(want) - {ij for _, ij in pairs}
            if short:
                return why or 'no pool subgrid for %s' % sorted(short)
            cells = self.cells_for(pairs)
            self.write_triangles(want)
            if covered:
                covered()
                covered = None
            seq = self.command('Reorigin', WowOrigin=list(self.origin), Cells=cells)
            ok, why = self.wait_done(seq, 45)
            if ok:
                for k, ij in pairs:
                    self.took(k, ij)
                self.rays = sorted(want)
                self.started = self.landed = time.time()
                return None
            # As in follow: a subgrid on its way to another square that something stands on waits, another one goes.
            m = re.search(r'stands on pool ([0-9 ]+)', str(why))
            if not m:
                return why
            for k in m.group(1).split():
                self.pinned[int(k)] = time.time() + 15.0
        return why

    def took(self, k, ij):
        """Pool subgrid k has square ij now, on both sides; one only blocked has no new ground yet."""
        if k in self.to_block:
            self.blocked.add(k)
            return
        self.placed[k] = ij
        self.split.pop(k, None)
        self.stale.discard(k)
        self.blocked.discard(k)

    def arrived(self):
        """benilla's player stands at this session's place: the server has placed it (camera.bin flag) within
        LANDED_YD of the origin or of Tav's mapped point. None when it does not; else (x, y, where) as read."""
        try:
            x, y, _ = player_wow()
        except (AssertionError, OSError, struct.error):
            return None
        t = tav(self.started + 1.0)
        where = [self.origin] + ([t[0]] if t else [])  # she may have walked on since
        if min(math.hypot(x - p[0], y - p[1]) for p in where) > LANDED_YD:
            return False, (x, y, where)
        return True, (x, y, where)

    def settle(self):
        """After a re-origin world.json stays Active false until benilla's player has followed the hero (about 2 s):
        with it Active the mirrors would map the mobs round the old place through the new mapping. A player that never
        follows says so and leaves world.json off: mobs mapped wrong would only hide that."""
        if self.landed is None:
            return
        got = self.arrived()
        if got is None:
            return
        ok, (x, y, where) = got
        if not ok:
            if time.time() - self.landed > 30:
                self.landed = time.time()
                print('move: benilla\'s player is still at WoW %.1f %.1f, not at %s' % (x, y, where), flush=True)
            return
        self.landed = None
        self.publish(True)

    def publish(self, active):
        """link/world.json: the session mapping, for the fight bridge (wow-bg3-link bridge)."""
        body = {'Active': active, 'WowOrigin': list(self.origin or (0, 0, 0)), 'Centre': list(self.centre), 'Map': self.map,
                'Time': time.time()}
        atomic_write(LINK / 'world.json', json.dumps(body).encode())

    def storey_floors(self, bx, bz, reach=4):
        """The nearest walkable storey floor (BG3 y) in the squares already cut, within `reach` tiles of
        BG3 (x, z), or None: the one a character put at (x, z) stands on."""
        tiles = [(dc, dr) for dc in range(-reach, reach + 1) for dr in range(-reach, reach + 1)]
        for dc, dr in sorted(tiles, key=lambda t: t[0] * t[0] + t[1] * t[1]):
            x, z = bx + dc * 0.5, bz + dr * 0.5
            i, j = int(math.floor((x - self.centre[0]) / CELL)), int(math.floor((z - self.centre[2]) / CELL))
            found = []
            # A level lies a tile off its square, so a tile on a square's low edge is in the neighbour's level:
            # a Start's point is on that edge, and Undercity's flight point fell to the storey under its own.
            for ij in ((i, j), (i - 1, j), (i, j - 1), (i - 1, j - 1)):
                for st in (self.cells.get(ij) or {}).get('storeys') or ():
                    c0, r0 = int((x - st['min'][0]) // 0.5), int((z - st['min'][1]) // 0.5)
                    if 0 <= c0 < TILES and 0 <= r0 < TILES and st['floors'][(r0 * TILES + c0) * 5] & 1:
                        found.append(struct.unpack_from('<f', st['floors'], (r0 * TILES + c0) * 5 + 1)[0])
            if found:
                return min(found, key=lambda v: abs(v - self.centre[1]))
        return None

    def floor_at(self, bx, bz):
        """(walkable, floor y) of the placed square's tile under BG3 (x, z), or None."""
        ij = (int(math.floor((bx - self.centre[0]) / CELL)), int(math.floor((bz - self.centre[2]) / CELL)))
        if ij not in {s for k, s in self.placed.items() if k not in self.stale} or ij not in self.cells:
            return None
        cell = self.cells[ij]
        c = min(TILES - 1, max(0, int((bx - cell['min'][0]) / 0.5)))
        r = min(TILES - 1, max(0, int((bz - cell['min'][1]) / 0.5)))
        walk, y = struct.unpack_from('<Bf', cell['tiles'], (r * TILES + c) * 5)
        return bool(walk), y

    def fall_alarm(self, w, bx, by, bz):
        """Says when a member is somewhere nothing of WoW's can hold it, and moves nobody: BG3's own fall stands
        (a shove off a cliff, a drop down a shaft). Every walkable tile of every level has a WoW face within 1 m
        and neighbouring squares' floors meet (read from real cuts), so walking finds no gap; what is left is a
        forced move over a hole in the terrain with nothing below it, or a square that did not arrive.
        A tile holds the ground's one floor, which in a cave is the hill over it, so being under a tile says
        nothing: the cave's floor is in a storey and in the faces, and the faces decide."""
        now = time.time()
        f = self.floor_at(bx, bz)
        if f is None:
            # Off every placed square, in the level's own grid (a member at camp, say) or on no ground at all.
            if not self.in_level((bx, by, bz)) and not camp_state(self.run) and now - w.fall_said >= FALL_SAY_S:
                w.fall_said = now
                print(time.strftime('%H:%M:%S'), 'fall alarm: %s at %.1f %.1f %.1f, no square placed under it; placed %d'
                      % (w.name, bx, by, bz, len(self.placed)), flush=True)
            return
        if by >= f[1] - FALL_UNDER_M or now - w.faces_read < FALL_READ_S:
            return
        w.faces_read = now
        ij = (int(math.floor((bx - self.centre[0]) / CELL)), int(math.floor((bz - self.centre[2]) / CELL)))
        if face_under(self.cells[ij]['tris'], bx, by, bz) is not None or now - w.fall_said < FALL_SAY_S:
            return
        w.fall_said = now
        print(time.strftime('%H:%M:%S'), 'fall alarm: %s at %.1f %.1f %.1f, under every WoW face of square %s, tile floor %.1f'
              % (w.name, bx, by, bz, ij, f[1]), flush=True)

    def publish_party(self):
        """link/party-wow.json (Format 1): every member's WoW position (for its WoW character, a bot
        on the server: party-multiplayer-design.md), on change."""
        if not self.origin:
            return
        rows = []
        for w, p, dead in self.members:
            wx, wy = self.wow_xy(p[0], p[2])
            wz = self.origin[2] + (p[1] - self.centre[1]) / YD
            rows.append({'Uuid': w.uuid, 'Name': w.name, 'Hero': w is getattr(self, 'hero_walker', None),
                         'Wow': [round(wx, 2), round(wy, 2), round(wz, 2)], 'Bg3': [round(v, 2) for v in p], 'Dead': dead})
        key = json.dumps(rows, sort_keys=True)
        if key == getattr(self, '_party_key', None) and time.time() - getattr(self, '_party_at', 0) < 5:
            return
        self._party_key, self._party_at = key, time.time()
        atomic_write(LINK / 'party-wow.json', json.dumps({'Format': 1, 'Map': self.map, 'Time': time.time(),
                                                          'Members': rows}).encode())

    def move(self, m):
        """World mode at the requested WoW point. On this continent the session goes on there (reorigin). Another
        continent, or a refused re-origin: Stop (hero, party and squares home), then a new
        session with that point as the origin, on the request's continent (Map; another continent: a
        server teleport first, see CONTINENTS). A failed Start is tried again; if the new place never
        starts, world mode starts again where WoW's player stands. Returns the World to go on with (self if
        refused or re-origined)."""
        def answer(ok, value):
            atomic_write(MOVE_RESULT, json.dumps({'RunId': self.run, 'Seq': m['Seq'], 'OK': ok, 'Value': value}).encode())
        try:
            x, y, z = (list(m.get('Wow') or []) + [None, None, None])[:3]
            x, y = float(x), float(y)
            target = int(m.get('Map', self.map))
        except (TypeError, ValueError):
            answer(False, 'Wow must be [x, y, z or null], Map 0 or 1')
            return self
        if target not in CONTINENTS:
            answer(False, 'world mode has ground for Eastern Kingdoms and Kalimdor only')
            return self
        cross = target != self.map
        arrived = m.get('Arrived') is True
        if cross and not arrived and not (isinstance(m.get('Via'), str) and isinstance(m.get('Char'), str)):
            answer(False, 'another continent needs Via (a game_tele place there) and Char')
            return self
        if tav_flags(self.started + 1.0) & 1:
            answer(False, 'not during a BG3 fight')
            return self
        if camp_state(self.run):
            answer(False, 'not from camp')
            return self
        try:
            terrain = wow_arena.Terrain(Path(self.a.maps), target)
            h = terrain.height(x, y)
        except Exception as e:  # no height map there (off the continent, or an ocean tile)
            answer(False, 'no WoW ground there: %s' % e)
            return self
        if z is None:
            try:
                z = wow_stand.height(target, x, y, terrain)  # a city's floor, not the terrain under it
            except Exception as e:  # noqa: BLE001  the terrain still works outdoors
                print('no floor from the movement map, the terrain instead:', e, flush=True)
                z = h
        elif m.get('Snap'):  # a waypoint's flight point: the floor nearest its height
            try:
                import wow_waypoints
                z = wow_waypoints.stand_z(target, x, y, float(z), terrain)
            except Exception as e:  # noqa: BLE001  its own height still works
                print('no floor for the waypoint, its own height:', e, flush=True)
        z = float(z)
        print(time.strftime('%H:%M:%S'), 'move to', m.get('What') or '', 'WoW %.1f %.1f %.1f map %d' % (x, y, z, target), flush=True)
        black = move_cover(True)

        def covered():
            # Once the cover is black (the cuts of a re-origin take that time first): the old mapping is off.
            cover_wait(black)
            self.publish(False)
        if not cross:
            try:
                why = self.reorigin((x, y, z), covered)
            except Exception as e:  # noqa: BLE001  Stop and Start cut again from nothing
                why = e
            if why is None:
                answer(True, {'Origin': list(self.origin), 'Map': target})
                return self
            print('move: no re-origin (%s), Stop and Start' % why, flush=True)
        covered()
        seq = self.command('Stop')
        ok, err = self.wait_done(seq, 60)
        if not ok:
            print('Stop before the move failed:', err, flush=True)
        if cross and arrived:
            err = self.arrive(target)
            if err:
                print('arrival on another continent:', err, flush=True)
                answer(False, err)
                return self.again()
        elif cross:
            err = self.worldport(m['Char'], m['Via'], target)
            if err:
                print('server teleport failed:', err, flush=True)
                answer(False, 'server teleport to %s failed: %s' % (m['Via'], err))
                return self.again()
        last = None
        for attempt in range(3):
            w = World(self.a, map_id=target)
            try:
                w.start((x, y, z))
                answer(True, {'Origin': list(w.origin), 'Map': target})
                return w
            except Exception as e:
                last = e
                print('move: Start at the new place failed (try %d): %s' % (attempt + 1, e), flush=True)
                time.sleep(3)
        answer(False, 'Start at the new place failed: %s' % last)
        return self.again()

    def again(self):
        """World mode again where WoW's player stands (after a failed move), tried a few times."""
        last = None
        for attempt in range(4):
            try:
                w = World(self.a)
                w.start()
                return w
            except Exception as e:
                last = e
                print('world mode again where WoW\'s player stands failed (try %d): %s' % (attempt + 1, e), flush=True)
                time.sleep(3)
        raise RuntimeError('world mode could not start again: %s' % last)

    def arrive(self, target):
        """WoW already moved the character to another continent (Arrived): wait for benilla to stand
        there. Returns None or an error."""
        due = time.time() + 90
        while time.time() < due:
            if player_map() == target:
                time.sleep(3)  # WoW's loading screen and the first placed packets
                return None
            time.sleep(0.5)
        return 'benilla did not arrive on %s within 90 s' % CONTINENTS[target]

    def worldport(self, char, via, target):
        """Server teleport of the WoW character to the game_tele place `via` (another continent),
        then wait for benilla to stand on that continent. Returns None or an error."""
        r = subprocess.run([sys.executable, str(ROOT / 'server/tools/world-console.py'), 'tele', char, via],
                           capture_output=True, text=True, timeout=60)
        print('server teleport', char, via, '->', (r.stdout or '').strip()[-200:], (r.stderr or '').strip()[-200:], flush=True)
        if r.returncode != 0:
            return 'world-console.py: %s' % ((r.stderr or r.stdout or '').strip()[-200:])
        return self.arrive(target)

    def follow(self):
        self.settle()
        # Tav's record counts once WorldMode has teleported her (it publishes only after Start).
        t = tav(self.started + 1.0)
        if not t:
            return
        _, hero_pos = t
        members = self.read_members(hero_pos)
        for w, p, dead in members:
            if not dead:
                self.fall_alarm(w, p[0], p[1], p[2])
        self.publish_party()
        want = self.wanted(members)
        # At most 8 squares a round, in `want` order (the square under every member first): each new
        # square is a wow_arena.py cut (about a second), so a member far off waits no longer than that.
        pairs = self.moves(want)[:8]
        # The WoW triangles go with the wanted squares and the placed ones near a member (KEEP_RAYS)
        # only (harness: with a pool of 84 the squares members left behind stay placed until they are
        # reused; their trails joined into one cluster over 1 km long and the plugin refused the rays).
        # A square farther off keeps its AI tiles but loses its triangles; one a member comes back to
        # gets them again, with no square to place too (a Stream with no cells).
        moving = {k for k, _ in pairs}
        where = [p for _, p, _ in members]
        def near(ij):
            minx, minz = self.cell_min(ij)
            return any(math.hypot(minx + CELL / 2 - p[0], minz + CELL / 2 - p[2]) <= KEEP_RAYS for p in where)
        # A stale square's WoW triangles would be another place's than its tiles: it gets them as it is retiled.
        squares = [ij for k, ij in self.placed.items() if k not in moving and k not in self.stale and (ij in want or near(ij))]
        squares += [ij for _, ij in pairs]
        # Squares one side still holds (a split pool subgrid) keep their WoW triangles too.
        squares += [ij for k, spots in self.split.items() if k not in moving for ij in spots
                    if ij not in squares and ij in want]
        if not pairs and sorted(squares) == self.rays:
            return
        cells = self.cells_for(pairs)
        self.write_triangles(squares)
        seq = self.command('Stream', Cells=cells, Rays=True)
        ok, err = self.wait_done(seq, 45)
        if ok:
            self.rays = sorted(squares)
            for k, ij in pairs:
                self.took(k, ij)
        else:
            print('Stream refused:', err, flush=True)
            # A square with something on it (a fight's stand-in, a corpse, a surface) stays put for a
            # while; the other squares fill in around it.
            # WorldMode names the busy pool subgrids ("stands on pool 3 4"): nothing moved on either
            # side, only those wait and the next round fills their squares from other subgrids.
            m = re.search(r'stands on pool ([0-9 ]+)', str(err))
            busy = {int(v) for v in m.group(1).split()} if m else None
            if busy is not None or 'stands on' in str(err) or 'surface' in str(err):
                for k, _ in pairs:
                    if busy is None or k in busy:
                        self.pinned[k] = time.time() + 15.0
            self.resync()


class FarSight:
    """Switching control (party-multiplayer-design.md "Far Sight"): when the player controls another member, WoW's view
    goes there, Far Sight onto that member's WoW character (its bot). The hero's own WoW character stays on the hero
    (tav.bin keeps the hero), and every treadmill, mirror and fight keeps working for all members. Back on the hero: off.
    Its own thread, every EVERY_S: a round of the ground waits for its cuts, and a square at a place never visited
    builds its movement tiles first, up to a minute each (0.1.3013.60: Astarion 699 m off, one round built two tiles
    for 68 s, the F2 to him came in that round and Far Sight never went out, so benilla never loaded his world and the
    switch's cover ran its 20 s). A move holds it as it is (hold): the party is gathered and the mapping changes then."""
    EVERY_S = 0.25

    def __init__(self, world):
        self.world = world      # the World now (main sets it after a move)
        self.seq = 0
        self.key = None         # the uuid Far Sight is on, None on the hero
        self.held = threading.Lock()
        self.halt = threading.Event()
        self.thread = None

    @staticmethod
    def controlled():
        """The member the player controls now (Mirrors.lua: mirror-status.json Tav.Controlled), or None."""
        try:
            st = json.loads((LINK / 'mirror-status.json').read_text())
            c = (st.get('Tav') or {}).get('Controlled')
            return c if isinstance(c, str) and c else None
        except (FileNotFoundError, json.JSONDecodeError, AttributeError):
            return None

    def target(self, w):
        """The controlled member as party.json (Party.lua) has it, when it is not the hero and stands in world mode's
        void (as World.read_members takes members): {Uuid, Name, Pos}, or None."""
        c = self.controlled()
        h = hero(w.run)
        if not c or c == (h or {}).get('Uuid'):
            return None
        try:
            party = json.loads((IO / 'party.json').read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return None
        if not isinstance(party, dict) or party.get('RunId') != w.run:
            return None
        for m in party.get('Members') or []:
            if not isinstance(m, dict) or m.get('Uuid') != c or m.get('Hero') or m.get('OnStage') is False:
                continue
            p = m.get('Pos')
            if not isinstance(p, list) or len(p) != 3 or not all(isinstance(v, (int, float)) for v in p):
                return None
            if abs(p[0] - w.centre[0]) > 20000 or abs(p[2] - w.centre[2]) > 20000 or w.in_level(p):
                return None
            return {'Uuid': c, 'Name': m.get('Name') or c[:8], 'Pos': tuple(p)}
        return None

    def write(self, w, t):
        """The one place Far Sight is asked for. link/farsight.json (Format 1): {Seq, On, Uuid, Name,
        Wow, Map}; the WoW backend (a server verb; benilla) turns the
        viewpoint of the player's WoW character onto that member's WoW character, or back (On false)."""
        self.seq += 1
        body = {'Format': 1, 'Seq': self.seq, 'On': t is not None, 'Map': w.map, 'Time': time.time()}
        if t is not None:
            body.update(Uuid=t['Uuid'], Name=t['Name'])
            p = t['Pos']
            if w.origin:
                wx, wy = w.wow_xy(p[0], p[2])
                body['Wow'] = [round(wx, 2), round(wy, 2), round(w.origin[2] + (p[1] - w.centre[1]) / YD, 2)]
        atomic_write(LINK / 'farsight.json', json.dumps(body).encode())
        print(time.strftime('%H:%M:%S'), 'far sight', ('on ' + t['Name']) if t else 'off (the hero again)', flush=True)

    def step(self):
        """Far Sight where the player's control is, unless a move holds it."""
        if not self.held.acquire(blocking=False):
            return
        try:
            w = self.world
            t = self.target(w)
            key = t['Uuid'] if t else None
            if key != self.key:
                self.key = key
                self.write(w, t)
        finally:
            self.held.release()

    def run(self):
        while not self.halt.wait(self.EVERY_S):
            try:
                self.step()
            except Exception as e:  # noqa: BLE001  keep watching; the next look may read whole files
                print('far sight error:', e, flush=True)

    def start(self):
        self.thread = threading.Thread(target=self.run, name='farsight', daemon=True)
        self.thread.start()

    def stop(self):
        """The thread ends; Far Sight back on the hero if it was out."""
        self.halt.set()
        if self.thread:
            self.thread.join(5)
        if self.key is not None:
            self.key = None
            self.write(self.world, None)


def main():
    p = argparse.ArgumentParser()
    # Tav's place in BG3 (in the void, well away from the tutorial scene, the whole
    # session): 1.1 km west of the level's AI grid (x -296..331, z -478..70), where no BG3 tile
    # or shape shares a column with the treadmill's squares (over the out-of-bounds layer, its
    # chasm tiles under the squares killed Tav on her first walk). Height: BG3's cursor
    # picked characters only while Tav stood below about 150 m over that layer (works at 151.7, not
    # at 161.7; physics still hits her higher up and the level's bounds are +-10 km).
    # WOW_BG3_WORLD_X, _Y and _Z override it.
    env = lambda k, d: float(os.environ.get(k, d))  # noqa: E731
    p.add_argument('--centre', type=float, nargs=3,
                   default=(env('WOW_BG3_WORLD_X', '-1400'), env('WOW_BG3_WORLD_Y', '120'), env('WOW_BG3_WORLD_Z', '-350')))
    p.add_argument('--pool', type=int, default=int(env('WOW_BG3_POOL', '6')), help='pool subgrids to use (4 to 128)')
    # A bigger host level: the out-of-bounds layer's height, the hero's floor side in
    # squares (4, since the friendly NPCs' stand-ins reach 40 yd: a 4 x 4 block, at least 88 m of ground
    # on every side; 2: the old 2 x 2 plus two ahead) and the same under every other party member.
    # One height or several, comma separated (plugin 0.2.9: the pool from every listed layer).
    p.add_argument('--layer', default=os.environ.get('WOW_BG3_LAYER', '-15.262'))
    p.add_argument('--floor', type=int, default=int(env('WOW_BG3_FLOOR', '4')))
    p.add_argument('--party-floor', action='store_true', default=os.environ.get('WOW_BG3_PARTY_FLOOR') == '1')
    p.add_argument('--maps', default=os.environ.get('WOW_BG3_MAPS') or (os.environ.get('WOWBG3_TEST_STACK') or '/opt/stacks/wow-bg3') + '/storage/mangosd/extracted-data/maps')
    p.add_argument('--wait-hero', type=float, default=120.0,
                   help='seconds to wait for the hero to be ready for world mode (wow-bg3/hero.json)')
    a = p.parse_args()
    a.pool = max(4, min(128, a.pool))
    a.layers = [float(v) for v in str(a.layer).replace(' ', '').split(',') if v]
    assert a.layers, '--layer needs at least one height'
    # --party-floor is what every run does now: a treadmill per party member.
    a.floor = max(2, min(6, a.floor))
    wait_hero(a.wait_hero)
    # The first trip to a flight point waits for nothing: its movement tiles and its squares are built ahead, at the lowest priority.
    if os.environ.get('WOW_BG3_SERVER') and os.environ.get('WOWBG3_SERVER_VAR'):
        try:
            subprocess.Popen(['nice', '-n', '19', sys.executable, str(Path(__file__).resolve().parent / 'wow_waypoints.py'), 'prewarm',
                              str(Path(os.environ['WOWBG3_SERVER_VAR']) / 'data/5875/dbc')] + [str(v) for v in a.centre] + [str(a.floor)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            print('movement tiles ahead:', e, flush=True)
    w = World(a)
    # A bridge that was killed left Active true: Camp.lua would take the coming Start's jump to the void for BG3's own travel.
    w.publish(False)
    stop = {'now': False}
    signal.signal(signal.SIGTERM, lambda *_: stop.update(now=True))
    signal.signal(signal.SIGINT, lambda *_: stop.update(now=True))
    w.start()
    sight = FarSight(w)
    sight.start()
    moved = moves_done(w.run)
    covered = None  # when the last move ended, while its cover waits for benilla's player there
    at_camp = False
    # A bridge that died at camp left benilla without its world.
    try:
        (LINK / 'wowcamp.flag').unlink(missing_ok=True)
    except OSError as e:
        print('camp: flag:', e, flush=True)
    while not stop['now']:
        time.sleep(0.5)
        try:
            now_camp = camp_state(w.run)
            if now_camp != at_camp:
                at_camp = now_camp
                print(time.strftime('%H:%M:%S'), 'at camp: WoW waits' if at_camp else 'back from camp', flush=True)
                camp_compose(at_camp)
            m = move_request(w.run, moved)
            if m:
                moved = m['Seq']
                try:
                    with sight.held:
                        w = w.move(m)
                        sight.world = w
                except BaseException:
                    move_cover(False)
                    raise
                covered = time.time()
                continue
            # The cover stays until the party can play at the new place: Tav and the party are there (the move has
            # answered) and benilla's player stands there too (the server placed it; WoW's own loading screen covers
            # its streaming from then on). Lifted without it after COVER_WAIT_S: a player that never follows is
            # settle's to report, not the cover's to hide.
            if covered is not None and cover_done(w, covered):
                covered = None
            w.follow()
        except Exception as e:  # keep following; the mod keeps the squares it has
            print('follow error:', e, flush=True)
    if covered is not None:
        move_cover(False)
    if at_camp:
        camp_compose(False)
    w.publish(False)
    sight.stop()
    w.command('Stop')


if __name__ == '__main__':
    main()
