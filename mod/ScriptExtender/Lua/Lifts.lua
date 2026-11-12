-- Lifts.lua: WoW's lifts carry BG3's characters (server).
--
-- A WoW lift (an elevator, a tram car) is a game object WoW's client moves by itself along its keyframes; BG3's
-- ground is WoW's static world only, so its car is no ground for BG3. BG3 has moving ground of its own: a platform,
-- a small level placed in ours as a level template, which Osiris PlatformMoveTo moves with whoever stands on it
-- (BG3's own lifts are that). The level keeps a few such platforms with round floors of several sizes, parked off
-- WoW's maps (tools/wow_lift_platforms.py). For every lift with a floor to ride near a party member this module
-- takes the smallest platform the floor fits in, puts it where WoW's car is, and repeats the car's moves: benilla
-- says where each lift is and where the keyframe span it is on ends, with the time left (link/wowlifts.bin, benilla
-- 0139), and a span is one straight move at one speed, which is exactly what PlatformMoveTo makes. WoW keeps the
-- timetable: nothing here calls or holds a car.
--
-- Reads: link/wowlifts.bin (WBL1, seq, count, count x (guid u64, entry u32, flags u32 (1 moving), period ms, cycle
-- ms, WoW xyz now, WoW xyz of the span's end, ms to it), seq again); link/lifts.json {Decks = {[entry] = {Radius,
-- Floor}}} (world_bridge.py: each car's floor from its own collision model, metres; a lift without one is a door);
-- link/world.json (Active, Centre: the mapping BG3 = Centre + (y, z, -x) * 0.9144).
-- A car keeps BG3's time, not the wall's (the maintainer, 2026-11-11: lifts are environment, they move on the environment's
-- turn and nobody is moved outside the turn order). BG3 does that to its own platforms: near a character in
-- turn-based mode, in a fight or not, a platform stands while the game waits on a turn and moves six seconds' worth
-- on the environment's turn (stock, measured). So the car's clock is run from here: while the car moves, from how far
-- its own platform has come along the span; while it rests, from a clock platform beside it (a platform with no
-- floor, kept moving at a known speed: how far BG3 moved it is how much of BG3's time passed there). The clock goes to
-- benilla every tick (link/liftclock.bin: WLC1, seq, count, count x (guid u64, cycle ms u32, rate u32: 1000 while
-- BG3's time runs there, 0 while it stands), seq), which draws the car and carries WoW's own character by it
-- (benilla 0140). A car far from the party has no platform and keeps WoW's own clock.
--
-- The platforms' floors go to the plugin every tick too (link/liftfloors.bin: WLF1, seq, count, count x (x, y, z,
-- radius), seq): BG3 asks its rays where a character stands and what a click points at, and a car is in none of
-- WoW's static faces.
--
-- Status: wow-bg3/lifts-status.json {RunId, Tick, Lifts = [{Entry, Guid, Key, Radius, At, Moving, Cycle, Flowing}], Free}.
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
M.CLOCKS = {Park = {{-15700, -60, -15020}, {-15660, -60, -15020}, {-15620, -60, -15020}, {-15580, -60, -15020}, {-15540, -60, -15020}, {-15500, -60, -15020}}, Keys = {"45b75e01-2586-5986-bfd3-a7d177dbe40a", "c97ad464-a11e-58d8-bcc5-730b742ebe7c", "92030418-9a9e-556d-8d48-3a6450ecaf0f", "bc822496-c35d-50e3-9e8b-4aed032b6601", "ced48ccd-c646-554d-9325-f31348cb71bf", "1adb2f70-f491-53e8-9765-44495c405bea"}}

M.NEAR = 160.0      -- metres from a party member within which a car gets its platform
M.FAR = 240.0       -- and beyond which the platform is parked again
M.FAST = 20000.0    -- metres a second for a platform nobody is on (from where it is parked to the car)
M.CLOCK_SPEED = 0.5  -- metres a second a clock platform is kept moving at
M.CLOCK_REACH = 4.0  -- metres it goes down and up again
M.CLOCK_UNDER = 3.0  -- metres under the car's floor where it turns
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

-- Where a moving car is in its cycle (ms), read from its platform: `spanEnd` the cycle at the end of the span it was
-- sent along, `remaining` the metres the platform still has to go, `speed` what it was sent at. Arrived (3 cm) is
-- the span's end itself.
function M.CycleOnSpan(spanEnd, remaining, speed)
    if remaining < 0.03 or not (speed and speed > 0) then return spanEnd end
    return spanEnd - remaining / speed * 1000
end

-- How much of BG3's time (ms) passed at a clock platform that went from y0 to y1 at M.CLOCK_SPEED; never more than
-- `most` (a platform brought back from elsewhere is no reading).
function M.Flow(y0, y1, most)
    return math.min(math.abs(y1 - y0) / M.CLOCK_SPEED * 1000, most)
end

-- link/liftclock.bin for benilla: rows of {Guid (16 hex digits), Cycle (ms), Rate}.
function M.Pack(seq, rows)
    local out = {"WLC1", string.pack("<I4I4", seq, #rows)}
    for _, r in ipairs(rows) do
        out[#out + 1] = string.pack("<I8I4I4", tonumber(r.Guid, 16), math.floor(r.Cycle) % 0x100000000, r.Rate)
    end
    out[#out + 1] = string.pack("<I4", seq)
    return table.concat(out)
end

-- link/liftfloors.bin for the plugin's rays: rows of {x, y, z, radius} in BG3 metres.
function M.Floors(seq, rows)
    local out = {"WLF1", string.pack("<I4I4", seq, #rows)}
    for _, r in ipairs(rows) do out[#out + 1] = string.pack("<ffff", r[1], r[2], r[3], r[4]) end
    out[#out + 1] = string.pack("<I4", seq)
    return table.concat(out)
end

function M.Start(runId)
    local ticks, world, decks, held, free, members = 0, nil, nil, {}, {}, {}
    local clocks, clockSeq, lastMs = {}, 0, nil
    for k = 1, #M.CLOCKS.Keys do clocks[k] = k end
    local function dist(a, b) return math.sqrt((a[1] - b[1]) ^ 2 + (a[2] - b[2]) ^ 2 + (a[3] - b[3]) ^ 2) end
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
        if h.Clock then
            local c = M.CLOCKS.Park[h.Clock]
            pcall(Osi.PlatformMoveTo, M.CLOCKS.Keys[h.Clock], c[1], c[2], c[3], M.FAST, "", 0)
            table.insert(clocks, h.Clock)
        end
        Ext.Utils.Print(string.format("WOW-BG3 lifts: lift %d (%s) left behind, its platform is parked", h.Entry, h.Guid))
    end
    local function status()
        local list, left = {}, 0
        for guid, h in pairs(held) do
            list[#list + 1] = {Entry = h.Entry, Guid = guid, Key = h.Key, Radius = M.PLATFORMS[h.Size].Radius, At = at(h.Key), Moving = h.Moving,
                Cycle = h.Cycle and math.floor(h.Cycle % h.Period) or nil, Flowing = h.Flowing, Clock = h.Clock}
        end
        for i = 1, #free do left = left + #free[i] end
        Ext.IO.SaveFile("wow-bg3/lifts-status.json", Ext.Json.Stringify({RunId = runId, Tick = ticks, Lifts = list, Free = left}))
    end
    -- The ms of BG3's time that passed this tick at a resting car whose platform is at `here`, read from its clock
    -- platform (none left for it: the wall's time). The clock is kept between M.CLOCK_UNDER and that plus
    -- M.CLOCK_REACH under the car's floor; while it is not there yet no time is counted (it and the car's platform
    -- are brought by the same kind of move, so BG3 holds both or neither).
    local function clockFlow(h, here, dt)
        local key = h.Clock and M.CLOCKS.Keys[h.Clock]
        local p = key and at(key)
        if not p then return dt end
        local top = here[2] - M.CLOCK_UNDER
        local home = {here[1], top, here[3]}
        local beside = math.abs(p[1] - home[1]) < 0.05 and math.abs(p[3] - home[3]) < 0.05
            and p[2] <= top + 0.05 and p[2] >= top - M.CLOCK_REACH - 0.05
        if not beside then
            if not h.ClockHome or dist(h.ClockHome, home) > 0.05 then
                h.ClockHome, h.ClockY, h.ClockTo = home, nil, nil
                pcall(Osi.PlatformMoveTo, key, home[1], home[2], home[3], M.FAST, "", 0)
            end
            return 0
        end
        h.ClockHome = home
        local flow = h.ClockY and M.Flow(h.ClockY, p[2], 4 * dt + 50) or 0
        h.ClockY = p[2]
        if not h.ClockTo or math.abs(p[2] - h.ClockTo) < 0.01 then
            h.ClockTo = (h.ClockTo and h.ClockTo < top - 0.5) and top or top - M.CLOCK_REACH
            pcall(Osi.PlatformMoveTo, key, home[1], h.ClockTo, home[3], M.CLOCK_SPEED, "", 0)
        end
        return flow
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        local nowMs = Ext.Utils.MonotonicTime()
        local dt = lastMs and math.min(nowMs - lastMs, 500) or 0
        lastMs = nowMs
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
                        h = {Entry = l.Entry, Guid = l.Guid, Size = size, Slot = slot, Key = M.PLATFORMS[size].Keys[slot], Span = nil,
                            Cycle = l.Cycle, Period = math.max(l.Period, 1), Clock = table.remove(clocks)}
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
                        local here = at(h.Key) or P
                        if span ~= h.Span or (not l.Moving and ticks >= (h.Check or 0)) then
                            local first = span ~= h.Span
                            h.Span, h.Check, h.Moving = span, ticks + 15, l.Moving
                            local o = M.Order(l, here, world.Centre, d.Floor)
                            if o then pcall(Osi.PlatformMoveTo, h.Key, o.To[1], o.To[2], o.To[3], o.Speed, "WOWBG3_Lift", 0) end
                            if first then
                                -- A moving span: its end in the car's cycle (benilla's record is of the clock sent to
                                -- it), the speed its platform was sent at, and the clock platform sent along to the
                                -- same stop, so it is there when the car rests again.
                                h.Speed, h.Left, h.SpanEnd = nil, nil, nil
                                if l.Moving and o then
                                    local e = h.Cycle - h.Cycle % h.Period + l.Cycle + l.ToMs
                                    while e < h.Cycle do e = e + h.Period end
                                    while e - h.Cycle > h.Period do e = e - h.Period end
                                    h.SpanEnd, h.Speed = e, o.Speed
                                    if h.Clock then
                                        h.ClockHome, h.ClockY, h.ClockTo = {o.To[1], o.To[2] - M.CLOCK_UNDER, o.To[3]}, nil, nil
                                        pcall(Osi.PlatformMoveTo, M.CLOCKS.Keys[h.Clock], o.To[1], o.To[2] - M.CLOCK_UNDER, o.To[3], o.Speed, "", 0)
                                    end
                                end
                            end
                        end
                        if l.Moving and h.SpanEnd then
                            local left = dist(here, M.ToBg3(l.To, world.Centre, d.Floor))
                            h.Flowing = h.Left ~= nil and math.abs(h.Left - left) > 1e-4
                            h.Left = left
                            h.Cycle = math.max(h.Cycle, M.CycleOnSpan(h.SpanEnd, left, h.Speed))
                            if left < 0.03 then h.Cycle = h.SpanEnd + 1 end
                        else
                            local flow = clockFlow(h, here, dt)
                            h.Cycle = h.Cycle + flow
                            h.Flowing = flow > 0.3 * dt
                        end
                    end
                end
            end
        end
        -- A lift the file no longer has (another WoW map): its platform is parked.
        for guid, h in pairs(held) do
            if not seen[guid] then park(h); held[guid] = nil; changed = true end
        end
        local rows = {}
        for guid, h in pairs(held) do
            rows[#rows + 1] = {Guid = guid, Cycle = h.Cycle % h.Period, Rate = h.Flowing and 1000 or 0}
        end
        clockSeq = clockSeq % 0x7fffffff + 1
        Ext.IO.SaveFile("wow-bg3/link/liftclock.bin", M.Pack(clockSeq, rows))
        -- Each platform's floor for BG3's rays, where it is now (the plugin: lift floors).
        local floors = {}
        for _, h in pairs(held) do
            local p = at(h.Key)
            if p and #floors < 16 then floors[#floors + 1] = {p[1], p[2], p[3], M.PLATFORMS[h.Size].Radius} end
        end
        Ext.IO.SaveFile("wow-bg3/link/liftfloors.bin", M.Floors(clockSeq, floors))
        if changed or ticks % 30 == 0 then status() end
    end)
end

return M
