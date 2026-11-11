-- Lights.lua: BG3's lights light WoW's world (client).
--
-- A spell's light, a torch or a burning surface lights BG3's characters and nothing of WoW's, since WoW draws its own
-- ground, buildings and units. So BG3's point lights near the party go to WoW as point lights of its own: wow-bg3/link/
-- bg3lights.txt, "BLT1 <count>" then a line a light, "<x y z, WoW yards> <range yd> <r g b>", which benilla (patch
-- 0130) turns into WoW point lights: every surface and unit takes them among its three nearest, as it takes a lamp's.
-- Which lights: BG3's point and spot lights that are on (a spot goes as a point: WoW 1.12 has none). Not the lights
-- BG3 gives every character for itself (directional, their own channels): they are no light in the world. Not
-- BG3's Darkvision lights either (templates Light_Player_Darkvision_*, a spot of 24 m on the head of a character who
-- sees in the dark): they are how BG3 shows that character's sight, no lamp, and in WoW's world one lit the ground
-- around a drow all night.
-- Strength: BG3's are physical, WoW's a colour that is added over 0.7 d + 0.03 d^2 yards. One scale for all of them:
-- BG3's Light cantrip (intensity 6000) is as strong as a torch WoW's guards carry (1.4 on its strongest channel).
-- Reach: WoW's falloff never ends, BG3's light does at its radius; WoW takes it up to one and a half times that.
-- The file is written again every second while there are lights, so benilla drops them when the game stops writing.
local M = {}

M.EVERY = 6            -- ticks: ten times a second
M.REACH = 60           -- metres from the controlled character
M.PER_INTENSITY = 1.4 / 6000
M.TOP = 2.5            -- no light stronger than this on its strongest channel
M.MAX = 8
local YD = 0.9144

local function mapping()
    local ok, w = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/link/world.json") or "")
    if ok and type(w) == "table" and w.Active == true and type(w.Centre) == "table" and type(w.WowOrigin) == "table" then return w end
end

local function collect(w, at)
    local rows = {}
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("Light")) do
        local L = e.Light
        local kind = L and tostring(L.LightType)
        local t = e.Transform and e.Transform.Transform and e.Transform.Transform.Translate
        local okn, name = pcall(function() return L.Template and L.Template.Name end)
        local sight = okn and type(name) == "string" and name:find("Darkvision", 1, true) ~= nil
        if (kind == "Point" or kind == "Spot") and t and (L.Intensity or 0) > 0 and not sight then
            local on = false
            for _, f in pairs(L.Flags or {}) do if tostring(f) == "Enabled" then on = true end end
            local d2 = (t[1] - at[1]) ^ 2 + (t[2] - at[2]) ^ 2 + (t[3] - at[3]) ^ 2
            if on and d2 < M.REACH * M.REACH then
                local c, b = L.Color or {1, 1, 1}, L.Blackbody or {1, 1, 1}
                local rgb = {c[1] * b[1], c[2] * b[2], c[3] * b[3]}
                local top = math.max(rgb[1], rgb[2], rgb[3], 1e-6)
                local s = math.min(M.TOP, L.Intensity * M.PER_INTENSITY) / top
                local C, W = w.Centre, w.WowOrigin
                rows[#rows + 1] = {d2, string.format("%.2f %.2f %.2f %.1f %.4f %.4f %.4f",
                    W[1] - (t[3] - C[3]) / YD, W[2] + (t[1] - C[1]) / YD, W[3] + (t[2] - C[2]) / YD,
                    math.max(4, (tonumber(L.Radius) or 8) * 1.5 / YD), rgb[1] * s, rgb[2] * s, rgb[3] * s)}
            end
        end
    end
    table.sort(rows, function(a, b) return a[1] < b[1] end)
    local lines = {}
    for i = 1, math.min(#rows, M.MAX) do lines[i] = rows[i][2] end
    return lines
end

function M.Start(runId)
    local ticks, last, lastAt = 0, nil, 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % M.EVERY ~= 3 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, err = pcall(function()
            local w = mapping()
            local lines = {}
            if w then
                local host = Ext.Entity.GetAllEntitiesWithComponent("ClientControl")[1]
                local t = host and host.Transform and host.Transform.Transform and host.Transform.Transform.Translate
                if t then lines = collect(w, t) end
            end
            local text = "BLT1 " .. #lines .. "\n" .. table.concat(lines, "\n") .. (#lines > 0 and "\n" or "")
            -- A change at once; the same lights again every second, so the file stays fresh while they burn.
            if text ~= last or (#lines > 0 and ticks - lastAt >= 60) then
                Ext.IO.SaveFile("wow-bg3/link/bg3lights.txt", text)
                if (last or "BLT1 0\n") ~= text and (#lines == 0 or not last or last:sub(1, 7) ~= text:sub(1, 7)) then
                    Ext.Log.Print("WOW-BG3 lights: " .. #lines .. " of BG3's lights in WoW's world")
                end
                last, lastAt = text, ticks
            end
        end)
        if not ok then Ext.Log.Print("WOW-BG3 lights: " .. tostring(err)) end
    end)
    Ext.Log.Print("WOW-BG3 lights: started " .. tostring(runId))
end

return M
