-- RenderScreen.lua (client): tells the capture layer / render plugin when a
-- BG3 screen with its own 3D picture is open (character creation's screen: level-up, respec at
-- Withers, the magic mirror). There the layer presents BG3's own picture untouched and stops leaving
-- the figures to WoW (plugins/capture/wowbg3_capture.c, "screen"), so the character BG3 draws there shows
-- instead of WoW's world. File: wow-bg3/link/bg3screen.txt, "1 <widget>" while one is open, "0" when
-- none is; written on a change and once at start. Inventory and character sheet need nothing: BG3
-- draws their figure into a small target of its own (466 x 460 at 1280x720), never on the scene depth.
--
-- Previews: BG3's examine and trade screens draw the character they show into such a picture too (a
-- "preview"); for a stand-in that is the BG3 body under the WoW model. While one shows a stand-in this
-- file writes wow-bg3/link/wowpreview.txt, "1 <screen> <previews> <index> <guid hex> <turn>": the
-- screen's preview count (examine 1; trade 2, the party member's first), which of them is the stand-in
-- (by the guid in Mirrors.lua's mirror-status.json) and BG3's turn of it (radians: the rotation of the
-- ecl::PaperdollComponent entity BG3 renders the preview from, which its rotate arrows and a drag
-- turn); "0" when none does. Looked at every PREVIEW_EVERY ticks, so the line is there within a frame
-- or two of the screen. benilla renders that unit's WoW body, animated and turned the same (patch
-- 0066), and the capture layer draws it over the preview (wowbg3_preview.inc.c); PreviewRaces.lua
-- names its WoW race. Party members need nothing: BG3's own body is theirs.
local M = {}

-- Top-level UI widgets that mean "BG3's own 3D screen". Character creation serves level-up, respec
-- and the mirror; the other names are guesses kept for BG3 versions that split them.
local SCREENS = {"CharacterCreation", "LevelUp", "Respec", "Mirror"}

-- The widgets with a preview: name -> how many previews it draws and which one shows `who`.
local PREVIEWS = {Examine = {1, 0}, Trade = {2, 1}}
local PREVIEW_EVERY = 2 -- ticks between looks for a preview in a running game (3D screens: every 10)

-- The open 3D screen's widget name (or nil), and the open preview widgets by name.
local function openScreen()
    local root = Ext.UI.GetRoot()
    if not root then return nil, {} end
    local found, previews, foundNode = nil, {}, nil
    local function walk(node, depth)
        if not node or depth > 5 then return end
        if node.Type == "ls.UIWidget" then
            local ok, name = pcall(function() return node.Name end)
            if ok and type(name) == "string" then
                local okv, vis = pcall(function() return node.Visibility end)
                local visible = not okv or tostring(vis) == "Visible"
                if PREVIEWS[name] and visible then previews[name] = node end
                for _, s in ipairs(SCREENS) do
                    if not found and name:find(s, 1, true) then
                        if visible then found, foundNode = name, node end
                        return
                    end
                end
            end
            return -- widgets do not nest screens
        end
        for i = 1, math.min(node.VisualChildrenCount or 0, 128) do walk(node:VisualChild(i), depth + 1) end
    end
    walk(root, 0)
    return found, previews, foundNode
end

