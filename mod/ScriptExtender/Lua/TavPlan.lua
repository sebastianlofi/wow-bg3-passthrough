-- Tav's geometry for WoW (Tav is drawn inside WoW with WoW's light and
-- fog, as SkyCraft copies its geometry). This walks Tav's render objects and writes where their
-- mesh data and poses live (wow-bg3/link/tavplan.txt); the capture layer, inside the same process,
-- copies the meshes once and the poses every frame for benilla (bg3/capture/wowbg3_capture.c).
--
-- Plan (text, one record a line, handles and addresses hex):
--   TAVPLAN 1 <seq> <Centre xyz> <WowOrigin xyz>      the world-mode mapping (see WorldMode.lua)
--   M <key> <kind> <part> <vcount> <icount> <isize> <vsize> <uvoff> <src> <skin> <vb> <ib> <mb> <wt> <material> <figure> <lod>
-- lod (the LOD format research): the object's level of detail (ObjectDescs LOD byte, 0 the
--   finest). Every LOD is planned (M.AllLods): BG3 draws a coarser LOD when the camera is far, and the
--   layer tells benilla per frame which planned meshes BG3 drew (tavpose.bin flags 2 and 4), so WoW
--   draws the LOD BG3 draws with the palette BG3 just used (heads vanished when
--   zoomed out and lagged the body when zoomed back in). Each figure's first M line is a LOD 0 object.
--   Coarser LODs keep their H lines too (older layers hide them by those).
-- figure (the party): 0 the hero, then each companion WoW draws (WorldMode.lua's
--   figures; a number per companion for the session, so figures may have gaps); a figure's
--   lines come together (F, M and K, H, C, V), the hero's first, so the plan's first mesh is still her root. F <figure> <uuid> names it. Readers before the party ignore the extra field.
-- part: which visual the object belongs to, 0 Tav's root visual, else a number kept per attachment
--   visual (by its address) for the whole session (it was the
--   attachment's index, so an effect or a weapon attaching renumbered every later mesh and made a
--   new plan without any mesh changing; benilla froze Tav for up to 0.5 s at each one). M lines come
--   in a fixed order: by part, then as the visual lists its objects.
-- kind 0: skinned. src (VkBuffer) holds 20 bytes a vertex (position f32x3, qtangent i16x4),
--   skin (VkBuffer) 8 (4 u8 weights, 4 u8 bones), vb the stream-0 vertex (uv0 half2 at uvoff), ib the
--   indices. mb is the MeshBinding (pose palette pointer at +16, bone count u16 at +24).
-- kind 1: rigid. vb holds position, qtangent and uv0 (at uvoff); src is vb, no skin or palette.
-- wt is the object's WorldTransform (quat xyzw, translate, scale: 10 f32), read every frame.
-- material: the applied material's resource UUID (link/tools/tavtex.py bakes its texture).
--   C <skin> <hair> <eyes> <figure>   a figure's character creation colours (preset UUIDs), for the skin, hair
--                and eye colours of those materials.
--   H <ib>       the index buffer of a farther level of detail: never copied, only hidden in BG3
--                (BG3 picks a coarser LOD when the camera is far, and Tav must not show there).
--   K <key> <offset> <word>...   the material constant buffer the M line's object is drawn with
--                (32-bit words, hex, from byte offset; several lines a buffer): BG3's colour inputs
--                with the item's colour preset or dye and the character creation colours applied.
--                tavtex.py runs the material's shader with it. Read when the object is first seen
--                (it waited 2 s, and tavtex up to 4 s for it, before a new
--                companion could be baked) and once more 2 s later (replaced when it changed: a
--                buffer BG3 filled late), then kept for it (by mesh key and material, also while the
--                mesh is away for a while: a weapon put away and drawn again keeps its K lines).
--   V <visual>   a visual resource the equipment shows (ClientEquipmentVisuals), sorted: its
--                VertexColorMaskSlots say which vertex-colour regions BG3 cuts from her body and
--                underwear (tavtex.py, benilla patch 0036; BG3's mask per character is exactly the
--                NakedBodySystem bytes of these visuals' slots).
-- Effects attached to Tav (Lacerate): a cast or a status can attach visuals
-- whose objects are effect billboards (materials under Materials/Effects/, such as Lacerate's
-- VFX_Billboard_..._UVDistortion_Refraction_01). They are not Tav: listed, the layer hid BG3's own
-- draws of them (so the effects replay never got them) and benilla drew them as part of her body,
-- and they made two new plans per cast. They are left out of M and H lines; the layer's effects
-- capture replays them like any other effect.
-- Offsets were found by research (bg3/docs or the m4b-world-mode notes, "geometry copy").
local M = {}
local I = Ext._Internal

local function A(base, class, path) return I.ObjectFieldAddress(base, class, path) end
local function p64(a) return a and a ~= 0 and I.Peek(a, 8) or nil end
local function u32(a) return a and I.Peek(a, 4) end
local function u8(a) return a and I.Peek(a, 1) end

-- Bytes per element of a vertex attribute format (FormatDesc.Format).
local FORMAT_BYTES = {[0] = 4, [3] = 2, [6] = 2, [8] = 1}

local function attributes(m)
    local at = A(m, "Model", "Attributes")
    local buf, count = p64(at), u32(at + 12)
    local list = {}
    for k = 0, (count or 0) - 1 do
        local b = buf + k * 6
        list[#list + 1] = {Stream = u8(b), Usage = u8(b + 1), Index = u8(b + 2), Format = u8(b + 4), Size = u8(b + 5)}
    end
    return list, at
end

-- Byte offset of uv0 in stream 0 (nil when there is none).
local function uv0Offset(list)
    local off = 0
    for _, a in ipairs(list) do
        if a.Stream == 0 then
            if a.Usage == 2 and a.Index == 0 then return off end
            off = off + (FORMAT_BYTES[a.Format] or 4) * a.Size
        end
    end
    return nil
end

local function hasStream0Position(list)
    for _, a in ipairs(list) do
        if a.Stream == 0 and a.Usage == 1 then return true end
    end
    return false
end

local function handleOf(obj) return p64(obj) end

-- Whether an applied material is an effect material (by its resource's source file), cached.
local effectMat = {}
local function isEffectMaterial(name)
    if type(name) ~= "string" or name == "" or name == "-" then return false end
    local v = effectMat[name]
    if v == nil then
        local ok, src = pcall(function() return Ext.Resource.Get(name, "Material").SourceFile end)
        v = ok and type(src) == "string" and src:find("/Materials/Effects/", 1, true) ~= nil or false
        effectMat[name] = v
    end
    return v
end
local function materialOf(ren)
    local am = p64(A(ren, "AnimatableObject", "ActiveMaterial"))
    local okm, mat = pcall(I.ObjectGetField, am, "AppliedMaterial", "MaterialName")
    return (okm and type(mat) == "string" and mat ~= "") and mat or "-"
end
M.EffectsLeftOut = 0
M.BlendShapeSources = 0  -- Blend-shape faces planned from BlendShapeModelData's source (counted per check)

-- One render object as a plan line, or nil and why not (M.Skips counts the reasons).
local function line(ren, part, figure, lod)
    local m = p64(A(ren, "AnimatableObject", "Model"))
    if not m then return nil, "no model" end
    local vcount, vsize = u32(A(m, "Model", "VertexCount")), u32(A(m, "Model", "VertexSize"))
    local icount, isize = u32(A(m, "Model", "IndexCount")), u32(A(m, "Model", "IndexSize"))
    if not vcount or vcount == 0 or not icount or icount == 0 then return nil, "no vertices" end
    local list, at = attributes(m)
    local uvoff = uv0Offset(list)
    if not uvoff then return nil, "no uv0 in stream 0" end
    local vb, ib = handleOf(p64(at + 16)), handleOf(p64(at + 24))
    if not vb or not ib then return nil, "no buffers" end
    local wt = A(ren, "AnimatableObject", "WorldTransform")
    local kind, src, skin, mb
    if hasStream0Position(list) then
        kind, src, skin, mb = 1, vb, 0, 0
    else
        local bsd = A(m, "TransformingVertexModel", "BlendShapeModelData")
        local smd = bsd and p64(bsd - 8)
        if not smd then return nil, "no skinned model data" end
        src = handleOf(p64(smd))
        -- Blend-shape models (the origin characters' faces, Lae'zel's, Astarion's, Gale's
        -- as the hero too): their SkinnedModelData has no source; the 20-byte vertex source is the buffer
        -- of BlendShapeModelData +0x10 (an object laid out like the source's: VkBuffer at +0, its size at
        -- +0x1c, vcount x 20). Left out, BG3 drew those faces itself, composited from its own frame: they
        -- lagged the WoW-drawn head and vanished when zoomed out, and Astarion's showed a black shell.
        if not src then
            local bv = p64(bsd)
            local bo = bv and p64(bv + 0x10)
            if bo and u32(bo + 0x1c) == vcount * 20 then
                src = handleOf(bo)
                M.BlendShapeSources = M.BlendShapeSources + 1
            end
        end
        local so = p64(smd + 8)
        skin = so and p64(so + 56)  -- the VkBuffer itself
        mb = p64(A(ren, "AnimatableObject", "Mesh"))
        if not src or not skin or not mb then return nil, "no source, skin or binding" end
        kind = 0
    end
    local mat = materialOf(ren)
    if isEffectMaterial(mat) then
        M.EffectsLeftOut = M.EffectsLeftOut + 1
        return nil, "effect material"
    end
    -- A mesh key per figure: two characters may draw the same model; the figure in the top byte
    -- keeps their poses and constant buffers apart (an address never reaches it).
    local key = (figure or 0) > 0 and (m ~ ((figure or 0) << 56)) or m
    return string.format("M %x %d %d %d %d %d %d %d %x %x %x %x %x %x %s %d %d",
        key, kind, part, vcount, icount, isize, vsize, uvoff, src, skin, vb, ib, mb, wt, mat, figure or 0, lod or 0), key, mat
end

-- K lines of a render object (see the header). Render data 0 of its AppliedMaterial holds the
-- constant buffer's CPU copy: pointer at +0x38, size at +0x40 (BG3 build of 2026-10). A snapshot
-- (values that change by the frame must not keep rewriting the plan): taken when the object first
-- shows (formerly CB_WAIT = 2000 ms later) and compared once CB_RECHECK ms later
-- (M.CbRefreshed counts the snapshots that changed then). Keyed by mesh key and material (once
-- keyed by the whole M line, they vanished whenever a part number changed and came back 2 s
-- later, a second new plan each time); a snapshot outlives its mesh by CB_KEEP ms.
local CB_RECHECK, CB_KEEP = 2000, 300000
local cbSeen, cbSnap, cbLast, cbChecked, publishes = {}, {}, {}, {}, 0
M.CbRefreshed = 0
local function nowMs()
    local ok, t = pcall(Ext.Utils.MonotonicTime)
    return ok and type(t) == "number" and t or publishes * 500
end
local function readCb(ren, m)
    local am = p64(A(ren, "AnimatableObject", "ActiveMaterial"))
    local cb, size = p64(am + 0x38), I.Peek(am + 0x40, 8)
    if not cb or not size or size <= 0 or size > 4096 or size % 4 ~= 0 then return nil end
    local lines = {}
    for off = 0, size - 4, 128 do
        local w = {}
        for o = off, math.min(off + 124, size - 4), 4 do w[#w + 1] = string.format("%08x", I.Peek(cb + o, 4)) end
        lines[#lines + 1] = string.format("K %x %d %s", m, off, table.concat(w, " "))
    end
    return table.concat(lines, "\n")
end
local function cbLines(ren, m, ck, now)
    cbLast[ck] = now
    cbSeen[ck] = cbSeen[ck] or now
    if not cbSnap[ck] then
        cbSnap[ck] = readCb(ren, m)
        return cbSnap[ck]
    end
    if not cbChecked[ck] and now - cbSeen[ck] >= CB_RECHECK then
        cbChecked[ck] = true
        local again = readCb(ren, m)
        if again and again ~= cbSnap[ck] then
            cbSnap[ck] = again
            M.CbRefreshed = M.CbRefreshed + 1
        end
    end
    return cbSnap[ck]
end

-- Part numbers of attachment visuals by address (see the header), and when each was last seen.
local partOf, partSeen, nextPart = {}, {}, 1
local function partFor(v, now)
    local p = partOf[v]
    if not p then
        p, nextPart = nextPart, nextPart + 1
        partOf[v] = p
    end
    partSeen[v] = now
    return p
end

-- The plan's mesh lines for an entity (LOD 0 of the root visual and its attachments), as figure
-- `figure` (0 the hero).
function M.Lines(uuid, figure)
    figure = figure or 0
    local e = Ext.Entity.Get(uuid)
    local handle = tonumber(tostring(e):match("Entity %((%x+)%)"), 16)
    local root = p64(I.FieldAddress(handle, "Visual", "Visual"))
    if not root then return {} end
    local now = nowMs()
    local meshes, lods, out, skips = {}, {}, {}, {}
    local function visual(v, part)
        local od = A(v, "Visual", "ObjectDescs")
        local buf, count = p64(od), u32(od + 12)
        for j = 0, (count or 0) - 1 do
            local ren = p64(buf + j * 16)
            local lod = u8(buf + j * 16 + 8) or 0
            if lod == 0 or M.AllLods then
                local ok, l, m, mat = pcall(line, ren, part, figure, lod)
                if ok and l then
                    local okk, k = pcall(cbLines, ren, m, string.format("%x %s", m, mat), now)
                    meshes[#meshes + 1] = {part, j, l, okk and k or nil, lod}
                else
                    local why = string.format("%d lod %d: %s", figure, lod, ok and tostring(m) or ("error " .. tostring(l)))
                    skips[why] = (skips[why] or 0) + 1
                end
            end
            if lod > 0 then
                local ok, ib = pcall(function()
                    if isEffectMaterial(materialOf(ren)) then return nil end
                    local m = p64(A(ren, "AnimatableObject", "Model"))
                    return m and handleOf(p64(A(m, "Model", "Attributes") + 24))
                end)
                if ok and ib then lods[#lods + 1] = string.format("H %x", ib) end
            end
        end
    end
    visual(root, 0)
    local att = A(root, "Visual", "Attachments")
    local abuf, acount = p64(att), u32(att + 12)
    for i = 0, (acount or 0) - 1 do
        local v = p64(abuf + i * 48)
        if v then pcall(visual, v, partFor(v, now)) end
    end
    -- A fixed order: by part, then as the visual lists its objects (Tav's root visual first: the
    -- first mesh's object origin is her root for the layer and benilla).
    table.sort(meshes, function(a, b)
        if a[1] ~= b[1] then return a[1] < b[1] end
        if a[2] ~= b[2] then return a[2] < b[2] end
        return a[3] < b[3]
    end)
    -- The figure's first M line is a LOD 0 object (the layer's and benilla's anchor, lod-format.md).
    if meshes[1] and meshes[1][5] ~= 0 then
        for i = 2, #meshes do
            if meshes[i][5] == 0 then table.insert(meshes, 1, table.remove(meshes, i)); break end
        end
    end
    M.Skips[figure] = skips
    out[#out + 1] = string.format("F %d %s", figure, tostring(uuid))
    for _, x in ipairs(meshes) do
        out[#out + 1] = x[3]
        if x[4] then out[#out + 1] = x[4] end
    end
    table.sort(lods)
    for _, l in ipairs(lods) do out[#out + 1] = l end
    pcall(function()
        local a = e.CharacterCreationAppearance
        out[#out + 1] = string.format("C %s %s %s %d", tostring(a.SkinColor), tostring(a.HairColor), tostring(a.EyeColor), figure)
    end)
    pcall(function()
        local vs = {}
        for _, slot in pairs(e.ClientEquipmentVisuals.Equipment) do
            local d = slot.VisualData
            if d and d.VisualTemplates then
                for _, t in ipairs(d.VisualTemplates) do vs[#vs + 1] = "V " .. tostring(t) .. " " .. figure end
            end
        end
        table.sort(vs)
        for _, l in ipairs(vs) do out[#out + 1] = l end
    end)
    for ck, t in pairs(cbLast) do
        if now - t > CB_KEEP then cbLast[ck], cbSeen[ck], cbSnap[ck], cbChecked[ck] = nil, nil, nil, nil end
    end
    for v, t in pairs(partSeen) do
        if now - t > CB_KEEP then partSeen[v], partOf[v] = nil, nil end
    end
    return out
end

local last, seq = nil, 0
-- Figure numbers: 0 the hero (the first UUID), each companion a number of its own for the
-- session, kept while it is away (a mesh key carries its figure: list positions re-keyed every companion
-- after one that left, and their meshes waited hidden for their looks). So figures may have gaps.
local figOf, nextFig = {}, 1
local function figureOf(u, position)
    if position == 1 then return 0 end
    local f = figOf[u]
    if not f then
        if nextFig > 60 then figOf, nextFig = {}, 1 end  -- the layer takes figures below 64
        f, nextFig = nextFig, nextFig + 1
        figOf[u] = f
    end
    return f
end
-- WorldMode.lua publishes every M.Every ticks; M.LastMs: what the last check cost (ms, smoothed),
-- M.Plans: plans written this session.
M.Every, M.LastMs, M.Plans = 10, 0, 0
-- Plan every LOD (see the header); M.Skips[figure]: render objects left out last time, by why.
M.AllLods, M.Skips = true, {}

-- Writes the plan when it changed. uuids: the hero's UUID, or the figures (the hero first, then the
-- companions); centre, wow: the world-mode mapping.
function M.Publish(uuids, centre, wow)
    publishes = publishes + 1
    local t0 = nowMs()
    if type(uuids) ~= "table" then uuids = {uuids} end
    local ok, lines = pcall(function()
        local all = {}
        for f, u in ipairs(uuids) do
            -- The hero's lines must come (an error is the plan's); a companion that fails is left out.
            local okf, l = pcall(M.Lines, u, figureOf(u, f))
            if not okf and f == 1 then error(l, 0) end
            for _, x in ipairs(okf and l or {}) do all[#all + 1] = x end
        end
        return all
    end)
    M.LastMs = M.LastMs + ((nowMs() - t0) - M.LastMs) * 0.2
    if not ok then return false, tostring(lines) end
    local body = table.concat(lines, "\n")
    local key = string.format("%.4f %.4f %.4f %.4f %.4f %.4f\n", centre[1], centre[2], centre[3], wow[1], wow[2], wow[3]) .. body
    if key == last then return true, #lines end
    last, seq = key, seq + 1
    M.Plans = M.Plans + 1
    Ext.IO.SaveFile("wow-bg3/link/tavplan.txt", string.format("TAVPLAN 1 %d %s", seq, key) .. "\n")
    return true, #lines
end

function M.Clear()
    last = nil
    cbSeen, cbSnap, cbLast, cbChecked = {}, {}, {}, {}
    partOf, partSeen, nextPart = {}, {}, 1
    Ext.IO.SaveFile("wow-bg3/link/tavplan.txt", "TAVPLAN 1 0\n")
end

-- BG3's map off (world mode, compose word "void"): the capture layer keeps drawing only the index
-- buffers listed in wow-bg3/link/keep.txt ("KEEP 1", then one hex VkBuffer handle a line; "KEEP 0":
-- the map is back). Listed: every level of detail of every character (anything with Health) within
-- radius metres of centre, their attachments included, so Tav, her party and a fight's stand-ins stay.
local function indexBuffers(e, out)
    local handle = tonumber(tostring(e):match("Entity %((%x+)%)"), 16)
    local root = handle and p64(I.FieldAddress(handle, "Visual", "Visual"))
    if not root then return end
    local function visual(v)
        local od = A(v, "Visual", "ObjectDescs")
        local buf, count = p64(od), u32(od + 12)
        for j = 0, (count or 0) - 1 do
            pcall(function()
                local m = p64(A(p64(buf + j * 16), "AnimatableObject", "Model"))
                local ib = m and handleOf(p64(A(m, "Model", "Attributes") + 24))
                if ib then out[#out + 1] = string.format("%x", ib) end
            end)
        end
    end
    visual(root)
    local att = A(root, "Visual", "Attachments")
    local abuf, acount = p64(att), u32(att + 12)
    for i = 0, (acount or 0) - 1 do
        local v = p64(abuf + i * 48)
        if v then pcall(visual, v) end
    end
end

-- World mode (WoW draws the world's people): a fight's stand-ins stand in for WoW mobs
-- that WoW draws, so by default only Tav is listed (her own BG3 draws are dropped by the Tav copy
-- anyway) and BG3's stand-ins are not drawn over WoW's mobs; their effects still are (the layer
-- keeps blended draws near Tav). M.KeepOthers = true lists every character within the radius.
M.KeepOthers = false
local Hero = Ext.Require("Hero.lua")
local lastKeep = nil
-- party: the figures WoW draws (the hero and her companions), kept like her.
-- The characters it leaves out (a fight's stand-ins, NPCs) go to wow-bg3/link/hide.txt ("HIDE 1",
-- then one hex VkBuffer handle a line): the layer's "fxprops" keeps a spell's young prop meshes
-- through the void, and a stand-in that has just spawned has young meshes too, so it must be told
-- which ones are characters. They have Health and a spell's props do not, so this side knows exactly.
-- The hide list reaches further than the keep list: the layer keeps a young prop out to 120 m from Tav
-- (FXC_RADIUS_VOID), and a character coming back into BG3's view is young again, so every character
-- inside that radius must be listed.
M.HideRadius = 130
function M.PublishKeep(centre, radius, party)
    local ids, hide = {}, {}
    local mine = {}
    for _, u in ipairs(type(party) == "table" and party or {}) do mine[u] = true end
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("Health")) do
        local ok, p = pcall(function() return e.Transform.Transform.Translate end)
        local kept = true
        if ok and p and not M.KeepOthers then
            local okU, u = pcall(function() return e.Uuid.EntityUuid end)
            if not (okU and (u == Hero.Uuid() or mine[u])) then kept = false end
        end
        if ok and p then
            local dx, dy, dz = p[1] - centre[1], p[2] - centre[2], p[3] - centre[3]
            local d2, r = dx * dx + dy * dy + dz * dz, kept and radius or math.max(radius, M.HideRadius)
            if d2 < r * r then pcall(indexBuffers, e, kept and ids or hide) end
        end
    end
    table.sort(ids)
    local body = "KEEP 1\n" .. table.concat(ids, "\n") .. "\n"
    if body ~= lastKeep then
        lastKeep = body
        Ext.IO.SaveFile("wow-bg3/link/keep.txt", body)
    end
    table.sort(hide)
    -- Written unchanged too: the layer lets a young skinned mesh through once it has read a list made after the
    -- mesh's first draw that does not name it, so a new file is the answer "nothing new has Health".
    Ext.IO.SaveFile("wow-bg3/link/hide.txt", "HIDE 1\n" .. table.concat(hide, "\n") .. "\n")
    return #ids
end

function M.ClearKeep()
    lastKeep = nil
    Ext.IO.SaveFile("wow-bg3/link/keep.txt", "KEEP 0\n")
    Ext.IO.SaveFile("wow-bg3/link/hide.txt", "HIDE 0\n")
end

return M
