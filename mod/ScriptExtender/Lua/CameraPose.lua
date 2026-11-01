-- CameraPose (client): what BG3's game camera wants, for the plugin's rigid orbit (`rigid_pose`).
-- BG3 puts its camera over the ground under its target, swings it up over what stands in the way and pulls it
-- in; in WoW's world those come from a hill over a cave and from trees. Playtest feedback: the camera stays at the
-- character's height, zooms and turns, and bumps into nothing. So the plugin takes only the zoom, the angle
-- and where it looks across from here, and the height from whoever the camera follows.
--   link/campose.bin  "CAM1", seq, flags (1 tactical), distance, pitch degrees, direction xyz, look xyz, target y
local M = {}

local function behavior()
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("GameCameraBehavior")) do
        local ok, c = pcall(function() return e.Camera end)
        if ok and c and c.Active == true and c.AcceptsInput == true then return e.GameCameraBehavior end
    end
end

function M.Start()
    local seq, ticks = 0, 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 2 ~= 0 then return end
        local ok, rec = pcall(function()
            local b = behavior()
            if not b then return nil end
            local d, t = b.Direction, b.TargetCurrent
            -- Whoever it follows: its own height, never the ground BG3 found under it.
            local y = t[2]
            local okY, ty = pcall(function() return b.Target.Transform.Transform.Translate[2] end)
            if okY and type(ty) == "number" then y = ty end
            local flags = b.TacticalMode == true and 1 or 0
            seq = seq + 1
            return string.pack("<c4I4I4fffffffff", "CAM1", seq, flags, b.Distance, b.PitchDegrees, d[1], d[2], d[3], t[1], t[2], t[3], y)
        end)
        if ok and rec then Ext.IO.SaveFile("wow-bg3/link/campose.bin", rec) end
    end)
end

return M
