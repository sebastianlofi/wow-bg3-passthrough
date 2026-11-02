-- MoveLoading.lua (client): BG3's own loading screen during a move. A move is no load to BG3, so its game
-- never fires GE.OnStateLoadingStart; mod/GUI/StateMachines adds a state holding BG3's LoadingScreen.xaml
-- (art, tips, spinner, all BG3's and live), opened and closed by events through a widget's CustomEvent
-- command, the way BG3's own widgets fire theirs.
-- link/wowmove.flag: written by tools/ when a move starts (a new text each time), gone when it ends.
-- link/bg3moveui.txt: "1" while the widget is up. Only then the capture layer lets BG3's picture through
-- instead of the move cover (wowbg3_capture.c, compose_frame), so a state that did not open leaves the
-- cover, and the log says so.
local M = {}

local CHECK_EVERY = 10   -- ticks
local RETRY_MS = 1000    -- an event that showed no result yet
local WARN_MS = 5000
local OLD_MS = 120000    -- as compose.move: a bridge killed mid-move does not leave the screen covered

local function widgets()
    local out = {}
    local function walk(node, depth)
        if not node or depth > 5 then return end
        if node.Type == "ls.UIWidget" then out[#out + 1] = node; return end
        for i = 1, math.min(node.VisualChildrenCount or 0, 128) do walk(node:VisualChild(i), depth + 1) end
    end
    walk(Ext.UI.GetRoot(), 0)
    return out
end

-- LoadingScreen.xaml's x:Name is "root", as for ten other widgets; of BG3's XAML only it binds LoadingHints.
local function isLoading(node)
    local ok, hints = pcall(function() return node.DataContext.LoadingHints end)
    return ok and hints ~= nil
end

local function fire(list, event)
    for _, node in ipairs(list) do
        local ok, done = pcall(function()
            local cmd = node.DataContext.CustomEvent
            if not cmd or not cmd:CanExecute(event) then return false end
            cmd:Execute(event)
            return true
        end)
        if ok and done then return true end
    end
    return false
end

function M.Start()
    local ticks = 0
    local seen = Ext.IO.LoadFile("wow-bg3/link/wowmove.flag") or "" -- one from before this start is no move
    local want, up, since, tried, warned = false, false, 0, nil, false
    Ext.IO.SaveFile("wow-bg3/link/bg3moveui.txt", "0\n")
    local function setUp(v)
        if v ~= up then
            up = v
            Ext.IO.SaveFile("wow-bg3/link/bg3moveui.txt", v and "1\n" or "0\n")
            Ext.Log.Print("WOW-BG3 move loading: BG3's loading screen " .. (v and "up" or "closed"))
        end
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % CHECK_EVERY ~= 0 then return end
        local now = Ext.Utils.MonotonicTime()
        local flag = Ext.IO.LoadFile("wow-bg3/link/wowmove.flag") or ""
        if flag ~= seen then
            seen = flag
            since, tried, warned = now, nil, false
            want = flag ~= ""
        elseif want and now - since > OLD_MS then
            Ext.Log.Print("WOW-BG3 move loading: no end of the move in " .. OLD_MS / 1000 .. " s: closing")
            want = false
        end
        if not want and not up then return end
        -- Only in a running game: BG3's UI is replaced around a real load (the crash of UiWatch.lua).
        if Ext.Utils.GetGameState() ~= "Running" then setUp(false); return end
        local okW, list = pcall(widgets)
        if not okW then Ext.Log.Print("WOW-BG3 move loading: " .. tostring(list)); return end
        local open = false
        for _, node in ipairs(list) do
            if isLoading(node) then open = true; break end
        end
        setUp(open)
        if want == open then return end
        if not tried or now - tried > RETRY_MS then
            tried = now
            if not fire(list, want and "OpenWowMoveLoading" or "CloseWowMoveLoading") then
                Ext.Log.Print("WOW-BG3 move loading: no widget took the event")
            end
        end
        if want and not warned and now - since > WARN_MS then
            warned = true
            Ext.Log.Print("WOW-BG3 move loading: the state did not open in " .. WARN_MS / 1000 .. " s (mod/GUI/StateMachines loaded?); the cover stays")
        end
    end)
end

return M
