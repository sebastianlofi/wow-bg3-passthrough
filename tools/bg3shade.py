#!/usr/bin/env python3
"""BG3's own material shaders, run on the CPU over a texture (Tav's colours).

BG3 builds a character's base colour in the fragment shader of each material (skin from HMVY and
CLEA masks and the skin preset, armour from a colour mask that picks one of twelve preset colours,
hair from its masks and the hair colour, and so on). Rather than re-deriving each formula, this
loads the material's compiled G-buffer shader from Materials.pak (<Shader>_ST_DEF_Vulkan.bshd,
SPIR-V) and evaluates it with numpy at every texel of the mesh's UV space: the G-buffer base
colour (fragment output location 1) becomes the baked albedo, in linear values as BG3 writes it.

What the shader reads, and what it gets here:
  material constant buffer (set 1 binding 1)   the bytes BG3 renders with (TavPlan.lua K lines), or
                                               one built from the shader's parameter offsets
                                               (<Shader>.lsf) and the material's values
  2D textures (set 1)                          the material's textures by binding (the shader's
                                               Texture2DParameters order), sRGB decoded when the
                                               texture resource says so
  virtual texture caches Cache0/1/2           the material's virtual texture layers 0/1/2 at the
                                               texel's UV (layer 0 is sRGB); the tile addressing
                                               the shader does around them is not evaluated
  tile set records (storage buffer)            2202.0, the value BG3's shaders test for a resident
                                               virtual texture
  other inputs                                 interpolants: uv0 (every vec2 input), normals
                                               (0, 0, 1), vertex colour (the caller's, e.g. the
                                               mesh's mean, else 1); engine and view constants 0;
                                               derivatives 0; front facing; discards ignored

Only instructions the output depends on are evaluated (straight-line SSA; an if/else becomes a
select on its condition), in chunks of texels to bound memory.

Several outputs at once (tavtex's G-buffer maps): Shader(spirv, outputs=[0, 1, 2, 3])
plans the union of what those outputs need and run_outputs() returns each one. The interpolated frame
can be given per input location (vec3_inputs={location: (x, y, z)}; tbn_roles() of the material's
vertex shader says which location carries BG3's normal N, its tangent X (+U) and bitangent Y (-V)),
and other uniform blocks by (set, binding) (ubos=...; e.g. the view matrix at set 0 binding 2).

  bg3shade.py SHADER.spv   lists the module's interface (bindings, inputs, outputs)"""
import struct, sys

import numpy as np

# ---------------------------------------------------------------- SPIR-V opcodes used here
OP = dict(Undef=1, ExtInstImport=11, ExtInst=12, EntryPoint=15, TypeVoid=19, TypeBool=20, TypeInt=21,
          TypeFloat=22, TypeVector=23, TypeMatrix=24, TypeImage=25, TypeSampler=26, TypeSampledImage=27,
          TypeArray=28, TypeRuntimeArray=29, TypeStruct=30, TypePointer=32, TypeFunction=33, ConstantTrue=41,
          ConstantFalse=42, Constant=43, ConstantComposite=44, ConstantNull=46, Function=54, FunctionEnd=56,
          Variable=59, Load=61, Store=62, AccessChain=65, InBoundsAccessChain=66, Decorate=71,
          MemberDecorate=72, VectorShuffle=79, CompositeConstruct=80, CompositeExtract=81,
          CompositeInsert=82, CopyObject=83, SampledImage=86, ImageSampleImplicitLod=87,
          ImageSampleExplicitLod=88, ImageSampleDrefImplicitLod=89, ImageSampleDrefExplicitLod=90,
          ImageFetch=95, Image=100, ConvertFToU=109, ConvertFToS=110, ConvertSToF=111, ConvertUToF=112,
          UConvert=113, SConvert=114, FConvert=115, Bitcast=124, SNegate=126, FNegate=127, IAdd=128,
          FAdd=129, ISub=130, FSub=131, IMul=132, FMul=133, UDiv=134, SDiv=135, FDiv=136, UMod=137,
          SRem=138, SMod=139, FRem=140, FMod=141, VectorTimesScalar=142, MatrixTimesScalar=143,
          VectorTimesMatrix=144, MatrixTimesVector=145, MatrixTimesMatrix=146, Dot=148, Any=154, All=155,
          IsNan=156, IsInf=157, LogicalEqual=164, LogicalNotEqual=165, LogicalOr=166, LogicalAnd=167,
          LogicalNot=168, Select=169, IEqual=170, INotEqual=171, UGreaterThan=172, SGreaterThan=173,
          UGreaterThanEqual=174, SGreaterThanEqual=175, ULessThan=176, SLessThan=177, ULessThanEqual=178,
          SLessThanEqual=179, FOrdEqual=180, FUnordEqual=181, FOrdNotEqual=182, FUnordNotEqual=183,
          FOrdLessThan=184, FUnordLessThan=185, FOrdGreaterThan=186, FUnordGreaterThan=187,
          FOrdLessThanEqual=188, FUnordLessThanEqual=189, FOrdGreaterThanEqual=190,
          FUnordGreaterThanEqual=191, ShiftRightLogical=194, ShiftRightArithmetic=195, ShiftLeftLogical=196,
          BitwiseOr=197, BitwiseXor=198, BitwiseAnd=199, Not=200, DPdx=207, DPdy=208, Fwidth=209,
          DPdxFine=210, DPdyFine=211, FwidthFine=212, DPdxCoarse=213, DPdyCoarse=214, FwidthCoarse=215,
          Phi=245, LoopMerge=246, SelectionMerge=247, Label=248, Branch=249, BranchConditional=250,
          Kill=252, Return=253, ReturnValue=254, Unreachable=255, DemoteToHelperInvocation=5380)
