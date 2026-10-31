-- Camp (server): BG3's camp in world mode, with a trip there and back. Behind the switch `camp`
-- (wow-bg3/camp.json {"On": true}, written by the coordinator from settings.conf); off, CampOff.lua keeps
-- the party out of camp as before.
--
-- Why camp broke the WoW world (a playtest report: a random spot of the WoW map): the camp of the host
-- level BGO_Main_A is BG3's Act 3 FARM camp (Act3_Camp, entrance near 384, 6, -752), in the same level as
-- the void (centre -2600, 120, -1000). No level swap happens. BG3's Go to Camp (GLO_Camp:
-- PROC_TeleportToFromCamp) is a plain teleport, so WorldMode.lua kept publishing the hero, mapped from the
-- void as ever: about 3.3 km from the spot left. Then:
--   * benilla put the WoW body there ("placed on BG3's character", any big gap);
--   * the bridge moved the hero's window of squares there, cutting WoW ground over the camp;
--   * BG3's camera, leading, took WoW's camera there too;
--   * the squares left the void, so at Leave Camp the story's FindValidPosition found no ground at the
--     hero's return spot (DB_LeaveCamp_ReturnPos) and took its fallbacks.
-- Party.lua's upkeep also read a companion at camp (y 6) as one that fell from the void (y 120).
--
-- The trip: while the hero is at camp, WoW and the void wait. WorldMode.lua (client) keeps publishing the
-- spot the hero left, so the WoW body stands still, the squares stay placed and the bridge has nothing to
-- do; BG3's lead of WoW's camera is off and the bridge moves the compositor's file aside, so camp is BG3's
-- own picture. Hostile mobs do not notice the standing body: sight mode stays armed, it only ends with
-- world mode. On Leave Camp the story's own return works (its spot still has ground) and the party is back
-- on the same squares; this module checks the hero stands there and puts the hero there if not.
--
-- wow-bg3/camp-state.json {RunId, Seq, InCamp, Hero, From, Back} is the one record of a trip, for
-- WorldMode.lua, world_bridge.py and any module that needs it (M.InTrip() on the server).
--
-- Long rests are BG3's own: End the Day gathers the party at camp through the same teleport (the same
-- trip), BG3 checks and spends the supplies, the night and morning run as in BG3, and every rest effect
-- with them. Supplies are WoW food and drink (Inventory.lua, wow_items.py `sv`).
local M = {}
local Hero = Ext.Require("Hero.lua")

M.JUMP = 150.0        -- metres in one teleport: a trip's start, never a walk
M.PENDING = 180       -- ticks a teleported hero may take to count as in camp
M.BACK_NEAR = 2.0     -- metres from the spot left: farther and the hero is put there
M.LEFT_AFTER = 30     -- ticks out of DB_PlayerInCamp before the trip counts as over

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end

-- The switch, read once per Lua session on each side (a new setting takes a restart).
local onCached = nil
function M.On()
    if onCached == nil then
        local raw = q(Ext.IO.LoadFile, "wow-bg3/camp.json")
        local ok, c = pcall(Ext.Json.Parse, raw or "")
        onCached = ok and type(c) == "table" and c.On == true
    end
    return onCached
end

local trip = nil
function M.InTrip() return trip ~= nil end

local function worldActive()
    local raw = Ext.IO.LoadFile("wow-bg3/link/world.json")
    if not raw then return false end
    local ok, w = pcall(Ext.Json.Parse, raw)
    return ok and type(w) == "table" and w.Active == true
end

local function inCampDb(hero)
    local rows = q(function() return Osi.DB_PlayerInCamp:Get(hero) end)
    return type(rows) == "table" and #rows > 0
end

-- BG3's camp nights are its story (GLO_CampNights picks one at every End the Day): at this Act 3 camp they
-- played Orin's murder of Zevlor in Elwynn. No story runs here, so none stays queued, as BG3's own debug word
-- "nonight" does; rows the story adds later go at the next check. The rest itself goes on without them.
local function noNights()
    for _, r in ipairs(q(function() return Osi.DB_CampNight:Get(nil, nil) end) or {}) do
        pcall(function() Osi.DB_CampNight:Delete(r[1], r[2]) end)
    end
    for _, r in ipairs(q(function() return Osi.DB_CampNight:Get(nil, nil, nil) end) or {}) do
        pcall(function() Osi.DB_CampNight:Delete(r[1], r[2], r[3]) end)
    end
