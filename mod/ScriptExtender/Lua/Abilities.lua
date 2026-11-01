-- Mob abilities: a WoW mob's stand-in uses the BG3 abilities that match what it casts in WoW. Switch: `mob_abilities`
-- in settings.conf, the launcher's wow-bg3/mob-abilities.json {On}; off, nothing here runs and stand-ins keep
-- their template's actions.
--
-- Data, tools/wow_abilities.py, made on the player's machine from the release's seed or the server's database and
-- BG3's stats files: wow-bg3/link/abilities.json: Mobs, creature entry -> copies; Spells, copy -> {B, the BG3 spell
-- it copies; Wn, the WoW spell, for the log}; Names, string handle -> WoW's name. abilities-stats.txt: the copies as
-- BG3 stats text. A copy has no slot cost or class requirement, its own damage and save DC, how often it may fire,
-- and WoW's name. The numbers follow the balance research (Scaling.lua): see the tool. abilities-stats-scaled.txt,
-- for the switch `scaling`, has SourceSpellDC() for the fixed DC: the stand-in's SpellSaveDC boost says the same at
-- the party's numbers.
--
-- BG3's own loader reads the copies: the stats file goes in place of a small game file whose text it starts with
-- (bg3le patch 0006, abilities_first in wowbg3-coord.py; no mod's Lua runs before BG3 loads its stats). No Lua per
-- copy: Ext.Stats.Create and SetRawAttribute cost 8.4 s for 1,355 copies on every start. Never create a copy later:
-- one made and synced in a running game goes into a spellbook, then crashes BG3 (signal 11 on a worker thread) when
-- the AI reaches it, even with no field changed (harness 2026-10-31).
--
-- Status: wow-bg3/link/abilities-status.json: what was granted, what failed, every cast of a copy and every hit a
-- stand-in lands, with the copy it was using.
local M = {}

local okScaling, Scaling = pcall(Ext.Require, "Scaling.lua")
local IO = "wow-bg3/link/"
local STATUS = IO .. "abilities-status.json"
local PREFIX = "WOWBG3_AB_"
local client = Ext.IsClient and Ext.IsClient()

local onCached = nil
function M.On()
    if onCached == nil then
        local ok, c = pcall(function() return Ext.Json.Parse(Ext.IO.LoadFile("wow-bg3/mob-abilities.json") or "") end)
        onCached = ok and type(c) == "table" and c.On == true
    end
    return onCached
end

local state = {Grants = 0, Mobs = 0, Made = 0, Failed = {}, Casts = {}, Hits = {}, Others = 0}
local granted = {} -- stand-in uuid -> creature entry
local using = {} -- stand-in uuid -> the spell it is casting
local tbl = nil

local function saveStatus()
    pcall(function() Ext.IO.SaveFile(STATUS, Ext.Json.Stringify(state)) end)
end
local function fail(what)
    if #state.Failed < 40 then state.Failed[#state.Failed + 1] = what end
    Ext.Log.Print("WOW-BG3 abilities: " .. what)
end

local function loadTable()
    if tbl ~= nil then return tbl end
    tbl = false
    local ok, t = pcall(function() return Ext.Json.Parse(Ext.IO.LoadFile(IO .. "abilities.json") or "") end)
    if ok and type(t) == "table" and type(t.Mobs) == "table" and type(t.Spells) == "table" then
        tbl = t
    end
    return tbl
end

-- A stand-in of WoW creature `entry` gets its abilities. Safe to call again.
function M.Grant(uuid, entry)
    if not M.On() or client or granted[uuid] or type(entry) ~= "number" then return end
    local t = loadTable()
    if not t then return end
    local list = t.Mobs[tostring(math.floor(entry))]
    granted[uuid] = entry
    if type(list) ~= "table" then return end
    state.Mobs = state.Mobs + 1
    for _, key in ipairs(list) do
        local name = PREFIX .. key
        -- Only what BG3 loaded (see the header).
        if Ext.Stats.Get(name) then
            local added = pcall(Osi.AddSpell, uuid, name, 0, 1)
            if added then state.Grants = state.Grants + 1 else fail("AddSpell " .. name) end
        else
            state.NotMade = (state.NotMade or 0) + 1
        end
    end
    saveStatus()
end

function M.Start(runId)
    if not M.On() then return end
    state.RunId = runId
    local t = loadTable()
    if not t then saveStatus(); return end
    state.File = okScaling and Scaling.On() and "abilities-stats-scaled.txt" or "abilities-stats.txt"
    -- The client gets StatsLoaded; the server starts later, with the stats long loaded.
    local function loaded()
        local made = 0
        for key in pairs(t.Spells) do
            if Ext.Stats.Get(PREFIX .. key) then made = made + 1 end
        end
        state.Made = made
        for handle, name in pairs(t.Names or {}) do
            pcall(Ext.Loca.UpdateTranslatedString, handle, name)
        end
        Ext.Log.Print(string.format("WOW-BG3 abilities: %d copies loaded from %s (%s)", made, state.File, client and "client" or "server"))
    end
    if client then Ext.Events.StatsLoaded:Subscribe(loaded); return end
    loaded()
    local function uuidOf(guid) return type(guid) == "string" and guid:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") end
    pcall(Ext.Osiris.RegisterListener, "UsingSpell", 5, "before", function(caster, spell)
        local uuid = uuidOf(caster)
        if uuid and granted[uuid] then using[uuid] = spell end
    end)
    -- The proof, and what to tune by: which copies BG3's AI really casts and what they do.
    pcall(Ext.Osiris.RegisterListener, "CastedSpell", 5, "after", function(caster, spell)
        local uuid = uuidOf(caster)
        if not (uuid and granted[uuid]) then return end
        if type(spell) == "string" and spell:sub(1, #PREFIX) == PREFIX then
            local def = (tbl and tbl.Spells[spell:sub(#PREFIX + 1)]) or {}
            if #state.Casts < 200 then
                state.Casts[#state.Casts + 1] = {Entry = granted[uuid], Spell = spell, Base = def.B, Wow = def.Wn}
            end
            saveStatus()
        else
            state.Others = state.Others + 1
        end
    end)
    pcall(Ext.Osiris.RegisterListener, "AttackedBy", 7, "after", function(_, _, attacker, damageType, amount)
        local uuid = uuidOf(attacker)
        if not (uuid and granted[uuid]) or #state.Hits >= 300 then return end
        state.Hits[#state.Hits + 1] = {Entry = granted[uuid], Spell = using[uuid], Type = damageType, Amount = amount}
        saveStatus()
    end)
    saveStatus()
end

return M
