-- OwnLevel (server): what BG3's story has to be told about a level of our own (Hero.OWN; tools/wowbg3_level.py).
--
-- Fog of war. BG3 covers the parts of a level's map the party has not seen (the minimap's and the map page's
-- "Shroud" layer, over everything laid on the map). The whole of our level is unseen ground to BG3, so WoW's
-- minimap art (WowUi.lua) lay under a black layer; in a borrowed level world mode's spot was outside the map's
-- frame and the layer did not reach it. WoW has no fog of war on its minimap. BG3's story has its own switch for
-- levels without one: PROC_DisableShroud (goal _Shroud: DB_ShroudDisabled(1), ShroudRender(0)), kept in the save.
local M = {}
local Hero = Ext.Require("Hero.lua")
local Session = Ext.Require("Session.lua")

local function step()
    if not (Hero.OWN and Session.Live()) then return end
    local ok, region = pcall(function() return Osi.DB_CurrentLevel:Get(nil)[1][1] end)
    if not ok or region ~= Hero.REGION then return end
    local okd, rows = pcall(function() return Osi.DB_ShroudDisabled:Get(1) end)
    if okd and #rows == 0 then
        Osi.PROC_DisableShroud()
        Ext.Log.Print("WOW-BG3 own level: fog of war off (PROC_DisableShroud)")
    end
end

function M.Start(runId)
    if not Hero.OWN then return end
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 60 ~= 17 then return end
        local ok, err = pcall(step)
        if not ok then Ext.Log.Print("WOW-BG3 own level: " .. tostring(err)) end
    end)
end

return M