NAME = {v: k for k, v in OP.items()}
DEC_BUILTIN, DEC_ROWMAJOR, DEC_ARRAYSTRIDE, DEC_MATRIXSTRIDE, DEC_BLOCK, DEC_BUFFERBLOCK = 11, 4, 6, 7, 2, 3
DEC_LOCATION, DEC_BINDING, DEC_SET, DEC_OFFSET = 30, 33, 34, 35
SC_INPUT, SC_UNIFORM, SC_OUTPUT, SC_UNIFORMCONSTANT, SC_PRIVATE, SC_FUNCTION, SC_STORAGE = 1, 2, 3, 0, 6, 7, 12
TILESET_MAGIC = 2202.0  # BG3's shaders sample a virtual texture only where its tile set record says this


class Module:
    """A parsed SPIR-V module: types, constants, decorations, variables, the entry function."""

    def __init__(self, data):
        w = np.frombuffer(data, '<u4')
        if len(w) < 5 or w[0] != 0x07230203:
            raise ValueError('not SPIR-V')
        self.types, self.consts, self.dec, self.mdec, self.vars = {}, {}, {}, {}, {}
        self.ext_glsl = set()
        self._cache = {}
        self.body = []  # (op, result type, result id, operands) of the entry function
        self.execution_model = None
        i, infunc = 5, False
        n = len(w)
        while i < n:
            wc, op = int(w[i]) >> 16, int(w[i]) & 0xffff
            if wc == 0:
                raise ValueError('bad instruction')
            ops = [int(x) for x in w[i + 1:i + wc]]
            i += wc
            if op == OP['ExtInstImport']:
                name = _string(ops[1:])
                if name.startswith('GLSL.std.450'):
                    self.ext_glsl.add(ops[0])
            elif op == OP['EntryPoint']:
                self.execution_model = ops[0]
            elif op == OP['Decorate']:
                self.dec.setdefault(ops[0], {})[ops[1]] = ops[2:]
            elif op == OP['MemberDecorate']:
                self.mdec.setdefault((ops[0], ops[1]), {})[ops[2]] = ops[3:]
            elif op == OP['TypeVoid']:
                self.types[ops[0]] = ('void',)
            elif op == OP['TypeBool']:
                self.types[ops[0]] = ('bool',)
            elif op == OP['TypeInt']:
                self.types[ops[0]] = ('int', ops[1], ops[2])
            elif op == OP['TypeFloat']:
                self.types[ops[0]] = ('float', ops[1])
            elif op == OP['TypeVector']:
                self.types[ops[0]] = ('vec', ops[1], ops[2])
            elif op == OP['TypeMatrix']:
                self.types[ops[0]] = ('mat', ops[1], ops[2])
            elif op == OP['TypeImage']:
                self.types[ops[0]] = ('image', ops[2], ops[4])  # dim, arrayed
            elif op == OP['TypeSampler']:
                self.types[ops[0]] = ('sampler',)
            elif op == OP['TypeSampledImage']:
                self.types[ops[0]] = ('sampledimage', ops[1])
            elif op == OP['TypeArray']:
                self.types[ops[0]] = ('array', ops[1], ops[2])
            elif op == OP['TypeRuntimeArray']:
                self.types[ops[0]] = ('rtarray', ops[1])
            elif op == OP['TypeStruct']:
                self.types[ops[0]] = ('struct', ops[1:])
            elif op == OP['TypePointer']:
                self.types[ops[0]] = ('ptr', ops[1], ops[2])
            elif op == OP['TypeFunction']:
                self.types[ops[0]] = ('func',)
            elif op in (OP['Constant'], OP['ConstantTrue'], OP['ConstantFalse'], OP['ConstantNull'],
                        OP['ConstantComposite'], OP['Undef']) and not infunc:
                self.consts[ops[1]] = (op, ops[0], ops[2:])
            elif op == OP['Variable'] and not infunc:
                self.vars[ops[1]] = (ops[0], ops[2])
            elif op == OP['Function']:
                infunc = True
            elif op == OP['FunctionEnd']:
                infunc = False
            elif infunc:
                self.body.append((op, ops))

    # ---- type helpers
    def scalar_kind(self, t):
        ty = self.types[t]
        if ty[0] == 'vec':
            return self.scalar_kind(ty[1])
        return ty[0]

    def ncomp(self, t):
        ty = self.types[t]
        return ty[2] if ty[0] == 'vec' else 1

    def dtype(self, t):
        k = self.types[t]
        if k[0] == 'vec':
            k = self.types[k[1]]
        if k[0] == 'float':
            return np.float32
        if k[0] == 'bool':
            return np.bool_
        if k[0] == 'int':
            return np.int64 if k[2] else np.uint32
        return np.float32

    def const_value(self, cid):
        c = self._cache.get(cid)
        if c is None:
            c = self._cache[cid] = self._const_value(cid)
        return c

    def _const_value(self, cid):
        op, t, ops = self.consts[cid]
        ty = self.types[t]
        if op in (OP['ConstantNull'], OP['Undef']):
            return self.zero(t)
        if op == OP['ConstantTrue']:
            return np.ones((1, 1), bool)
        if op == OP['ConstantFalse']:
            return np.zeros((1, 1), bool)
        if op == OP['Constant']:
            if ty[0] == 'float':
                return np.array([[struct.unpack('<f', struct.pack('<I', ops[0]))[0]]], np.float32)
            if ty[0] == 'int':
                v = ops[0]
                if ty[2] and v >= 1 << 31:
                    v -= 1 << 32
                return np.array([[v]], self.dtype(t))
        if op == OP['ConstantComposite']:
            return self.compose(t, [self.const_value(c) for c in ops])
        raise ValueError(f'constant {op}')

    def zero(self, t):
        ty = self.types[t]
        if ty[0] in ('float', 'int', 'bool'):
            return np.zeros((1, 1), self.dtype(t))
        if ty[0] == 'vec':
            return np.zeros((1, ty[2]), self.dtype(t))
        if ty[0] == 'mat':
            return np.zeros((1, ty[2], self.ncomp(ty[1])), np.float32)
        if ty[0] == 'array':
            return [self.zero(ty[1]) for _ in range(int(self.const_value(ty[2])[0, 0]))]
        if ty[0] == 'struct':
            return [self.zero(m) for m in ty[1]]
        return None

    def fill(self, t, v):
        """A value of type t with every float set to v (storage buffer stand-in)."""
        ty = self.types[t]
        if ty[0] in ('float', 'vec', 'mat'):
            z = self.zero(t)
            return np.full_like(z, v) if z.dtype == np.float32 else z
        if ty[0] in ('int', 'bool'):
            return self.zero(t)
        if ty[0] == 'array':
            return [self.fill(ty[1], v) for _ in range(int(self.const_value(ty[2])[0, 0]))]
        if ty[0] == 'rtarray':
            return _Repeat(self.fill(ty[1], v))
        if ty[0] == 'struct':
            return [self.fill(m, v) for m in ty[1]]
        return None

    def compose(self, t, parts):
        ty = self.types[t]
        if ty[0] == 'vec':
            p = max(x.shape[0] for x in parts)
            return np.concatenate([np.broadcast_to(x, (p, x.shape[1])) for x in parts], 1).astype(self.dtype(t))
        if ty[0] == 'mat':
            p = max(x.shape[0] for x in parts)
            return np.stack([np.broadcast_to(x, (p, x.shape[1])) for x in parts], 1)
        return list(parts)

    def decode(self, t, buf, off, row_major=False, mstride=16):
        """A uniform block member of type t at byte offset off of buf (std140 / std430 decorations)."""
        ty = self.types[t]
        if ty[0] == 'float':
            return np.array([[_f32(buf, off)]], np.float32)
        if ty[0] == 'int':
            v = _u32(buf, off)
            if ty[2] and v >= 1 << 31:
                v -= 1 << 32
            return np.array([[v]], self.dtype(t))
        if ty[0] == 'bool':
            return np.array([[_u32(buf, off) != 0]])
        if ty[0] == 'vec':
            k = self.scalar_kind(t)
            vals = [self.decode(ty[1], buf, off + 4 * i)[0, 0] for i in range(ty[2])]
            return np.array([vals], self.dtype(t) if k != 'bool' else bool)
        if ty[0] == 'mat':
            cols, rows = ty[2], self.ncomp(ty[1])
            m = np.zeros((1, cols, rows), np.float32)
            for c in range(cols):
                for r in range(rows):
                    o = off + (r * mstride + 4 * c if row_major else c * mstride + 4 * r)
                    m[0, c, r] = _f32(buf, o)
            return m
        if ty[0] == 'array':
            n = int(self.const_value(ty[2])[0, 0])
            stride = self.dec.get(t, {}).get(DEC_ARRAYSTRIDE, [16])[0]
            return [self.decode(ty[1], buf, off + i * stride, row_major, mstride) for i in range(n)]
        if ty[0] == 'struct':
            out = []
            for i, m in enumerate(ty[1]):
                d = self.mdec.get((t, i), {})
                o = d.get(DEC_OFFSET, [0])[0]
                out.append(self.decode(m, buf, off + o, DEC_ROWMAJOR in d, d.get(DEC_MATRIXSTRIDE, [16])[0]))
            return out
        return None

    def interface(self):
        """{'inputs': {location: (var, type)}, 'outputs': {location: var}, 'resources': {(set, binding):
        (var, kind)}, 'builtins': {var: builtin}}."""
        r = {'inputs': {}, 'outputs': {}, 'resources': {}, 'builtins': {}}
        for v, (pt, sc) in self.vars.items():
            t = self.types[pt][2]
            d = self.dec.get(v, {})
            if DEC_BUILTIN in d:
                r['builtins'][v] = d[DEC_BUILTIN][0]
            elif sc == SC_INPUT and DEC_LOCATION in d:
                r['inputs'][d[DEC_LOCATION][0]] = (v, t)
            elif sc == SC_OUTPUT and DEC_LOCATION in d:
                r['outputs'][d[DEC_LOCATION][0]] = v
            elif DEC_BINDING in d:
                kind = self.types[t][0]
                if kind == 'struct':
                    block = self.dec.get(t, {})
                    kind = 'ssbo' if sc == SC_STORAGE or DEC_BUFFERBLOCK in block else 'ubo'
                elif kind == 'image':
                    kind = 'image_array' if self.types[t][2] else 'image'
                r['resources'][(d.get(DEC_SET, [0])[0], d[DEC_BINDING][0])] = (v, kind)
        return r