-- The character a preview widget shows: trade's trader, examine's character (the first ls.Character
-- view model under it: a Grid's DataContext).
local function previewUuid(name, node)
    if name == "Trade" then
        local ok, uuid = pcall(function() return node.DataContext.Trader.EntityUUID end)
        return ok and uuid or nil
    end
    local uuid
    local function walk(n, depth)
        if uuid or not n or depth > 12 then return end
        local okd, dc = pcall(function() return n.DataContext end)
        if okd and dc and tostring(dc):sub(1, 13) == "ls.Character " then
            local oku, u = pcall(function() return dc.EntityUUID end)
            if oku and u then uuid = u return end
        end
        for i = 1, math.min(n.VisualChildrenCount or 0, 64) do walk(n:VisualChild(i), depth + 1) end
    end
    walk(node, 0)
    return uuid
end

-- Stand-in uuid -> WoW guid (hex), from Mirrors.lua's status (remembered: looked at every tick).
local guids = {}
local function standInGuid(uuid)
    if guids[uuid] ~= nil then return guids[uuid] or nil end
    local raw = Ext.IO.LoadFile("wow-bg3/link/mirror-status.json")
    local ok, st = pcall(Ext.Json.Parse, raw or "")
    if not ok or type(st) ~= "table" or type(st.Mobs) ~= "table" then return nil end
    guids = {}
    for g, m in pairs(st.Mobs) do
        if type(m) == "table" and type(m.Uuid) == "string" then guids[m.Uuid] = g end
    end
    -- Not a stand-in (a party member): false until the next stand-in shows.
    if guids[uuid] == nil then guids[uuid] = false end
    return guids[uuid] or nil
end

-- BG3's turn of the preview of `uuid`: the yaw of the paperdoll entity made for it (cached).
local dolls = {} -- character uuid -> its paperdoll entity
local function previewTurn(uuid)
    local function yaw(doll)
        local q = doll.Transform.Transform.RotationQuat
        return 2 * math.atan(q[2], q[4])
    end
    local doll = dolls[uuid]
    if doll then
        local ok, y = pcall(yaw, doll)
        if ok then return y end
        dolls[uuid] = nil
    end
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("ClientPaperdoll") or {}) do
        local ok, of = pcall(function() return e.ClientPaperdoll.Entity.Uuid.EntityUuid end)
        if ok and of == uuid then
            dolls[uuid] = e
            local okY, y = pcall(yaw, e)
            return okY and y or nil
        end
    end
end

-- wowpreview.txt's line for the open preview widgets.
local function previewLine(previews)
    for _, name in ipairs({"Trade", "Examine"}) do
        local node = previews[name]
        if node then
            local okU, uuid = pcall(previewUuid, name, node)
            local guid = okU and type(uuid) == "string" and standInGuid(uuid)
            if guid then
                local p = PREVIEWS[name]
                local okT, turn = pcall(previewTurn, uuid)
                turn = okT and turn or nil
                return string.format("1 %s %d %d %s%s", name, p[1], p[2], guid, turn and string.format(" %.3f", turn) or "")
            end
        end
    end
    return "0"
end

function M.Start(runId)
    local ticks, last, lastPreview, lastFor = 0, nil, nil, nil
    local partyDialog = false
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % PREVIEW_EVERY ~= 0 then return end
        -- Only in a running game for the preview look (every tick through the splash screen, BG3's UI
        -- crashed in the Script Extender's UI bridge, harness); the 3D screens' look keeps its pace.
        if ticks % 10 ~= 0 and Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, name, previews, node = pcall(openScreen)
        local now = ok and name and ("1 " .. name) or "0"
        -- A dialog among party members alone (Hero.lua's hero.json PartyDialog): BG3's own picture, as for
        -- its 3D screens, so the actors stand, turn and speak as BG3's dialog has them.
        if now == "0" and ticks % 10 == 0 then
            local okD, party = pcall(function()
                local h = Ext.Json.Parse(Ext.IO.LoadFile("wow-bg3/hero.json") or "{}")
                return h.PartyDialog == true and h.InDialog == true
            end)
            partyDialog = okD and party or false
        end
        if now == "0" and partyDialog then now = "1 Dialogue" end
        if now ~= last and ticks % 10 == 0 then
            last = now
            Ext.IO.SaveFile("wow-bg3/link/bg3screen.txt", now .. "\n")
        end
        -- Whose level-up BG3's Level Up screen shows (its LevelUpCharacter's display-name handle, as
        -- Osiris GetDisplayName gives it): link/levelupfor.txt, empty when none. Progression.lua then
        -- withdraws every other member's offer, so accepting it does not chain BG3's screen on to them
        -- (a playtest: the screen went on to Shadowheart after Tav took the pool's only level-up,
        -- and she took one more than WoW gave).
        local okF, who = pcall(function()
            if ok and name == "CharacterLevelUp" and node then return tostring(node.DataContext.LevelUpCharacter.Name) end
            return ""
        end)
        who = okF and who or ""
        if who ~= lastFor then
            lastFor = who
            Ext.IO.SaveFile("wow-bg3/link/levelupfor.txt", who .. "\n")
        end
        local okP, preview = pcall(previewLine, ok and previews or {})
        preview = okP and preview or "0"
        if preview ~= lastPreview then
            lastPreview = preview
            Ext.IO.SaveFile("wow-bg3/link/wowpreview.txt", preview .. "\n")
        end
    end)
end

return M
