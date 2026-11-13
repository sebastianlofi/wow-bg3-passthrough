#!/usr/bin/env python3
"""Tav's textures for WoW: bakes one albedo texture per material of Tav's meshes from BG3's
game data, so benilla can draw her textured with WoW's light (wowbg3_tav.rs).

BG3 builds every character colour in its material shaders (skin from HMVY and CLEA masks and the
character creation skin preset, hair from its masks and the hair colour, armour from a colour mask
that picks one of twelve colours of the item's colour preset or dye, and so on). The albedo is
BG3's own: the material's compiled G-buffer shader (Materials.pak) is run over the texture with
bg3shade.py, and its base colour output is the baked albedo.

The shader's inputs are BG3's: its textures and virtual texture layers, its constant buffer, and
the mean vertex colour of the material's meshes (from the layer's mesh copy, TAV_DIR/tavmesh.bin).
TavPlan.lua publishes the constant buffer each of Tav's render objects is drawn with (K lines: the
item colour presets, dyes and character creation colours already applied). Without K lines (an
older TavPlan) the buffer is built from the shader's parameter offsets, the material's values and
the character creation presets ("C" line), which leaves out item colour presets.

The party: the plan holds the hero and her companions, each a figure (M lines end with
the figure, C lines carry each figure's character creation presets); each mesh bakes with its own
figure's presets, and the mean vertex colour is taken per material and figure.

When the shader cannot run, the older approximations below serve (bake_skin, bake_hair, bake_vt,
bake_fur, bake_eyes):

  skin   (CHAR_Skin*)  absorption by melanin, haemoglobin, yellowing and veins (HMVY masks and the
                       skin preset's amounts and colours), brows (CLEA green), occlusion (CLEA alpha)
  hair   (CHAR_Hair, CHAR_Scalp)  hair colour by depth and root (MSKA), alpha from MSKA alpha
  armour (*_VT with MSKColor/MSKcloth)  virtual texture base, tinted by the mask channels
  other  *_VT: the virtual texture's albedo layer; fur: its base map times its colour

Alpha (alpha-tested materials): the virtual texture's albedo alpha, hair and scalp MSKA alpha, fur
IDMsk alpha.

Besides the albedo, each material gets BG3's surface detail for WoW's light (the goal
being "ok 6, as long as it looks close to the original"):

  normal  the tangent-space normal map (virtual texture layer 1, or the DDS normal map), stored
          as RGB = normal * 0.5 + 0.5 in BG3's convention (BG3 keeps X in alpha and Y in green;
          Z is rebuilt here)
  surface R occlusion, G roughness, B metalness (the glTF layout): armour and props from the
          virtual texture's physical layer (R metalness, G roughness, B occlusion in BG3), skin,
          hair and eyes from their materials' roughness scalars

G-buffer maps (benilla 0037): the normal and surface maps are BG3's own G-buffer outputs of the
material's shader, not raw texture layers: the shader runs once more with every output planned (bg3shade
outputs=[0, 1, 2, 3]), its vertex frame given as a known basis (bg3shade.tbn_roles: X along +U, Y along -V, N) and
the view matrix the identity, so output 0 is BG3's shading normal in the mesh's tangent frame (normal map, wrinkle
and layer normals as BG3 combines them). Files per full bake (its key h): h-n.tvt the normal in benilla's
(+U, +V, N) frame (A: the detail normal's strength x 2), h-s.tvt R occlusion (base colour alpha), G roughness, B
metalness (a hair's backlit), A specular level, h-x.tvt the extra output (a skin's diffusion profile data, a hair's
strand direction in the same frame and its scatter), h-g.json what benilla needs besides (model: BG3's shading
model 0/3/5, the detail map and tiling, the diffusion profile). A skin's tiled detail normal map (its pores; 15 to 40
times the body's UV frequency, BG3 samples it at uv x tiling) is not baked into h-n: it is flattened for that run,
its strength comes from a second run with a known detail texel, and the map itself goes to d-<hash>.tvt (RGB =
X, +V, Z) for benilla to tile. tavtex.txt carries them as tokens after the five columns: model:<m> extra:<file>
detail:<file>,<tiling> profile:<lobe A>,<lobe B>,<mix>,<distance>,<scatter rgb>,<thickness min>,<max>,<tint rgb>
(the material's DiffusionProfileUUID, or its shader's, in the DiffusionProfileBank resources of Engine.pak and
Shared.pak; cached in profiles.json beside the index). Materials whose shader cannot run keep the old raw maps and
get no model token (benilla then keeps WoW's law for them). They arrive progressively: a G-buffer job runs after
the full albedo bake landed (tavtex.txt lists that albedo at once, with the raw maps while its G-buffer maps are
made), behind every waiting full bake; a mesh keeps the maps of the bake it showed until the new ones land. The
G-buffer run is converted to RGBA8 chunk by chunk (memory), and a queued job whose bake the current plan no
longer needs is dropped when its turn comes (a plan change no longer waits behind the old plan's bakes).

Mip chain: BG3 shades every pixel from mip levels of its inputs, so at a distance its colours come
from blurred masks (hair and armour come out darker than the average of the full-size result).
Each albedo level is therefore shaded from inputs at that level's density (box-filtered, linear)
and stored after level 0 (TVT flag 8); benilla 0037 uploads those levels instead of averaging
level 0 (they differ from level 0's averages by at most 5%, hair 0.96, armour 0.95 to
0.99, skin 0.98 to 1.00, at no measurable bake time; WOWBG3_TAVTEX_MIPS=0 bakes level 0 only).

Vertex cuts (benilla 0036): BG3 hides the parts of a figure its equipment covers (body
and underwear under the clothes, legs and trouser legs in boots) in the vertex shader of every material with
a VertexColor_MSK texture: a vertex whose colour G byte k has texel k set in the character's 16x16 mask moves
to its object's origin. BG3 sets texel k for the NakedBodySystem byte (Mods/Shared/EquipmentSettings/
VisualSlots.lsx) of every VertexColorMaskSlots entry (VisualBank) of the figure's equipped visuals, one mask
per character, rewritten when the equipment changes (checked against BG3's own masks in
a test script). TavPlan.lua lists the equipped visuals (V lines); tavtex.txt gets the figure's bytes
as the token "cut:<b>,<b>..." after the positional columns on the meshes whose shader cuts, and benilla 0036
leaves out the triangles whose three vertices are cut.

Calibration (link/tools/tavcalib.py, <cache>/../calib/<material>.npz) multiplies the albedo per UV
cell toward BG3's G-buffer colour. It was a stand-in for the colour inputs above and is off unless
WOWBG3_TAVCALIB=1 (a calibration measured against an older bake no longer fits).

Run beside world mode: it watches the plan (TavPlan.lua) and writes, into the Tav directory,
<hash>.tvt files in the cache directory ("TVT1", width, height, flags u32; RGBA8 rows; flags
1 alpha test, 2 hidden, 4 linear data (normal and surface maps; albedo is sRGB), 8 mip chain
(levels 1.. down to 1x1 follow level 0, each max(1, size >> k)); up to 2048 px; benilla builds
the mips of files without flag 8) and, in the Tav directory, tavtex.txt ("TAVTEX 1 <plan seq>",
then "<mesh key> <albedo file> <flags> <normal file or -> <surface file> [cut:<bytes>] [model:... extra:...
detail:... profile:...]").

Speed (on a desktop: a companion added with party.sh stayed hidden for 12 s,
then showed untextured, and got its colours 20 to 45 s after BG3 planned it; a cold bake of a three
figure plan took 92 s on one thread): every material new to the cache first gets a PREVIEW_SIZE
(256 px) preview from its inputs' coarse mips (BG3 streams its own textures coarse first), then the
full bakes run FULL_JOBS at a time (default 2, memory) with their shader chunks on CHUNK_THREADS
threads (default 4) while WORKERS threads (default 2) make the previews; tavtex.txt is
written as soon as anything changes, so a new figure shows within about a second and sharpens
material by material. A mesh that had a full bake keeps it until its new one lands (a blood or dirt
change of the head's constant buffer does not drop to a preview). Bakes are cached across sessions
by their inputs (material, presets, constant buffer, and the mean vertex colour only for shaders
that read it). Materials BG3 never draws (Helper_Invisible, e.g. SHARED_InvisibleMat in Astarion's
scalp slot) are hidden. Log lines carry the time since the plan was read.

LODs (the LOD format research): TavPlan's M lines may end with the object's
LOD; every key gets a line, and the mean vertex colour comes from the LOD 0 meshes.

Virtual texture levels: a tile set holds each layer only down to the level its source
reached (Gale's robe bracers: albedo and physical layers stop at 512 px of 1024; Astarion's pants'
physical layer, his shirt lace's normal and physical layers). Those layers are read from their finest
level present, as BG3 samples them; read at level 0 they were zeros (the bracers baked black, zero
occlusion and roughness on the pants). Such materials carry ' vt<layer>+<levels>' in their cache keys
(Ctx.vt_note); every other key is unchanged.

Sides: tavtex.txt's flags column (not the .tvt header) adds 16 on meshes whose BG3
material is drawn one-sided (the shader .lsf's Material RasterizerStateID 1; the _2S shaders, hair,
fur and pubes are 0, two-sided); benilla 0035 culls their back faces.
CHAR_Pubes is alpha-tested from its ID_Depth_Root_Alpha_MSKA like hair (baked opaque before, a dark shape).

  tavtex.py [--once] PLAN GAME_DATA INDEX.json TAV_DIR CACHE_DIR
  (--once: serve the plan until every mesh has its full bake, then exit)"""
import glob, hashlib, heapq, itertools, json, os, re, struct, sys, threading, time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bg3pak  # noqa: E402
import bg3shade  # noqa: E402
import bg3tex  # noqa: E402

MAX_SIZE = 2048
# See "Speed" above.
PREVIEW_SIZE = 256
WORKERS = max(1, int(os.environ.get('WOWBG3_TAVTEX_WORKERS') or 2))  # preview threads
# Memory (the unit's MemoryMax=4G; one 2048 px bake holds about 0.7 GB of inputs and values): at
# most FULL_JOBS full bakes at once, their shader chunks on CHUNK_THREADS threads.
FULL_JOBS = max(1, int(os.environ.get('WOWBG3_TAVTEX_FULL') or 2))
CHUNK_THREADS = max(1, int(os.environ.get('WOWBG3_TAVTEX_CHUNKS') or 4))
TILESETS_KEPT = 6  # tile sets kept loaded (their .gts, about 80 MB each; page files are dropped after use)
CHUNK = 1 << 17  # texels a shader run evaluates at once (bg3shade's default 1 << 18, halved for the threads)
K_GRACE = 3.0  # s a mesh without K lines waits for them (TavPlan.lua) before it bakes with built constants
MESH_GRACE = 5.0  # s a mesh whose shader reads the vertex colour waits for the layer's mesh copy
# Materials BG3 never draws: their meshes are hidden (SHARED_InvisibleMat, baked, was an opaque black
# shell over Astarion's face).
HIDDEN_SHADERS = ('Helper_Invisible',)
BAKE_VERSION = 'v4'  # in the cache key: v3 albedo from BG3's shaders, v4 with its mip chain
# tavtex.txt's flags column (never a .tvt header) carries 16 when BG3 draws the mesh's material
# one-sided (its shader's Material RasterizerStateID 1, back faces culled); benilla 0035 culls those back faces.
# Two-sided (0): the _2S shaders, hair, fur, pubes. Drawn two-sided, a robe's inner faces showed through its outside.
ONE_SIDED = 16
TVT_MIPS = 8
MIPS = os.environ.get('WOWBG3_TAVTEX_MIPS', '1') != '0'  # shade a mip chain (WOWBG3_TAVTEX_MIPS=0: level 0 only)
# Engine_Default_Mesh_Lit_Opaque (Engine.pak, not in index.json): the material of BG3's cloth
# proxies (e.g. HUM_F_ARM_Barbarian_A_Skirt_Cloth_Mesh), which drive the cloth simulation and are
# never drawn. Drawn, they showed as a flat grey shell around her skirt.
ENGINE_DEFAULT = {'9e2966c7-b61c-4bc1-bef1-a79cb5fde067'}
# Textures that set the bake's size when the material has no virtual texture (atlases and tiled
# detail maps do not).
SIZE_TEXTURES = ('CLEA', 'HMVY', 'ID_Depth_Root_Alpha_MSKA', 'ColorsMask', 'Body_color_texture', 'IDMsk',
                 'MSKColor', 'MSKcloth', 'normalmap')


