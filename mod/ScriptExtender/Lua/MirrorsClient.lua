-- Mirrored WoW mobs on BG3's client (found in desktop
-- playtests). The client copy of a stand-in is what BG3 draws its selection ring, hover and vision cone at
-- and what BG3's cursor picks, so it has to stand where WoW draws the mob, every frame.
--
-- Before: the server copy was teleported to the mob every 10 server ticks once 0.35 m off, from
-- positions that went benilla mobs.txt (every 12 WoW frames), the bridge's mirror.json (every
-- 300 ms) and Mirrors.lua: in the harness the client copy trailed the WoW model by 380 to 470 ms
-- (0.8 yd at a walk, 2.1 yd at most), stood still for 98% of the frames and jumped up to 7.7 yd.
-- Its rotation never followed at all: BG3's Steering turns only the server copy, so the client
-- copy kept its spawn or last-teleport rotation (90 to 117 degrees off its model; its cone pointed
-- there).
--
-- Now benilla writes link/wowmobs.bin every WoW frame (benilla 0025: each mob where WoW draws it,
-- its velocity, held by a fight or not) and every client tick this file puts the client copy of
-- each idle stand-in there: the record moved on by its velocity for its age (plus M.LeadMs), mapped
-- with world mode's mapping, and turned to the mob's facing (a character looks along its rotated -z).
-- The server copy follows in Mirrors.lua (every server tick, BG3's own teleport). BG3's client
-- applies such a replicated move in ecl::CharacterManager's update, after this tick and before the
-- frame is drawn: in the harness the client copy was then 0.05 to 0.37 m behind its model at the end
-- of 17% of the frames while it walked. So the same placement is applied again right after that
-- system's update (Ext.Entity.OnSystemPostUpdate, M.HookSystem): 0 of 174 frames over 0.05 m.
-- Left alone: stand-ins in a BG3 combat or dead (BG3 runs them, LinkLive streams them to WoW), mobs
-- a fight holds, and everything without a fresh record (the server's placement shows). Also a robbed
-- NPC searching for the thief (mirror-status.json Searching, Mirrors.lua): BG3's crime reaction walks
-- its stand-in, and outside a fight this file streams it to benilla the way LinkLive streams a fight's
-- enemies (standins.bin, the same sequence; benilla 0067 holds its model there).
-- Status: wow-bg3/link/mirrors-client-status.json every 60 ticks.
local M = {}
local World = Ext.Require("WorldMode.lua")
local DIR = "wow-bg3/link/"
local FRESH_MS, LEAD_MAX = 250, 0.2 -- record age (ms) that still places; seconds a record may be moved on
M.On = true -- research knobs
M.LeadMs = 0
-- The client system after whose update the placements are applied again (see the header);
-- M.Hook(system) moves it, M.Unhook() ends it; stats.Reapplied counts the runs, HookFixed the fixes.
M.HookSystem = "ecl::CharacterManager"
local hookId

function M.Start(runId)
    local ticks = 0
    local owned, ownedAt = {}, -1000 -- stand-in uuid -> {guid, skip}
    local recs, seq, recT = {}, nil, 0
    local last = {} -- uuid -> the position and rotation written last tick
    local stats = {Placed = 0, Overwritten = 0, OverMax = 0, Skipped = 0, Stale = 0, CostMs = 0, CostMax = 0, Ticks = 0}
    -- Which stand-ins exist and which BG3 runs (mirror-status.json, Mirrors.lua's).
    local function readOwned()
        local raw = Ext.IO.LoadFile(DIR .. "mirror-status.json")
        if not raw then return end
        local ok, st = pcall(Ext.Json.Parse, raw)
        if not ok or type(st) ~= "table" or type(st.Mobs) ~= "table" then return end
        local out = {}
        for g, m in pairs(st.Mobs) do
            if type(m) == "table" and type(m.Uuid) == "string" then
                out[m.Uuid] = {Guid = g, Skip = m.Dead == true or m.InCombat == true or m.Searching == true,
                    Searching = m.Searching == true and m.Dead ~= true}
            end
        end
        owned = out
    end
    -- link/wowmobs.bin (benilla 0025): "WMB1", seq u32, n u32, CLOCK_MONOTONIC ms f64, n x (guid hi
    -- u32, guid lo u32, WoW x, y, z, orientation, velocity x, y, z f32, flags u32: 1 held by a BG3
    -- fight, 2 moving), seq again.
    local function readLive()
        local raw = Ext.IO.LoadFile(DIR .. "wowmobs.bin")
        if not raw or #raw < 24 or raw:sub(1, 4) ~= "WMB1" then return end
        local s, n, t = string.unpack("<I4I4d", raw, 5)
        if s == seq or n > 512 or #raw ~= 24 + 40 * n or string.unpack("<I4", raw, 21 + 40 * n) ~= s then return end
        local out = {}
        for i = 0, n - 1 do
            local hi, lo, x, y, z, o, vx, vy, vz, fl = string.unpack("<I4I4fffffffI4", raw, 21 + 40 * i)
            out[string.format("%08x%08x", hi, lo)] = {x, y, z, o, vx, vy, vz, fl}
        end
        recs, seq, recT = out, s, t
    end
    -- A stand-in BG3 runs right now (its combat, its death), read on the client every tick.
    local function busy(e)
        local ok, v = pcall(function()
            if e.Health and e.Health.Hp <= 0 then return true end
            local tb = e.TurnBased
            return tb ~= nil and tb.IsInCombat_M == true
        end)
        return not ok or v == true
    end
    local function place()
        -- New stand-ins are placed within 5 ticks of Mirrors.lua reporting them.
        if ticks - ownedAt >= 5 then ownedAt = ticks; readOwned() end
        readLive()
        if not seq then return end
        local age = Ext.Utils.MonotonicTime() - recT
        if age > FRESH_MS then
            -- No fresh record (benilla paused or gone): the server's placement shows, nothing re-applied.
            stats.Stale = stats.Stale + 1
            last = {}
            return
        end
        local dt = math.max(-LEAD_MAX, math.min(LEAD_MAX, (age + M.LeadMs) / 1000))
        -- Stand-ins Mirrors.lua no longer lists are not placed again (nor after CharacterManager).
        for uuid in pairs(last) do if not owned[uuid] then last[uuid] = nil end end
        for uuid, o in pairs(owned) do
            local r = recs[o.Guid]
            local e = r and not o.Skip and (r[8] & 1) == 0 and Ext.Entity.Get(uuid)
            if e and not busy(e) then
                local b = World.ToBg3({r[1] + r[5] * dt, r[2] + r[6] * dt, r[3] + r[7] * dt})
                local t = e.Transform.Transform
                local l = last[uuid]
                if l then
                    -- Something else moved it since our last write (the server's teleport replicated).
                    local c = t.Translate
                    local d = math.sqrt((c[1] - l[1]) ^ 2 + (c[3] - l[3]) ^ 2)
                    if d > 0.002 then
                        stats.Overwritten = stats.Overwritten + 1
                        if d > stats.OverMax then stats.OverMax = d end
                    end
                end
                -- WoW orientation o faces BG3's (sin o, -cos o); yaw a turns -z to (-sin a, -cos a): a = -o.
                local a = -r[4]
                local qy, qw = math.sin(a / 2), math.cos(a / 2)
                t.Translate = {b[1], b[2], b[3]}
                t.RotationQuat = {0.0, qy, 0.0, qw}
                last[uuid] = {b[1], b[2], b[3], qy, qw}
                stats.Placed = stats.Placed + 1
            else
                last[uuid] = nil
                stats.Skipped = stats.Skipped + 1
            end
        end
    end
    -- Robbed NPCs searching (see the header): where BG3 draws them, for benilla, while no fight streams.
    local searchLast = {}
    local function publishSearch()
        if WOWBG3SinAt and Ext.Utils.MonotonicTime() - WOWBG3SinAt < 1000 then return end -- a fight writes it
        local parts = {}
        for uuid, o in pairs(owned) do
            local e = o.Searching and Ext.Entity.Get(uuid)
            local t = e and e.Transform and e.Transform.Transform
            local g = o.Guid and string.rep("0", 16 - #o.Guid) .. o.Guid
            if t and g and #g == 16 then
                local p, r = t.Translate, t.RotationQuat
                local w = World.ToWow({p[1], p[2], p[3]})
                -- Facing as LinkLive.lua's: along the rotated -z, WoW's o = atan(-fx, fz).
                local x, y, z, qw = r[1], r[2], r[3], r[4]
                local fx, fz = 2 * (x * z + qw * y), 1 - 2 * (x * x + y * y)
                local l = searchLast[uuid]
                local moving = l and (math.abs(p[1] - l[1]) + math.abs(p[3] - l[3]) > 0.005)
                searchLast[uuid] = {p[1], p[2], p[3]}
                parts[#parts + 1] = string.pack("<I4I4ffffI4", tonumber(g:sub(1, 8), 16), tonumber(g:sub(9, 16), 16),
                    w[1], w[2], w[3], math.atan(-fx, fz), moving and 2 or 0)
            end
        end
        if #parts == 0 then return end
        WOWBG3SinSeq = (WOWBG3SinSeq or 0) + 1
        Ext.IO.SaveFile(DIR .. "standins.bin", string.pack("<c4I4I4d", "SIN1", WOWBG3SinSeq, #parts, Ext.Utils.MonotonicTime() * 1.0)
            .. table.concat(parts) .. string.pack("<I4", WOWBG3SinSeq))
        stats.Searching = #parts
    end
    local function reapply()
        stats.Reapplied = (stats.Reapplied or 0) + 1
        for uuid, b in pairs(last) do
            local e = Ext.Entity.Get(uuid)
            if e then
                local t = e.Transform.Transform
                local c, r = t.Translate, t.RotationQuat
                if math.abs(c[1] - b[1]) + math.abs(c[3] - b[3]) > 0.002
                    or math.abs(r[2] - b[4]) + math.abs(r[4] - b[5]) > 0.002 then
                    stats.HookFixed = (stats.HookFixed or 0) + 1
                    t.Translate = {b[1], b[2], b[3]}
                    t.RotationQuat = {0.0, b[4], 0.0, b[5]}
                end
            end
        end
    end
    function M.Hook(system)
        if hookId then pcall(Ext.Entity.Unsubscribe, hookId) end
        hookId = Ext.Entity.OnSystemPostUpdate(system, function() pcall(reapply) end)
        stats.Hook = system
        return hookId
    end
    function M.Unhook()
        if hookId then pcall(Ext.Entity.Unsubscribe, hookId) end
        hookId, stats.Hook = nil, nil
    end
    function M.Stats() return stats end
    local hookTried = -1000
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if not M.On or Ext.Utils.GetGameState() ~= "Running" or not World.IsActive() or not World.ToBg3 then
            if next(last) then last = {} end -- nothing re-applied outside world mode
            return
        end
        -- The system hook needs the level's world: taken once world mode runs, retried every 2 s.
        if M.HookSystem and not hookId and ticks - hookTried > 160 then
            hookTried = ticks
            local ok, err = pcall(M.Hook, M.HookSystem)
            stats.HookError = (not ok) and tostring(err) or nil
        end
        local t0 = Ext.Utils.MonotonicTime()
        local ok, err = pcall(place)
        if not ok then stats.Error = tostring(err) end
        local okS, errS = pcall(publishSearch)
        if not okS then stats.SearchError = tostring(errS) end
        local ms = Ext.Utils.MonotonicTime() - t0
        stats.Ticks = stats.Ticks + 1
        stats.CostMs = stats.CostMs + ms
        if ms > stats.CostMax then stats.CostMax = ms end
        if ticks % 60 == 0 then
            stats.RunId, stats.Seq, stats.Age = runId, seq, seq and (Ext.Utils.MonotonicTime() - recT) or nil
            stats.CostAvg = stats.Ticks > 0 and stats.CostMs / stats.Ticks or 0
            Ext.IO.SaveFile(DIR .. "mirrors-client-status.json", Ext.Json.Stringify(stats))
        end
    end)
end
return M
