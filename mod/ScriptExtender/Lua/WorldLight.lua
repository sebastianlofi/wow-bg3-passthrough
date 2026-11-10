-- WoW's light as BG3's gameplay light round the party (server).
--
-- World mode's arena lies in the host level's void, where BG3 has no light: there BG3 counts everyone
-- as in darkness, so enemies' sight cones shrink to a metre or two and a sneaking hero is never seen,
-- even three metres in front of a wolf at noon (harness, BGO_Main_A: 13 cone tiles at 127
-- instead of several hundred at 255; the tutorial level, which is lit, was right).
-- The render plugin writes WoW's light on level ground where the player stands to wow-bg3/link/wowlight.txt
-- ("<luma> <r> <g> <b> <n>", gamma 0..255, WoW's own lighting: time of day, zone, weather). By WoW's day
-- every party member carries a sunlight gameplay light (a boost as BG3's Daylight has, GameplayLight(Radius,
-- LOS, Sharpening, Sunlight): no picture, BG3's sight and stealth only), so the party and everything near it
-- is clear, as BG3's sunlit outdoors (Darkness.Obscurity 1; the void alone is dark, 3: cones of a metre or
-- two, a sneaking hero unseen at three metres). By WoW's night the same light without sunlight: never
-- pitch black. Not yet darker than day: BG3's dim light (lightly obscured) needs a half-lit light, which a
-- boost cannot give, and IsHalfLit set on the server's GameplayLight component did not change the
-- obscurity (harness). Off outside world mode.
-- Test override: wow-bg3/link/wowlight-force.txt holding "day" or "night".
-- Status: wow-bg3/worldlight-status.json.
local M = {}
local Hero = Ext.Require("Hero.lua")
local Party = Ext.Require("Party.lua")
local Session = Ext.Require("Session.lua")

M.BOOST = {day = "GameplayLight(30,false,0.1,true)", night = "GameplayLight(30,false,0.1,false)"}
local CAUSE = "WOWBG3_WOWLIGHT"
M.DAY, M.HYST = 90, 10                       -- luma of WoW's ground light: day at 100 and up, night under 80
local applied = {}                           -- uuid -> "day" | "night"
local kind, luma, seen, swept = nil, nil, 0, false

local function osi(fn, ...)
    local ok, err = pcall(fn, ...)
    if not ok then Ext.Log.Print("WOW-BG3 worldlight: " .. tostring(err)) end
end
local function put(u, k)
    local old = applied[u]
    if old == k then return end
    if old and M.BOOST[old] then osi(Osi.RemoveBoosts, u, M.BOOST[old], 0, CAUSE, u) end
    if k and M.BOOST[k] then osi(Osi.AddBoosts, u, M.BOOST[k], CAUSE, u) end
    applied[u] = k
end
local function status(extra)
    local members = {}
    for u, k in pairs(applied) do members[#members + 1] = u .. " " .. k end
    local s = {Kind = kind, Luma = luma, Seen = seen, Members = members}
    for k, v in pairs(extra or {}) do s[k] = v end
    Ext.IO.SaveFile("wow-bg3/worldlight-status.json", Ext.Json.Stringify(s))
end
local function inWorld()
    local ok, p = pcall(Hero.Position)
    return ok and Hero.InWorld(p)
end

local function step()
    if not Session.Live() then return end
    if not inWorld() then
        if next(applied) or not swept then
            -- also a boost a save made in world mode carried here (the first look after a load)
            for _, u in ipairs(Party.Party()) do
                applied[u] = "day"; put(u, nil)
                applied[u] = "night"; put(u, nil)
            end
            for u in pairs(applied) do put(u, nil) end
            applied, kind, swept = {}, nil, true
            status({World = false})
        end
        return
    end
    swept = false
    local force = (Ext.IO.LoadFile("wow-bg3/link/wowlight-force.txt") or ""):match("%a+")
    local line = Ext.IO.LoadFile("wow-bg3/link/wowlight.txt") or ""
    local l, n = line:match("^(%d+) %d+ %d+ %d+ (%d+)")
    l, n = tonumber(l), tonumber(n)
    if l then luma, seen = l, n or seen end
    local want = kind or "day"               -- no word from WoW yet: BG3's day
    if luma then
        if luma >= M.DAY + M.HYST then want = "day" elseif luma < M.DAY - M.HYST then want = "night" end
    end
    if force == "day" or force == "night" then want = force end
    local changed = want ~= kind
    kind = want
    local live = {}
    for _, u in ipairs(Party.Party()) do live[u] = true; put(u, kind) end
    for u in pairs(applied) do if not live[u] then put(u, nil) end end
    if changed then
        Ext.Log.Print(string.format("WOW-BG3 worldlight: %s (WoW's ground light luma %s%s)", kind, tostring(luma),
            force and (", forced " .. force) or ""))
        status({World = true, Force = force})
    end
end

function M.Start(runId)
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 30 ~= 7 then return end
        local ok, err = pcall(step)
        if not ok then Ext.Log.Print("WOW-BG3 worldlight: " .. tostring(err)) end
    end)
    Ext.Log.Print("WOW-BG3 worldlight: started " .. tostring(runId))
end
return M
