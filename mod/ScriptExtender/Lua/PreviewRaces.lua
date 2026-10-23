-- WoW races in BG3's examine and trade screens (server context). Those screens name a character's race
-- from its eoc::RaceComponent (the race resource's DisplayName; examine's line above it is the race's
-- DisplayTypeUUID's name), so a stand-in reads "Human" whatever its WoW mob is. While a screen shows a
-- stand-in (RenderScreen.lua's wow-bg3/link/wowpreview.txt, "1 <screen> <previews> <index> <guid> ..."),
-- this file gives the stand-in a race named as WoW names it and puts its own race back when the screen
-- closes or shows someone else; the screens follow a change while open (in the harness: "Lv 10
-- Half-Orc" became "Lv 10 Orc" with the trade screen open, and back).
-- The names come from benilla (patch 0066): wow-bg3/link/wowbody/<guid>.txt, `race` (the race of a
-- player-race model, else the beast family, else the creature type) and `type` (the creature type).
-- The races exist only at run time: made at start (Ext.StaticData.Create, names through new
-- Ext.Loca handles), one per name in NAMES and TYPES, with fixed uuids, so a game saved while a screen
-- was open finds them again on load. A WoW race's parent and display type is a type race (Humanoid for
-- the races, Beast for the families); a type race displays itself.
local M = {}
local DIR = "wow-bg3/link/"
local EVERY = 1 -- ticks between looks at the request (a few bytes)
-- Creature types (CreatureType.dbc), then WoW's races and 1.12's beast families.
local TYPES = {"Beast", "Dragonkin", "Demon", "Elemental", "Giant", "Undead", "Humanoid", "Critter", "Mechanical", "Creature"}
local NAMES = {"Human", "Orc", "Dwarf", "Night Elf", "Tauren", "Gnome", "Troll", "Wolf", "Cat", "Spider", "Bear", "Boar",
    "Crocolisk", "Carrion Bird", "Crab", "Gorilla", "Raptor", "Tallstrider", "Felhunter", "Voidwalker", "Succubus",
    "Doomguard", "Scorpid", "Turtle", "Imp", "Bat", "Hyena", "Owl", "Wind Serpent"}
local races = {} -- name -> race uuid
M.Stats = {Made = 0, Failed = 0, Swapped = 0, Restored = 0}

-- The n-th name's uuid and loca handle (fixed: n is its place in TYPES then NAMES).
local function ids(n)
    local tail = string.format("%012x", 0xa000 + n)
    return "7f0b5e2a-0001-4c1d-9a3e-" .. tail, "h7f0b5e2ag0001g4c1dg9a3eg" .. tail
end

local function make(n, name, typeUuid)
    local uuid, handle = ids(n)
    pcall(Ext.Loca.UpdateTranslatedString, handle, name)
    local race = Ext.StaticData.Get(uuid, "Race")
    if not race then
        local ok, made = pcall(Ext.StaticData.Create, "Race", uuid)
        race = ok and made or nil
    end
    if not race then
        M.Stats.Failed = M.Stats.Failed + 1
        return nil
    end
    local ok = pcall(function()
        race.Name = "WowBg3" .. name:gsub("%s", "")
        race.DisplayName.Handle.Handle = handle
        race.Description.Handle.Handle = handle
        -- A type names itself above the race line; a race's parent is its type (never itself: a
        -- parent chain must end).
        race.DisplayTypeUUID = typeUuid or uuid
        if typeUuid then race.ParentGuid = typeUuid end
    end)
    if not ok then
        M.Stats.Failed = M.Stats.Failed + 1
        return nil
    end
    M.Stats.Made = M.Stats.Made + 1
    return uuid
end

local function makeAll()
    for i, name in ipairs(TYPES) do races[name] = make(i, name, nil) end
    -- A WoW race's type is Humanoid, a family's Beast (the races files name the actual type; these
    -- are only the races' own parents).
    for i, name in ipairs(NAMES) do
        if not races[name] then races[name] = make(#TYPES + i, name, races[i <= 7 and "Humanoid" or "Beast"]) end
    end
end

-- The stand-in a preview shows: its guid from the request, its uuid from Mirrors.lua's table.
local function shown()
    local line = Ext.IO.LoadFile(DIR .. "wowpreview.txt") or ""
    local guid = line:match("^1 %S+ %d+ %d+ (%x+)")
    if not guid then return nil end
    for uuid, g in pairs(WOWBG3MirrorOwned or {}) do
        if tostring(g):lower() == guid:lower() then return uuid, guid end
    end
end

local function kindOf(guid)
    local text = Ext.IO.LoadFile(DIR .. "wowbody/" .. guid:lower() .. ".txt") or ""
    return text:match("race ([^\n]+)"), text:match("type ([^\n]+)")
end

function M.Start(runId)
    local ok, err = pcall(makeAll)
    if not ok then Ext.Log.Print("WOW-BG3 preview races: " .. tostring(err)) end
    local ticks = 0
    local swapped = {} -- stand-in uuid -> its own race uuid
    local function restore(except)
        for uuid, own in pairs(swapped) do
            if uuid ~= except then
                pcall(function()
                    local e = Ext.Entity.Get(uuid)
                    e.Race.Race = own
                    e:Replicate("Race")
                end)
                swapped[uuid] = nil
                M.Stats.Restored = M.Stats.Restored + 1
            end
        end
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % EVERY ~= 0 then return end
        local okS, uuid, guid = pcall(shown)
        if not okS then uuid = nil end
        restore(uuid)
        if not uuid or swapped[uuid] then return end
        local race, kind = kindOf(guid)
        local want = races[race or ""] or races[kind or ""]
        if not want then return end -- benilla has not named it yet: next look
        pcall(function()
            local e = Ext.Entity.Get(uuid)
            local own = e.Race.Race
            if own == want then return end
            e.Race.Race = want
            e:Replicate("Race")
            swapped[uuid] = own
            M.Stats.Swapped = M.Stats.Swapped + 1
        end)
    end)
end

return M
