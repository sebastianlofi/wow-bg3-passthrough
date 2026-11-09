-- Mob abilities: a WoW mob's stand-in uses the BG3 abilities that match what it casts in WoW. Switch: `mob_abilities`
-- in settings.conf, the launcher's wow-bg3/mob-abilities.json {On}; off, nothing here runs and stand-ins keep
-- their template's actions.
--
-- Data, tools/wow_abilities.py, made on the player's machine from its seed of the server's database (and the rules) and
-- BG3's stats files: wow-bg3/link/abilities.json: Mobs, creature entry -> copies; Spells, copy -> {B, the BG3 spell
-- it copies; Wn, the WoW spell, for the log}; Names, string handle -> WoW's name. abilities-stats.txt: the copies as
-- BG3 stats text. A copy has no slot cost or class requirement, its own damage and save DC, how often it may fire,
-- and WoW's name. The numbers follow the balance research (Scaling.lua): see the tool. abilities-stats-scaled.txt,
-- for the switch `scaling`, has SourceSpellDC() for the fixed DC: the stand-in's SpellSaveDC boost says the same at
-- the party's numbers.
--
-- Made when needed: a stand-in's copies are created as it is granted (Ext.Stats.Create from its entry in the stats
-- file, its fields, Sync), only those of the mobs in the scene, about 6 ms each; nothing at BG3's boot. A copy made
-- in a running game used to crash BG3 on its holder's first AI turn: bg3le's Sync left it out of the spell manager's
-- CombatAIOverrideSpells, which the AI reads unchecked (bg3le patch 0009 files it there, as BG3's own load does).
-- On a new install's first session the table is made once the server runs (wowbg3-coord.py datagen): stand-ins
-- spawned before it wait in `pending` and are granted when it is there.
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
local defs = nil -- copy name -> {Type, Using, Data = {{key, value}, ...}}, from the stats file
local tried = 0  -- Ext.Utils.MonotonicTime of the last look for the files, while they are not there
local pending = {} -- stand-in uuid -> creature entry, granted once the table is there
-- The BG3 spell a copy copies; any other spell's own name.
function M.BaseOf(spell)
    local def = tbl and spell:sub(1, #PREFIX) == PREFIX and tbl.Spells[spell:sub(#PREFIX + 1)]
    return def and def.B or spell
end

local function saveStatus()
    pcall(function() Ext.IO.SaveFile(STATUS, Ext.Json.Stringify(state)) end)
end
local function fail(what)
    if #state.Failed < 40 then state.Failed[#state.Failed + 1] = what end
    Ext.Log.Print("WOW-BG3 abilities: " .. what)
end

-- The copies' stats text (wow_abilities.py: `new entry`, `type`, `using`, `data` lines; the file starts with the
-- text of the game file it once stood in for, whose entries are not ours).
local function parseStats(text)
    local out, cur = {}, nil
    for line in text:gmatch("[^\r\n]+") do
        local name = line:match('^new entry "(.-)"')
        if name then
            cur = name:sub(1, #PREFIX) == PREFIX and {Type = "SpellData", Data = {}} or nil
            if cur then out[name] = cur end
        elseif cur then
            local t = line:match('^type "(.-)"')
            local u = line:match('^using "(.-)"')
            local k, v = line:match('^data "(.-)" "(.*)"$')
            if t then cur.Type = t elseif u then cur.Using = u elseif k then cur.Data[#cur.Data + 1] = {k, v} end
        end
    end
    return out
end

-- The table and the stats file, read once they are there (looked for again every 5 s until then).
local function loadTable()
    if tbl then return tbl end
    local now = Ext.Utils.MonotonicTime()
    if tried ~= 0 and now - tried < 5000 then return nil end
    tried = now
    local ok, t = pcall(function() return Ext.Json.Parse(Ext.IO.LoadFile(IO .. "abilities.json") or "") end)
    if not (ok and type(t) == "table" and type(t.Mobs) == "table" and type(t.Spells) == "table") then return nil end
    local file = okScaling and Scaling.On() and "abilities-stats-scaled.txt" or "abilities-stats.txt"
    local text = Ext.IO.LoadFile(IO .. file)
    if not text then return nil end
    defs, tbl, state.File = parseStats(text), t, file
    for handle, name in pairs(t.Names or {}) do
        pcall(Ext.Loca.UpdateTranslatedString, handle, name)
    end
    Ext.Log.Print(string.format("WOW-BG3 abilities: table read, %d copies defined in %s (%s)", (function()
        local n = 0 for _ in pairs(defs) do n = n + 1 end return n end)(), file, client and "client" or "server"))
    return tbl
end

-- The copy `name`, made now if this game has none yet. True when it exists.
local function make(name)
    if Ext.Stats.Get(name) then return true end
    local def = defs and defs[name]
    if not def or not def.Using then return false end
    local ok, err = pcall(function()
        local stat = Ext.Stats.Create(name, def.Type, def.Using)
        assert(stat, "Create returned nothing")
        for _, kv in ipairs(def.Data) do stat:SetRawAttribute(kv[1], kv[2]) end
        stat:Sync()
    end)
    if not ok then fail("make " .. name .. ": " .. tostring(err)) return false end
    state.Made = state.Made + 1
    return Ext.Stats.Get(name) ~= nil
end

-- A stand-in of WoW creature `entry` gets its abilities. Safe to call again.
function M.Grant(uuid, entry)
    if not M.On() or client or granted[uuid] or type(entry) ~= "number" then return end
    local t = loadTable()
    if not t then
        pending[uuid] = entry
        return
    end
    pending[uuid] = nil
    local list = t.Mobs[tostring(math.floor(entry))]
    granted[uuid] = entry
    if type(list) ~= "table" then return end
    state.Mobs = state.Mobs + 1
    for _, key in ipairs(list) do
        local name = PREFIX .. key
        if make(name) then
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
    -- The table (and with it WoW's names for the copies) as soon as it is there; on the server also the stand-ins
    -- that spawned before it.
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 30 ~= 0 or (tbl and next(pending) == nil) then return end
        if not loadTable() or client then return end
        for uuid, entry in pairs(pending) do
            pending[uuid] = nil
            if pcall(Osi.Exists, uuid) and Osi.Exists(uuid) == 1 then M.Grant(uuid, entry) end
        end
    end)
    loadTable()
    if client then return end
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
