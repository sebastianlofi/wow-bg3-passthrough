-- InventoryClient (client): the client half of Inventory.lua.
--
-- 1. WoW's icons in BG3's icon atlas: link/tools/wow_icons.py writes a bigger copy of BG3's
--    Icons_Items_6 atlas (BG3's own 163 icons plus every WoW item icon the item table uses, from the
--    player's own files) to Script Extender/wow-bg3/gui/. At bootstrap, before BG3 loads its GUI
--    atlases, both files go in place of the game's (Ext.IO.AddPathOverride; bg3le honours it in
--    ls::FileReader, as for WowUi.lua's XAML), so the names "WowBg3_<icon>" that Inventory.lua puts on
--    its items' templates resolve to WoW's pictures in every BG3 screen (inventory, trade, loot,
--    tooltips, hotbar). Without the files (tool not run) nothing changes: items keep the base's icon.
-- 2. The same stats entries as the server (Inventory.MakeUsed at StatsLoaded) and the one string table they
--    share (Inventory.MakeStrings), so tooltips show WoW names, WoW text and the mapped numbers.
-- Status: wow-bg3/inventory-client.json.
local M = {}
local SE_DIR = "Larian Studios/Baldur's Gate 3/Script Extender"
local GAME_LSX = "Public/Shared/GUI/Icons_Items_6.lsx"
local GAME_DDS = "Public/Shared/Assets/Textures/Icons/Icons_Items_6.dds"

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

function M.Start(runId)
    local status = {RunId = runId}
    local ok, err = pcall(function()
        local list = Ext.IO.LoadFile("wow-bg3/gui/wow-icons.json")
        if not list then status.Atlas = "no wow-icons.json (run link/tools/wow_icons.py)"; return end
        local root, se = assert(dataRoot(), "no data root"), assert(seRoot(), "no SE root")
        Ext.IO.AddPathOverride(GAME_LSX, fromRoot(root, se .. "/wow-bg3/gui/Icons_Items_6.lsx"))
        Ext.IO.AddPathOverride(GAME_DDS, fromRoot(root, se .. "/wow-bg3/gui/Icons_Items_6.dds"))
        status.Atlas = "overridden"
        status.Lsx = Ext.IO.GetPathOverride(GAME_LSX)
    end)
    if not ok then status.Atlas = "error: " .. tostring(err) end
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
    Ext.Events.SessionLoaded:Subscribe(save)
    Ext.Events.GameStateChanged:Subscribe(function(e) if tostring(e.ToState) == "Running" then save() end end)
end

return M
