-- CameraPose (client): what BG3's game camera wants, for the plugin's rigid orbit (`rigid_pose`).
-- BG3 puts its camera over the ground under its target, swings it up over what stands in the way and pulls it
-- in; in WoW's world those come from a hill over a cave and from trees. Playtest feedback: the camera stays at the
-- character's height, zooms and turns, and bumps into nothing. So the plugin takes only the zoom, the angle
-- and where it looks across from here, and the height from whoever the camera follows.
--   link/campose.bin  "CAM1", seq, flags (1 tactical, 2 height from a character), distance, pitch degrees, direction xyz, look xyz, target y
local M = {}

local function behavior()
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("GameCameraBehavior")) do
        local ok, c = pcall(function() return e.Camera end)
        if ok and c and c.Active == true and c.AcceptsInput == true then return e.GameCameraBehavior end
    end
end

-- Written every tick (one BG3 tick per rendered frame). `rigid_pose` builds the eye of every frame, BG3's and the
-- one sent to benilla, from the newest record and holds it until the next: written every other tick, the look
-- point, direction and zoom moved once per two frames while the hero, drawn from that frame's own pose, moved
-- every frame. Walking or turning, she and the camera then shook against each other at half the frame rate, by
-- one frame's step: worst zoomed in, where she is near the eye and the frame rate is lowest.
function M.Start()
    local seq, lastY = 0, nil
    Ext.Events.Tick:Subscribe(function()
        local ok, rec = pcall(function()
            local b = behavior()
            if not b then return nil end
            local d, t = b.Direction, b.TargetCurrent
            -- Whoever it follows: its own height, never the ground BG3 found under it. The nearest of the
            -- camera's targets and the character the player controls to where it looks.
            local best, y = 36.0, nil
            local function try(e)
                local okP, pos = pcall(function() return e.Transform.Transform.Translate end)
                if okP and pos then
                    local dx, dz = pos[1] - t[1], pos[3] - t[3]
                    local far = dx * dx + dz * dz
                    if far < best then best, y = far, pos[2] end
                end
            end
            for _, k in ipairs({"Target", "FollowTarget", "TrackTarget"}) do
                local okE, e = pcall(function() return b[k] end)
                if okE and e then try(e) end
            end
            for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("ClientControl")) do try(e) end
            -- A camera panned off them all keeps the last height.
            lastY = y or lastY
            local own = y ~= nil
            y = lastY or t[2]
            -- DistanceCurrent is the zoom. Distance is that pulled in by BG3's arm collision, measured 6.07 against 12
            -- on a ledge, 2.33 after camp, and held there until the next step.
            local flags = (b.TacticalMode == true and 1 or 0) | (own and 2 or 0)
            seq = seq + 1
            return string.pack("<c4I4I4fffffffff", "CAM1", seq, flags, b.DistanceCurrent, b.PitchDegrees, d[1], d[2], d[3], t[1], t[2], t[3], y)
        end)
        if ok and rec then Ext.IO.SaveFile("wow-bg3/link/campose.bin", rec) end
    end)
end

return M
