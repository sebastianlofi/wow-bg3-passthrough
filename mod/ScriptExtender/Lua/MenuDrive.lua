-- BG3's main menu and character creation driven through their own view-model commands, the ones
-- their buttons run: the test harness's headless New Game
-- and loading a named save (with WOW_BG3_SAVE). A human does the same with
-- the mouse; nothing here changes BG3's rules or files.
-- wow-bg3/menu-drive.json {RunId, Sequence, Action, ...} -> wow-bg3/menu-drive-result.json
-- {RunId, Sequence, State, OK, Value}; never retried (the caller looks at the result and the UI):
--   Widgets {}                  the UI widgets now present (which page shows)
--   Load {Playthrough, Save}    main menu: a save of the Load Game page's list: the newest of the
--                               playthrough whose protagonist is named Playthrough (e.g. "Tav",
--                               "Gale"), or within it (or any) the save titled Save (e.g. "QuickSave_2").
--                               The main menu's own view model knows only the newest save (Continue's)
--                               until the Load Game page lists them all (harness), so the
--                               driver calls Load until it says "loading": the first call opens the page
--                               (OpenLoadGameDialog, Phase "listing"), the next selects the save as a
--                               click in the page's list does (DCSavegames SelectedSave, the save's
--                               IsSelected, and the main menu's SelectedSave, which BG3 sets a tick
--                               later; Phase "selecting"), the next, once the main menu's SelectedSave is
--                               that save, loads it (DoLoad, the Load button's command). Phase "no such
--                               save" lists what there is.
--   NewGame {Preset}            main menu: New Game (StartGameCommand), then on the difficulty page
--                               (widget DMSettings) the preset whose title (or asset name) contains
--                               Preset (default: BG3's current one, Balanced) and StartNewGame (its
--                               Confirm button's command)
--   Origin {Name}               character creation: the origin titled Name (SelectableOrigins: Custom,
--                               Astarion, Lae'zel, Gale, Shadowheart, Wyll, Karlach, The Dark Urge)
--                               selected in the origins list as a click does (presetOrigins.SelectedItem;
--                               BG3 takes it a tick later)
--   Finish {}                   character creation (or the respec screen, CharacterRespec: finished at
--                               once): once BG3 says the character is complete, first the
--                               guardian page (what the "To Companion" button does: the flow page's
--                               index 2), then (the next call) Venture Forth (FinishCreating)
--   Key {Key}                   one key press (Ext.Input), only ESCAPE, SPACE, RETURN, F5 (BG3's
--                               quicksave) or F8 (its quickload) (lifecycle tests)
-- bg3le rebuilds the Lua states at every load, so a fresh start of this module takes the command already
-- in the file as done (a quickload's F8 or a Load must not run again in the session it loaded).
-- Translated names (origins, presets) are read with Ext.Loca.GetTranslatedString.
local M = {}

local function readJson(name)
    local raw = Ext.IO.LoadFile(name)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end
local function scalar(v)
    local t = type(v)
    if t == "string" or t == "number" or t == "boolean" or t == "nil" then return v end
    local ok, s = pcall(function() return v:ToString() end)
    return ok and tostring(s) or tostring(v)
end
local function text(v) return tostring(scalar(v) or "") end
local function loca(handle)
    local ok, s = pcall(Ext.Loca.GetTranslatedString, tostring(handle))
    return ok and type(s) == "string" and s ~= "" and s or tostring(handle)
end

-- Named nodes: widgets (ls.UIWidget) by name, and a named node inside a widget.
local function nodes(want)
    local out, seen, visited = {}, {}, 0
    local function walk(node, owner, depth)
        if not node or depth > 40 or visited >= 8000 or seen[tostring(node)] then return end
        seen[tostring(node)] = true
        visited = visited + 1
        local okn, name = pcall(function() return node.Name end)
        if node.Type == "ls.UIWidget" and okn then
            owner = name
            out.widgets[#out.widgets + 1] = name
            out.byWidget[name] = out.byWidget[name] or node
        end
        if okn and want and owner == want.Widget and name == want.Name then out.found = out.found or node end
        for i = 1, math.min(node.VisualChildrenCount or 0, 256) do walk(node:VisualChild(i), owner, depth + 1) end
    end
    out.widgets, out.byWidget = {}, {}
    walk(Ext.UI.GetRoot(), nil, 0)
    return out
end
local function widget(name) return nodes().byWidget[name] end
local function named(widgetName, name) return nodes({Widget = widgetName, Name = name}).found end

local function items(list, max)
    local out = {}
    for i = 1, max or 64 do
        local ok, v = pcall(function() return list[i] end)
        if not ok or v == nil then break end
        out[#out + 1] = v
    end
    return out
end
local function run(cmd, param)
    assert(cmd, "no such command")
    assert(cmd:CanExecute(param), "command unavailable (CanExecute false)")
    cmd:Execute(param)
end
local function choose(listBox, item, dc, vmProp)
    -- As a click does: the list's SelectedItem (its binding sets the view model's); the view model's
    -- property directly when the list is not there.
    if listBox then
        local ok = pcall(function() listBox:SetProperty("SelectedItem", item) end)
        if ok then return "list" end
    end
    dc[vmProp] = item
    return "view model"
end

local actions = {}

function actions.Widgets()
    return {Widgets = nodes().widgets}
end

local function saveKey(v)
    if v == nil then return nil end
    local owner = v.OwnerPlaythrough
    return text(v.Title) .. "|" .. text(v.TimeString) .. "|" .. (owner and text(owner.ProtagonistName) or "")
end

function actions.Load(c)
    assert(Ext.Utils.GetGameState() == "Menu", "Load is for the main menu")
    local mm = assert(widget("MainMenu"), "no main menu").DataContext
    local page = widget("LoadGame")
    if not page then
        run(mm.OpenLoadGameDialog, nil)
        return {Phase = "listing"}
    end
    local dc = page.DataContext
    local wantP = c.Playthrough and tostring(c.Playthrough):lower()
    local wantS = c.Save and tostring(c.Save):lower()
    local rows, pick = {}, nil
    for _, p in ipairs(items(dc.ExistingPlaythroughs)) do
        local name = text(p.ProtagonistName)
        local row = {Protagonist = name, Saves = {}}
        local mine = not wantP or name:lower() == wantP
        for _, s in ipairs(items(p.Saves, 256)) do
            local title = text(s.Title)
            row.Saves[#row.Saves + 1] = title .. " (" .. text(s.TimeString) .. ")"
            if mine and wantS and not pick and title:lower() == wantS then pick = s end
        end
        if mine and not wantS and not pick then pick = p.LatestSave end
        rows[#rows + 1] = row
    end
    if not pick then return {Phase = "no such save", Playthroughs = rows} end
    local info = {Save = text(pick.Title), Time = text(pick.TimeString), Playthroughs = rows}
    if saveKey(mm.SelectedSave) ~= saveKey(pick) then
        pcall(function() dc.SelectedSave = pick end)
        pcall(function() pick.IsSelected = true end)
        pcall(function() mm.SelectedSave = pick end)
        info.Phase = "selecting"
        return info
    end
    run(mm.DoLoad, nil)
    info.Phase = "loading"
    return info
end

function actions.NewGame(c)
    assert(Ext.Utils.GetGameState() == "Menu", "NewGame is for the main menu")
    local w = widget("DMSettings")
    if not w then
        local mm = assert(widget("MainMenu"), "no main menu")
        run(mm.DataContext.StartGameCommand, nil)
        return {Phase = "opening New Game"}
    end
    local dc = w.DataContext
    local preset, names = nil, {}
    for _, p in ipairs(items(dc.Presets)) do
        local n = loca(p.Title)
        names[#names + 1] = n .. " (" .. text(p.AssetName) .. ")"
        local want = c.Preset and tostring(c.Preset):lower()
        if want and not preset and (n:lower():find(want, 1, true) or text(p.AssetName):lower() == want) then preset = p end
    end
    if c.Preset and not preset then return {Phase = "no such preset", Presets = names} end
    preset = preset or dc.CurrentPreset
    assert(preset, "no difficulty preset")
    run(dc.StartNewGame, preset)
    return {Phase = "starting", Preset = loca(preset.Title), Presets = names}
end

-- Character creation, or BG3's respec screen (Withers; widget CharacterRespec, the same view model:
-- FinishCreating, IsCharacterComplete). Returns the view model
-- and the widget's name.
local function ccDc()
    local all = nodes().byWidget
    local name = all.CharacterCreation and "CharacterCreation" or (all.CharacterRespec and "CharacterRespec") or nil
    assert(name, "no character creation or respec screen")
    return all[name].DataContext, name
end

function actions.Origin(c)
    local dc = ccDc()
    local want = tostring(c.Name or ""):lower()
    local pick, names = nil, {}
    for _, o in ipairs(items(dc.SelectableOrigins)) do
        local n = loca(o.Name)
        names[#names + 1] = n
        if n:lower() == want and not pick then pick = o end
    end
    if not pick then return {Phase = "no such origin", Origins = names} end
    local how = choose(named("CharacterCreation", "presetOrigins"), pick, dc, "SelectedOrigin")
    return {Phase = "selected", How = how, Step = text(dc.CharacterCreationStep), Origins = names}
end

-- What character creation shows now (the driver waits for the origin to take).
function actions.CharacterCreation()
    local dc, w = ccDc()
    return {Widget = w, Step = text(dc.CharacterCreationStep), Origin = dc.SelectedOrigin and loca(dc.SelectedOrigin.Name) or nil,
        Complete = dc.IsCharacterComplete == true}
end

function actions.Finish()
    local dc, w = ccDc()
    local complete = dc.IsCharacterComplete == true
    if not complete then return {Phase = "not complete", Widget = w} end
    -- A respec has no guardian page: its finish button runs FinishCreating at once.
    if w == "CharacterRespec" then
        run(dc.FinishCreating, nil)
        return {Phase = "respec finished", Widget = w, Step = text(dc.CharacterCreationStep)}
    end
    -- The guardian page first (the "To Companion" buttons set the flow page's index 2).
    local flow = named("CharacterCreation", "flowPage")
    local step = text(dc.CharacterCreationStep)
    if step ~= "Companion" and flow then
        pcall(function() flow:SetProperty("SelectedIndex", 2) end)
        return {Phase = "to the guardian page", Step = step}
    end
    run(dc.FinishCreating, nil)
    return {Phase = "venture forth", Step = step}
end

local KEYS = {ESCAPE = true, SPACE = true, RETURN = true, F5 = true, F8 = true}
function actions.Key(c)
    assert(KEYS[c.Key], "Key: ESCAPE, SPACE, RETURN, F5 or F8")
    Ext.Input.InjectKeyPress(c.Key)
    return {Phase = "pressed " .. c.Key}
end

function M.Start(runId)
    local sequence, ticks = 0, 0
    local old = readJson("wow-bg3/menu-drive.json")
    if old and old.RunId == runId and math.type(old.Sequence) == "integer" then sequence = old.Sequence end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 15 ~= 0 then return end
        local c = readJson("wow-bg3/menu-drive.json")
        if not c or c.RunId ~= runId or math.type(c.Sequence) ~= "integer" or c.Sequence <= sequence then return end
        sequence = c.Sequence
        local fn = actions[c.Action]
        local ok, value = pcall(function() return assert(fn, "unknown action")(c) end)
        Ext.IO.SaveFile("wow-bg3/menu-drive-result.json", Ext.Json.Stringify({RunId = runId, Sequence = sequence,
            Action = c.Action, State = tostring(Ext.Utils.GetGameState()), OK = ok, Value = ok and value or tostring(value)}))
    end)
end
return M
