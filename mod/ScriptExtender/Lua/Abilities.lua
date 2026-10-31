-- Mob abilities: a WoW mob's stand-in uses the BG3 abilities that match what it casts in WoW. Switch: `mob_abilities`
-- in settings.conf, the launcher's wow-bg3/mob-abilities.json {On}; off, nothing here runs and stand-ins keep
-- their template's actions.
--
-- Data: wow-bg3/link/abilities.json (tools/wow_abilities.py, made on the player's machine from the server's
-- database, WoW's DBCs and BG3's stats files): Mobs, creature entry -> copies; Spells, copy -> {B, the BG3
-- spell it copies; Dc; Wn, the WoW spell, for the log; F, stats text by field}. A copy has no slot cost or class
-- requirement, its own damage and save DC, and how often it may fire. The numbers follow the balance
-- research (Scaling.lua): see the tool. `@DC@` in the text is the fixed DC, or SourceSpellDC() with the
-- switch `scaling` on, whose SpellSaveDC boost on the stand-in says the same at the party's numbers.
--
-- Every copy is made at StatsLoaded on both sides (Ext.Stats.Create, then SetRawAttribute: bg3le compiles functor
-- text only that way), before BG3 builds its spell prototypes. Never later: a copy created and synced in a running
-- game goes into a spellbook, then crashes BG3 (signal 11 on a worker thread) when the AI reaches it, even with
-- no field changed (harness 2026-10-31). So a table that appears after StatsLoaded counts from the next start.
--
-- Status: wow-bg3/link/abilities-status.json: what was granted, what failed, and every cast of a copy.
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

local state = {Grants = 0, Mobs = 0, Made = 0, MakeMs = 0, Failed = {}, Casts = {}, Others = 0}
local granted = {} -- stand-in uuid -> creature entry
local tbl = nil

local function saveStatus()
    pcall(function() Ext.IO.SaveFile(STATUS, Ext.Json.Stringify(state)) end)
end
local function fail(what)
    if #state.Failed < 40 then state.Failed[#state.Failed + 1] = what end
    Ext.Log.Print("WOW-BG3 abilities: " .. what)
end

local function make(key, def)
    local name = PREFIX .. key
    if Ext.Stats.Get(name) then return end
    if not Ext.Stats.Get(def.B) then fail("no BG3 spell " .. tostring(def.B)); return end
    local s = Ext.Stats.Create(name, "SpellData", def.B)
    if not s then fail("Create failed for " .. tostring(def.B)); return end
    local dc = okScaling and Scaling.On() and "SourceSpellDC()" or tostring(def.Dc)
    for field, text in pairs(def.F or {}) do
        local ok, err = pcall(function() s:SetRawAttribute(field, (text:gsub("@DC@", dc))) end)
        if not ok then fail(def.B .. "." .. field .. ": " .. tostring(err)) end
    end
    state.Made = state.Made + 1
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
        -- Only what StatsLoaded made (see the header).
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
    -- Both sides, before BG3 builds its prototypes.
    Ext.Events.StatsLoaded:Subscribe(function()
        local t = loadTable()
        if not t then return end
        local t0 = Ext.Utils.MonotonicTime()
        for key, def in pairs(t.Spells) do
            local ok, err = pcall(make, key, def)
            if not ok then fail("make " .. tostring(key) .. ": " .. tostring(err)) end
        end
        state.MakeMs = Ext.Utils.MonotonicTime() - t0
        Ext.Log.Print(string.format("WOW-BG3 abilities: %d copies in %d ms (%s)", state.Made, state.MakeMs, client and "client" or "server"))
    end)
    if client then return end
    -- The proof, and what to tune by: which copies BG3's AI really casts.
    pcall(Ext.Osiris.RegisterListener, "CastedSpell", 5, "after", function(caster, spell)
        local uuid = type(caster) == "string" and caster:match("(%x+%-%x+%-%x+%-%x+%-%x+)$")
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
    saveStatus()
end

return M