def srgb_to_lin(c):
    c = np.asarray(c, np.float32)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


_SRGB_LUT = None


def srgb_u8_to_lin(u8):
    """srgb_to_lin of 8-bit values by table (the same float32 values, without 4M powers a texture)."""
    global _SRGB_LUT
    if _SRGB_LUT is None:
        _SRGB_LUT = srgb_to_lin(np.arange(256, dtype=np.float32) / 255.0).astype(np.float32)
    return _SRGB_LUT[u8]


def lin_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def ui_color(hexs):
    h = hexs.lstrip('#')
    if len(h) == 8:
        h = h[2:]
    return srgb_to_lin([int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)])


class Ctx:
    def __init__(self, data, index_path):
        self.data = data
        self.ix = json.load(open(index_path))
        self.cc = {}
        for kind in ('SkinColors', 'HairColors', 'EyeColors'):
            self.cc.update(self._cc(kind))
        self.tilesets = {}
        self.shaders = {}
        self._engine = None
        self.lock = threading.RLock()  # the caches below are shared by the bake threads
        self._colour_use = {}
        self._vt_notes = {}  # Material -> Ctx.vt_note
        self._raster = {}  # Shader source -> its RasterizerStateID (Ctx.one_sided)
        self._gshaders = {}  # Shader source -> Ctx.gshader
        self._profiles = None  # DiffusionProfile id -> values (Ctx.diffusion_profile)
        self.index_path = index_path
        self.index_dir = os.path.dirname(os.path.abspath(index_path))
        self._visual_slots = None
        self._mask_bytes = None

    def _cc(self, kind):
        import xml.etree.ElementTree as ET
        name = f'Public/Shared/CharacterCreationPresets/CharacterCreation{kind}.lsx'
        p = bg3pak.find_in_paks(self.data, name, ['Shared.pak'])
        if not p:
            return {}
        root = ET.fromstring(p.read(name))
        out = {}
        for node in root.iter('node'):
            a = {x.get('id'): x.get('value') for x in node.findall('attribute')}
            if 'UUID' in a and 'MaterialPresetUUID' in a:
                out[a['UUID']] = a
        return out

    def preset_params(self, cc_uuid):
        """(scalars, vectors, ui colour) of a character creation colour."""
        c = self.cc.get(cc_uuid)
        if not c:
            return {}, {}, None
        pr = self.ix['presets'].get(c['MaterialPresetUUID']) or {}
        sc, vec = {}, {}
        for block in pr.get('Presets', []):
            for x in block.get('ScalarParameters', []):
                sc[x.get('Parameter')] = x.get('Value')
            for x in block.get('Vector3Parameters', []):
                vec[x.get('Parameter')] = x.get('Value')
        return sc, vec, c.get('UIColor')

    def visual_slots(self, vid):
        """A VisualBank resource's VertexColorMaskSlots (names). Read once from the packages'
        Content banks and kept beside index.json (visualslots.json)."""
        if self._visual_slots is None:
            path = os.path.join(self.index_dir, 'visualslots.json')
            try:
                self._visual_slots = json.load(open(path))
            except (OSError, ValueError):
                self._visual_slots = scan_visual_slots(self.data)
                try:
                    with open(path + '.tmp', 'w') as f:
                        json.dump(self._visual_slots, f)
                    os.replace(path + '.tmp', path)
                except OSError:
                    pass
        return self._visual_slots.get(vid, [])

    def mask_bytes(self):
        """VertexColorMaskSlots name -> the vertex colour G byte it marks (NakedBodySystem in
        Mods/Shared/EquipmentSettings/VisualSlots.lsx)."""
        if self._mask_bytes is None:
            import xml.etree.ElementTree as ET
            self._mask_bytes = {}
            name = 'Mods/Shared/EquipmentSettings/VisualSlots.lsx'
            p = bg3pak.find_in_paks(self.data, name, ['Shared.pak'])
            if p:
                root = ET.fromstring(p.read(name))
                for node in root.iter('node'):
                    if node.get('id') == 'NakedBodySystem':
                        for o in node.iter('node'):
                            a = {x.get('id'): x.get('value') for x in o.findall('attribute')}
                            if 'MapKey' in a and 'MapValue' in a:
                                self._mask_bytes[a['MapKey']] = int(a['MapValue'])
        return self._mask_bytes

    def texture(self, tid):
        """A TextureBank resource: index.json, or the engine's own (Engine.pak; defaults such as
        Engine_Black, which index.json does not cover)."""
        t = self.ix['textures'].get(tid)
        if t is None:
            with self.lock:
                if self._engine is None:
                    engine = {}
                    p = bg3pak.find_in_paks(self.data, 'Public/Engine/Content/[PAK]_Engine/_merged.lsf', ['Engine.pak'])
                    for n in (p.files if p else []):
                        if '/Content/' in n and n.endswith('_merged.lsf'):
                            for root in bg3pak.read_lsf(p.read(n)):
                                for node in root.walk():
                                    if node.name == 'TextureBank':
                                        for r in node.find('Resource'):
                                            engine[r.attrs.get('ID')] = dict(r.attrs)
                    self._engine = engine
            t = self._engine.get(tid)
        return t

    def dds(self, name, raw=False, size=MAX_SIZE):
        p = bg3pak.find_in_paks(self.data, name) or bg3pak.find_in_paks(self.data, name, ['Engine.pak'])
        if not p:
            return None
        b = p.read(name)
        img = bg3tex.dds_rgba(b, mip=self._mip_for(b, size))
        return (img, bg3pak.dds_info(b)[3]) if raw else img

    def _mip_for(self, b, size=MAX_SIZE):
        w, h, mips, fmt, head = bg3pak.dds_info(b)
        m = 0
        while max(w, h) > size and m < mips - 1:
            w, h, m = w // 2, h // 2, m + 1
        return m

    def tex_linear(self, tid, size=MAX_SIZE):
        """A texture as the GPU samples it: float32 RGBA, sRGB decoded when the resource says so,
        single-channel formats as (r, 0, 0, 1); its mip of at most `size` px."""
        t = self.texture(tid)
        if not t or not t.get('SourceFile'):
            return None
        r = self.dds(t['SourceFile'], raw=True, size=size)
        if r is None:
            return None
        img, fmt = r
        f = img.astype(np.float32) / 255.0
        if t.get('SRGB') or fmt.endswith('s') and fmt.startswith('BC'):
            f[..., :3] = srgb_u8_to_lin(img[..., :3])
        if fmt in ('ATI1', 'BC4U', 'BC4', 'BC4s'):
            f[..., 1:3] = 0.0
            f[..., 3] = 1.0
        return f

    def vt(self, gtex, layer, size=MAX_SIZE):
        gts = bg3tex.find_tileset(self.data, gtex)
        if not gts:
            return None
        with self.lock:
            # The last TILESETS_KEPT tile sets stay loaded.
            ts = self.tilesets.pop(gts, None) or bg3tex.TileSet(self.data, gts)
            self.tilesets[gts] = ts
            while len(self.tilesets) > TILESETS_KEPT:
                self.tilesets.pop(next(iter(self.tilesets)))
        if gtex not in ts.textures:
            return None
        w = max(ts.textures[gtex][2], ts.textures[gtex][3])
        level = 0
        while w > size and level < len(ts.levels) - 1:
            w, level = w // 2, level + 1
        # A layer the tile set leaves out at that level (bg3tex TileSet.missing) is read
        # from its finest level that is there, as BG3 samples it; it was read as zeros (Gale's robe
        # bracers baked black, zero roughness and occlusion on Astarion's pants, a zero normal map).
        while level < len(ts.levels) - 1 and ts.missing(gtex, layer, level):
            level += 1
        img = ts.texture(gtex, layer, level)
        # The page files it read (about 200 MB a tile set) are read again when next needed: kept for
        # every tile set they held 1.7 GB for a three figure plan, and the bake threads OOMed at 4 GB.
        ts._gtp.clear()
        return img

    def vt_note(self, mat_id):
        """'' or, for a material whose virtual texture layers stop above the level a full bake reads
        (Ctx.vt reads them from a coarser level), ' vt<layer>+<levels>...': put in
        its cache keys so the bakes made from the zero tiles are not reused; other keys are unchanged."""
        with self.lock:
            if mat_id in self._vt_notes:
                return self._vt_notes[mat_id]
        note = ''
        try:
            mat = self.ix['materials'].get(mat_id) or {}
            gtex = None
            for p in mat.get('VirtualTextureParameters', []):
                t = self.ix['vtextures'].get(p.get('ID'))
                if t and t.get('GTexFileName'):
                    gtex = t['GTexFileName']
                    break
            gts = bg3tex.find_tileset(self.data, gtex) if gtex else None
            if gts:
                with self.lock:
                    ts = self.tilesets.pop(gts, None) or bg3tex.TileSet(self.data, gts)
                    self.tilesets[gts] = ts
                    while len(self.tilesets) > TILESETS_KEPT:
                        self.tilesets.pop(next(iter(self.tilesets)))
                if gtex in ts.textures:
                    w = max(ts.textures[gtex][2], ts.textures[gtex][3])
                    level = 0
                    while w > MAX_SIZE and level < len(ts.levels) - 1:
                        w, level = w // 2, level + 1
                    for layer in range(ts.num_layers):
                        lv = level
                        while lv < len(ts.levels) - 1 and ts.missing(gtex, layer, lv):
                            lv += 1
                        if lv > level:
                            note += f' vt{layer}+{lv - level}'
        except Exception:  # noqa: BLE001
            note = ''
        with self.lock:
            self._vt_notes[mat_id] = note
        return note

    def one_sided(self, mat_id):
        """Whether BG3 culls the back faces of a material (its shader .lsf's Material RasterizerStateID is 1;
        0 for two-sided shaders), cached per shader; False when unknown (drawn two-sided as before)."""
        src = (self.ix['materials'].get(mat_id) or {}).get('SourceFile', '')
        with self.lock:
            if src in self._raster:
                return self._raster[src] == 1
        state = None
        try:
            if src.endswith('.lsf'):
                p = bg3pak.find_in_paks(self.data, src, ['Materials.pak'])
                if p and src in p.files:
                    for root in bg3pak.read_lsf(p.read(src)):
                        for node in root.walk():
                            if node.name == 'Material' and node.attrs.get('RasterizerStateID') is not None:
                                state = int(node.attrs.get('RasterizerStateID'))
        except Exception:  # noqa: BLE001
            state = None
        with self.lock:
            self._raster[src] = state
        return state == 1

    def shader(self, source):
        """(bg3shade.Shader, parameter layout) of a material shader (its .lsf in Materials.pak), or
        None."""
        with self.lock:
            return self._shader(source)

    def gshader(self, source):
        """(Shader planning outputs 0..3, Shader for output 0 alone, layout, {location: 'N'|'X'|'Y'} of
        its vertex frame, the shader .lsf's DiffusionProfileUUID) of a material shader, or None."""
        with self.lock:
            if source in self._gshaders:
                return self._gshaders[source]
        r = None
        try:
            if source.endswith('.lsf'):
                bshd = source[:-4] + '_ST_DEF_Vulkan.bshd'
                p = bg3pak.find_in_paks(self.data, bshd, ['Materials.pak'])
                if p and source in p.files:
                    b = p.read(bshd)
                    frag, vert = bg3shade.fragment_from_bshd(b), bg3shade.vertex_from_bshd(b)
                    if frag and vert:
                        roots = bg3pak.read_lsf(p.read(source))
                        dp = ''
                        for root in roots:
                            for node in root.walk():
                                if node.name == 'Material' and node.attrs.get('DiffusionProfileUUID'):
                                    dp = node.attrs.get('DiffusionProfileUUID')
                        r = (bg3shade.Shader(frag, outputs=[0, 1, 2, 3]), bg3shade.Shader(frag, outputs=[0]),
                             shader_layout(roots), bg3shade.tbn_roles(vert), dp)
        except Exception as e:  # noqa: BLE001
            print(f'tavtex: G-buffer shader {source}: {type(e).__name__}: {e}', file=sys.stderr)
            r = None
        with self.lock:
            self._gshaders[source] = r
        return r

    def diffusion_profile(self, uuid):
        """A DiffusionProfile resource's values (Engine.pak and Shared.pak DiffusionProfileBanks), or None;
        the table is read once and cached in profiles.json beside the index."""
        with self.lock:
            if self._profiles is None:
                path = os.path.join(os.path.dirname(os.path.abspath(self.index_path)), 'profiles.json')
                try:
                    self._profiles = json.load(open(path))
                except Exception:  # noqa: BLE001
                    self._profiles = scan_profiles(self.data)
                    try:
                        json.dump(self._profiles, open(path + '.tmp', 'w'))
                        os.replace(path + '.tmp', path)
                    except OSError:
                        pass
            return self._profiles.get(uuid or '')

    def _shader(self, source):
        if source in self.shaders:
            return self.shaders[source]
        r = None
        if source.endswith('.lsf'):
            bshd = source[:-4] + '_ST_DEF_Vulkan.bshd'
            p = bg3pak.find_in_paks(self.data, bshd, ['Materials.pak'])
            if p and source in p.files:
                spv = bg3shade.fragment_from_bshd(p.read(bshd))
                if spv:
                    r = (bg3shade.Shader(spv), shader_layout(bg3pak.read_lsf(p.read(source))))
        self.shaders[source] = r
        return r


