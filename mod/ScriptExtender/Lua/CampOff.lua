-- CampOff (server): no camp in world mode, for now.
--
-- Going to camp (and leaving it) broke the WoW world for a while and left the party at a random spot
-- of the WoW map (a playtest report: back in Mulgore). Until camp and resting are designed for WoW
-- (README, "Open question: resting"), BG3's own blocks keep the party out of it, and `/wowbg3 refill`
-- does a long rest's work:
--   * TRAVELBLOCK_BLOCKEDZONE on every party member: BG3's "no fast travel from here" status (the
--     story's DB_FastTravelBlock_BlockedZone_StatusSet), which greys out Go to Camp (and waypoint travel);
--   * Osiris SetLongRestAvailable 0: Long Rest ("Go to camp and end the day") does nothing.
-- Harness: Go to Camp greyed, a click on Long Rest left the party where it stood. Short rests stay.
-- Both are set while world mode runs (link/world.json Active) and every M.Every ticks again (the story
-- may lift the status when it thinks a blocked zone was left); they are lifted when world mode stops.
local M = {}

M.Status = "TRAVELBLOCK_BLOCKEDZONE"
M.Every = 60 -- ticks between checks (about 2 s)
M.On = true  -- research knob
-- With the switch `camp` on, Camp.lua's trip replaces all this; a save made under these blocks lifts them.
local Camp = Ext.Require("Camp.lua")

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
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

local function worldActive()
    local raw = Ext.IO.LoadFile("wow-bg3/link/world.json")
    if not raw then return false end
    local ok, w = pcall(Ext.Json.Parse, raw)
    return ok and type(w) == "table" and w.Active == true
end

function M.Start(runId)
    local ticks, blocked, marked = 0, false, {}
    M.State = {Blocked = false, Applied = 0}
    local lifted = false
    local function liftSaved()
        lifted = true
        pcall(Osi.SetLongRestAvailable, 1)
        for _, u in ipairs(players()) do pcall(Osi.RemoveStatus, u, M.Status) end
    end
    local function lift()
        for u in pairs(marked) do pcall(Osi.RemoveStatus, u, M.Status) end
        marked = {}
        pcall(Osi.SetLongRestAvailable, 1)
        blocked = false
        M.State.Blocked = false
        Ext.Utils.Print("WOW-BG3 camp: camp and long rest back (world mode off)")
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % M.Every ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, err = pcall(function()
            if Camp.On() then
                if not lifted then liftSaved() end
                return
            end
            local want = M.On and worldActive()
            if not want then
                if blocked then lift() end
                return
            end
            if not blocked then
                pcall(Osi.SetLongRestAvailable, 0)
                blocked = true
                M.State.Blocked = true
                Ext.Utils.Print("WOW-BG3 camp: camp travel and long rest off in world mode (use /wowbg3 refill)")
            end
            for _, u in ipairs(players()) do
                if q(Osi.HasActiveStatus, u, M.Status) ~= 1 then
                    pcall(Osi.ApplyStatus, u, M.Status, -1.0, 1, u)
                    marked[u] = true
                    M.State.Applied = M.State.Applied + 1
                end
            end
        end)
        if not ok then M.State.Error = tostring(err) end
    end)
end

return M
