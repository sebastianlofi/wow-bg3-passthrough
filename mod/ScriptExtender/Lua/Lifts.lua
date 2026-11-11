-- Lifts.lua: WoW's lifts carry BG3's characters (server).
--
-- A WoW lift (an elevator, a tram car) is a game object WoW's client moves by itself along its keyframes; BG3's
-- ground is WoW's static world only, so its car is no ground for BG3. BG3 has moving ground of its own: a platform,
-- a small level placed in ours as a level template, which Osiris PlatformMoveTo moves with whoever stands on it
-- (BG3's own lifts are that). The level keeps a few such platforms with round floors of several sizes, parked off
-- WoW's maps (tools/wow_lift_platforms.py). For every lift with a floor to ride near a party member this module
-- takes the smallest platform the floor fits in, puts it where WoW's car is, and repeats the car's moves: benilla
-- says where each lift is and where the keyframe span it is on ends, with the time left (link/wowlifts.bin, benilla
-- 0135), and a span is one straight move at one speed, which is exactly what PlatformMoveTo makes. WoW keeps the
-- timetable: nothing here calls or holds a car.
--
-- Reads: link/wowlifts.bin (WBL1, seq, count, count x (guid u64, entry u32, flags u32 (1 moving), period ms, cycle
-- ms, WoW xyz now, WoW xyz of the span's end, ms to it), seq again); link/lifts.json {Decks = {[entry] = {Radius,
-- Floor}}} (world_bridge.py: each car's floor from its own collision model, metres; a lift without one is a door);
-- link/world.json (Active, Centre: the mapping BG3 = Centre + (y, z, -x) * 0.9144).
-- Status: wow-bg3/lifts-status.json {RunId, Tick, Lifts = [{Entry, Guid, Key, Radius, At, Moving}], Free}.
local M = {}

-- The platforms the level keeps (tools/wow_lift_platforms.py lua; tools/test_wow_lift_platforms.py holds the two
-- together): a floor's radius, where each one is parked, each one's id in the level.
M.MARGIN = 1
M.PLATFORMS = {
    {Radius = 3, Park = {{-15700, -60, -15500}, {-15660, -60, -15500}, {-15620, -60, -15500}}, Keys = {"220f65b4-9d06-5eb8-ba9b-5cc6b626e1fd", "8a90c2da-f97c-5819-9cee-4d1ca915c9b3", "22d1ec38-9e6f-5c1d-81d5-33f372e25c7e"}},
    {Radius = 4, Park = {{-15700, -60, -15460}, {-15660, -60, -15460}, {-15620, -60, -15460}}, Keys = {"cb651fd2-586d-598f-a97b-7af338ff47b8", "640b6074-815a-502b-92e3-b798698f5412", "509abf70-1773-591d-8e0b-d2890580ad2c"}},
    {Radius = 5, Park = {{-15700, -60, -15420}, {-15660, -60, -15420}, {-15620, -60, -15420}}, Keys = {"301ee89f-fada-5f01-99ce-502e0e5b9d8a", "ad780899-e497-5f8b-ab06-cece00a1f29e", "44a36d1b-fa43-5a35-a272-af6b2907d1d3"}},
    {Radius = 6, Park = {{-15700, -60, -15380}, {-15660, -60, -15380}, {-15620, -60, -15380}}, Keys = {"d03eadc5-6649-5b6f-b864-5c56a8090239", "e0a60539-e02e-5fd8-b07d-1c223083f4f6", "5403cc5a-282d-5b1e-bd99-dbfb32ac6fec"}},
    {Radius = 7, Park = {{-15700, -60, -15340}, {-15660, -60, -15340}, {-15620, -60, -15340}}, Keys = {"0f602d5b-72e4-5e70-b45a-346b1f418635", "8d5f9632-6499-5938-a514-c0cf85da88fa", "1c47f144-fc4d-5c84-af69-12af73995304"}},
    {Radius = 8, Park = {{-15700, -60, -15300}, {-15660, -60, -15300}, {-15620, -60, -15300}}, Keys = {"96c792fb-ee15-5999-82ef-9318dde41a99", "2179f7f3-a3fb-5b03-ac87-69e3082f7174", "08bf5761-9d6f-51d7-82ef-789406d5ccaa"}},
    {Radius = 9, Park = {{-15700, -60, -15260}, {-15660, -60, -15260}, {-15620, -60, -15260}}, Keys = {"6cd121b5-6b65-5f54-8d6e-de8110a646e7", "36d379bb-8dde-5924-9b1c-da756db54337", "5c873def-7d32-525e-b53d-6a3596afb49f"}},
    {Radius = 10, Park = {{-15700, -60, -15220}, {-15660, -60, -15220}, {-15620, -60, -15220}}, Keys = {"fdc0a767-ed88-525e-8100-3636ca1cba0b", "1acc403e-86cb-51d2-b488-106cfbf86662", "e6e8fc01-84b8-5148-b042-78007060fe81"}},
    {Radius = 11, Park = {{-15700, -60, -15180}, {-15660, -60, -15180}, {-15620, -60, -15180}}, Keys = {"778cccdd-1195-54a4-8bd8-367974bc14d2", "aef67ace-983a-5f63-bf93-061c1527fa14", "f0d8bf76-8f44-5641-93aa-fe25e760b094"}},
    {Radius = 12, Park = {{-15700, -60, -15140}, {-15660, -60, -15140}, {-15620, -60, -15140}}, Keys = {"65c326bd-e32a-50ed-ba0c-8d01b4dcf013", "01d41cc9-c5d3-5b69-bd0a-b284d1946b9b", "23e9df94-d29c-5f8f-ad4b-e61d49ff324c"}},
    {Radius = 13.5, Park = {{-15700, -60, -15100}, {-15660, -60, -15100}, {-15620, -60, -15100}}, Keys = {"26506ce0-573a-599c-b210-5aa41fdfa77d", "9db9bdfc-05a3-52aa-a7f0-994e38505adf", "55ea08aa-ceee-5116-a009-a46a2cab1f3b"}},
}

M.NEAR = 160.0      -- metres from a party member within which a car gets its platform
M.FAR = 240.0       -- and beyond which the platform is parked again
M.FAST = 20000.0    -- metres a second for a platform nobody is on (from where it is parked to the car)
local YD = 0.9144

local function q(fn, ...) local ok, r = pcall(fn, ...); return ok and r end

-- link/wowlifts.bin as a list of {Guid, Entry, Moving, Period, Cycle, Pos, To, ToMs}, or nil for a file that is not whole.
function M.Parse(raw)
    if type(raw) ~= "string" or #raw < 16 or raw:sub(1, 4) ~= "WBL1" then return nil end
    local seq, n = string.unpack("<I4I4", raw, 5)
    if #raw ~= 16 + 52 * n or string.unpack("<I4", raw, 13 + 52 * n) ~= seq then return nil end
    local out = {}
    for i = 0, n - 1 do
        local guid, entry, flags, period, cycle, x, y, z, tx, ty, tz, toMs = string.unpack("<I8I4I4I4I4ffffffI4", raw, 13 + 52 * i)
        out[#out + 1] = {Guid = string.format("%016x", guid), Entry = entry, Moving = flags & 1 == 1, Period = period, Cycle = cycle,
            Pos = {x, y, z}, To = {tx, ty, tz}, ToMs = toMs}
    end
    return out
end

-- The floor's place in BG3 for a car at WoW point w: world mode's mapping, raised by the floor's height in the car.
-- Its x and z go to the nearest half metre, BG3's tile lattice: the platform's tiles then lie on the ground squares'
-- own tiles, so the floor's rim and the ground beside it are neighbours with no half tile between (the bridge takes
-- the ground away under the same rounded disc: world_bridge.py carve_lifts). The car is at most 35 cm beside.
function M.ToBg3(w, centre, floor)
    local function half(v) return math.floor(v * 2 + 0.5) / 2 end
    return {half(centre[1] + w[2] * YD), centre[2] + w[3] * YD + floor, half(centre[3] - w[1] * YD)}
