-- WoW's signature spells at BG3's level-ups (both sides). Behind the switch `wow_spells`
-- (wow-bg3/wow-spells.json {"On": true}, written by the coordinator from settings.conf), off by default.
-- The first batch is the Mage's, for Wizards and Sorcerers: the stats are data files in the pak
-- (Public/WOWBG3/Stats/Generated/Data/*_WowBg3.txt), always loaded and inert until a passive unlocks them.
--
-- The way in, as BG3 gives Rogue its Sneak Attack (a passive whose boost is UnlockSpell): a sweep on the server gives
-- each member the passives of its class levels, takes them back from a member who no longer has the level, and from
-- everyone with the switch off. A Progression row can't do it: one written from Lua at the session's start reads back
-- fine, and neither the level-up nor its screen sees it (seen in game), so a notice says what was learned.
-- BG3's own effects draw these spells for now; WoW's kits on the hidden body come later.
-- Names: link/spells.json (wow_spells.py, first run, from the player's own client), else plain words.
-- Conjuring asks WoW for its real conjured items by entry (Inventory.lua, op add): 3 a cast, one cast a rest.
local M = {}
local Hero = Ext.Require("Hero.lua")

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function log(s) Ext.Utils.Print("WOW-BG3 spells: " .. s) end

-- The switch, read once per Lua session on each side (a new setting takes a restart).
local onCached = nil
function M.On()
    if onCached == nil then
        local ok, c = pcall(Ext.Json.Parse, q(Ext.IO.LoadFile, "wow-bg3/wow-spells.json") or "")
        onCached = ok and type(c) == "table" and c.On == true
    end
    return onCached
end

-- Class name -> class level -> unlock passives. The level rule: docs in the project's spell report.
M.ROWS = {
    Wizard = {[2] = {"WowBg3_Unlock_FrostArmor"}, [3] = {"WowBg3_Unlock_Nova_Int", "WowBg3_Unlock_Conjure"},
              [5] = {"WowBg3_Unlock_IceBarrier"}},
    Sorcerer = {[2] = {"WowBg3_Unlock_FrostArmor"}, [3] = {"WowBg3_Unlock_Nova_Cha", "WowBg3_Unlock_Conjure"},
                [5] = {"WowBg3_Unlock_IceBarrier"}},
}

local function H(n) return string.format("h7f0b5e2ag0003g4c1dg9a3eg%012d", n) end
-- key in spells.json, handle number, plain name, our own description
M.TEXT = {
    {"FrostArmor", 1, "Frost Ward", "Ice wraps you until your next long rest: +2 Armour Class while you wear no armour, and a melee attacker that hits you loses 3 m of movement for a turn. It replaces Mage Armour, and Mage Armour replaces it."},
    {"Nova", 3, "Frost Burst", "Enemies within 4 m make a Dexterity save. On a failure they take 1d8 cold damage and can't move for 2 turns. On a success they take half."},
    {"IceBarrier", 5, "Ice Shield", "You gain 15 temporary hit points until they are gone. Once per fight."},
    {"Conjure", 7, "Conjure Supplies", "Out of combat, conjure 2 food and 1 drink for your WoW level into your WoW bag. They count as camp supplies and sell for nothing. Once per long rest."},
    {nil, 9, "Chilled", "Movement reduced by 3 m."},
    {nil, 11, "Frostbound", "Can't move."},
}

local function writeText()
    local ok, names = pcall(Ext.Json.Parse, q(Ext.IO.LoadFile, "wow-bg3/link/spells.json") or "")
    names = ok and type(names) == "table" and names or {}
    for _, t in ipairs(M.TEXT) do
        pcall(Ext.Loca.UpdateTranslatedString, H(t[2]), t[1] and names[t[1]] or t[3])
        pcall(Ext.Loca.UpdateTranslatedString, H(t[2] + 1), t[4])
    end
end

-- Class name -> levels of a member.
local function classLevels(u)
    local out = {}
    local e = q(Ext.Entity.Get, u)
    for _, c in ipairs(e and q(function() return e.Classes.Classes end) or {}) do
        local d = q(Ext.StaticData.Get, c.ClassUUID, "ClassDescription")
        if d then out[tostring(d.Name)] = (out[tostring(d.Name)] or 0) + c.Level end
    end
    return out
end

-- Unlock passive -> its spell's name handle (M.TEXT), for the notice.
local NAME_OF = {WowBg3_Unlock_FrostArmor = 1, WowBg3_Unlock_Nova_Int = 3, WowBg3_Unlock_Nova_Cha = 3,
                 WowBg3_Unlock_IceBarrier = 5, WowBg3_Unlock_Conjure = 7}
local ALL = {}
for _, levels in pairs(M.ROWS) do
    for _, ps in pairs(levels) do
        for _, p in ipairs(ps) do ALL[p] = true end
    end
end

-- What a member should have: every passive of a class row at or below its level in that class.
function M.Wanted(levels, on)
    local want = {}
    if not on then return want end
    for class, lv in pairs(levels) do
        for at, ps in pairs(M.ROWS[class] or {}) do
            if lv >= at then
                for _, p in ipairs(ps) do want[p] = true end
            end
        end
    end
    return want
end

local function sweep(members)
    for _, u in ipairs(members) do
        local want = M.Wanted(classLevels(u), M.On())
        for p in pairs(ALL) do
            local has = q(Osi.HasPassive, u, p) == 1
            if want[p] and not has then
                pcall(Osi.AddPassive, u, p)
                log("gave " .. p .. " to " .. u)
                pcall(Osi.ShowNotification, u, "New spell: " .. Ext.Loca.GetTranslatedString(H(NAME_OF[p])))
            elseif has and not want[p] then
                pcall(Osi.RemovePassive, u, p)
                log("took " .. p .. " from " .. u)
            end
        end
    end
end

-- WoW's conjured items by the level they ask for: {level, food, drink}. Entries only, the names stay in the client.
M.CONJURED = {{55, 22895, 8079}, {45, 8076, 8078}, {35, 8075, 8077}, {25, 1487, 3772}, {15, 1114, 2136}, {5, 1113, 2288}, {1, 5349, 5350}}
function M.ConjuredFor(wowLevel)
    for _, row in ipairs(M.CONJURED) do
        if wowLevel >= row[1] then return row[2], row[3] end
    end
    return M.CONJURED[#M.CONJURED][2], M.CONJURED[#M.CONJURED][3]
end

local function conjure(caster)
    local Inventory = Ext.Require("Inventory.lua")
    local Progression = Ext.Require("Progression.lua")
    local food, drink = M.ConjuredFor(Progression.WowLevel())
    Inventory.Request({Op = "add", Member = caster, Entry = food, Count = 2})
    Inventory.Request({Op = "add", Member = caster, Entry = drink, Count = 1})
    log(string.format("conjured %d x2, %d x1 for %s", food, drink, caster))
end

function M.Start(context)
    writeText()
    local session = false
    Ext.Events.GameStateChanged:Subscribe(function(e)
        local to = tostring(e.ToState)
        if to ~= "Running" and to ~= "Save" and to ~= "Paused" and to ~= "GameMasterPause" then session = false end
    end)
    local Party = context == "server" and Ext.Require("Party.lua") or nil
    if Party then
        pcall(Ext.Osiris.RegisterListener, "CastedSpell", 5, "after", function(caster, spell)
            if M.On() and spell == "Shout_WowBg3_Conjure" then
                pcall(conjure, Hero.UuidOf(tostring(caster)))
            end
        end)
        -- Stock's Mage Armour knows nothing of ours: the two exclude each other, as WoW's armours do.
        pcall(Ext.Osiris.RegisterListener, "StatusApplied", 4, "after", function(target, status)
            if M.On() and status == "MAGE_ARMOR" then pcall(Osi.RemoveStatus, target, "WOWBG3_FROST_ARMOR") end
        end)
    end
    local tick, again = 0, false
    if Party then
        for _, ev in ipairs({"LeveledUp", "RespecCompleted"}) do
            pcall(Ext.Osiris.RegisterListener, ev, 1, "after", function() again = true end)
        end
    end
    Ext.Events.Tick:Subscribe(function()
        if Ext.Utils.GetGameState() ~= "Running" then return end
        tick = tick + 1
        if not session then
            session = true
            tick, again = 0, true
            writeText()
        end
        -- Ten ticks and more after the event: BG3 has applied a level's rows by then. Every 10 s for a new member.
        if Party and ((again and tick >= 30) or tick >= 600) then
            again, tick = false, 0
            local ok, err = pcall(sweep, Party.Party())
            if not ok then log(tostring(err)) end
        end
    end)
end

return M
