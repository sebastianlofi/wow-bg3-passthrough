-- MoveLoading.lua: a move's cover is BG3's own screen fade (Osiris ScreenFadeTo, as its camp and prison
-- scripts use it). BG3 draws it in its UI (ScreenFade.xaml, in every running game), which the capture layer
-- composites over WoW's picture, so it fades over the world, stays black while the party is away, and fades
-- out at the new place. Its wheel is that widget's too, shown when BG3's ScreenFadeHasSpin says so.
-- link/wowmove.flag: written by tools/ when a move starts, a new text each time; gone when it ends.
local M = {}

local CHECK_EVERY = 5     -- ticks
local FADE_S, CLEAR_S = 0.5, 1.0
local OLD_MS = 120000     -- a bridge killed mid-move does not leave the screen black
local ID = "WOWBG3_Move"
local FLAG = "wow-bg3/link/wowmove.flag"

function M.StartServer()
    local ticks, seen, on, since, first = 0, nil, false, 0, true
    local function fade(open)
        local host = Osi.GetHostCharacter()
        local ok, err
        if open then ok, err = pcall(Osi.ScreenFadeTo, host, FADE_S, 0, ID)
        else ok, err = pcall(Osi.ClearScreenFade, host, CLEAR_S, ID, 0) end
        Ext.Log.Print("WOW-BG3 move fade: " .. (open and "to black" or "cleared") .. (ok and "" or " failed: " .. tostring(err)))
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % CHECK_EVERY ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local now = Ext.Utils.MonotonicTime()
        local flag = Ext.IO.LoadFile(FLAG) or ""
        if first then
            first = false
            seen = flag -- from before this load: no move, but a save or reload may have kept its fade
            if flag ~= "" then fade(false) end
        elseif flag ~= seen then
            seen = flag
            since = now
            if (flag ~= "") ~= on then
                on = flag ~= ""
                fade(on)
            end
        elseif on and now - since > OLD_MS then
            Ext.Log.Print("WOW-BG3 move fade: no end of the move in " .. OLD_MS / 1000 .. " s")
            on = false
            fade(false)
        end
    end)
end

-- What the in-game check needs: BG3's own values for the fade, 3 s into a move (its wheel stays hidden for 2).
function M.StartClient()
    local ticks, seen, at = 0, Ext.IO.LoadFile(FLAG) or "", nil
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 30 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local flag = Ext.IO.LoadFile(FLAG) or ""
        if flag ~= seen then
            seen = flag
            at = flag ~= "" and Ext.Utils.MonotonicTime() + 3000 or nil
        end
        if not at or Ext.Utils.MonotonicTime() < at then return end
        at = nil
        local found = false
        local function walk(node, depth)
            if not node or depth > 5 or found then return end
            if node.Type == "ls.UIWidget" then
                if node.Name == "ScreenFade" then
                    local d = node.DataContext.CurrentPlayer.UIData
                    if tostring(d.ScreenFadeVisibility) == "Visible" then
                        found = true
                        Ext.Log.Print(string.format("WOW-BG3 move fade: BG3 says opacity %s, spin %s",
                            tostring(d.ScreenFadeOpacity), tostring(d.ScreenFadeHasSpin)))
                    end
                end
                return
            end
            for i = 1, math.min(node.VisualChildrenCount or 0, 128) do walk(node:VisualChild(i), depth + 1) end
        end
        local ok, err = pcall(walk, Ext.UI.GetRoot(), 0)
        if not ok or not found then Ext.Log.Print("WOW-BG3 move fade: no visible ScreenFade 3 s in " .. tostring(err or "")) end
    end)
end

return M
