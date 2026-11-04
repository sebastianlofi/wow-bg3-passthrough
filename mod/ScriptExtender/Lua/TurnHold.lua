-- TurnHold (server; party-multiplayer-design.md "Turn-based mode"). Design: turning on
-- BG3's turn-based mode outside combat stops every WoW creature and NPC near a member in that mode in place
-- (hostile, neutral, friendly, critters, patrols mid-path, their WoW models too); turning it off lets
-- them go. In combat nothing changes: the fight holds its own mobs as before.
--
-- This module only says where to hold (BG3 side of the backend contract, wow-backend-contract.md
-- "Turn-based hold"): wow-bg3/link/turnhold.json
--   {Schema = 1, RunId, Seq, Tick, Hold = true|false, Guids = [WoW guid, ...], Members = [{Uuid, Name,
--    Bg3 = {x, y, z}, Wow = {x, y, z} | nil}]}
-- Members: every party member (Osiris DB_Players) in turn-based mode (the eoc::ftb::IsInFtbComponent
-- tag, or the Osiris EnteredForceTurnBased/LeftForceTurnBased events) and in no combat. Hold is true
-- while there is at least one. Wow is the member's BG3 position in WoW yards through world mode's
-- mapping (link/world.json, the same one Mirrors.lua uses the other way); nil while world mode is off.
-- Seq goes up whenever the set changes: on and off, a member joins or leaves, or a member moved more
-- than M.Move metres from where it was last sent (so creatures that come near a walking member are
-- held too). Tick is rewritten every M.Keep ticks while held (a heartbeat: a backend releases when it
-- stops). A new Lua session (a load) starts with Hold = false, and so does any state that is not
-- Running or a held state (Session.lua: a save or a pause keeps the hold).
-- The backend (today the fight bridge, bridge/src, with the server's hold verbs, vmangos 0026) turns it
-- into "hold exactly these creatures", "let these go" and "release".
-- Guids: every WoW creature loaded round a member in turn-based mode (mirror.json Mobs, the list stand-ins
-- are made from, within WoW's visibility of that member, M.Seen; the list also holds what other members
-- far away see, and those stay free) and every creature whose stand-in BG3's turn-based mode froze
-- (WOWBG3MirrorOwned, Mirrors.lua: stand-in uuid -> WoW guid).
-- A creature in a fight stays the fight's. Seq goes up when that set changes too.
-- Why not BG3's list alone: its tag covers only what is close. What BG3 freezes was measured: every
-- character it takes into the party's turn-based mode gets the eoc::ftb::IsInFtbComponent tag (IsInFTB,
-- GetAllEntitiesWithComponent lists them) and ls::TimeFactor 0 (time stopped), the same as the party
-- has the tag with TimeFactor 1. It takes a character within about 18 m of a member or of a character
-- it already took, and so spreads along a chain (Durotar: 11.7 and 17.4 m in, 21.5 m out; one in at
-- 29.4 m, 12.2 m from a frozen boar; Elwynn: in up to 35 m, one out at 24.6 m). So in an open field
-- it froze 1 to 4 of 29 boars, and the rest walked on in WoW's picture. SightRangeOverride on the
-- member or the stand-ins, Sight.Sight, ForceTurnBasedMode on a stand-in and
-- ZoneSystem.EnterExistingSharedFTB widened nothing. FTBParticipant is on every character in or out
-- of turn-based mode, and FTBPaused was on none, so neither says frozen. Stats.Bg3 records each check:
-- stand-ins, how many BG3 froze, the farthest frozen and the nearest left free (WoW yards from the
-- nearest member), and the turn-based zones' fields; Stats.Loaded how many creatures mirror.json lists.
-- The world's turn (a playtest report: the environment's turn came and nothing in WoW moved): BG3
-- pauses the world only while the party takes its turns; once every member in turn-based mode has
-- ended its turn, the turn order shows the environment's turn and the rest of the world runs for that
-- round. Force Turn-Based raises no TurnStarted events, so the client tells it from the party's view
-- models (M.StartClient): someone in turn-based mode and nobody's turn (IsCurrentTurn) for
-- M.WorldTicks checks in a row is the world's turn. It writes wow-bg3/link/ftbturn.txt, "<seq> world" or
-- "<seq> party", on a change; while it says world this file releases the hold (Hold false) and holds
-- again when the party's turn comes back. A companion can be in the mode without the hero, so the client
-- goes by link/ftbparty.txt, this file's word on whether any member is ("1" or "0").
local M = {}
local Hero = Ext.Require("Hero.lua")
local Session = Ext.Require("Session.lua")