def scan_visual_slots(data):
    """{visual resource id: [VertexColorMaskSlots]} of every VisualBank resource that has any."""
    out = {}
    order = ['Shared.pak', 'Gustav.pak', 'GustavX.pak'] + sorted(
        f for f in os.listdir(data) if f.startswith('Patch') and f.endswith('.pak'))
    for pk in order:
        path = os.path.join(data, pk)
        if not os.path.exists(path):
            continue
        p = bg3pak.Pak(path)
        for n in p.files:
            if '/Content/' not in n or not n.endswith('_merged.lsf'):
                continue
            try:
                roots = bg3pak.read_lsf(p.read(n))
            except Exception:  # noqa: BLE001
                continue
            for root in roots:
                for bank in root.walk():
                    if bank.name != 'VisualBank':
                        continue
                    for res in bank.find('Resource'):
                        slots = [c.attrs.get('Object') for c in res.find('VertexColorMaskSlots') if c.attrs.get('Object')]
                        if slots:
                            out[res.attrs.get('ID')] = slots
    return out


def scan_profiles(data):
    """{DiffusionProfile resource id: its values} of Engine.pak's and Shared.pak's DiffusionProfileBanks."""
    out = {}
    for pk in ('Engine.pak', 'Shared.pak'):
        path = os.path.join(data, pk)
        if not os.path.exists(path):
            continue
        p = bg3pak.Pak(path)
        for n in p.files:
            if '/Content/' not in n or not n.endswith('_merged.lsf'):
                continue
            b = p.read(n)
            if b'DiffusionProfileBank' not in b:
                continue
            try:
                roots = bg3pak.read_lsf(b)
            except Exception:  # noqa: BLE001
                continue
            for root in roots:
                for bank in root.walk():
                    if bank.name != 'DiffusionProfileBank':
                        continue
                    for res in bank.find('Resource'):
                        for dp in res.children:
                            if dp.name == 'DiffusionProfile':
                                out[res.attrs.get('ID')] = {k: v for k, v in dp.attrs.items() if not k.startswith('_')}
    return out


def shader_layout(roots):
    """A material shader's parameters (.lsf in Materials.pak): the StaticDeferred constant buffer
    size, each parameter's name, default value and buffer offset, the 2D textures in order with
    their (set, binding) and default texture, and the virtual texture cache bindings."""
    out = {'cb_size': 0, 'params': [], 'textures': [], 'vt': {}}
    for root in roots:
        for node in root.walk():
            if node.name == 'ShaderDesc_StaticDeferred':
                out['cb_size'] = int(node.attrs.get('MaterialCBSize') or 0)
        for c in root.children:
            a = c.attrs
            if c.name in ('ScalarParameters', 'Vector2Parameters', 'Vector3Parameters', 'VectorParameters'):
                off = a.get('StaticDeferred')
                out['params'].append({'name': a.get('ParameterName'), 'value': a.get('Value'),
                                      'offset': -1 if off is None else int(off)})
            elif c.name == 'Texture2DParameters':
                b = next((x.attrs for x in c.children if x.name == 'StaticDeferred'), {})
                out['textures'].append({'name': a.get('ParameterName') or '', 'id': a.get('ID'),
                                        'key': (b.get('VkDescriptorSet'), b.get('VkBindingIndex'))})
            elif c.name == 'VirtualTextureParameters':
                for x in c.children:
                    if x.name == 'StaticDeferred':
                        for y in x.children:
                            out['vt'][y.name] = (y.attrs.get('VkDescriptorSet'), y.attrs.get('VkBindingIndex'))
    return out


def f01(img):
    return img.astype(np.float32) / 255.0


def same_size(a, b):
    """b resized to a's size (nearest is enough for masks)."""
    if a.shape[:2] == b.shape[:2]:
        return b
    from PIL import Image
    return np.array(Image.fromarray(b).resize((a.shape[1], a.shape[0]), Image.BILINEAR))


def material_params(ctx, mat):
    tex, vt, vec, sc = {}, None, {}, {}
    for p in mat.get('Texture2DParameters', []):
        if p.get('Enabled'):
            t = ctx.ix['textures'].get(p.get('ID'))
            if t and t.get('SourceFile'):
                tex[p.get('ParameterName')] = t['SourceFile']
    for p in mat.get('VirtualTextureParameters', []):
        if p.get('Enabled'):
            t = ctx.ix['vtextures'].get(p.get('ID'))
            if t and t.get('GTexFileName'):
                vt = t['GTexFileName']
    for p in mat.get('Vector3Parameters', []):
        vec[p.get('ParameterName')] = p.get('Value')
    for p in mat.get('ScalarParameters', []):
        sc[p.get('ParameterName')] = p.get('Value')
    return tex, vt, vec, sc


# ---------------------------------------------------------------- BG3's shader
def build_cb(ctx, mat, layout, cc):
    """A material constant buffer from the shader's defaults, the material's values and the
    character creation presets (no item colour presets: those come with TavPlan's K lines)."""
    buf = bytearray(layout['cb_size'])
    vals = {}
    for kind in ('ScalarParameters', 'Vector2Parameters', 'Vector3Parameters', 'VectorParameters'):
        for p in mat.get(kind, []):
            vals[p.get('ParameterName')] = p.get('Value')
    for k in ('skin', 'hair', 'eyes'):
        sc, vec, _ = ctx.preset_params(cc.get(k))
        vals.update(sc)
        vals.update(vec)
    for p in layout['params']:
        if p['offset'] < 0:
            continue
        v = vals.get(p['name'], p['value'])
        v = v if isinstance(v, (list, tuple)) else [v]
        for i, x in enumerate(v):
            o = p['offset'] + 4 * i
            if x is not None and o + 4 <= len(buf):
                struct.pack_into('<f', buf, o, float(x))
    return bytes(buf)