class _Repeat:
    """A runtime array whose every element is the same value (the tile set stand-in)."""

    def __init__(self, v):
        self.v = v

    def __getitem__(self, i):
        return self.v


def _string(words):
    b = struct.pack(f'<{len(words)}I', *words)
    return b.split(b'\0', 1)[0].decode('utf-8', 'replace')


def _f32(buf, off):
    return struct.unpack_from('<f', buf, off)[0] if off + 4 <= len(buf) else 0.0


def _u32(buf, off):
    return struct.unpack_from('<I', buf, off)[0] if off + 4 <= len(buf) else 0


def sample(img, uv):
    """Bilinear, wrapping lookup: img (h, w, c) float32, uv (p, 2) -> (p, c)."""
    h, w = img.shape[:2]
    if h == 1 and w == 1:
        return np.broadcast_to(img[0, 0], (uv.shape[0], img.shape[2])).astype(np.float32)
    x = uv[:, 0].astype(np.float32) * w - 0.5
    y = uv[:, 1].astype(np.float32) * h - 0.5
    x = np.nan_to_num(x, nan=0.0, posinf=0.0, neginf=0.0)
    y = np.nan_to_num(y, nan=0.0, posinf=0.0, neginf=0.0)
    x0, y0 = np.floor(x), np.floor(y)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    x0 = x0.astype(np.int64) % w
    y0 = y0.astype(np.int64) % h
    x1, y1 = (x0 + 1) % w, (y0 + 1) % h
    a, b, c, d = img[y0, x0], img[y0, x1], img[y1, x0], img[y1, x1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


class Shader:
    """A fragment shader ready to evaluate one output."""

    def __init__(self, spirv, output_location=1, outputs=None):
        self.m = Module(spirv)
        if self.m.execution_model != 4:
            raise ValueError('not a fragment shader')
        self.io = self.m.interface()
        locs = list(outputs) if outputs else [output_location]
        missing = [loc for loc in locs if loc not in self.io['outputs']]
        if missing and not outputs:
            raise ValueError(f'no output at location {output_location}')
        self.out_locs = [loc for loc in locs if loc in self.io['outputs']]
        self.out_vars = {loc: self.io['outputs'][loc] for loc in self.out_locs}
        self.out_var = self.out_vars[self.out_locs[0]]
        self.uv_location = min((loc for loc, (v, t) in self.io['inputs'].items()
                                if self.m.types[t][0] == 'vec' and self.m.types[t][2] == 2), default=None)
        self._plan()

    def _plan(self):
        """The instructions the output depends on, in program order, and when each value dies."""
        m, body = self.m, self.m.body
        defs, stores = {}, {}
        for k, (op, ops) in enumerate(body):
            rid = _result_id(op, ops)
            if rid is not None:
                defs[rid] = k
            if op == OP['Store']:
                stores.setdefault(self._root(ops[0], defs), []).append(k)
        self.defs = defs
        need, work = set(), []

        def want(i):
            if i in defs and defs[i] not in need:
                need.add(defs[i])
                work.append(defs[i])
        for var in self.out_vars.values():
            for k in stores.get(var, []):
                need.add(k)
                work.append(k)
        # branch conditions feed phis
        for k, (op, ops) in enumerate(body):
            if op == OP['BranchConditional']:
                need.add(k)
                work.append(k)
        while work:
            k = work.pop()
            op, ops = body[k]
            for i in _operand_ids(op, ops, self):
                want(i)
                if i in m.vars or i in defs:
                    root = self._root(i, defs)
                    if root in m.vars and m.vars[root][1] in (SC_PRIVATE, SC_FUNCTION):
                        for s in stores.get(root, []):
                            if s not in need:
                                need.add(s)
                                work.append(s)
        self.order = sorted(need | {k for k, (op, ops) in enumerate(body) if op in (
            OP['Label'], OP['Branch'], OP['Variable'])})
        last = {}
        for k in self.order:
            op, ops = body[k]
            for i in _operand_ids(op, ops, self):
                last[i] = k
        self.last_use = last

    def _root(self, i, defs):
        """The variable a pointer id is rooted at."""
        seen = 0
        while i in defs and seen < 64:
            op, ops = self.m.body[defs[i]]
            if op in (OP['AccessChain'], OP['InBoundsAccessChain'], OP['CopyObject']):
                i = ops[2]
                seen += 1
            else:
                break
        return i

    def used_resources(self):
        """(set, binding) of the images the output reads."""
        out = set()
        by_var = {v: key for key, (v, kind) in self.io['resources'].items()}
        for k in self.order:
            op, ops = self.m.body[k]
            if op == OP['Load']:
                root = self._root(ops[2], self.defs)
                if root in by_var and self.io['resources'][by_var[root]][1] in ('image', 'image_array'):
                    out.add(by_var[root])
        return out

    def interface_summary(self):
        lines = [f'uv input: location {self.uv_location}']
        for loc, (v, t) in sorted(self.io['inputs'].items()):
            lines.append(f'input {loc}: {self.m.types[t]}')
        for (s, b), (v, kind) in sorted(self.io['resources'].items()):
            lines.append(f'set {s} binding {b}: {kind}')
        lines.append(f'{len(self.order)} of {len(self.m.body)} instructions needed for the output')
        return '\n'.join(lines)

    # ---- evaluation
    def run_outputs(self, uv, cb=b'', textures=None, vt=None, colour=None, chunk=1 << 18, ubos=None,
                    vec3_inputs=None, probe=None):
        """{location: (p, 4)} for every planned output (Shader(..., outputs=[...])). probe: {(set, binding): None}
        gets the coordinates the first sample of each of those textures was taken at (first chunk)."""
        parts = {loc: [] for loc in self.out_locs}
        for s in range(0, len(uv), chunk):
            r = self._run(uv[s:s + chunk], cb, textures or {}, vt or {}, colour, ubos, vec3_inputs, multi=True,
                          probe=probe)
            for loc in self.out_locs:
                parts[loc].append(r[loc])
        return {loc: np.concatenate(v, 0) for loc, v in parts.items()}

    def run(self, uv, cb=b'', textures=None, vt=None, colour=None, chunk=1 << 18):
        """The output (p, 4) at uv (p, 2). cb: material constant buffer bytes (set 1 binding 1);
        textures: {(set, binding): float32 (h, w, 4) image, linear}; vt: {(set, binding): image}
        for virtual texture caches (sampled at uv whatever coordinates the shader computes);
        colour: the vertex colour (RGBA, 0..1) for vec4 interpolants (default 1). Other vec2
        interpolants (a second UV set) get uv too."""
        out = []
        for s in range(0, len(uv), chunk):
            out.append(self._run(uv[s:s + chunk], cb, textures or {}, vt or {}, colour))
        return np.concatenate(out, 0)

    def _run(self, uv, cb, textures, vt, colour=None, ubos=None, vec3_inputs=None, multi=False, probe=None):
        m, body = self.m, self.m.body
        p = len(uv)
        val = {}
        mem = {}  # variable -> current value (inputs, uniforms, private and function variables)
        res = self.io['resources']
        by_var = {v: key for key, (v, kind) in res.items()}
        for loc, (v, t) in self.io['inputs'].items():
            ty = m.types[t]
            n = ty[2] if ty[0] == 'vec' else 1
            if loc == self.uv_location or n == 2:
                mem[v] = uv.astype(np.float32)
            elif n == 3:
                mem[v] = np.array([(vec3_inputs or {}).get(loc, (0.0, 0.0, 1.0))], np.float32)
            elif n == 4 and colour is not None:
                mem[v] = np.asarray(colour, np.float32).reshape(1, 4)
            else:
                mem[v] = np.ones((1, n), np.float32)
        for v, b in self.io['builtins'].items():
            if b == 17:  # FrontFacing
                mem[v] = np.ones((1, 1), bool)
            elif b == 15:  # FragCoord
                mem[v] = np.array([[0.5, 0.5, 0.5, 1.0]], np.float32)
        for (s, b), (v, kind) in res.items():
            t = m.types[m.vars[v][0]][2]
            if kind == 'ubo':
                mem[v] = m.decode(t, cb if (s, b) == (1, 1) else (ubos or {}).get((s, b), b''), 0)
            elif kind == 'ssbo':
                mem[v] = m.fill(t, TILESET_MAGIC)
            else:
                mem[v] = ('res', (s, b), kind)
        for v, (pt, sc) in m.vars.items():
            if v not in mem and sc in (SC_PRIVATE, SC_OUTPUT):
                mem[v] = m.zero(m.types[pt][2])
        block, masks, edges, cur = None, {}, {}, None
        for k in self.order:
            op, ops = body[k]
            if op == OP['Label']:
                cur = ops[0]
                masks.setdefault(cur, np.ones((1, 1), bool) if block is None else np.zeros((1, 1), bool))
                block = cur
                continue
            if op == OP['Branch']:
                e = masks.get(cur, np.ones((1, 1), bool))
                edges[(cur, ops[0])] = e
                masks[ops[0]] = masks.get(ops[0], np.zeros((1, 1), bool)) | e
                continue
            if op == OP['BranchConditional']:
                c = val.get(ops[0])
                c = np.ones((1, 1), bool) if c is None else c.astype(bool)
                e = masks.get(cur, np.ones((1, 1), bool))
                t_e, f_e = e & c, e & ~c
                edges[(cur, ops[1])], edges[(cur, ops[2])] = t_e, f_e
                masks[ops[1]] = masks.get(ops[1], np.zeros((1, 1), bool)) | t_e
                masks[ops[2]] = masks.get(ops[2], np.zeros((1, 1), bool)) | f_e
                continue
            if op == OP['Variable']:
                mem[ops[1]] = m.zero(m.types[ops[0]][2]) if len(ops) < 4 else self._v(val, ops[3])
                continue
            if op == OP['Store']:
                self._store(ops[0], self._v(val, ops[1]), mem, val)
                continue
            if op == OP['Phi']:
                pairs = list(zip(ops[2::2], ops[3::2]))
                r = self._v(val, pairs[-1][0])
                for v, parent in pairs[:-1]:
                    e = edges.get((parent, cur), np.zeros((1, 1), bool))
                    r = np.where(e, self._v(val, v), r)
                val[ops[1]] = r
                continue
            r = self._eval(op, ops, val, mem, by_var, textures, vt, uv, p, probe)
            rid = _result_id(op, ops)
            if rid is not None:
                val[rid] = r
            for i in _operand_ids(op, ops, self):
                if self.last_use.get(i) == k and i in val:
                    del val[i]
        if multi:
            return {loc: np.broadcast_to(mem[var], (p, mem[var].shape[1])).astype(np.float32)
                    for loc, var in self.out_vars.items()}
        o = mem[self.out_var]
        return np.broadcast_to(o, (p, o.shape[1])).astype(np.float32)

    def _v(self, val, i):
        if i in val:
            return val[i]
        if i in self.m.consts:
            v = self.m.const_value(i)
            return v
        raise KeyError(f'%{i} not evaluated')

    def _ptr(self, i, val):
        """(variable, [indices]) of a pointer id."""
        if i in self.m.vars:
            return i, []
        r = val.get(i)
        if isinstance(r, tuple) and r and r[0] == 'ptr':
            return r[1], r[2]
        raise KeyError(f'pointer %{i}')

    def _load(self, i, mem, val):
        var, path = self._ptr(i, val)
        x = mem[var]
        for idx in path:
            x = _index(x, idx)
        return x

    def _store(self, i, v, mem, val):
        var, path = self._ptr(i, val)
        if not path:
            mem[var] = v
            return
        mem[var] = _set(mem[var], path, v)

    def _eval(self, op, ops, val, mem, by_var, textures, vt, uv, p, probe=None):
        m = self.m
        V = lambda i: self._v(val, i)  # noqa: E731
        t = ops[0] if op not in (OP['Store'],) else None
        if op in (OP['AccessChain'], OP['InBoundsAccessChain']):
            var, path = self._ptr(ops[2], val)
            idx = []
            for i in ops[3:]:
                if i in m.consts:
                    idx.append(int(m.const_value(i)[0, 0]))
                else:
                    x = val.get(i)
                    idx.append(int(np.asarray(x).flat[0]) if x is not None else 0)
            return ('ptr', var, path + idx)
        if op == OP['Load']:
            return self._load(ops[2], mem, val)
        if op == OP['CopyObject']:
            return V(ops[2])
        if op in (OP['Undef'],):
            return m.zero(t)
        if op == OP['SampledImage']:
            return ('sampled', V(ops[2]) if ops[2] in val else self._load(ops[2], mem, val))
        if op == OP['Image']:
            x = V(ops[2])
            return x[1] if x[0] == 'sampled' else x
        if op in (OP['ImageSampleImplicitLod'], OP['ImageSampleExplicitLod'], OP['ImageFetch']):
            img = val.get(ops[2])
            if isinstance(img, tuple) and img[0] == 'sampled':
                img = img[1]
            key = img[1] if isinstance(img, tuple) and img[0] == 'res' else None
            if op == OP['ImageFetch']:
                return np.zeros((1, 4), m.dtype(t))
            if key in vt:
                return sample(vt[key], uv)
            tex = textures.get(key)
            if tex is None or _is_array_image(self, ops[2]):
                return np.zeros((1, 4), np.float32)
            coords = V(ops[3])
            c = np.broadcast_to(coords, (p, coords.shape[1]))[:, :2]
            if probe is not None and key in probe and probe[key] is None:
                probe[key] = np.array(c)
            return sample(tex, c)
        if op == OP['VectorShuffle']:
            a, b = V(ops[2]), V(ops[3])
            pp = max(a.shape[0], b.shape[0])
            ab = np.concatenate([np.broadcast_to(a, (pp, a.shape[1])), np.broadcast_to(b, (pp, b.shape[1]))], 1)
            sel = [c if c != 0xffffffff else 0 for c in ops[4:]]
            return ab[:, sel]
        if op == OP['CompositeConstruct']:
            return m.compose(t, [V(i) for i in ops[2:]])
        if op == OP['CompositeExtract']:
            return _extract(V(ops[2]), ops[3:])
        if op == OP['CompositeInsert']:
            return _set(V(ops[3]), list(ops[4:]), V(ops[2]))
        if op in (OP['ConvertFToS'], OP['ConvertFToU']):
            x = V(ops[2])
            x = np.nan_to_num(np.trunc(x), nan=0.0, posinf=0.0, neginf=0.0)
            return x.astype(np.int64) if op == OP['ConvertFToS'] else np.clip(x, 0, 2 ** 32 - 1).astype(np.uint32)
        if op in (OP['ConvertSToF'], OP['ConvertUToF'], OP['FConvert']):
            return V(ops[2]).astype(np.float32)
        if op in (OP['UConvert'], OP['SConvert']):
            return V(ops[2]).astype(m.dtype(t))
        if op == OP['Bitcast']:
            x = V(ops[2])
            if m.dtype(t) == np.float32:
                return x.astype(np.uint32).view(np.float32) if x.dtype != np.float32 else x
            if x.dtype == np.float32:
                return x.view(np.uint32).astype(m.dtype(t))
            return x.astype(m.dtype(t))
        if op == OP['FNegate'] or op == OP['SNegate']:
            return -V(ops[2])
        bin_ops = {OP['FAdd']: np.add, OP['IAdd']: np.add, OP['FSub']: np.subtract, OP['ISub']: np.subtract,
                   OP['FMul']: np.multiply, OP['IMul']: np.multiply, OP['VectorTimesScalar']: np.multiply,
                   OP['MatrixTimesScalar']: None, OP['FRem']: np.fmod, OP['UMod']: np.mod, OP['SRem']: np.fmod,
                   OP['SMod']: np.mod}
        if op in bin_ops and bin_ops[op] is not None:
            with np.errstate(all='ignore'):
                return bin_ops[op](V(ops[2]), V(ops[3])).astype(m.dtype(t))
        if op == OP['FDiv']:
            with np.errstate(all='ignore'):
                return np.divide(V(ops[2]), V(ops[3])).astype(np.float32)
        if op == OP['FMod']:
            with np.errstate(all='ignore'):
                a, b = V(ops[2]), V(ops[3])
                return (a - b * np.floor(a / b)).astype(np.float32)
        if op in (OP['UDiv'], OP['SDiv']):
            with np.errstate(all='ignore'):
                b = V(ops[3])
                return np.where(b == 0, 0, V(ops[2]) // np.where(b == 0, 1, b)).astype(m.dtype(t))
        if op == OP['Dot']:
            a, b = V(ops[2]), V(ops[3])
            with np.errstate(all='ignore'):
                return (a * b).sum(1, keepdims=True).astype(np.float32)
        if op == OP['MatrixTimesVector']:  # (p, cols, rows) x (p, cols) -> (p, rows)
            mat, v = V(ops[2]), V(ops[3])
            return (mat * v[:, :, None]).sum(1)
        if op == OP['VectorTimesMatrix']:  # (p, rows) x (p, cols, rows) -> (p, cols)
            v, mat = V(ops[2]), V(ops[3])
            return (mat * v[:, None, :]).sum(2)
        if op == OP['MatrixTimesScalar']:
            return V(ops[2]) * V(ops[3])[:, :, None]
        if op == OP['MatrixTimesMatrix']:
            a, b = V(ops[2]), V(ops[3])  # (p, k, r) x (p, c, k)
            return np.einsum('pkr,pck->pcr', a, b)
        cmp = {OP['FOrdEqual']: np.equal, OP['FUnordEqual']: np.equal, OP['FOrdNotEqual']: np.not_equal,
               OP['FUnordNotEqual']: np.not_equal, OP['FOrdLessThan']: np.less, OP['FUnordLessThan']: np.less,
               OP['FOrdGreaterThan']: np.greater, OP['FUnordGreaterThan']: np.greater,
               OP['FOrdLessThanEqual']: np.less_equal, OP['FUnordLessThanEqual']: np.less_equal,
               OP['FOrdGreaterThanEqual']: np.greater_equal, OP['FUnordGreaterThanEqual']: np.greater_equal,
               OP['IEqual']: np.equal, OP['INotEqual']: np.not_equal, OP['UGreaterThan']: np.greater,
               OP['SGreaterThan']: np.greater, OP['UGreaterThanEqual']: np.greater_equal,
               OP['SGreaterThanEqual']: np.greater_equal, OP['ULessThan']: np.less, OP['SLessThan']: np.less,
               OP['ULessThanEqual']: np.less_equal, OP['SLessThanEqual']: np.less_equal,
               OP['LogicalEqual']: np.equal, OP['LogicalNotEqual']: np.not_equal}
        if op in cmp:
            with np.errstate(all='ignore'):
                r = cmp[op](V(ops[2]), V(ops[3]))
            if op == OP['FOrdNotEqual']:
                r = r & ~np.isnan(V(ops[2])) & ~np.isnan(V(ops[3]))
            return r
        if op == OP['IsNan']:
            return np.isnan(V(ops[2]))
        if op == OP['IsInf']:
            return np.isinf(V(ops[2]))
        if op == OP['LogicalOr']:
            return V(ops[2]) | V(ops[3])
        if op == OP['LogicalAnd']:
            return V(ops[2]) & V(ops[3])
        if op == OP['LogicalNot']:
            return ~V(ops[2])
        if op == OP['Any']:
            return V(ops[2]).any(1, keepdims=True)
        if op == OP['All']:
            return V(ops[2]).all(1, keepdims=True)
        if op == OP['Select']:
            c, a, b = V(ops[2]), V(ops[3]), V(ops[4])
            return np.where(c, a, b)
        ints = {OP['ShiftRightLogical']: np.right_shift, OP['ShiftRightArithmetic']: np.right_shift,
                OP['ShiftLeftLogical']: np.left_shift, OP['BitwiseOr']: np.bitwise_or,
                OP['BitwiseXor']: np.bitwise_xor, OP['BitwiseAnd']: np.bitwise_and}
        if op in ints:
            a, b = V(ops[2]).astype(np.int64), V(ops[3]).astype(np.int64)
            return (ints[op](a, b) & 0xffffffff).astype(m.dtype(t))
        if op == OP['Not']:
            return (~V(ops[2]).astype(np.int64) & 0xffffffff).astype(m.dtype(t))
        if op in (OP['DPdx'], OP['DPdy'], OP['Fwidth'], OP['DPdxFine'], OP['DPdyFine'], OP['FwidthFine'],
                  OP['DPdxCoarse'], OP['DPdyCoarse'], OP['FwidthCoarse']):
            x = V(ops[2])
            return np.zeros((1, x.shape[1]), np.float32)
        if op == OP['ExtInst']:
            return self._glsl(ops[3], [V(i) for i in ops[4:]], t)
        if op in (OP['SelectionMerge'], OP['LoopMerge'], OP['Kill'], OP['Return'], OP['ReturnValue'],
                  OP['Unreachable'], OP['DemoteToHelperInvocation']):
            return None
        raise NotImplementedError(NAME.get(op, op))

    def _glsl(self, inst, a, t):
        with np.errstate(all='ignore'):
            if inst == 50:  # Fma
                return (a[0] * a[1] + a[2]).astype(np.float32)
            if inst in (80, 40):  # NMax, FMax
                return np.fmax(a[0], a[1]) if inst == 80 else np.maximum(a[0], a[1])
            if inst in (79, 37):  # NMin, FMin
                return np.fmin(a[0], a[1]) if inst == 79 else np.minimum(a[0], a[1])
            if inst in (41, 42):
                return np.maximum(a[0], a[1])
            if inst in (38, 39):
                return np.minimum(a[0], a[1])
            if inst == 26:  # Pow
                return np.power(a[0], a[1]).astype(np.float32)
            if inst == 46:  # FMix
                return (a[0] * (1 - a[2]) + a[1] * a[2]).astype(np.float32)
            if inst in (43, 81):  # FClamp, NClamp
                return np.fmin(np.fmax(a[0], a[1]), a[2])
            if inst in (44, 45):
                return np.minimum(np.maximum(a[0], a[1]), a[2])
            if inst == 8:
                return np.floor(a[0])
            if inst == 9:
                return np.ceil(a[0])
            if inst == 3:
                return np.trunc(a[0])
            if inst in (1, 2):
                return np.rint(a[0])
            if inst == 4:
                return np.abs(a[0])
            if inst == 5:
                return np.abs(a[0])
            if inst in (6, 7):
                return np.sign(a[0]).astype(a[0].dtype)
            if inst == 10:  # Fract
                return a[0] - np.floor(a[0])
            if inst == 30:
                return np.log2(a[0])
            if inst == 29:
                return np.exp2(a[0])
            if inst == 28:
                return np.log(a[0])
            if inst == 27:
                return np.exp(a[0])
            if inst == 31:
                return np.sqrt(a[0])
            if inst == 32:
                return 1.0 / np.sqrt(a[0])
            if inst == 13:
                return np.sin(a[0])
            if inst == 14:
                return np.cos(a[0])
            if inst == 66:  # Length
                return np.sqrt((a[0] * a[0]).sum(1, keepdims=True))
            if inst == 67:  # Distance
                d = a[0] - a[1]
                return np.sqrt((d * d).sum(1, keepdims=True))
            if inst == 69:  # Normalize
                return a[0] / np.sqrt((a[0] * a[0]).sum(1, keepdims=True))
            if inst == 68:  # Cross
                x, y = np.broadcast_arrays(a[0], a[1])
                return np.cross(x, y).astype(np.float32)
            if inst == 48:  # Step(edge, x)
                return (a[1] >= a[0]).astype(np.float32)
            if inst == 49:  # SmoothStep(e0, e1, x)
                tt = np.clip((a[2] - a[0]) / (a[1] - a[0]), 0, 1)
                return (tt * tt * (3 - 2 * tt)).astype(np.float32)
            if inst == 71:  # Reflect
                return a[0] - 2 * (a[1] * a[0]).sum(1, keepdims=True) * a[1]
        raise NotImplementedError(f'GLSL.std.450 {inst}')


def _result_id(op, ops):
    """The result id an instruction defines (None for those without one)."""
    if op in (OP['Store'], OP['Branch'], OP['BranchConditional'], OP['SelectionMerge'], OP['LoopMerge'],
              OP['Kill'], OP['Return'], OP['ReturnValue'], OP['Unreachable'], OP['DemoteToHelperInvocation']):
        return None
    if op == OP['Label']:
        return ops[0]
    return ops[1] if len(ops) > 1 else None


def _operand_ids(op, ops, sh):
    """Ids an instruction reads (for the dependency walk); literals are left out."""
    if op in (OP['Label'], OP['Branch'], OP['SelectionMerge'], OP['LoopMerge'], OP['Kill'], OP['Return'],
              OP['Unreachable']):
        return []
    if op == OP['BranchConditional']:
        return [ops[0]]
    if op == OP['Store']:
        return [ops[0], ops[1]]
    if op == OP['Variable']:
        return ops[3:4]
    if op == OP['Phi']:
        return list(ops[2::2])
    if op == OP['VectorShuffle']:
        return ops[2:4]
    if op == OP['CompositeExtract']:
        return ops[2:3]
    if op == OP['CompositeInsert']:
        return ops[2:4]
    if op == OP['ExtInst']:
        return ops[4:]
    if op in (OP['ImageSampleImplicitLod'], OP['ImageSampleExplicitLod'], OP['ImageFetch']):
        # Virtual texture caches are sampled at the texel's uv: their computed coordinates (the tile
        # addressing) are not needed. Image operands (gradients, lod) never are.
        img = ops[2]
        if sh is not None and _is_array_image(sh, img):
            return [img]
        return [img] if op == OP['ImageFetch'] else [img, ops[3]]
    if op in (OP['AccessChain'], OP['InBoundsAccessChain']):
        return ops[2:]
    return ops[2:]


def _is_array_image(sh, i):
    """Whether a sampled image id refers to an arrayed image (BG3's virtual texture caches)."""
    m, defs = sh.m, sh.defs
    seen = 0
    while i in defs and seen < 16:
        op, ops = m.body[defs[i]]
        if op == OP['SampledImage'] or op == OP['Image']:
            i = ops[2]
        elif op == OP['Load']:
            i = ops[2]
        else:
            break
        seen += 1
    if i in m.vars:
        t = m.types[m.types[m.vars[i][0]][2]]
        return t[0] == 'image' and bool(t[2])
    return False


def _index(x, idx):
    if isinstance(x, (list, _Repeat)):
        return x[idx]
    if x.ndim == 3:  # matrix -> column
        return x[:, idx, :]
    return x[:, idx:idx + 1]


def _extract(x, path):
    for i in path:
        x = _index(x, i)
    return x


def _set(x, path, v):
    if not path:
        return v
    i = path[0]
    if isinstance(x, list):
        y = list(x)
        y[i] = _set(x[i], path[1:], v)
        return y
    if x.ndim == 3:
        col = _set(x[:, i, :], path[1:], v)
        pp = max(x.shape[0], col.shape[0])
        y = np.array(np.broadcast_to(x, (pp,) + x.shape[1:]))
        y[:, i, :] = np.broadcast_to(col, (pp, col.shape[1]))
        return y
    pp = max(x.shape[0], v.shape[0])
    y = np.array(np.broadcast_to(x, (pp, x.shape[1])))
    y[:, i:i + 1] = np.broadcast_to(v, (pp, 1))
    return y


def tbn_roles(vertex_spirv):
    """{location: 'N' | 'X' | 'Y'} of a BG3 material's vertex shader outputs: BG3 builds a
    vertex's frame from its qtangent (X axis along +U, Y axis along -V, N = (X x Y) times the sign of w) and
    hands it to the fragment shader in locations that differ between shaders. N is the output that
    depends on a cross product; X and Y are normalize(axis * M), told apart by the axis's first component
    (X: fma(-2, ..., 1), Y: 2 * ...)."""
    try:
        m = Module(vertex_spirv)
    except Exception:  # noqa: BLE001
        return {}
    if m.execution_model != 0:
        return {}
    defs = {}
    for k, (op, ops) in enumerate(m.body):
        rid = _result_id(op, ops)
        if rid is not None:
            defs[rid] = k
    outs = {v: m.dec[v][DEC_LOCATION][0] for v, (pt, sc) in m.vars.items()
            if sc == SC_OUTPUT and DEC_LOCATION in m.dec.get(v, {})}

    def deps(i):
        seen, stack = set(), [i]
        while stack:
            j = stack.pop()
            if j in seen or j not in defs:
                continue
            seen.add(j)
            op, ops = m.body[defs[j]]
            stack.extend(_operand_ids(op, ops, None))
        return seen

    def is_const(i, value):
        if i not in m.consts:
            return False
        try:
            return abs(float(m.const_value(i).ravel()[0]) - value) < 1e-6
        except Exception:  # noqa: BLE001
            return False

    def axis_of(i):
        # through Normalize / vector x matrix / copies to the axis's construct
        for _ in range(12):
            if i not in defs:
                return None
            op, ops = m.body[defs[i]]
            if op == OP['ExtInst'] and ops[3] == 69:
                i = ops[4]
            elif op in (OP['VectorTimesMatrix'],):
                i = ops[2]
            elif op in (OP['MatrixTimesVector'],):
                i = ops[3]
            elif op == OP['CopyObject']:
                i = ops[2]
            elif op == OP['CompositeConstruct'] and len(ops) == 5:
                c0 = ops[2]
                if c0 not in defs:
                    return None
                op0, ops0 = m.body[defs[c0]]
                if op0 == OP['ExtInst'] and ops0[3] == 50 and is_const(ops0[4], -2.0):
                    return 'X'
                if op0 == OP['FMul'] and (is_const(ops0[2], 2.0) or is_const(ops0[3], 2.0)):
                    return 'Y'
                return None
            else:
                return None
        return None
    roles = {}
    for op, ops in m.body:
        if op == OP['Store'] and ops[0] in outs:
            loc = outs[ops[0]]
            d = deps(ops[1])
            if any(m.body[defs[j]][0] == OP['ExtInst'] and m.body[defs[j]][1][3] == 68 for j in d if j in defs):
                roles[loc] = 'N'
            else:
                a = axis_of(ops[1])
                if a:
                    roles[loc] = a
    return roles


def vertex_from_bshd(b):
    """The vertex shader SPIR-V in a .bshd, or None."""
    return _module_from_bshd(b, 0)


def fragment_from_bshd(b):
    """The fragment shader SPIR-V in a .bshd (BG3's compiled shader: vertex then fragment module)."""
    return _module_from_bshd(b, 4)


def _module_from_bshd(b, model):
    magic = struct.pack('<I', 0x07230203)
    starts, i = [], 0
    while True:
        j = b.find(magic, i)
        if j < 0:
            break
        starts.append(j)
        i = j + 4
    for k, s in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(b)
        data = b[s:s + (end - s) // 4 * 4]
        try:
            mod = Module(data)
        except Exception:  # noqa: BLE001
            continue
        if mod.execution_model == model:
            return data
    return None


if __name__ == '__main__':
    sh = Shader(open(sys.argv[1], 'rb').read())
    print(sh.interface_summary())