local OUT = "wow-bg3/link/turnhold.json"
local YD = 0.9144
M.Seen = 100   -- WoW yards round a member: WoW's visibility on continents (mirror.rs FEED_RANGE)
M.Move = 4.0   -- metres a member walks before the points are sent again
M.Every = 6    -- ticks between checks
M.Keep = 150   -- ticks between heartbeats while held (about 5 s)
M.On = true    -- research knob

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end

local function readJson(path)
    local raw = Ext.IO.LoadFile(path)
    if not raw or #raw > 262144 then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end

local function players()
    local out = {}
    for _, row in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
        local u = uuidOf(tostring(type(row) == "table" and row[1] or row))
        if u then out[#out + 1] = u end
    end
    return out
end

local function position(uuid)
    local ok, x, y, z = pcall(Osi.GetPosition, uuid)
    if ok and type(x) == "number" then return {x, y, z} end
end

-- Whether BG3's turn-based mode froze a stand-in (see the header: IsInFTB, time factor 0); nil: no entity.
local function ftbOf(uuid)
    local e = q(Ext.Entity.Get, uuid)
    if not e then return nil end
    local tf = q(function() return e.TimeFactor and e.TimeFactor.Value end)
    return {InFtb = q(function() return e.IsInFTB ~= nil end) == true, Stopped = type(tf) == "number" and tf == 0}
end

-- The WoW guids of the stand-ins BG3's turn-based mode froze (sorted), and the census behind them
-- (see the header). Stand-ins: WOWBG3MirrorOwned (Mirrors.lua, uuid -> guid).
local function bg3Frozen(members)
    local far, near, census, guids = 0.0, nil, {StandIns = 0, InFtb = 0, Stopped = 0, Frozen = 0}, {}
    for uuid, guid in pairs(WOWBG3MirrorOwned or {}) do
        local f, p = ftbOf(uuid), position(uuid)
        if f and p then
            census.StandIns = census.StandIns + 1
            if f.InFtb then census.InFtb = census.InFtb + 1 end
            if f.Stopped then census.Stopped = census.Stopped + 1 end
            local d = math.huge
            for _, m in ipairs(members) do
                local b = m.Bg3
                d = math.min(d, math.sqrt((p[1] - b[1]) ^ 2 + (p[2] - b[2]) ^ 2 + (p[3] - b[3]) ^ 2) / YD)
            end
            if f.InFtb or f.Stopped then
                census.Frozen = census.Frozen + 1
                far = math.max(far, d)
                if type(guid) == "string" and guid:match("^%x+$") then guids[#guids + 1] = guid end
            elseif not near or d < near then
                near = d
            end
        end
    end
    census.FarthestFrozenYd = census.Frozen > 0 and math.floor(far * 10 + 0.5) / 10 or nil
    census.NearestFreeYd = near and math.floor(near * 10 + 0.5) / 10 or nil
    -- The zones themselves (esv::ftb::ZoneComponent; field_1C read 6.0 in the measurement, not the
    -- 35 m reach: its meaning is unknown, kept in the census).
    local zones = {}
    for _, z in ipairs(q(Ext.Entity.GetAllEntitiesWithComponent, "FTBZone") or {}) do
        local c = q(function() return z.FTBZone end)
        if c then
            zones[#zones + 1] = {F1C = q(function() return c.field_1C end), F8 = q(function() return c.field_8 end),
                F18 = q(function() return c.field_18 end), ByPlayer = q(function() return c.ByPlayer end),
                Shared = q(function() return c.Shared end)}
        end
    end
    census.Zones = zones
    table.sort(guids)
    return guids, census
end

-- What the hold takes (see the header): the creatures BG3 froze plus the ones loaded round a member in
-- the mode and outside a fight;
-- sorted, so the key below only changes when the set does. Second value: how many WoW has loaded.
local function heldGuids(frozen, members)
    local seen, out = {}, {}
    local function add(g)
        if not seen[g] then seen[g] = true; out[#out + 1] = g end
    end
    for _, g in ipairs(frozen) do add(g) end
    local plan = readJson("wow-bg3/link/mirror.json")
    local mobs = plan and plan.Active == true and type(plan.Mobs) == "table" and plan.Mobs or {}
    local fight = {}
    for _, g in ipairs(plan and type(plan.Fight) == "table" and plan.Fight or {}) do fight[g] = true end
    local reach = (M.Seen * YD) ^ 2
    local function seen(p)
        if type(p) ~= "table" or #p ~= 3 then return false end
        for _, b in ipairs(members) do
            if (p[1] - b.Bg3[1]) ^ 2 + (p[2] - b.Bg3[2]) ^ 2 + (p[3] - b.Bg3[3]) ^ 2 <= reach then return true end
        end
        return false
    end
    for _, m in ipairs(mobs) do
        local g = m.Guid
        if type(g) == "string" and g:match("^%x+$") and not fight[g] and seen(m.Pos) then add(g) end
    end
    table.sort(out)
    return out, #mobs
end

-- BG3 metres -> WoW yards, world mode's mapping (WorldMode.lua toWow); nil when world mode is off.
local function toWow(world, p)
    if not world then return nil end
    local C, W = world.Centre, world.WowOrigin
    return {W[1] - (p[3] - C[3]) / YD, W[2] + (p[1] - C[1]) / YD, W[3] + (p[2] - C[2]) / YD}
end

function M.Start(runId)
    local ticks, seq, held, sent, lastWrite = 0, 0, false, {}, -100000
    local events = {} -- uuid -> true/false from the Osiris events (the tag is checked too)
    local world, worldAt = nil, -1000
    local stats = {Holds = 0, Releases = 0, Updates = 0}
    M.State = {Stats = stats}

    for _, ev in ipairs({"EnteredForceTurnBased", "LeftForceTurnBased"}) do
        pcall(Ext.Osiris.RegisterListener, ev, 1, "after", function(o)
            local u = uuidOf(tostring(o))
            if u then events[u] = ev == "EnteredForceTurnBased" end
        end)
    end

    local function inFtb(uuid)
        local e = q(Ext.Entity.Get, uuid)
        local tag = e and q(function() return e.IsInFTB ~= nil end)
        if tag ~= nil then return tag == true end
        return events[uuid] == true
    end

    local frozen, frozenKey = {}, "" -- the guids last written, and them joined (to see a change)
    local function write(members)
        lastWrite = ticks
        Ext.IO.SaveFile(OUT, Ext.Json.Stringify({Schema = 1, RunId = runId, Seq = seq, Tick = ticks, Hold = held,
            Guids = held and frozen or {}, Members = members, Stats = stats}))
    end

    -- Whether the members differ from what was last sent (membership, or one moved more than M.Move).
    local function changed(members)
        if #members ~= #sent then return true end
        local was = {}
        for _, m in ipairs(sent) do was[m.Uuid] = m.Bg3 end
        for _, m in ipairs(members) do
            local p = was[m.Uuid]
            if not p or (p[1] - m.Bg3[1]) ^ 2 + (p[2] - m.Bg3[2]) ^ 2 + (p[3] - m.Bg3[3]) ^ 2 > M.Move * M.Move then
                return true
            end
        end
        return false
    end

    local function release(why)
        if not held then return end
        held, sent, seq = false, {}, seq + 1
        stats.Releases = stats.Releases + 1
        write({})
        Ext.Utils.Print("WOW-BG3 turnhold: released (" .. why .. ")")
    end

    -- Whether any member is in the mode, for the client half (see the header).
    local partyWord
    local function sayParty(on)
        local word = on and "1" or "0"
        if word ~= partyWord then
            partyWord = word
            Ext.IO.SaveFile("wow-bg3/link/ftbparty.txt", word)
        end
    end

    -- A new Lua session (game start or a load): nothing held from before.
    write({})
    sayParty(false)
    -- The world's turn (see the header): the client's word, written in this session.
    local turnSeen = Ext.IO.LoadFile("wow-bg3/link/ftbturn.txt")
    local function worldTurn()
        local raw = Ext.IO.LoadFile("wow-bg3/link/ftbturn.txt")
        if raw == turnSeen then return false end -- nothing written since the load
        turnSeen = nil
        return raw ~= nil and raw:match("^%d+ world") ~= nil
    end

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % M.Every ~= 0 then return end
        local state = Session.State()
        if state ~= "Running" then
            if not Session.Held(state) then release("state " .. state) end
            return
        end
        local ok, err = pcall(function()
            if ticks - worldAt >= 30 then
                worldAt = ticks
                local w = readJson("wow-bg3/link/world.json")
                world = (w and w.Active == true and type(w.Centre) == "table" and type(w.WowOrigin) == "table") and w or nil
            end
            local members = {}
            if M.On then
                for _, u in ipairs(players()) do
                    if inFtb(u) and q(Osi.IsInCombat, u) ~= 1 and q(Osi.IsDead, u) ~= 1 then
                        local p = position(u)
                        if p then
                            members[#members + 1] = {Uuid = u, Bg3 = p, Wow = toWow(world, p),
                                Name = q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(u)) end),
                                Hero = Hero.Is(u) or nil}
                        end
                    end
                end
            end
            sayParty(#members > 0)
            if #members == 0 then
                release(M.On and "nobody in turn-based mode out of combat" or "off")
                return
            end
            if worldTurn() then
                if held then stats.WorldTurns = (stats.WorldTurns or 0) + 1 end
                release("the world's turn")
                return
            end
            local bg3, census = bg3Frozen(members)
            stats.Bg3 = census
            local guids, loaded = heldGuids(bg3, members)
            stats.Loaded = loaded
            local key = table.concat(guids, ",")
            local other = key ~= frozenKey
            frozen, frozenKey = guids, key
            if not held or changed(members) or other then
                local first = not held
                held, sent, seq = true, members, seq + 1
                if first then stats.Holds = stats.Holds + 1 else stats.Updates = stats.Updates + 1 end
                write(members)
                if first then
                    local names = {}
                    for _, m in ipairs(members) do names[#names + 1] = tostring(m.Name) end
                    Ext.Utils.Print(string.format("WOW-BG3 turnhold: hold %d creatures (BG3 froze %d) round %s", #guids, #bg3, table.concat(names, ", ")))
                end
            elseif ticks - lastWrite >= M.Keep then
                write(sent)
            end
        end)
        if not ok then Ext.Utils.Print("WOW-BG3 turnhold: " .. tostring(err)) end
    end)
end

-- The client half (see the header): the party's turn or the world's, from the view models.
M.WorldTicks = 3 -- checks (every 2 ticks)
function M.StartClient(runId)
    local Probe = Ext.Require("ActionProbe.lua")
    local ticks, quiet, said, seq = 0, 0, nil, 0
    -- BG3's UI is read only in a running game and only while a member is in turn-based mode (the
    -- server half's word): reading the view models every tick through the splash screen crashed BG3 in
    -- the Script Extender's UI bridge (harness).
    local function memberInFtb()
        return Ext.IO.LoadFile("wow-bg3/link/ftbparty.txt") == "1"
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 2 ~= 0 then return end
        if Ext.Utils.GetGameState() ~= "Running" then return end
        local okH, inFtb = pcall(memberInFtb)
        if not (okH and inFtb) then
            quiet = 0
            if said ~= "party" and said ~= nil then
                said = "party"; seq = seq + 1
                Ext.IO.SaveFile("wow-bg3/link/ftbturn.txt", string.format("%d party\n", seq))
            end
            return
        end
        local ok, ftb, turn = pcall(function()
            local list = Probe.FindUnique("HotBar", "HotBar").DataContext.CurrentPlayer.AssignedCharacters
            local inFtb, anyTurn = false, false
            for i = 1, 16 do
                local c = list[i]
                if c == nil then break end
                if tostring(c.CharacterMode) == "InFTB" then inFtb = true end
                if c.IsCurrentTurn == true then anyTurn = true end
            end
            return inFtb, anyTurn
        end)
        if not ok then return end
        quiet = (ftb and not turn) and quiet + 1 or 0
        local now = (ftb and quiet >= M.WorldTicks) and "world" or "party"
        if now ~= said then
            said = now
            seq = seq + 1
            Ext.IO.SaveFile("wow-bg3/link/ftbturn.txt", string.format("%d %s\n", seq, now))
        end
    end)
end

return M
