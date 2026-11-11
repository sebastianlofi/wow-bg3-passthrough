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
M.SLACK = 0.25
M.PLATFORMS = {
    {Radius = 3, Park = {{-15700, -60, -15500}, {-15660, -60, -15500}, {-15620, -60, -15500}}, Keys = {"220f65b4-9d06-5eb8-ba9b-5cc6b626e1fd", "8a90c2da-f97c-5819-9cee-4d1ca915c9b3", "22d1ec38-9e6f-5c1d-81d5-33f372e25c7e"}},
    {Radius = 5, Park = {{-15700, -60, -15460}, {-15660, -60, -15460}, {-15620, -60, -15460}}, Keys = {"301ee89f-fada-5f01-99ce-502e0e5b9d8a", "ad780899-e497-5f8b-ab06-cece00a1f29e", "44a36d1b-fa43-5a35-a272-af6b2907d1d3"}},
    {Radius = 6.5, Park = {{-15700, -60, -15420}, {-15660, -60, -15420}, {-15620, -60, -15420}}, Keys = {"29ea571b-1b36-5f17-9020-e4fdc5875d7e", "e065d746-7a84-5c02-83b7-b4dabc889a8f", "569ba09d-8408-5ff2-8630-d6ddd14907b8"}},
    {Radius = 8, Park = {{-15700, -60, -15380}, {-15660, -60, -15380}, {-15620, -60, -15380}}, Keys = {"96c792fb-ee15-5999-82ef-9318dde41a99", "2179f7f3-a3fb-5b03-ac87-69e3082f7174", "08bf5761-9d6f-51d7-82ef-789406d5ccaa"}},
    {Radius = 10, Park = {{-15700, -60, -15340}, {-15660, -60, -15340}, {-15620, -60, -15340}}, Keys = {"fdc0a767-ed88-525e-8100-3636ca1cba0b", "1acc403e-86cb-51d2-b488-106cfbf86662", "e6e8fc01-84b8-5148-b042-78007060fe81"}},
    {Radius = 12.5, Park = {{-15700, -60, -15300}, {-15660, -60, -15300}, {-15620, -60, -15300}}, Keys = {"2254d232-4b91-547b-b962-af20f3139a31", "764139ce-5ac3-5d3d-a312-bb71c5a6c6c9", "9d3f12e8-b115-5ce7-8cf0-5d43e0225c3d"}},
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
function M.ToBg3(w, centre, floor)
    return {centre[1] + w[2] * YD, centre[2] + w[3] * YD + floor, centre[3] - w[1] * YD}
end

-- The index in M.PLATFORMS of the smallest floor a car's fits in, or nil.
function M.Size(radius)
    for i, p in ipairs(M.PLATFORMS) do
        if p.Radius >= radius - M.SLACK then return i end
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