def box_down(img):
    """One mip level down: 2x2 box average (edges clamped), as benilla's box_mip."""
    h, w = img.shape[:2]
    nh, nw = max(1, h // 2), max(1, w // 2)
    ys0, xs0 = np.minimum(2 * np.arange(nh), h - 1), np.minimum(2 * np.arange(nw), w - 1)
    ys1, xs1 = np.minimum(ys0 + 1, h - 1), np.minimum(xs0 + 1, w - 1)
    return (img[ys0][:, xs0] + img[ys0][:, xs1] + img[ys1][:, xs0] + img[ys1][:, xs1]) * 0.25


class Pyramid:
    """An input texture's box-filtered mip levels, made as they are asked for."""

    def __init__(self, img):
        self.levels = [img]

    def level(self, j):
        while len(self.levels) <= j and max(self.levels[-1].shape[:2]) > 1:
            self.levels.append(box_down(self.levels[-1]))
        return self.levels[min(j, len(self.levels) - 1)]


_chunk_pool = None
_chunk_lock = threading.Lock()


def run_shader(sh, uv, cb, textures, vt, colour):
    """sh.run, its chunks spread over CHUNK_THREADS threads (one big bake uses several
    cores; at most that many chunks are evaluated at once, whichever bakes they belong to)."""
    global _chunk_pool
    if len(uv) <= CHUNK:
        return sh.run(uv, cb, textures, vt, colour, chunk=CHUNK)
    with _chunk_lock:
        if _chunk_pool is None:
            from concurrent.futures import ThreadPoolExecutor
            _chunk_pool = ThreadPoolExecutor(CHUNK_THREADS, thread_name_prefix='chunk')
    parts = [uv[s:s + CHUNK] for s in range(0, len(uv), CHUNK)]
    return np.concatenate(list(_chunk_pool.map(lambda p: sh._run(p, cb, textures or {}, vt or {}, colour), parts)), 0)


def shade(ctx, mat, cc, cb=None, colour=None, mips=False, size=MAX_SIZE):
    """BG3's G-buffer base colour of a material over its UV space (linear RGB, h x w x 3), or None
    when its shader is not available. cb: the constant buffer BG3 draws with (TavPlan K lines);
    colour: the mean vertex colour of the material's meshes (hair and fur read it). mips: a list of
    levels down to 1x1 instead, each shaded from its inputs at that level's density (an input of
    width W_in is at level k + log2(W_in / W) where the bake's level k is used)."""
    s = ctx.shader(mat.get('SourceFile', ''))
    if s is None:
        return None
    sh, layout = s
    if cb is None or len(cb) != layout['cb_size']:
        cb = build_cb(ctx, mat, layout, cc)
    used = sh.used_resources()
    mtex = mat.get('Texture2DParameters', [])
    textures, sizes = {}, []
    for i, tp in enumerate(layout['textures']):
        key = (int(tp['key'][0]), int(tp['key'][1])) if None not in tp['key'] else None
        if key not in used:
            continue
        tid = None
        if tp['name']:
            tid = next((p.get('ID') for p in mtex if p.get('ParameterName') == tp['name']), None)
        if tid is None and i < len(mtex) and (mtex[i].get('ParameterName') or '') == tp['name']:
            tid = mtex[i].get('ID')
        img = ctx.tex_linear(tid or tp['id'], size)
        if img is None:
            img = np.zeros((1, 1, 4), np.float32)
        textures[key] = img
        if tp['name'] in SIZE_TEXTURES and img.shape[0] > 16:
            sizes.append((SIZE_TEXTURES.index(tp['name']), img.shape[:2]))
    vt = {}
    gtex = None
    for p in mat.get('VirtualTextureParameters', []):
        t = ctx.ix['vtextures'].get(p.get('ID'))
        if t and t.get('GTexFileName'):
            gtex = t['GTexFileName']
            break
    for layer, cache in enumerate(('Cache0', 'Cache1', 'Cache2')):
        key = layout['vt'].get(cache)
        if key is None or None in key:
            continue
        key = (int(key[0]), int(key[1]))
        if key not in used:
            continue
        img = ctx.vt(gtex, layer, size) if gtex else None
        if img is None:
            vt[key] = np.zeros((1, 1, 4), np.float32)
            continue
        f = f01(img)
        if layer == 0:
            f[..., :3] = srgb_u8_to_lin(img[..., :3])
        vt[key] = f
        if layer == 0:
            sizes.insert(0, (-1, img.shape[:2]))
    if sizes:
        h, w = min(sizes)[1]
    else:
        h = w = min(256, size)
    while max(h, w) > size:
        h, w = max(1, h // 2), max(1, w // 2)
    pyr = {key: Pyramid(img) for key, img in list(textures.items()) + list(vt.items())}
    levels, k = [], 0
    while True:
        hk, wk = max(1, h >> k), max(1, w >> k)

        def at(key, img):
            j = k + int(round(np.log2(max(img.shape[1], 1) / w))) if img.shape[1] > 1 else 0
            return pyr[key].level(max(0, j))
        tk = {key: at(key, img) for key, img in textures.items()}
        vk = {key: at(key, img) for key, img in vt.items()}
        v, u = np.meshgrid((np.arange(hk, dtype=np.float32) + 0.5) / hk, (np.arange(wk, dtype=np.float32) + 0.5) / wk,
                           indexing='ij')
        uv = np.stack([u.ravel(), v.ravel()], 1)
        out = run_shader(sh, uv, cb, tk, vk, colour)
        rgb = np.clip(np.nan_to_num(out[:, :3].reshape(hk, wk, 3), nan=0.0), 0, 1)
        if not mips:
            return rgb
        levels.append(rgb)
        if hk == 1 and wk == 1:
            return levels
        k += 1


def alpha_of(ctx, tex, vt, shader, size=MAX_SIZE):
    """(alpha or None, flags) of a material: alpha-tested virtual textures, hair and scalp MSKA,
    fur IDMsk."""
    # CHAR_Pubes too (alpha-tested in BG3; baked opaque it was a dark shape poking through).
    if shader.startswith(('CHAR_Hair', 'CHAR_Scalp', 'CHAR_Pubes')):
        if 'ID_Depth_Root_Alpha_MSKA' in tex:
            m = ctx.dds(tex['ID_Depth_Root_Alpha_MSKA'], size=size)
            if m is not None:
                return f01(m)[..., 3], 1
        return None, 0
    if 'Fur' in shader:
        if 'IDMsk' in tex:
            m = ctx.dds(tex['IDMsk'], size=size)
            if m is not None:
                return f01(m)[..., 3], 1
        return None, 0
    if vt and 'AlphaTest' in shader:
        base = ctx.vt(vt, 0, size)
        if base is not None:
            return f01(base)[..., 3], 1
    return None, 0


# ---------------------------------------------------------------- older approximations (fallback)
def bake_skin(ctx, tex, vt, vec, sc, skin):
    psc, pvec, ui = skin
    vec = {**vec, **pvec}
    sc = {**sc, **psc}
    clea = ctx.dds(tex['CLEA']) if 'CLEA' in tex else (ctx.vt(vt, 0) if vt else None)
    hmvy = ctx.dds(tex['HMVY']) if 'HMVY' in tex else (ctx.vt(vt, 2) if vt else None)
    if clea is None or hmvy is None:
        return None
    hmvy = same_size(clea, hmvy)
    c, m = f01(clea), f01(hmvy)
    out = np.ones(c.shape[:2] + (3,), np.float32)
    for mask, color, amount in ((m[..., 1], 'MelaninColor', 'MelaninAmount'), (m[..., 0], 'HemoglobinColor', 'HemoglobinAmount'),
                                (m[..., 3], 'YellowingColor', 'YellowingAmount'), (m[..., 2], 'VeinColor', 'VeinAmount')):
        col = np.array(vec.get(color, [1, 1, 1]), np.float32)
        a = np.clip(mask * float(sc.get(amount, 0.5)), 0, 1)[..., None]
        out *= 1 - a + a * col
    out *= (0.45 + 0.55 * c[..., 3])[..., None]
    skin_px = c[..., 1] < 0.2
    if ui and skin_px.any():
        target = ui_color(ui)
        mean = out[skin_px].mean(0) + 1e-4
        out *= np.clip(target / mean, 0.1, 10)
    brow = np.array(vec.get('Eyebrow_Color', [0.1, 0.07, 0.045]), np.float32)
    out = out * (1 - c[..., 1:2]) + brow * c[..., 1:2]
    return out, None, 0


def bake_hair(ctx, tex, vec, hair, scalp=False):
    psc, pvec, ui = hair
    vec = {**vec, **pvec}
    mska = ctx.dds(tex.get('ID_Depth_Root_Alpha_MSKA', '')) if 'ID_Depth_Root_Alpha_MSKA' in tex else None
    if mska is None:
        return None
    k = f01(mska)
    if scalp:
        col = np.array(vec.get('Hair_Scalp_Color', [0.15, 0.09, 0.05]), np.float32)
    else:
        col = np.array(vec.get('Hair_Color', [0.3, 0.2, 0.1]), np.float32)
    root = np.array(vec.get('Hair_Scalp_Color', col * 0.6), np.float32)
    out = col * (0.35 + 0.65 * k[..., 1:2])
    out = out * (1 - 0.5 * k[..., 2:3]) + root * 0.5 * k[..., 2:3]
    return out, k[..., 3], 1


def bake_vt(ctx, tex, vt, vec, shader):
    base = ctx.vt(vt, 0)
    if base is None:
        return None
    b = srgb_to_lin(f01(base)[..., :3])
    alpha = f01(base)[..., 3] if 'AlphaTest' in shader else None
    mask_name = tex.get('MSKColor') or tex.get('MSKcloth')
    if mask_name:
        mk = ctx.dds(mask_name)
        if mk is not None:
            mk = f01(same_size(base, mk))
            tints = [vec.get('Cloth_Primary') or vec.get('Color_01'), vec.get('Cloth_Secondary') or vec.get('Color_02'),
                     vec.get('Cloth_Tertiary') or vec.get('Color_03')]
            for ch, t in enumerate(tints):
                if t is None:
                    continue
                t = np.array(t, np.float32)
                a = mk[..., ch:ch + 1]
                b = b * (1 - a) + np.clip(b * t * 2.0, 0, 1) * a
    return b, alpha, 1 if alpha is not None else 0


def bake_fur(ctx, tex, vec):
    bm = ctx.dds(tex['Body_color_texture']) if 'Body_color_texture' in tex else None
    if bm is None:
        return None
    b = srgb_to_lin(f01(bm)[..., :3]) * np.array(vec.get('Color_01', [1, 1, 1]), np.float32) * 2.0
    alpha = None
    if 'IDMsk' in tex:
        idm = ctx.dds(tex['IDMsk'])
        if idm is not None:
            alpha = f01(same_size(bm, idm))[..., 3]
    return b, alpha, 1 if alpha is not None else 0


def bake_eyes(ctx, tex, vec, eyes):
    psc, pvec, ui = eyes
    vec = {**vec, **pvec}
    parts = ctx.dds(tex['ColorsMask']) if 'ColorsMask' in tex else None
    if parts is None:
        return None
    k = f01(parts)
    sclera = np.array([0.75, 0.72, 0.7], np.float32)
    iris = np.array(vec.get('Eyes_IrisColour', ui_color(ui) if ui else [0.2, 0.3, 0.4]), np.float32)
    out = sclera * (1 - k[..., 0:1]) + iris * k[..., 0:1]
    out *= (1 - 0.9 * k[..., 1:2])
    return out, None, 0


def bake_approx(ctx, shader, tex, vt, vec, sc, cc):
    skin, hair, eyes = (ctx.preset_params(cc.get(k)) for k in ('skin', 'hair', 'eyes'))
    if shader.startswith('CHAR_Skin'):
        return bake_skin(ctx, tex, vt, vec, sc, skin)
    if shader.startswith('CHAR_Hair'):
        return bake_hair(ctx, tex, vec, hair)
    if shader.startswith('CHAR_Scalp'):
        return bake_hair(ctx, tex, vec, hair, scalp=True)
    if shader.startswith('CHAR_Eye'):
        return bake_eyes(ctx, tex, vec, eyes)
    if 'Fur' in shader:
        return bake_fur(ctx, tex, vec)
    if vt:
        return bake_vt(ctx, tex, vt, vec, shader)
    return None


def bake(ctx, mat_id, cc, cb=None, colour=None, size=MAX_SIZE):
    """(rgba uint8 level 0, flags, [lower levels]) or (None, flags, []). cb: the material constant
    buffer BG3 draws the mesh with (bytes), when TavPlan published it; colour: its meshes' mean
    vertex colour. The lower levels (flag 8) come with BG3's shader."""
    mat = ctx.ix['materials'].get(mat_id)
    if not mat:
        return np.full((4, 4, 4), (128, 128, 128, 255), np.uint8), 0, []
    shader = os.path.basename(mat.get('SourceFile', ''))
    tex, vt, vec, sc = material_params(ctx, mat)
    if 'Tearline' in shader or 'LashesShadow' in shader or shader.startswith(HIDDEN_SHADERS):
        return None, 2, []
    levels = None
    try:
        levels = shade(ctx, mat, cc, cb, colour, mips=True, size=size)
        if levels is not None and not MIPS:
            levels = levels[:1]
    except Exception as e:  # noqa: BLE001
        print(f'tavtex: {mat_id} ({shader}): BG3 shader failed ({type(e).__name__}: {e}), approximating', file=sys.stderr)
    if levels is not None:
        rgb = levels[0]
        alpha, flags = alpha_of(ctx, tex, vt, shader, size)
        if alpha is not None and alpha.shape != rgb.shape[:2]:
            from PIL import Image
            alpha = np.array(Image.fromarray(alpha.astype(np.float32), 'F').resize((rgb.shape[1], rgb.shape[0]), Image.BILINEAR))
    else:
        r = bake_approx(ctx, shader, tex, vt, vec, sc, cc)
        if r is None:
            return np.full((4, 4, 4), (128, 128, 128, 255), np.uint8), 0, []
        rgb, alpha, flags = r
        levels = [rgb]
    out = []
    a = None if alpha is None else np.clip(alpha.astype(np.float32), 0, 1)
    for k, lv in enumerate(levels):
        if k and a is not None:
            a = box_down(a)
        px = np.empty(lv.shape[:2] + (4,), np.uint8)
        px[..., :3] = (lin_to_srgb(lv) * 255 + 0.5).astype(np.uint8)
        px[..., 3] = 255 if a is None else (a[:lv.shape[0], :lv.shape[1]] * 255 + 0.5).astype(np.uint8)
        out.append(px)
    return out[0], flags | (TVT_MIPS if len(out) > 1 else 0), out[1:]


def chunked(fn, uv):
    """fn over the CHUNK-texel parts of uv, on the chunk threads (as run_shader); the list of results."""
    global _chunk_pool
    parts = [uv[s:s + CHUNK] for s in range(0, len(uv), CHUNK)]
    if len(parts) == 1:
        return [fn(parts[0])]
    with _chunk_lock:
        if _chunk_pool is None:
            from concurrent.futures import ThreadPoolExecutor
            _chunk_pool = ThreadPoolExecutor(CHUNK_THREADS, thread_name_prefix='chunk')
    return list(_chunk_pool.map(fn, parts))


def gbuffer_normal(out0):
    """BG3's encoded G-buffer normal (stereographic, ClusteredLighting's decode) of the bake's basis (X = +x,
    Y = +y, N = -z) as (X, Y, N) coefficients."""
    v = out0[:, :2] * 3.5554 - 1.7777
    l2 = np.maximum((v * v).sum(1, keepdims=True) + 1.0, 1e-6)
    n = np.concatenate([v * (2.0 / l2), 1.0 - 2.0 / l2], 1)
    n[:, 2] *= -1.0
    return n


def octahedral_decode(out3):
    """BG3's 12-bit octahedral direction in a G-buffer target's rgb (ClusteredLighting's hair decode)."""
    c = out3[:, :3] * 255.0
    y16 = c[:, 1] * 0.0625
    e = np.stack([c[:, 0] * 16.0 + np.floor(y16), (y16 - np.floor(y16)) * 4096.0 + c[:, 2]], 1)
    e = np.clip(e * 0.00048851978 - 1.0, -1.0, 1.0)
    z = 1.0 - np.abs(e[:, 0]) - np.abs(e[:, 1])
    sgn = np.where(e >= 0, 1.0, -1.0)
    folded = (1.0 - np.abs(e[:, ::-1])) * sgn
    xy = np.where((z >= 0)[:, None], e, folded)
    v = np.concatenate([xy, z[:, None]], 1)
    return v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-6)


def bake_gbuffer(ctx, mat_id, cc, cb=None, colour=None, size=MAX_SIZE):
    """BG3's G-buffer outputs of a material over its UV space, or None when its shader cannot run:
    {'normal', 'surface', 'extra' (or None): RGBA8 images, 'model': BG3's shading model, 'detail': (texture
    source, tiling) or None, 'profile': its DiffusionProfile values or None}. See "G-buffer maps" above."""
    mat = ctx.ix['materials'].get(mat_id)
    if not mat:
        return None
    g = ctx.gshader(mat.get('SourceFile', ''))
    if g is None:
        return None
    sh, nsh, layout, roles, shader_dp = g
    if cb is None or len(cb) != layout['cb_size']:
        cb = build_cb(ctx, mat, layout, cc)
    used = sh.used_resources()
    mtex = mat.get('Texture2DParameters', [])
    textures, sizes, detail = {}, [], None
    for i, tp in enumerate(layout['textures']):
        key = (int(tp['key'][0]), int(tp['key'][1])) if None not in tp['key'] else None
        if key not in used:
            continue
        tid = None
        if tp['name']:
            tid = next((p.get('ID') for p in mtex if p.get('ParameterName') == tp['name']), None)
        if tid is None and i < len(mtex) and (mtex[i].get('ParameterName') or '') == tp['name']:
            tid = mtex[i].get('ID')
        if tp['name'] == 'DetailNormalMap':
            t = ctx.texture(tid or tp['id'])
            if t and t.get('SourceFile'):
                detail = (key, t['SourceFile'])
        img = ctx.tex_linear(tid or tp['id'], size)
        if img is None:
            img = np.zeros((1, 1, 4), np.float32)
        textures[key] = img
        if tp['name'] in SIZE_TEXTURES and img.shape[0] > 16:
            sizes.append((SIZE_TEXTURES.index(tp['name']), img.shape[:2]))
    vt = {}
    gtex = None
    for p in mat.get('VirtualTextureParameters', []):
        t = ctx.ix['vtextures'].get(p.get('ID'))
        if t and t.get('GTexFileName'):
            gtex = t['GTexFileName']
            break
    for layer, cache in enumerate(('Cache0', 'Cache1', 'Cache2')):
        key = layout['vt'].get(cache)
        if key is None or None in key:
            continue
        key = (int(key[0]), int(key[1]))
        if key not in used:
            continue
        img = ctx.vt(gtex, layer, size) if gtex else None
        if img is None:
            vt[key] = np.zeros((1, 1, 4), np.float32)
            continue
        f = f01(img)
        if layer == 0:
            f[..., :3] = srgb_u8_to_lin(img[..., :3])
        vt[key] = f
        if layer == 0:
            sizes.insert(0, (-1, img.shape[:2]))
    if sizes:
        h, w = min(sizes)[1]
    else:
        h = w = min(256, size)
    while max(h, w) > size:
        h, w = max(1, h // 2), max(1, w // 2)
    basis = {'X': (1.0, 0.0, 0.0), 'Y': (0.0, 1.0, 0.0), 'N': (0.0, 0.0, -1.0)}
    vec3_inputs = {loc: basis[r] for loc, r in roles.items()}
    ubos = {(0, 2): np.eye(4, dtype=np.float32).tobytes()}
    tiling = 0.0
    if detail is not None:
        # The detail map's tiling: where its first sample lands for two texels.
        probe = {detail[0]: None}
        sh.run_outputs(np.array([[0.1, 0.2], [0.3, 0.45]], np.float32), cb, textures, vt, colour, ubos=ubos,
                       vec3_inputs=vec3_inputs, probe=probe)
        c = probe[detail[0]]
        if c is not None and len(c) >= 2:
            su, sv = (c[1, 0] - c[0, 0]) / 0.2, (c[1, 1] - c[0, 1]) / 0.25
            if abs(su - sv) < 0.01 * max(abs(su), 1e-6) and su > 1.5:
                tiling = float(su)
        if tiling == 0.0:
            detail = None
        else:
            # Flat for the main run (BG3's decode: x = A, N = B, Y = -G, each * 2 - 1): (G 0.5, B 1, A 0.5).
            textures[detail[0]] = np.array([[[0.0, 0.5, 1.0, 0.5]]], np.float32)
    v, u = np.meshgrid((np.arange(h, dtype=np.float32) + 0.5) / h, (np.arange(w, dtype=np.float32) + 0.5) / w,
                       indexing='ij')
    uv = np.stack([u.ravel(), v.ravel()], 1)
    # BG3's shading model (the material byte's low bits): the most common over a coarse grid of the UV space.
    cq = (np.arange(32, dtype=np.float32) + 0.5) / 32
    cv, cu = np.meshgrid(cq, cq, indexing='ij')
    coarse = sh._run(np.stack([cu.ravel(), cv.ravel()], 1), cb, textures, vt or {}, colour, ubos, vec3_inputs, multi=True)
    model_bytes = np.rint(np.nan_to_num(coarse[2][:, 3]) * 255.0).astype(np.int64) & 7
    bg3_model = int(np.bincount(model_bytes, minlength=8).argmax())
    # benilla shades BG3's standard (0), skin (3) and hair (5) models; the others (1 unlit, 2 anisotropic, 4
    # foliage, 6 and 7 cloth sheen) as the standard model.
    model = bg3_model if bg3_model in (0, 3, 5) else 0
    tx = None
    if detail is not None:
        # A detail texel along +X (A 1, B 0.5, G 0.5): BG3 adds k * (1, 0, 0) to the unnormalised sum.
        tx = dict(textures)
        tx[detail[0]] = np.array([[[0.0, 0.5, 0.5, 1.0]]], np.float32)

    def u8(x):
        return (np.clip(np.nan_to_num(x, nan=0.0), 0, 1) * 255 + 0.5).astype(np.uint8)

    def texels(part):
        """One chunk of the UV grid as the three RGBA8 maps (converted here, so no full-size float outputs)."""
        outs = sh._run(part, cb, textures, vt or {}, colour, ubos, vec3_inputs, multi=True)
        n0 = gbuffer_normal(outs[0])
        k = None
        if tx is not None:
            n1 = gbuffer_normal(nsh._run(part, cb, tx, vt or {}, colour, ubos, vec3_inputs, multi=True)[0])
            # n1 is parallel to n0 + k e_x: its Y and N parts scale n0's by s = 1 / |n0 + k e_x|.
            den = np.where(np.abs(n0[:, 2]) > np.abs(n0[:, 1]), n0[:, 2], n0[:, 1])
            num = np.where(np.abs(n0[:, 2]) > np.abs(n0[:, 1]), n1[:, 2], n1[:, 1])
            sc = np.clip(num / np.where(np.abs(den) < 1e-4, 1e-4, den), 1e-3, 2.0)
            k = np.clip(n1[:, 0] / sc - n0[:, 0], 0.0, 0.5)
        nb = np.stack([n0[:, 0], -n0[:, 1], n0[:, 2]], 1)
        nb /= np.maximum(np.linalg.norm(nb, axis=1, keepdims=True), 1e-6)
        normal = np.empty((len(part), 4), np.uint8)
        normal[:, :3] = u8(nb * 0.5 + 0.5)
        normal[:, 3] = 0 if k is None else u8(k * 2.0)
        o1, o2, o3 = outs[1], outs[2], outs[3]
        surface = np.stack([u8(o1[:, 3]), u8(o2[:, 0]), u8(o2[:, 2]), u8(o2[:, 1])], 1)
        extra = None
        if model == 3:
            extra = u8(o3)
        elif model == 5:
            t = octahedral_decode(np.nan_to_num(o3, nan=0.0))  # (X, Y, N) of the strand
            extra = np.empty((len(part), 4), np.uint8)
            extra[:, :3] = u8(np.stack([t[:, 0], -t[:, 1], -t[:, 2]], 1) * 0.5 + 0.5)
            extra[:, 3] = u8(o3[:, 3])
        return normal, surface, extra

    rs = chunked(texels, uv)
    normal = np.concatenate([r[0] for r in rs], 0)
    surface = np.concatenate([r[1] for r in rs], 0)
    extra = np.concatenate([r[2] for r in rs], 0) if model in (3, 5) else None
    dp = mat.get('DiffusionProfileUUID') or shader_dp
    profile = ctx.diffusion_profile(dp) if model == 3 else None
    return {'normal': normal.reshape(h, w, 4), 'surface': surface.reshape(h, w, 4),
            'extra': None if extra is None else extra.reshape(h, w, 4), 'model': model,
            'detail': (detail[1], tiling) if detail is not None else None, 'profile': profile, 'bg3_model': bg3_model}


def detail_tvt(ctx, source):
    """A detail normal map (DDS: X in alpha, Y in green as BG3 decodes it, Z in blue) as RGBA8 with
    RGB = (X, +V, Z) * 0.5 + 0.5, the mesh frame benilla tiles it in (BG3's -G along -V is +G along +V)."""
    img = ctx.dds(source)
    if img is None:
        return None
    out = np.empty(img.shape[:2] + (4,), np.uint8)
    out[..., 0], out[..., 1], out[..., 2], out[..., 3] = img[..., 3], img[..., 1], img[..., 2], 255
    return out


def profile_token(p):
    """tavtex.txt's profile:<12 numbers> of a DiffusionProfile's values."""
    sc, tt = p.get('ScatterColor') or [1, 1, 1], p.get('TransmissionTint') or [1, 1, 1]
    tmin, tmax = p.get('ThicknessRemapMin', 0.0), p.get('ThicknessRemapMax', 20.0)
    if p.get('ThickObjectTransmission'):
        # BG3 measures a thick object's thickness in its sun's shadow map, which WoW has not: no transmission
        # (a thickness far past the scatter distance) rather than the thin-object law on a whole body.
        tmin = tmax = 1000.0
    vals = [p.get('DualSpecularRoughnessA', 0.75), p.get('DualSpecularRoughnessB', 1.3), p.get('DualSpecularMix', 0.85),
            p.get('ScatterDistance', 0.0), *sc[:3], tmin, tmax, *tt[:3]]
    return 'profile:' + ','.join(f'{float(x):.4g}' for x in vals)


def normal_map(ctx, tex, vt, shader):
    """RGB = tangent-space normal * 0.5 + 0.5 (BG3's convention), or None."""
    if 'Hair' in shader or 'Scalp' in shader or 'Fur' in shader or 'Tearline' in shader or 'Lashes' in shader:
        return None
    src = None
    for name in ('normalmap', 'IrisNormal', 'NM', 'Normal'):
        if name in tex:
            src = ctx.dds(tex[name])
            if src is not None:
                break
    if src is None and vt:
        src = ctx.vt(vt, 1)
    if src is None:
        return None
    f = f01(src)
    x, y = f[..., 3] * 2 - 1, f[..., 1] * 2 - 1
    z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
    out = np.empty(f.shape[:2] + (4,), np.uint8)
    for ch, v in enumerate((x, y, z)):
        out[..., ch] = (np.clip(v * 0.5 + 0.5, 0, 1) * 255 + 0.5).astype(np.uint8)
    out[..., 3] = 255
    return out


def surface_map(ctx, tex, vt, sc, shader):
    """RGBA8: R occlusion, G roughness, B metalness."""
    def const(rough, metal=0.0):
        px = np.empty((4, 4, 4), np.uint8)
        px[...] = (255, int(rough * 255 + 0.5), int(metal * 255 + 0.5), 255)
        return px
    if shader.startswith('CHAR_Skin'):
        return const(float(sc.get('TMP_BODY_ROUGH', 0.55)))
    if shader.startswith('CHAR_Hair'):
        return const(float(sc.get('Roughness', 0.45)))
    if shader.startswith('CHAR_Scalp'):
        return const(float(sc.get('Scalp_Roughness', 0.45)))
    if shader.startswith('CHAR_Eye'):
        return const(0.1)
    if 'Fur' in shader:
        return const(float(sc.get('Roughness_Base', 0.5)))
    if vt:
        pm = ctx.vt(vt, 2)
        if pm is not None:
            out = np.empty(pm.shape[:2] + (4,), np.uint8)
            out[..., 0] = pm[..., 2]  # occlusion
            out[..., 1] = pm[..., 1]  # roughness
            out[..., 2] = pm[..., 0]  # metalness
            out[..., 3] = 255
            return out
    return const(float(sc.get('Roughness', 0.6)))


def bake_maps(ctx, mat_id):
    """(normal or None, surface) for a material."""
    mat = ctx.ix['materials'].get(mat_id)
    if not mat:
        return None, np.full((4, 4, 4), (255, 153, 0, 255), np.uint8)
    shader = os.path.basename(mat.get('SourceFile', ''))
    tex, vt, vec, sc = material_params(ctx, mat)
    return normal_map(ctx, tex, vt, shader), surface_map(ctx, tex, vt, sc, shader)


def calibrate(img, cal):
    """The baked albedo (RGBA8, sRGB) corrected toward BG3's own base colour: tavcalib.py sums, per
    UV cell, BG3's G-buffer colour and the bake's (linear) where BG3 drew the same surface. The
    per-cell ratio (borrowing neighbours, and the material's overall ratio where cells saw too
    little) multiplies the bake, so its detail stays and BG3's colours come through. Off unless
    WOWBG3_TAVCALIB=1: the albedo now comes from BG3's own shader and colour inputs."""
    bg, bk, n = cal['bg3'], cal['baked'], cal['n']
    if n.sum() < 30:
        return img
    whole = bg.sum((0, 1)) / np.maximum(bk.sum((0, 1)), 1e-4)

    def box(a):
        return sum(np.roll(np.roll(a, dy, 0), dx, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1))
    sb, sk, sn = box(bg), box(bk), box(n)
    ratio = np.where((sn >= 4)[..., None], sb / np.maximum(sk, 1e-4), whole)
    w = np.clip(sn / 12.0, 0, 1)[..., None]
    ratio = np.clip(ratio * w + whole * (1 - w), 0.05, 20).astype(np.float32)
    from PIL import Image
    h, wd = img.shape[:2]
    up = np.stack([np.array(Image.fromarray(ratio[..., c], 'F').resize((wd, h), Image.BILINEAR)) for c in range(3)], -1)
    out = img.copy()
    out[..., :3] = (lin_to_srgb(srgb_to_lin(img[..., :3] / 255.0) * up) * 255 + 0.5).astype(np.uint8)
    return out


def write_tvt(path, img, flags, mips=()):
    """A TVT1 file; with flag 8 the lower mip levels follow level 0."""
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(struct.pack('<4sIII', b'TVT1', img.shape[1], img.shape[0], flags))
        f.write(np.ascontiguousarray(img).tobytes())
        for lv in mips:
            f.write(np.ascontiguousarray(lv).tobytes())
    os.replace(tmp, path)


def read_plan(path):
    """(seq, [(mesh key, material, figure, lod)], {figure: cc colours}, {mesh key: constant buffer
    bytes}, {figure: [visual resource ids of the equipment]}). lod: the M line's 18th field (0 when absent)."""
    seq, meshes, cc, cbs, visuals = None, [], {}, {}, {}
    for line in open(path):
        f = line.split()
        if not f:
            continue
        if f[0] == 'TAVPLAN':
            seq = int(f[2])
        elif f[0] == 'M' and len(f) >= 16:
            meshes.append((f[1], f[15], int(f[16]) if len(f) >= 17 and f[16].isdigit() else 0,
                           int(f[17]) if len(f) >= 18 and f[17].isdigit() else 0))
        elif f[0] == 'C' and len(f) >= 4:
            cc[int(f[4]) if len(f) >= 5 and f[4].isdigit() else 0] = {'skin': f[1], 'hair': f[2], 'eyes': f[3]}
        elif f[0] == 'V' and len(f) >= 2:
            visuals.setdefault(int(f[2]) if len(f) >= 3 and f[2].isdigit() else 0, []).append(f[1])
        elif f[0] == 'K' and len(f) >= 4:
            try:
                off, words = int(f[2]), [int(x, 16) for x in f[3:]]
            except ValueError:
                continue
            b = cbs.setdefault(f[1], bytearray())
            end = off + 4 * len(words)
            if len(b) < end:
                b.extend(bytes(end - len(b)))
            struct.pack_into(f'<{len(words)}I', b, off, *words)
    return seq, meshes, cc, {k: bytes(v) for k, v in cbs.items()}, visuals


def cuts_vertices(ctx, mat_id):
    """Whether BG3's vertex shader for this material cuts by the vertex colour mask (its shader
    has a VertexColor_MSK texture)."""
    mat = ctx.ix['materials'].get(mat_id) or {}
    try:
        s = ctx.shader(mat.get('SourceFile', ''))
    except Exception:  # noqa: BLE001
        s = None
    return bool(s) and any(t['name'] == 'VertexColor_MSK' for t in s[1]['textures'])


def mesh_colours(tav_dir, meshes):
    """({material: mean vertex colour, RGBA 0..1}, mesh keys found) from the layer's copy of the
    plan's meshes (TAV_DIR/tavmesh.bin, TVM1; a skinned mesh's stream 0 starts with a u8x4 colour
    when uv0 is at offset 4). BG3's hair and fur shaders read the vertex colour; the bake, in
    texture space, uses the mean."""
    try:
        b = open(os.path.join(tav_dir, 'tavmesh.bin'), 'rb').read()
    except OSError:
        return {}, set()
    if len(b) < 40 or b[:4] != b'TVM1':
        return {}, set()
    n = struct.unpack_from('<I', b, 12)[0]
    want = {int(k, 16): (m, fig) for k, m, fig, *_ in meshes}
    coarse = {int(k, 16) for k, m, fig, *rest in meshes if rest and rest[0] > 0}
    sums, found = {}, set()
    at = 40 + 64 * n
    for i in range(n):
        key, = struct.unpack_from('<Q', b, 40 + 64 * i)
        kind, part, vc, ic, isz, vs, uvoff, sstride, nsrc, nskin, nvb, nib = struct.unpack_from('<12I', b, 48 + 64 * i)
        vb_at = at + nsrc + nskin
        at += nsrc + nskin + nvb + nib
        if key in want:
            found.add(key)
        if key not in want or kind != 0 or uvoff != 4 or vs < 4 or nvb < vc * vs or vb_at + nvb > len(b):
            continue
        v = np.frombuffer(b, np.uint8, vc * vs, vb_at).reshape(vc, vs)[:, :4].astype(np.float64)
        # LOD 0 meshes give a material's mean (as before LODs were planned); coarser LODs only
        # when it has no LOD 0 mesh.
        s = sums.setdefault(want[key], [[np.zeros(4), 0], [np.zeros(4), 0]])[1 if key in coarse else 0]
        s[0] += v.sum(0)
        s[1] += vc
    out = {}
    for m, (s0, s1) in sums.items():
        s = s0 if s0[1] else s1
        if s[1]:
            out[m] = s[0] / s[1] / 255.0
    return out, found


# ---------------------------------------------------------------- serving a plan
def uses_vertex_colour(ctx, mat):
    """Whether the material's shader reads the vertex colour (a vec4 interpolant) for its base
    colour: only then is the meshes' mean colour an input of the bake and of its cache key."""
    src = (ctx.ix['materials'].get(mat) or {}).get('SourceFile', '')
    with ctx.lock:
        if src in ctx._colour_use:
            return ctx._colour_use[src]
    use = True  # the approximations (no shader) may read it
    try:
        s = ctx.shader(src)
    except Exception:  # noqa: BLE001
        s = None
    if s is not None:
        sh = s[0]
        m = sh.m
        inputs = {v for loc, (v, t) in sh.io['inputs'].items() if m.types[t][0] == 'vec' and m.types[t][2] == 4}
        use = any(m.body[k][0] == bg3shade.OP['Load'] and sh._root(m.body[k][1][2], sh.defs) in inputs for k in sh.order)
    with ctx.lock:
        ctx._colour_use[src] = use
    return use


class Workers:
    """A few bake threads, the most urgent job first (previews before full bakes). numpy releases
    the interpreter lock in the shader's array work, so threads bake in parallel."""

    def __init__(self, n):
        self.q, self.cv, self.count, self.busy = [], threading.Condition(), itertools.count(), 0
        for i in range(n):
            threading.Thread(target=self._run, name=f'bake{i}', daemon=True).start()

    def submit(self, prio, fn):
        with self.cv:
            heapq.heappush(self.q, (prio, next(self.count), fn))
            self.cv.notify()

    def idle(self):
        with self.cv:
            return not self.q and not self.busy

    def _run(self):
        while True:
            with self.cv:
                while not self.q:
                    self.cv.wait()
                _, _, fn = heapq.heappop(self.q)
                self.busy += 1
            try:
                fn()
            except Exception as e:  # noqa: BLE001
                print(f'tavtex: a bake failed: {type(e).__name__}: {e}', file=sys.stderr, flush=True)
            finally:
                with self.cv:
                    self.busy -= 1


def tvt_flags(path, memo={}):  # noqa: B006
    st = os.stat(path)
    k = (path, st.st_mtime_ns)
    if k not in memo:
        with open(path, 'rb') as f:
            memo[k] = struct.unpack('<4sIII', f.read(16))[3]
    return memo[k]


CACHE_CAP = 3 << 30        # bytes of bakes kept; past it the least recently used go first
CACHE_IDLE_DAYS = 30       # a bake not used for this long goes whatever the size


def prune_cache(cache):
    """At start: bakes (all files of one key h) unused for CACHE_IDLE_DAYS go, then the least recently used
    while the cache is over CACHE_CAP; then the detail maps (d-*.tvt) no remaining bake's h-g.json names. A bake
    served from the cache is marked used (Baker.touch), so the party's own stay; a removed one is baked again
    when it is needed. Returns (bakes removed, bytes freed)."""
    groups = {}
    try:
        names = os.listdir(cache)
    except OSError:
        return 0, 0
    for name in names:
        if name.startswith('d-') or name.endswith('.tmp'):
            continue
        p = os.path.join(cache, name)
        try:
            st = os.stat(p)
        except OSError:
            continue
        g = groups.setdefault(name.split('.')[0].split('-')[0], [0.0, 0, []])
        g[0] = max(g[0], st.st_mtime)
        g[1] += st.st_size
        g[2].append(p)
    now, total = time.time(), sum(g[1] for g in groups.values())
    gone = freed = 0
    for _, (used, size, paths) in sorted(groups.items(), key=lambda kv: kv[1][0]):
        if now - used < CACHE_IDLE_DAYS * 86400 and total <= CACHE_CAP:
            break
        for p in paths:
            try:
                os.unlink(p)
            except OSError:
                pass
        total -= size
        freed += size
        gone += 1
    if gone:
        refs = set()
        for name in os.listdir(cache):
            if name.endswith('-g.json'):
                try:
                    refs.update(re.findall(r'd-[0-9a-f]+\.tvt', open(os.path.join(cache, name)).read()))
                except OSError:
                    pass
        for name in os.listdir(cache):
            if name.startswith('d-') and name.endswith('.tvt') and name not in refs:
                p = os.path.join(cache, name)
                try:
                    freed += os.stat(p).st_size
                    os.unlink(p)
                except OSError:
                    pass
    return gone, freed


class Baker:
    """Serves the plan: tavtex.txt lists for every mesh the best look the cache holds now (its full
    bake; else the full bake it showed before, while its new one is made; else a preview) and is
    written again whenever that changes; the missing bakes run on the worker threads."""

    def __init__(self, ctx, plan, tav_dir, cache, workers=WORKERS):
        self.ctx, self.plan, self.tav_dir, self.cache = ctx, plan, tav_dir, cache
        # Previews on threads of their own, so a new figure's never wait behind full bakes; at most
        # FULL_JOBS full bakes at once (memory), their shader chunks on the chunk threads.
        self.previews, self.fulls = Workers(workers), Workers(FULL_JOBS)
        self.lock = threading.Lock()
        self.queued = set()                 # cache files a job is making
        self.wanted = set()                 # The bake keys the current plan still needs
        self.changed = threading.Event()    # a job finished: look again
        self.shown = {}                     # mesh key -> the full albedo it showed last
        self.cur = None                     # the plan served
        self.colours, self.colour_stamp = ({}, set()), None
        self.written = None
        self.deferred = 0
        self.workers = workers
        self.touched = set()                # bake keys marked used this run (prune_cache)
        os.makedirs(cache, exist_ok=True)
        gone, freed = prune_cache(cache)
        if gone:
            print(f'tavtex: cache: {gone} bakes not used lately removed ({freed >> 20} MB)', file=sys.stderr, flush=True)

    def touch(self, h):
        """A bake served from the cache is in use: its files' times say so (prune_cache keeps it)."""
        if h in self.touched:
            return
        self.touched.add(h)
        for p in glob.glob(os.path.join(self.cache, h + '*')):
            try:
                os.utime(p)
            except OSError:
                pass

    def log(self, msg):
        t = time.time() - self.cur['t0'] if self.cur else 0.0
        print(f'tavtex: {time.strftime("%H:%M:%S")} plan {self.cur["seq"] if self.cur else "-"} +{t:.2f}s {msg}',
              file=sys.stderr, flush=True)

    def load_plan(self):
        """Reads the plan when its file changed; True when it did."""
        st = os.stat(self.plan)
        stamp = (st.st_mtime_ns, st.st_size)
        if self.cur and self.cur['stamp'] == stamp:
            return False
        r = read_plan(self.plan)
        seq, meshes, cc, cbs = r[:4]
        new = not self.cur or self.cur['seq'] != seq
        self.cur = {'stamp': stamp, 'seq': seq, 'meshes': meshes, 'cc': cc, 'cbs': cbs,
                    'visuals': r[4] if len(r) > 4 else {}, 't0': time.time() if new or not self.cur else self.cur['t0'],
                    'mtime': st.st_mtime, 'first': None, 'whole': None, 'cuts': None}
        return True

    def refresh_colours(self):
        """The mean vertex colours, again when the layer wrote a new tavmesh.bin."""
        try:
            st = os.stat(os.path.join(self.tav_dir, 'tavmesh.bin'))
            stamp = (st.st_mtime_ns, st.st_size)
        except OSError:
            stamp = None
        if stamp == self.colour_stamp:
            return False
        self.colour_stamp = stamp
        self.colours = mesh_colours(self.tav_dir, self.cur['meshes']) if stamp else ({}, set())
        return True

    def cut_suffix(self, fig, mat):
        """benilla 0036's vertex cut token (cut:<bytes>: the figure's BG3 mask bytes) on the meshes whose
        material cuts; '' without the slot code."""
        if not hasattr(self.ctx, 'visual_slots') or 'cuts_vertices' not in globals():
            return ''
        c = self.cur
        if c['cuts'] is None:
            c['cuts'] = {}
            for f, vs in c['visuals'].items():
                mb = self.ctx.mask_bytes()
                cut = sorted({mb[n] for v in vs for n in self.ctx.visual_slots(v) if n in mb})
                c['cuts'][f] = (' cut:' + ','.join(str(b) for b in cut)) if cut else ''
        col = c['cuts'].get(fig, '')
        return col if col and cuts_vertices(self.ctx, mat) else ''

    def final(self, full, h, mat):
        """The full bake, or its calibration toward BG3's G-buffer (tavcalib.py, WOWBG3_TAVCALIB=1)."""
        if os.environ.get('WOWBG3_TAVCALIB') != '1':
            return full
        calp = os.path.join(os.path.dirname(os.path.abspath(self.cache)), 'calib', f'{mat}.npz')
        if not os.path.exists(calp):
            return full
        ch = hashlib.sha1(open(calp, 'rb').read()).hexdigest()[:12]
        calibrated = os.path.join(self.cache, f'{h}-c{ch}.tvt')
        if not os.path.exists(calibrated):
            try:
                b = open(full, 'rb').read()
                _, tw, th, fl = struct.unpack_from('<4sIII', b)
                base = np.frombuffer(b, np.uint8, tw * th * 4, 16).reshape(th, tw, 4)
                write_tvt(calibrated, calibrate(base, np.load(calp)), fl & ~8)
                self.log(f'calibrated {mat} -> {h}-c{ch}')
            except Exception as e:  # noqa: BLE001
                self.log(f'calibrating {mat}: {e}')
                return full
        return calibrated

    def look(self, key, mat, fig):
        """(tavtex.txt line or None, [(job, preview)], why it waits or None)."""
        c, ctx = self.cur, self.ctx
        m = ctx.ix['materials'].get(mat) or {}
        src = os.path.basename(m.get('SourceFile', ''))
        # Effects attached to her visual (replayed with BG3's effects, fx-replay-format.md), cloth
        # proxies, tear lines, lash shadows and materials BG3 never draws.
        if (src.startswith('VFX_') or mat in ENGINE_DEFAULT or src.startswith(HIDDEN_SHADERS) or 'Tearline' in src
                or 'LashesShadow' in src):
            return f'{key} - 2 - -', [], None
        vtn = ctx.vt_note(mat) if m else ''  # '' unless its virtual texture layers stop above level 0
        if src.startswith('CHAR_Pubes'):
            vtn += ' alpha'  # Now alpha-tested (was baked opaque)
        hm = hashlib.sha1(f'{mat} maps v1{vtn}'.encode()).hexdigest()[:16]
        nrm, srf = os.path.join(self.cache, hm + '-n.tvt'), os.path.join(self.cache, hm + '-s.tvt')

        def line_for(albedo):
            # The G-buffer maps of that albedo's bake (its key h), once made; else the raw maps.
            g = self.gmaps(self.base_key(albedo))
            n_path, s_path = (g['normal'], g['surface']) if g else (nrm, srf)
            n_col = os.path.abspath(n_path) if n_path and os.path.exists(n_path) else '-'
            s_col = os.path.abspath(s_path) if s_path and os.path.exists(s_path) else '-'
            sides = ONE_SIDED if m and ctx.one_sided(mat) else 0  # BG3 culls its back faces
            return (f'{key} {os.path.abspath(albedo)} {tvt_flags(albedo) | sides} {n_col} {s_col}{self.cut_suffix(fig, mat)}'
                    f'{self.gtokens(g)}')
        old = self.shown.get(key)
        old = old if old and os.path.exists(old) else None
        young = time.time() - c['mtime']
        cb = c['cbs'].get(key)
        if cb is None and m and young < K_GRACE:
            return (line_for(old), [], None) if old else (None, [], 'K lines')
        ccf = c['cc'].get(fig, c['cc'].get(0, {}))  # this figure's character creation presets
        colour = None
        if m and uses_vertex_colour(ctx, mat):
            colour = self.colours[0].get((mat, fig))
            if colour is None and int(key, 16) not in self.colours[1] and young < MESH_GRACE:
                return (line_for(old), [], None) if old else (None, [], 'mesh copy')
        cbh = hashlib.sha1(cb).hexdigest()[:16] if cb else '-'
        col = ','.join(f'{x:.2f}' for x in colour) if colour is not None else '-'
        h = hashlib.sha1(f'{mat} {ccf.get("skin")} {ccf.get("hair")} {ccf.get("eyes")} {cbh} {col} {BAKE_VERSION}{vtn}'.encode()).hexdigest()[:16]
        full = os.path.join(self.cache, h + '.tvt')
        prev = os.path.join(self.cache, f'{h}-p{PREVIEW_SIZE}.tvt')
        job = (h, mat, ccf, cb, colour, full, prev, nrm, srf)
        if os.path.exists(full):
            self.touch(h)
            albedo = self.final(full, h, mat)
            # The full albedo shows at once; its G-buffer maps follow (job 'g'), the mesh showing the
            # maps it had (the old bake's, or the raw ones) meanwhile.
            g_ready = os.path.exists(os.path.join(self.cache, h + '-g.json'))
            if g_ready or not old or self.gmaps(self.base_key(old)) is None:
                self.shown[key] = albedo
            else:
                albedo = old
            jobs = [] if g_ready else [(job, 'g')]
        else:
            jobs = [(job, 'full')]
            albedo = old or (prev if os.path.exists(prev) else None)
            if albedo is None:
                jobs.append((job, 'preview'))
        if albedo is None:
            return None, jobs, 'bake'
        return line_for(albedo), jobs, None

    @staticmethod
    def base_key(albedo):
        """The bake key h of an albedo file (h.tvt, h-p256.tvt, h-c<calibration>.tvt)."""
        return os.path.basename(albedo).split('.')[0].split('-')[0]

    def gmaps(self, h):
        """A full bake's G-buffer maps (h-g.json: model, normal, surface, extra, detail, profile; file names
        relative to the cache), or None."""
        path = os.path.join(self.cache, h + '-g.json')
        try:
            st = os.stat(path)
        except OSError:
            return None
        memo = self.__dict__.setdefault('_gmemo', {})
        k = (path, st.st_mtime_ns)
        if k not in memo:
            try:
                g = json.load(open(path))
            except Exception:  # noqa: BLE001
                return None
            # Every map lives in the cache (the oldest files carry paths: their names count).
            for f in ('normal', 'surface', 'extra'):
                if g.get(f):
                    g[f] = os.path.join(self.cache, os.path.basename(g[f]))
            if g.get('detail'):
                g['detail'] = [os.path.join(self.cache, os.path.basename(g['detail'][0])), g['detail'][1]]
            memo[k] = g
        return memo[k]

    def gtokens(self, g):
        """benilla 0037's tokens for a bake's G-buffer maps ('' without them or without a model)."""
        if not g or g.get('model') is None:
            return ''
        out = f' model:{g["model"]}'
        if g.get('extra') and os.path.exists(g['extra']):
            out += f' extra:{os.path.abspath(g["extra"])}'
        if g.get('detail') and os.path.exists(g['detail'][0]):
            out += f' detail:{os.path.abspath(g["detail"][0])},{g["detail"][1]:.4g}'
        if g.get('profile'):
            out += ' ' + profile_token(g['profile'])
        return out

    def submit(self, job, kind):
        """Queues a bake: kind 'preview' (preview threads), 'full' (the albedo) or 'g' (its G-buffer maps, after
        every waiting full bake)."""
        h, mat, ccf, cb, colour, full, prev, nrm, srf = job
        out = {'preview': prev, 'full': full, 'g': os.path.join(self.cache, h + '-g.json')}[kind]
        with self.lock:
            if out in self.queued:
                return
            self.queued.add(out)

        def run():
            try:
                if h in self.wanted:  # Dropped when the current plan no longer needs it
                    self.make(job, kind)
            finally:
                with self.lock:
                    self.queued.discard(out)
                self.changed.set()
        if kind == 'preview':
            self.previews.submit(0, run)
        else:
            self.fulls.submit(0 if kind == 'full' else 1, run)

    def make(self, job, kind):
        h, mat, ccf, cb, colour, full, prev, nrm, srf = job
        preview = kind == 'preview'
        out, size = (prev, PREVIEW_SIZE) if preview else (full, MAX_SIZE)
        while not preview and not self.previews.idle():
            time.sleep(0.05)  # previews first: they make a new figure show
        if kind == 'g':
            self.make_gmaps(job)
        elif not os.path.exists(out):
            t0 = time.time()
            try:
                r = bake(self.ctx, mat, ccf, cb, colour, size=size)
                img, flags, mips = r[0], r[1], (r[2] if len(r) > 2 else [])
            except Exception as e:  # noqa: BLE001
                self.log(f'{mat}: {type(e).__name__}: {e}')
                img, flags, mips = np.full((4, 4, 4), (128, 128, 128, 255), np.uint8), 0, []
            if img is None:
                img, mips = np.zeros((1, 1, 4), np.uint8), []
            write_tvt(out, img, flags, mips)
            self.log(f'{"preview" if preview else "baked"} {mat} -> {os.path.basename(out)} {img.shape[1]}x{img.shape[0]} '
                     f'flags {flags} ({"BG3 constants" if cb else "built constants"}) in {time.time() - t0:.2f}s')

    def make_gmaps(self, job):
        """BG3's G-buffer outputs of a full bake (its inputs: material, presets, constant buffer, colour)."""
        h, mat, ccf, cb, colour, full, prev, nrm, srf = job
        gpath = os.path.join(self.cache, h + '-g.json')
        if not os.path.exists(gpath):
            t0 = time.time()
            g = None
            try:
                g = bake_gbuffer(self.ctx, mat, ccf, cb, colour, size=MAX_SIZE)
            except Exception as e:  # noqa: BLE001
                self.log(f'G-buffer maps {mat}: {type(e).__name__}: {e}')
            if g is not None:
                gn, gs, gx = (os.path.join(self.cache, f'{h}-{x}.tvt') for x in ('n', 's', 'x'))
                write_tvt(gn, g['normal'], 4)
                write_tvt(gs, g['surface'], 4)
                if g['extra'] is not None:
                    write_tvt(gx, g['extra'], 4)
                det = None
                if g['detail']:
                    src, tiling = g['detail']
                    dpath = os.path.join(self.cache, 'd-' + hashlib.sha1(src.encode()).hexdigest()[:16] + '.tvt')
                    if not os.path.exists(dpath):
                        dimg = detail_tvt(self.ctx, src)
                        if dimg is not None:
                            write_tvt(dpath, dimg, 4)
                    det = [dpath, tiling] if os.path.exists(dpath) else None
                b = os.path.basename
                meta = {'model': g['model'], 'bg3_model': g['bg3_model'], 'normal': b(gn), 'surface': b(gs),
                        'extra': b(gx) if g['extra'] is not None else None,
                        'detail': [b(det[0]), det[1]] if det else None, 'profile': g['profile'], 'material': mat}
            else:
                # The shader cannot run: the raw maps as before, without a model (benilla keeps WoW's law).
                if not os.path.exists(srf):
                    try:
                        n_img, s_img = bake_maps(self.ctx, mat)
                    except Exception as e:  # noqa: BLE001
                        self.log(f'maps {mat}: {e}')
                        n_img, s_img = None, np.full((4, 4, 4), (255, 153, 0, 255), np.uint8)
                    if n_img is not None:
                        write_tvt(nrm, n_img, 4)
                    write_tvt(srf, s_img, 4)
                meta = {'model': None, 'normal': os.path.basename(nrm), 'surface': os.path.basename(srf), 'extra': None,
                        'detail': None, 'profile': None, 'material': mat}
            with open(gpath + '.tmp', 'w') as f:
                json.dump(meta, f)
            os.replace(gpath + '.tmp', gpath)
            self.log(f'G-buffer maps {mat} -> {h} model {meta["model"]} detail {meta["detail"][1] if meta["detail"] else "-"} '
                     f'profile {"yes" if meta["profile"] else "-"} in {time.time() - t0:.2f}s')

    def serve_once(self):
        """Looks at every mesh of the plan, queues what is missing, writes tavtex.txt when its text
        changed; returns (meshes listed, full bakes still missing, meshes waiting)."""
        c = self.cur
        lines, missing, gmissing, waiting, prevs, queue = [f'TAVTEX 1 {c["seq"]}'], 0, 0, {}, 0, []
        for key, mat, fig, *_ in c['meshes']:
            line, jobs, why = self.look(key, mat, fig)
            queue += jobs
            missing += any(k == 'full' for _, k in jobs)
            gmissing += any(k == 'g' for _, k in jobs)
            if line is None:
                waiting[why] = waiting.get(why, 0) + 1
            else:
                lines.append(line)
                prevs += f'-p{PREVIEW_SIZE}.tvt' in line
        self.wanted = {job[0] for job, _ in queue}
        order = {'preview': 0, 'full': 1, 'g': 2}
        for job, kind in sorted(queue, key=lambda j: order[j[1]]):  # previews, then full bakes, then G-buffer maps
            self.submit(job, kind)
        self.deferred = waiting.get('K lines', 0) + waiting.get('mesh copy', 0)
        body = '\n'.join(lines) + '\n'
        if body != self.written:
            out = os.path.join(self.tav_dir, 'tavtex.txt')
            with open(out + '.tmp', 'w') as f:
                f.write(body)
            os.replace(out + '.tmp', out)
            self.written = body
            wait = ', '.join(f'{n} wait for {w}' for w, n in waiting.items())
            self.log(f'tavtex.txt: {len(lines) - 1} of {len(c["meshes"])} meshes ({prevs} previews){", " + wait if wait else ""}, '
                     f'{missing} full bakes to go, {gmissing} G-buffer maps to go')
        if not missing and not waiting and c['whole'] is None:
            c['whole'] = time.time()
            self.log('every mesh has its full bake')
        if not missing and not gmissing and not waiting and c.get('gwhole') is None:
            c['gwhole'] = time.time()
            self.log('every mesh has its G-buffer maps')
        return len(lines) - 1, missing + gmissing, sum(waiting.values())

    def serve(self, once=False):
        self.log(f'serving {self.plan}: {self.workers} preview threads, {FULL_JOBS} full bakes at once, '
                 f'{CHUNK_THREADS} shader chunk threads')
        last = 0.0
        while True:
            look = False
            try:
                if self.load_plan():
                    look = True
                    self.colour_stamp = None
            except FileNotFoundError:
                pass
            if self.cur and self.cur['seq']:
                look |= self.refresh_colours()
                if self.changed.is_set():
                    self.changed.clear()
                    look = True
                if self.deferred and time.time() - last > 0.25:
                    look = True
                if look:
                    last = time.time()
                    listed, missing, waiting = self.serve_once()
                    if once and not missing and not waiting and self.previews.idle() and self.fulls.idle():
                        return
            elif once:
                return
            self.changed.wait(0.1)


def main():
    args = sys.argv[1:]
    once = args and args[0] == '--once'
    if once:
        args = args[1:]
    plan, data, index, tav_dir, cache = args
    Baker(Ctx(data, index), plan, tav_dir, cache).serve(once)


if __name__ == '__main__':
    main()
