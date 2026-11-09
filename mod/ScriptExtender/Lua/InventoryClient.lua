-- InventoryClient (client): the client half of Inventory.lua.
--
-- 1. WoW's icons as an icon atlas of our own: tools/wow_icons.py writes Icons_WowBg3 (every WoW item icon the item
--    table uses, from the player's own files) to Script Extender/wow-bg3/gui/, in the background of the first
--    session. BG3 reads its atlases once, at boot, so a new install's first session has no such files then. When
--    they exist (wow-icons.json is written last), both go in place of Public/WOWBG3's own paths
--    (Ext.IO.AddPathOverride; bg3le honours it in ls::FileReader, as for WowUi.lua's XAML) and BG3's own atlas load
--    takes the file (Ext.StaticData.LoadIconAtlas, bg3le): the names "WowBg3_<icon>" that Inventory.lua puts on its
--    items' templates resolve to WoW's pictures in every BG3 screen (inventory, trade, loot, tooltips, hotbar),
--    in the running game. Looked for at start and then every two seconds until it is in.
-- 2. The same stats entries as the server (Inventory.MakeUsed at StatsLoaded) and the one string table they
--    share (Inventory.MakeStrings), so tooltips show WoW names, WoW text and the mapped numbers.
-- Status: wow-bg3/inventory-client.json.
local M = {}
local SE_DIR = "Larian Studios/Baldur's Gate 3/Script Extender"
local GAME_LSX = "Public/WOWBG3/GUI/Icons_WowBg3.lsx"
local GAME_DDS = "Public/WOWBG3/Assets/Textures/Icons/Icons_WowBg3.dds"

local function dataRoot()
    Ext.IO.AddPathOverride("WowBg3Inv/root-probe", "x")
    local abs = Ext.IO.GetPathOverride("WowBg3Inv/root-probe")
    if type(abs) ~= "string" or abs:sub(-2) ~= "/x" then return nil end
    return abs:sub(1, -3)
end

local function fromRoot(root, abs)
    local depth = 0
    for _ in root:gmatch("[^/]+") do depth = depth + 1 end
    return string.rep("../", depth) .. abs:gsub("^/+", "")
end

local function seRoot()
    local xdg = Ext._Internal.Getenv("XDG_DATA_HOME")
    if type(xdg) ~= "string" or xdg == "" then
        local home = Ext._Internal.Getenv("HOME")
        if type(home) ~= "string" or home == "" then return nil end
        xdg = home .. "/.local/share"
    end
    return xdg .. "/" .. SE_DIR
end

-- The atlas into the running game, once its files exist. True when it is in.
local function loadAtlas(status)
    local ok, err = pcall(function()
        local text = Ext.IO.LoadFile("wow-bg3/gui/wow-icons.json")
        local list = text and Ext.Json.Parse(text)
        if not list or list.Atlas ~= "Icons_WowBg3" then status.Atlas = "not made yet"; return end
        if not (Ext.StaticData and Ext.StaticData.LoadIconAtlas) then status.Atlas = "this bg3le cannot load an atlas"; return end
        local root, se = assert(dataRoot(), "no data root"), assert(seRoot(), "no SE root")
        Ext.IO.AddPathOverride(GAME_LSX, fromRoot(root, se .. "/wow-bg3/gui/Icons_WowBg3.lsx"))
        Ext.IO.AddPathOverride(GAME_DDS, fromRoot(root, se .. "/wow-bg3/gui/Icons_WowBg3.dds"))
        local r = Ext.StaticData.LoadIconAtlas(root .. "/" .. GAME_LSX)
        if r == true then status.Atlas = "loaded"; status.Icons = #(list.Icons or {})
        elseif r == false then status.Atlas = "BG3 refused the atlas file"
        else status.Atlas = "BG3's atlas map is not up yet" end
    end)
    if not ok then status.Atlas = "error: " .. tostring(err) end
    return status.Atlas == "loaded"
end

function M.Start(runId)
    local status = {RunId = runId}
    local atlasIn = loadAtlas(status)
    local Inv = Ext.Require("Inventory.lua")
    Inv.Start(runId)
    local function save()
        status.MadeMs = Inv.MadeMs
        pcall(function()
            local a = Ext.StaticData and Ext.StaticData.GetIconAtlas and Ext.StaticData.GetIconAtlas("WowBg3_inv_sword_04")
            status.IconKnown = a ~= nil
        end)
        Ext.IO.SaveFile("wow-bg3/inventory-client.json", Ext.Json.Stringify(status))
    end
    save()
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        if atlasIn then return end
        ticks = ticks + 1
        if ticks % 120 ~= 0 then return end
        local before = status.Atlas
        atlasIn = loadAtlas(status)
        if status.Atlas ~= before then save() end
    end)
    Ext.Events.SessionLoaded:Subscribe(save)
    Ext.Events.GameStateChanged:Subscribe(function(e) if tostring(e.ToState) == "Running" then save() end end)
end

return M
