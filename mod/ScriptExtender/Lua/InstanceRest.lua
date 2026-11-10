-- InstanceRest.lua: no camp and no Long Rest inside a WoW instance (server).
--
-- WoW has no rest inside a dungeon or a raid, and the maintainer asked for BG3 to show that as it shows its own such
-- places (the colony under Moonrise: a red ring round the minimap, no fast travel, no camp). BG3's story has two
-- rules for them, and an instance is both for as long as the party is in it:
--   * a danger zone (DB_DangerZone, GLO_DangerZone): the red ring, no camp and no rest;
--   * a blocked waypoint zone (DB_BlockedWaypointZone, _Waypoints): the status TRAVELBLOCK_BLOCKEDZONE, the map
--     screen's "You cannot fast travel from this area", and the story's QRY_WaypointTeleport_PlayerBlocked, which
--     Camp.lua asks for the camp of our own level too.
-- Both key on DB_InRegion(player, trigger): the mod states that fact for one zone of its own and the story's rules
-- do the rest. A WoW map that is not a continent (world.json Map) is an instance.
local M = {}

M.ZONE = "8c41d7e2-5b9a-4f36-a0d8-3e7f2c915b64"   -- stands for "the instance the party is in"; no trigger of the level
M.DANGER = "WOWBG3_Instance"                        -- the danger zone's name in the story
M.EVERY = 30

local function q(fn, ...) local ok, r = pcall(fn, ...); return ok and r end

local function players()
    local out = {}
    for _, r in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do out[#out + 1] = r[1] end
    return out
end

local inside = {}   -- player -> true while the story has it in the zone

local function set(on)
    if on then
        if #(q(function() return Osi.DB_BlockedWaypointZone:Get(M.ZONE) end) or {}) == 0 then pcall(Osi.DB_BlockedWaypointZone, M.ZONE) end
        if #(q(function() return Osi.DB_DangerZone:Get(M.ZONE, nil) end) or {}) == 0 then pcall(Osi.DB_DangerZone, M.ZONE, M.DANGER) end
        for _, p in ipairs(players()) do
            if not inside[p] then
                inside[p] = pcall(Osi.DB_InRegion, p, M.ZONE)
                Ext.Log.Print("WOW-BG3 instance rest: " .. tostring(p) .. " is in an instance: no camp, no Long Rest")
            end
        end
    else
        if next(inside) then
            -- The zones first: the story then looks at every player still in the region and takes its marks off.
            pcall(function() Osi.DB_DangerZone:Delete(M.ZONE, M.DANGER) end)
            pcall(function() Osi.DB_BlockedWaypointZone:Delete(M.ZONE) end)
        end
        for p in pairs(inside) do pcall(function() Osi.DB_InRegion:Delete(p, M.ZONE) end) end
        if next(inside) then
            Ext.Log.Print("WOW-BG3 instance rest: out of the instance")
        end
        inside = {}
    end
end

function M.Start(runId)
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % M.EVERY ~= 11 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, w = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/link/world.json") or "")
        local on = ok and type(w) == "table" and w.Active == true and type(w.Map) == "number" and w.Map ~= 0 and w.Map ~= 1
        local okS, err = pcall(set, on)
        if not okS then Ext.Log.Print("WOW-BG3 instance rest: " .. tostring(err)) end
    end)
    Ext.Log.Print("WOW-BG3 instance rest: started " .. tostring(runId))
end

return M
