-- MoveLoading.lua: a move's cover is BG3's own screen fade (Osiris ScreenFadeTo, as its camp and prison
-- scripts use it). BG3 draws it in its UI (ScreenFade.xaml, in every running game), which the capture layer
-- composites over WoW's picture, so it fades over the world, stays black while the party is away, and fades
-- out at the new place. Its wheel is that widget's too, shown when BG3's ScreenFadeHasSpin says so.
-- link/wowmove.flag: written by tools/ when a move starts, a new text each time; gone when it ends (world_bridge.py
-- removes it once benilla's player stands at the new place).
--
-- A fade from ScreenFadeTo lasts about 10 s once it is black, then BG3 lifts it by itself: in the 0.1.3013.58
-- test (ten moves of 16.5 to 39.5 s) the picture came back 10.3 to 10.7 s after each "to black", whatever the
-- move was doing then (its Stop came 2 to 12 s in), and nothing of ours cleared it. BG3's own covers are
-- short (fade, teleport, clear). A longer move asks for a new fade before the last one lapses (RENEW_MS): it
-- starts black over a black screen, so nothing shows; at the end every one still standing is cleared.
-- ScreenFadeTo's arguments are (Character, Seconds, FadeStyle, FadeID) (the names BG3 registers for Osiris):
-- the third is the style (0 black, 1 bright), not the wheel; ScreenFadeHasSpin is not one of its arguments.
local M = {}

local CHECK_EVERY = 5     -- ticks
local FADE_S, RENEW_S, CLEAR_S = 0.5, 0.1, 1.0
local RENEW_MS = 5000     -- a new fade this often while the move lasts (BG3 lifts one ~10 s after it is black)
local OLD_MS = 120000     -- a bridge killed mid-move does not leave the screen black
local ID = "WOWBG3_Move"
local FLAG = "wow-bg3/link/wowmove.flag"

function M.StartServer()
    local ticks, seen, on, since, first = 0, nil, false, 0, true
    local live, n, renewed = {}, 0, 0  -- fades asked for in this move: {Id, At}
    local function fadeTo(seconds, now)
        n = n + 1
        local id = ID .. "_" .. n
        local ok, err = pcall(Osi.ScreenFadeTo, Osi.GetHostCharacter(), seconds, 0, id)
        if ok then live[#live + 1] = {Id = id, At = now} end
        renewed = now
        Ext.Log.Print("WOW-BG3 move fade: " .. id .. " to black in " .. seconds .. " s" .. (ok and "" or " failed: " .. tostring(err)))
    end
    local function clearAll()
        local host = Osi.GetHostCharacter()
        local bad = {}
        for _, f in ipairs(live) do
            local ok, err = pcall(Osi.ClearScreenFade, host, CLEAR_S, f.Id, 0)
            if not ok then bad[#bad + 1] = f.Id .. ": " .. tostring(err) end
        end
        Ext.Log.Print("WOW-BG3 move fade: cleared " .. #live .. (#bad > 0 and " (failed: " .. table.concat(bad, "; ") .. ")" or ""))
        live = {}
    end
    -- What BG3 says of our fades: when it reports one done (black) and when one is cleared, with its age. A
    -- clear while the move is on is BG3's own lifting it (the ~10 s above).
    local function heard(event)
        return function(user, fadeId)
            local s = tostring(fadeId)
            if s:sub(1, #ID) ~= ID then return end
            local age = "?"
            for _, f in ipairs(live) do
                if f.Id == s then age = string.format("%.1f", (Ext.Utils.MonotonicTime() - f.At) / 1000) end
            end
            Ext.Log.Print(string.format("WOW-BG3 move fade: BG3 %s %s at %s s (move %s)", event, s, age, on and "on" or "off"))
        end
    end
    pcall(Ext.Osiris.RegisterListener, "ScreenFadeDone", 2, "after", heard("done"))
    pcall(Ext.Osiris.RegisterListener, "ScreenFadeCleared", 2, "after", heard("cleared"))
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % CHECK_EVERY ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local now = Ext.Utils.MonotonicTime()
        local flag = Ext.IO.LoadFile(FLAG) or ""
        if first then
            first = false
            seen = flag -- from before this load: no move, but a save or reload may have kept its fade
            if flag ~= "" then
                live = {{Id = ID .. "_1", At = now}}
                clearAll()
            end
        elseif flag ~= seen then
            seen = flag
            since = now
            if (flag ~= "") ~= on then
                on = flag ~= ""
                if on then fadeTo(FADE_S, now) else clearAll() end
            end
        elseif on and now - since > OLD_MS then
            Ext.Log.Print("WOW-BG3 move fade: no end of the move in " .. OLD_MS / 1000 .. " s")
            on = false
            clearAll()
        elseif on and now - renewed >= RENEW_MS then
            fadeTo(RENEW_S, now)
        end
    end)
end

-- What the in-game check needs: BG3's own values for the fade while a move is on, each time they change
-- (visibility, opacity, wheel), with the time since the move began.
function M.StartClient()
    local ticks, seen, at, last = 0, Ext.IO.LoadFile(FLAG) or "", nil, nil
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 30 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local flag = Ext.IO.LoadFile(FLAG) or ""
        if flag ~= seen then
            if (flag ~= "") ~= (seen ~= "") then at, last = flag ~= "" and Ext.Utils.MonotonicTime() or nil, nil end
            seen = flag
        end
        if not at then return end
        local state = "no ScreenFade widget"
        local function walk(node, depth)
            if not node or depth > 5 then return end
            if node.Type == "ls.UIWidget" then
                if node.Name == "ScreenFade" then
                    local d = node.DataContext.CurrentPlayer.UIData
                    state = string.format("visibility %s, opacity %.2f, spin %s", tostring(d.ScreenFadeVisibility),
                        tonumber(d.ScreenFadeOpacity) or -1, tostring(d.ScreenFadeHasSpin))
                end
                return
            end
            for i = 1, math.min(node.VisualChildrenCount or 0, 128) do walk(node:VisualChild(i), depth + 1) end
        end
        local ok, err = pcall(walk, Ext.UI.GetRoot(), 0)
        if not ok then state = "unreadable: " .. tostring(err) end
        if state ~= last then
            last = state
            Ext.Log.Print(string.format("WOW-BG3 move fade: BG3 says %s, %.1f s into the move",
                state, (Ext.Utils.MonotonicTime() - at) / 1000))
        end
    end)
end

return M
