-- CharLight.lua: BG3's own close-up character light is off in WoW's world (client).
--
-- BG3 lights every character with a small setup of its own that follows the head (the CharacterLight component: a
-- key, a fill and rim lights on the "character only" light channel). BG3 fades that setup in as the camera comes
-- close (in full within about 4.4 m of camera distance, gone by 5.3 m), so a figure's light changed with the zoom:
-- bright and modelled up close, and a step further out only what WoW's light gives it (a playtest report: "why does
-- zooming in and out change my character's lighting"). WoW has no such light: a unit is lit the same at every
-- camera distance, and WoW's light for BG3's figures comes from the render plugin (WoW's ambient, its room and
-- point lights, on BG3's own normals). So in WoW's world the component is switched off, and outside it (camp)
-- it is as BG3 set it.
--
-- Before, this file scaled each light's LightDesc.Amount by WoW's ambient level. That value has no effect on what
-- BG3 draws (held at 0 and at 1, at both zooms: no change in the picture), so it never did what it said.
local M = {}

M.EVERY = 30        -- ticks

local own = {}      -- "<entity>" -> the Enabled BG3 gave that character's light
local last = nil

-- Pure: what a character light's Enabled should be. `inWorld`: WoW's world is what shows; `bg3`: what BG3 gave it.
function M.Want(inWorld, bg3)
    if inWorld then return false end
    return bg3
end

local function apply(inWorld)
    local n = 0
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("CharacterLight")) do
        local cl = e.CharacterLight
        if cl then
            local key = tostring(e)
            if own[key] == nil then own[key] = cl.Enabled end
            local want = M.Want(inWorld, own[key])
            if cl.Enabled ~= want then cl.Enabled = want; n = n + 1 end
        end
    end
    return n
end

function M.Start(runId)
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % M.EVERY ~= 7 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, err = pcall(function()
            local okW, w = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/link/world.json") or "")
            local inWorld = okW and type(w) == "table" and w.Active == true
            local n = apply(inWorld)
            if n > 0 and last ~= inWorld then
                Ext.Log.Print(string.format("WOW-BG3 charlight: BG3's close-up character light %s (%d characters set)", inWorld and "off in WoW's world" or "as BG3 set it", n))
                last = inWorld
            end
        end)
        if not ok then Ext.Log.Print("WOW-BG3 charlight: " .. tostring(err)) end
    end)
    Ext.Log.Print("WOW-BG3 charlight: started " .. tostring(runId))
end

return M
