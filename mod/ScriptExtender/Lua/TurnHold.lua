-- TurnHold (server; party-multiplayer-design.md "Turn-based mode"). Design: turning on
-- BG3's turn-based mode outside combat stops every WoW creature and NPC near any party member in place
-- (hostile, neutral, friendly, critters, patrols mid-path, their WoW models too); turning it off lets
-- them go. In combat nothing changes: the fight holds its own mobs as before.
--
-- This module only says where to hold (BG3 side of the backend contract, wow-backend-contract.md
-- "Turn-based hold"): wow-bg3/link/turnhold.json
--   {Schema = 1, RunId, Seq, Tick, Hold = true|false, Radius (WoW yards), Members = [{Uuid, Name,
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
-- The backend (today the fight bridge, link/src, with the server's hold-area verb) turns it into
-- "hold every creature within Radius of these points" and "release".
-- The world's turn (a playtest report: the environment's turn came and nothing in WoW moved): BG3
-- pauses the world only while the party takes its turns; once every member in turn-based mode has
-- ended its turn, the turn order shows the environment's turn and the rest of the world runs for that
-- round. Force Turn-Based raises no TurnStarted events, so the client tells it from the party's view
-- models (M.StartClient): someone in turn-based mode and nobody's turn (IsCurrentTurn) for
-- M.WorldTicks checks in a row is the world's turn. It writes wow-bg3/link/ftbturn.txt, "<seq> world" or
-- "<seq> party", on a change; while it says world this file releases the hold (Hold false) and holds
-- again when the party's turn comes back.
local M = {}
local Hero = Ext.Require("Hero.lua")
local Session = Ext.Require("Session.lua")

local OUT = "wow-bg3/link/turnhold.json"
local YD = 0.9144
M.Radius = 50  -- WoW yards around each member (the mirrors reach 45 yd; BG3's view in world mode)
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

    local function write(members)
        lastWrite = ticks
        Ext.IO.SaveFile(OUT, Ext.Json.Stringify({Schema = 1, RunId = runId, Seq = seq, Tick = ticks, Hold = held,
            Radius = M.Radius, Members = members, Stats = stats}))
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

    -- A new Lua session (game start or a load): nothing held from before.
    write({})
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
            if #members == 0 then
                release(M.On and "nobody in turn-based mode out of combat" or "off")
                return
            end
            if worldTurn() then
                if held then stats.WorldTurns = (stats.WorldTurns or 0) + 1 end
                release("the world's turn")
                return
            end
            if not held or changed(members) then
                local first = not held
                held, sent, seq = true, members, seq + 1
                if first then stats.Holds = stats.Holds + 1 else stats.Updates = stats.Updates + 1 end
                write(members)
                if first then
                    local names = {}
                    for _, m in ipairs(members) do names[#names + 1] = tostring(m.Name) end
                    Ext.Utils.Print(string.format("WOW-BG3 turnhold: hold %d yd round %s", M.Radius, table.concat(names, ", ")))
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
    -- BG3's UI is read only in a running game and only while the hero is in turn-based mode (her
    -- IsInFTB tag): reading the view models every tick through the splash screen crashed BG3 in the
    -- Script Extender's UI bridge (harness).
    local function heroInFtb()
        local e = Ext.Entity.Get(Hero.Uuid())
        return e ~= nil and e.IsInFTB ~= nil
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 2 ~= 0 then return end
        if Ext.Utils.GetGameState() ~= "Running" then return end
        local okH, inFtb = pcall(heroInFtb)
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