end

function M.Start(runId)
    if not M.On() then return end
    local ticks, seq, pending, outFor = 0, 0, nil, 0
    local own = nil -- where this module just sent the hero: its Teleported arrives ticks later, no one's travel
    M.State = {Trips = 0, Corrected = 0}
    local function publish(inCamp, extra)
        seq = seq + 1
        local s = {RunId = runId, Seq = seq, InCamp = inCamp, Hero = trip and trip.Hero or nil,
            From = trip and trip.From or nil}
        for k, v in pairs(extra or {}) do s[k] = v end
        Ext.IO.SaveFile("wow-bg3/camp-state.json", Ext.Json.Stringify(s))
    end
    -- Nothing left from an earlier session (a load starts a new one).
    publish(false)

    -- Fires before the frame's client tick reads the state (server and client share the process, and the
    -- client holds its publish over a jump until it hears: WorldMode.lua).
    local function onTeleported(who, _, ox, oy, oz, nx, ny, nz, reason)
        if trip or reason == "RegionSwap" or not worldActive() then return end
        local hero = Hero.Uuid()
        if uuidOf(who) ~= hero then return end
        local dx, dz = nx - ox, nz - oz
        if math.sqrt(dx * dx + dz * dz) < M.JUMP then return end
        local mine = own and ticks <= own.Until and math.sqrt((nx - own.X) ^ 2 + (nz - own.Z) ^ 2) < M.JUMP
        own = nil
        pending = {Hero = hero, From = {ox, oy, oz}, Until = ticks + M.PENDING, Own = mine}
    end
    pcall(Ext.Osiris.RegisterListener, "Teleported", 9, "before", onTeleported)

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 5 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, err = pcall(function()
            if ticks % 30 == 0 then noNights() end
            if pending then
                if inCampDb(pending.Hero) then
                    trip, pending = {Hero = pending.Hero, From = pending.From}, nil
                    outFor = 0
                    M.State.Trips = M.State.Trips + 1
                    publish(true)
                    Ext.Utils.Print("WOW-BG3 camp: at camp, the WoW side waits at "
                        .. Ext.Json.Stringify(trip.From))
                elseif ticks > pending.Until and pending.Own then
                    pending = nil
                elseif ticks > pending.Until then
                    -- A long teleport that is not to camp: BG3's own waypoint panel, no longer greyed with
                    -- CampOff's status gone. Its target is in the level, not on WoW's ground, and the next
                    -- record would move the WoW body there, so the party goes back. Moves on the WoW map
                    -- are never this: they run with world.json Active false.
                    local f, pendingHero = pending.From, pending.Hero
                    pending = nil
                    own = {X = f[1], Z = f[3], Until = ticks + M.PENDING}
                    Osi.TeleportToPosition(pendingHero, f[1], f[2], f[3], "", 1, 1, 1, 0, 1)
                    M.State.Refused = (M.State.Refused or 0) + 1
                    Ext.Utils.Print("WOW-BG3 camp: BG3's own travel in the level undone, the party back on WoW's ground")
                end
            elseif trip then
                outFor = inCampDb(trip.Hero) and 0 or outFor + 5
                if outFor >= M.LEFT_AFTER then
                    local x, y, z = Osi.GetPosition(trip.Hero)
                    local f = trip.From
                    local d = x and math.sqrt((x - f[1]) ^ 2 + (z - f[3]) ^ 2) or 1e9
                    if d > M.BACK_NEAR then
                        own = {X = f[1], Z = f[3], Until = ticks + M.PENDING}
                        Osi.TeleportToPosition(trip.Hero, f[1], f[2], f[3], "", 0, 0, 0, 0, 1)
                        M.State.Corrected = M.State.Corrected + 1
                        Ext.Utils.Print(string.format("WOW-BG3 camp: the story left the hero %.1f m from the spot; put back", d))
                    end
                    local from = trip.From
                    Ext.Utils.Print("WOW-BG3 camp: back from camp")
                    trip = nil
                    publish(false, {Back = from, Off = d})
                end
            end
        end)
        if not ok then M.State.Error = tostring(err) end
    end)
end

return M
