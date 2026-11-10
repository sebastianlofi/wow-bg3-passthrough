-- WowSky.lua: WoW's scene light drives BG3's own light at the fight spot (server), so BG3's characters are lit
-- from WoW's sun or moon, with its colour and strength at WoW's time of day, not by Rivington's fixed dusk sun.
--
-- benilla writes wow-bg3/link/wowsky.txt (patch 0125): "WSK1 <to-sun x y z, WoW axes> <diffuse r g b>
-- <ambient r g b>", Light.dbc as WoW samples it for the time of day (sRGB 0..1). WoW's lighting sun keeps a
-- fixed bearing and only rises and falls a little; night is in its colours (benilla lighting/daynight.rs).
-- BG3's side is its own lighting path: the fight spot's lighting zone (6d2f0a51, in our pak) lists stock
-- Rivington and two copies of it, LTN_WOWBG3_WoWSun and LTN_WOWBG3_WoWSun_B. A lighting's values reach the renderer
-- when the zone applies it (a write to the active one did nothing until the camera left the zone and came back;
-- measured), so each change is written into the inactive copy and TriggerSetLighting switches the zone to it.
-- Never back to stock Rivington: under it the far ground stood in haze and characters took a purple cast that
-- the copies do not show, so the picture changed with every switch (seen in play: hazy for the minutes one was
-- active, clear for the next).
-- Mapping (measured, the renderer's sun vector): to-sun = (cos(Yaw-180)*cosE, sinE, sin(Yaw-180)*cosE) with
-- elevation E = 180 - Pitch; BG3 axes from WoW's: x = WoW y, y = WoW z, z = -WoW x. The physical sun (this
-- lighting lights by its scattering: SunIntensity 0) takes WoW's diffuse colour and its strength by WoW's
-- diffuse luminance against REF_LUMA (WoW's daytime), both in linear light.
-- Status: wow-bg3/wowsky-status.json.
local M = {}
local Hero = Ext.Require("Hero.lua")
local Session = Ext.Require("Session.lua")

M.ZONE = "6d2f0a51-77c4-4b0e-9a5e-1f3c2b8d4e01"
M.LIGHTINGS = {"a7d3c1e9-5f2b-4c8e-9d16-3b0e7f4a2c58", "c4f81b27-3a6d-4e09-8b52-7d1e9a0c6f34"}
M.SUN_INTENSITY = 100000  -- stock Rivington's SkyLight.ScatteringSunIntensity
M.REF_LUMA = 0.65         -- WoW's noon diffuse in Durotar, linear luminance (measured 0.651): BG3's sun is stock then
M.EVERY_MS = 5000         -- at most one switch this often

local function lin(c) return math.max(0, c) ^ 2.2 end

-- The BG3 lighting values for one wowsky.txt line, or nil. Pure: tools can run it without a game.
function M.Target(line)
    local v = {}
    for x in tostring(line):gmatch("%S+") do v[#v + 1] = x end
    if v[1] ~= "WSK1" or #v < 10 then return nil end
    for i = 2, 10 do v[i] = tonumber(v[i]) if not v[i] then return nil end end
    local wx, wy, wz = v[2], v[3], v[4]
    local x, y, z = wy, wz, -wx
    local n = math.sqrt(x * x + y * y + z * z)
    if n < 1e-6 then return nil end
    x, y, z = x / n, y / n, z / n
    local elev = math.deg(math.asin(math.max(-1, math.min(1, y))))
    local yaw = (180 + math.deg(math.atan(z, x))) % 360
    local r, g, b = lin(v[5]), lin(v[6]), lin(v[7])
    local top = math.max(r, g, b)
    local luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    local colour = top > 1e-6 and {r / top, g / top, b / top} or {1, 1, 1}
    return {Yaw = yaw, Pitch = 180 - elev, Colour = colour, Intensity = M.SUN_INTENSITY * luma / M.REF_LUMA, Luma = luma}
end

local active = 2        -- the zone starts on stock Rivington; the first change goes to the first copy
local lastLine, lastAt = nil, nil
local function status(t)
    pcall(Ext.IO.SaveFile, "wow-bg3/wowsky-status.json", Ext.Json.Stringify(t))
end

local function step()
    if not Session.Live() then return end
    local ok, p = pcall(Hero.Position)
    if not (ok and Hero.InWorld(p)) then return end
    local line = Ext.IO.LoadFile("wow-bg3/link/wowsky.txt")
    if not line or line == lastLine then return end
    local now = Ext.Utils.MonotonicTime()
    if lastAt and now - lastAt < M.EVERY_MS then return end
    local t = M.Target(line)
    if not t then return end
    local nextOne = 3 - active
    local id = M.LIGHTINGS[nextOne]
    local res = Ext.Resource.Get(id, "Lighting")
    if not res then
        Ext.Log.Print("WOW-BG3 wowsky: lighting " .. id .. " not loaded (the pak's lighting bank)")
        lastLine = line
        return
    end
    local L = res.Lighting
    L.Sun.Yaw, L.Sun.Pitch = t.Yaw, t.Pitch
    L.SkyLight.ScatteringSunColor = t.Colour
    L.SkyLight.ScatteringSunIntensity = t.Intensity
    Osi.TriggerSetLighting(M.ZONE, id)
    active, lastLine, lastAt = nextOne, line, now
    status({Line = line, Yaw = t.Yaw, Pitch = t.Pitch, Colour = t.Colour, Intensity = t.Intensity, Luma = t.Luma, Lighting = id})
    Ext.Log.Print(string.format("WOW-BG3 wowsky: sun yaw %.1f pitch %.1f, colour %.2f %.2f %.2f, intensity %.0f (%s)",
        t.Yaw, t.Pitch, t.Colour[1], t.Colour[2], t.Colour[3], t.Intensity, id:sub(1, 8)))
end

function M.Start(runId)
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 30 ~= 13 then return end
        local ok, err = pcall(step)
        if not ok then Ext.Log.Print("WOW-BG3 wowsky: " .. tostring(err)) end
    end)
    Ext.Log.Print("WOW-BG3 wowsky: started " .. tostring(runId))
end

return M
