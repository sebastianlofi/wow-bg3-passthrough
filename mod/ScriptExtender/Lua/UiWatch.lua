-- UiWatch (client): a log line for each BG3 window or notice that shows on screen, with its words and how long
-- it stayed. Playtesters saw a popup too quick to read; a report's logs now say what it was.
--   bg3le log: "WOW-BG3 ui watch: NAME shows: WORDS" and "WOW-BG3 ui watch: NAME gone after S s"
local M = {}

-- Always there. NOTICES among them hold their words only while they show.
local HUD = {WorldTooltips = true, CombatantsOverlay = true, CombatLog = true, TurnModeInfo = true,
    TargetInfo = true, Minimap = true, OverheadInfo = true, HudIndicator = true, HotBar = true,
    PassiveRoll = true, AlwaysOnTopOverlay = true, CursorText = true, PlayerPortraits = true,
    Overlay = true, DragAndDropPreview = true, ScreenFade = true, CrossplayOverlay = true, WorldContextMenu = true}
local NOTICES = {NotificationWidget = true, AlwaysOnTopOverlay = true, Overlay = true}
local EVERY = 4 -- ticks: a notice stays far longer than this

local function field(n, key)
    local ok, v = pcall(function() return n[key] end)
    return ok and v or nil
end

-- The words under a node, as one line: what is hidden stays out.
local function words(node)
    local out, count = {}, 0
    local function walk(n, d)
        if not n or d > 30 or count > 600 or #out > 24 then return end
        count = count + 1
        if tostring(field(n, "Visibility")) == "Collapsed" then return end
        local t = field(n, "Text")
        if type(t) == "string" and t:find("%S") and out[#out] ~= t then out[#out + 1] = t end
        for i = 1, math.min(field(n, "VisualChildrenCount") or 0, 128) do
            local okC, c = pcall(function() return n:VisualChild(i) end)
            if okC then walk(c, d + 1) end
        end
    end
    pcall(walk, node, 0)
    return (table.concat(out, " | "):gsub("%s+", " "):sub(1, 400))
end

local function widgets()
    local found = {}
    local function walk(n, d)
        if not n or d > 6 then return end
        local name = field(n, "Name")
        if n.Type == "ls.UIWidget" and name then
            name = tostring(name)
            if not HUD[name] or NOTICES[name] then found[name] = n end
            return
        end
        for i = 1, math.min(n.VisualChildrenCount or 0, 256) do walk(n:VisualChild(i), d + 1) end
    end
    pcall(walk, Ext.UI.GetRoot(), 0)
    return found
end

function M.Start()
    local ticks, shown = 0, {} -- name -> {At, Words}
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % EVERY ~= 0 then return end
        local now = Ext.Utils.MonotonicTime()
        local found = widgets()
        for name, node in pairs(found) do
            local s = shown[name]
            -- A window's words once, as it opens; a notice's at every look.
            local w = (NOTICES[name] or not s) and words(node) or ""
            -- A notice widget is always there: it counts as showing while it has words.
            if not s and (w ~= "" or not NOTICES[name]) then
                shown[name] = {At = now, Words = w}
                Ext.Log.Print("WOW-BG3 ui watch: " .. name .. " shows: " .. w)
            elseif s and w ~= "" and w ~= s.Words then
                s.Words = w
                Ext.Log.Print("WOW-BG3 ui watch: " .. name .. " now: " .. w)
            elseif s and NOTICES[name] and w == "" then
                found[name] = nil
            end
        end
        for name, s in pairs(shown) do
            if not found[name] then
                shown[name] = nil
                Ext.Log.Print(string.format("WOW-BG3 ui watch: %s gone after %.1f s", name, (now - s.At) / 1000))
            end
        end
    end)
end

return M
