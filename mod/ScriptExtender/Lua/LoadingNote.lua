-- LoadingNote.lua: what the WoW side is doing, on BG3's own loading screen while a load waits for it (the
-- WoW server's first start, its start, movement maps). wowbg3-coord.py writes wow-bg3/loading-note.json
-- {RunId, Text} while that work is Busy and Text "" once it is done; the line goes into the loading screen's
-- LongTimeCodeSyncMessage, the TextBlock above the percentage that BG3 uses for its own long waits
-- (Public/Game/GUI/Widgets/LoadingScreen.xaml), and is emptied again when the note is.
local M = {}

local function readJson(name)
    local raw = Ext.IO.LoadFile(name)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

-- The loading screen's TextBlock, found by its name in BG3's UI tree; nil while no loading screen shows.
local function findLine()
    local found, seen, visited = nil, {}, 0
    local function walk(node, depth)
        if found or not node or depth > 32 or visited >= 4096 or seen[tostring(node)] then return end
        seen[tostring(node)] = true
        visited = visited + 1
        local ok, name = pcall(function() return node.Name end)
        if ok and tostring(name) == "LongTimeCodeSyncMessage" then found = node return end
        local okc, count = pcall(function() return node.VisualChildrenCount end)
        for i = 1, math.min(okc and count or 0, 256) do
            local okg, child = pcall(function() return node:VisualChild(i) end)
            if okg then walk(child, depth + 1) end
        end
    end
    local ok, root = pcall(Ext.UI.GetRoot)
    if ok then walk(root, 0) end
    return found
end

function M.Start(runId)
    local ticks, shown, line = 0, "", nil
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 15 ~= 0 then return end
        local note = readJson("wow-bg3/loading-note.json")
        local want = note and note.RunId == runId and type(note.Text) == "string" and note.Text or ""
        if want == shown then return end
        -- A node from an earlier loading screen is gone with it: look again.
        if not (line and pcall(function() return line.Name end)) then line = findLine() end
        if not line then
            if want == "" then shown = "" end
            return
        end
        if pcall(function() line:SetProperty("Text", want) end) then
            Ext.Log.Print("WOW-BG3 loading note: " .. (want ~= "" and want or "(cleared)"))
            shown = want
        else
            line = nil
        end
    end)
end

return M
