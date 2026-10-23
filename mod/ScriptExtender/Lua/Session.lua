-- Session (the F5 bug): which game states keep world mode alive.
--
-- A save (F5 quicksave, an autosave) takes both the server and the client from Running to Save and
-- back in under a second, in the same session: the level, the moved ground squares, stand-ins, the
-- camera hook and the fight link are all still there. Modules that tore down on "anything but
-- Running" sent the ground home in the middle of the save (with stand-ins still on it), stopped the
-- fight link and forgot the mirrors, so world mode was broken until a reload (harness,
-- bg3le.93: "server Running -> Save", then "pool (server): 4 subgrids back home"). Held states skip
-- a tick and keep everything; any other state still tears down.
local M = {}

local HELD = {Save = true, Paused = true, GameMasterPause = true}

function M.State() return tostring(Ext.Utils.GetGameState()) end

-- Not Running, but the same session goes on: skip this tick, keep all state.
function M.Held(state) return HELD[state or M.State()] == true end

-- The level runs: Running, or the client held in PrepareRunning by the
-- launcher's loading-screen hold (wowbg3-launcher setting "holding", bg3le patch 0004). During that hold
-- BG3's server runs the level and world mode starts under BG3's own loading screen (ground, teleport,
-- camera), so the client modules that set world mode up count it as running. Only the client is ever in
-- PrepareRunning; on the server this is the same as Running.
-- Once held, it stays live until the client leaves PrepareRunning: after the release the engine takes a
-- moment more (the server first makes the save it deferred), and world mode must not tear down in it
-- (harness: the ground went home 0.6 s before Running).
local heldSeen = false
function M.Live(state)
    state = state or M.State()
    if state ~= "PrepareRunning" then
        heldSeen = false
        return state == "Running"
    end
    if heldSeen then return true end
    local ok, v = pcall(Ext.Plugins.Get, "wowbg3-launcher", "holding")
    heldSeen = ok and tonumber(v) == 1
    return heldSeen
end

return M