end

-- The index in M.PLATFORMS of the smallest floor that reaches M.MARGIN past a car's own (onto the landings: see
-- tools/wow_lift_platforms.py), or nil.
function M.Size(radius)
    for i, p in ipairs(M.PLATFORMS) do
        if p.Radius >= radius + M.MARGIN then return i end
    end
    return nil
end

-- What a platform is told for a lift's record: nil (it is where the car is), or {To, Speed}: the end of the span the
-- car is on and the speed that reaches it when the car does. `here` is where the platform is now.
function M.Order(l, here, centre, floor)
    local to = M.ToBg3(l.To, centre, floor)
    local d = math.sqrt((to[1] - here[1]) ^ 2 + (to[2] - here[2]) ^ 2 + (to[3] - here[3]) ^ 2)
    if d < 0.02 then return nil end
    -- A car at rest that the platform is not on (it was just brought here, or fell behind): there in a quarter second.
    local seconds = l.Moving and math.max(l.ToMs, 50) / 1000 or 0.25
    return {To = to, Speed = math.min(math.max(d / seconds, 0.05), M.FAST)}
end

function M.Start(runId)
    local ticks, world, decks, held, free, members = 0, nil, nil, {}, {}, {}
    for i, p in ipairs(M.PLATFORMS) do
        free[i] = {}
        for k = 1, #p.Keys do free[i][k] = k end
    end
    local function readJson(name)
        local ok, v = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/" .. name) or "")
        return ok and type(v) == "table" and v or nil
    end
    local function at(key)
        return q(function() local p = Ext.Entity.Get(key).Transform.Transform.Translate; return {p[1], p[2], p[3]} end)
    end
    local function park(h)
        local p = M.PLATFORMS[h.Size].Park[h.Slot]
        pcall(Osi.PlatformMoveTo, h.Key, p[1], p[2], p[3], M.FAST, "", 0)
        table.insert(free[h.Size], h.Slot)
        Ext.Utils.Print(string.format("WOW-BG3 lifts: lift %d (%s) left behind, its platform is parked", h.Entry, h.Guid))
    end
    local function status()
        local list, left = {}, 0
        for guid, h in pairs(held) do
            list[#list + 1] = {Entry = h.Entry, Guid = guid, Key = h.Key, Radius = M.PLATFORMS[h.Size].Radius, At = at(h.Key), Moving = h.Moving}
        end
        for i = 1, #free do left = left + #free[i] end
        Ext.IO.SaveFile("wow-bg3/lifts-status.json", Ext.Json.Stringify({RunId = runId, Tick = ticks, Lifts = list, Free = left}))
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 30 == 1 then
            world = readJson("link/world.json")
            local d = readJson("link/lifts.json")
            decks = d and type(d.Decks) == "table" and d.Decks or nil
        end
        local on = world and world.Active == true and type(world.Centre) == "table" and decks
        if not on then
            if next(held) then
                for guid, h in pairs(held) do park(h); held[guid] = nil end
                status()
            end
            return
        end
        if ticks % 10 == 1 then
            members = {}
            for _, r in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
                local ok, x, y, z = pcall(Osi.GetPosition, r[1])
                if ok and x and y and z then members[#members + 1] = {x, y, z} end
            end
        end
        local lifts = M.Parse(Ext.IO.LoadFile("wow-bg3/link/wowlifts.bin"))
        if not lifts then return end
        local seen, changed = {}, false
        for _, l in ipairs(lifts) do
            local d = decks[tostring(l.Entry)]
            if d and tonumber(d.Radius) and tonumber(d.Floor) then
                local P = M.ToBg3(l.Pos, world.Centre, d.Floor)
                local near = math.huge
                for _, m in ipairs(members) do
                    near = math.min(near, math.sqrt((m[1] - P[1]) ^ 2 + (m[2] - P[2]) ^ 2 + (m[3] - P[3]) ^ 2))
                end
                local h = held[l.Guid]
                if not h and near <= M.NEAR then
                    local size = M.Size(d.Radius)
                    -- The next size up when every platform of its own is out.
                    while size and #free[size] == 0 do size = M.PLATFORMS[size + 1] and size + 1 or nil end
                    if size then
                        local slot = table.remove(free[size])
                        h = {Entry = l.Entry, Guid = l.Guid, Size = size, Slot = slot, Key = M.PLATFORMS[size].Keys[slot], Span = nil}
                        held[l.Guid] = h
                        changed = true
                        Ext.Utils.Print(string.format("WOW-BG3 lifts: lift %d (%s), floor %.1f m: platform %s (%g m) at %.1f %.1f %.1f",
                            l.Entry, l.Guid, d.Radius, h.Key, M.PLATFORMS[size].Radius, P[1], P[2], P[3]))
                    elseif not M.Said then
                        M.Said = true
                        Ext.Utils.Print(string.format("WOW-BG3 lifts: no platform left for lift %d (floor %.1f m)", l.Entry, d.Radius))
                    end
                end
                if h then
                    seen[l.Guid] = true
                    if near > M.FAR then
                        park(h); held[l.Guid] = nil; changed = true
                    else
                        -- One order a span: its end and the time left say the whole move. A car at rest is checked
                        -- again every half second, in case the platform is not on it.
                        local span = string.format("%d %.2f %.2f %.2f", l.Moving and 1 or 0, l.To[1], l.To[2], l.To[3])
                        if span ~= h.Span or (not l.Moving and ticks >= (h.Check or 0)) then
                            h.Span, h.Check, h.Moving = span, ticks + 15, l.Moving
                            local o = M.Order(l, at(h.Key) or P, world.Centre, d.Floor)
                            if o then pcall(Osi.PlatformMoveTo, h.Key, o.To[1], o.To[2], o.To[3], o.Speed, "WOWBG3_Lift", 0) end
                        end
                    end
                end
            end
        end
        -- A lift the file no longer has (another WoW map): its platform is parked.
        for guid, h in pairs(held) do
            if not seen[guid] then park(h); held[guid] = nil; changed = true end
        end
        if changed or ticks % 150 == 0 then status() end
    end)
end

return M
