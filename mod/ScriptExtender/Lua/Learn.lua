-- Learn (server, test helper): grant the hero a BG3 spell, or take one back: /wowbg3 learn | unlearn SPELL.
--
-- wow-bg3/learn-command.json {RunId, Sequence, Action = Learn | Unlearn, Spell}, wow-bg3/learn-result.json
-- {RunId, Sequence, Action, OK, Value}. Files of its own: the stand-in and party commands are one job at a time
-- and the fight sends its own through them (WorldMode.lua), so a command here can neither overwrite one of those
-- nor wait behind one. BG3's AddSpell and RemoveSpell on the hero, as BG3's own story uses them; the save keeps
-- what they did. Nothing here touches a turn or a stand-in, so it also works during a fight.
-- tools/test_learn.py runs this file under a fake game.
local M = {}
local Hero = Ext.Require("Hero.lua")

local COMMAND, RESULT = "wow-bg3/learn-command.json", "wow-bg3/learn-result.json"
local SETTLE = 30    -- ticks BG3 gets to show the change in HasSpell
local SUGGEST = 8    -- names offered when the name is not a spell

local function knows(hero, spell)
    local ok, v = pcall(Osi.HasSpell, hero, spell)
    return ok and v == 1
end

-- BG3's spell names with this text in them, for a name that is not one ("Grease": Target_Grease).
local function similar(name)
    local out, low = {}, name:lower()
    pcall(function()
        for _, n in ipairs(Ext.Stats.GetStats("SpellData")) do
            if n:lower():find(low, 1, true) then out[#out + 1] = n end
        end
    end)
    table.sort(out)
    local more = #out - SUGGEST
    for i = #out, SUGGEST + 1, -1 do out[i] = nil end
    return table.concat(out, ", ") .. (more > 0 and string.format(" and %d more", more) or "")
end

-- Refuses anything but a spell in BG3's stats, by its exact name.
local function checkSpell(name)
    if type(name) ~= "string" or not name:match("^[%w_]+$") then
        error("a spell's name in BG3's stats, e.g. Target_Grease", 0)
    end
    local ok, list = pcall(function()
        local s = Ext.Stats.Get(name)
        return s and s.ModifierList
    end)
    if ok and list == "SpellData" then return end
    if ok and list then error(name .. " is in BG3's stats, but as " .. tostring(list) .. ", not a spell", 0) end
    local near = similar(name)
    error("BG3 has no spell " .. name .. (near ~= "" and ("; with that in the name: " .. near) or
        "; nothing has that in its name either"), 0)
end

-- A job waiting for HasSpell, or the Value of a command with nothing to wait for.
local function begin(c)
    if c.Action ~= "Learn" and c.Action ~= "Unlearn" then error("unknown action (Learn, Unlearn)", 0) end
    checkSpell(c.Spell)
    local hero = Hero.Uuid()
    if not hero then error("no hero (no character controlled)", 0) end
    local want = c.Action == "Learn"
    local has = knows(hero, c.Spell)
    if want and has then return nil, {Spell = c.Spell, Already = true} end
    if not want and not has then error("the hero does not have " .. c.Spell, 0) end
    if want then Osi.AddSpell(hero, c.Spell, 0, 1) else Osi.RemoveSpell(hero, c.Spell, 1) end
    return {Hero = hero, Want = want, Spell = c.Spell, Action = c.Action, Sequence = c.Sequence}
end

function M.Start(runId)
    local sequence, ticks, job = 0, 0, nil
    -- bg3le builds the Lua states again at every load: the command already in the file was carried out by the
    -- previous state, so its Sequence counts as done (as in Party.lua).
    do
        local okc, c = pcall(Ext.Json.Parse, Ext.IO.LoadFile(COMMAND) or "")
        if okc and type(c) == "table" and c.RunId == runId and math.type(c.Sequence) == "integer" then sequence = c.Sequence end
    end
    local function answer(seq, action, ok, value)
        Ext.IO.SaveFile(RESULT, Ext.Json.Stringify({RunId = runId, Sequence = seq, Action = tostring(action), OK = ok, Value = value}))
        Ext.Utils.Print("WOW-BG3 learn: " .. tostring(action) .. (ok and " done" or (" failed: " .. tostring(value))))
    end
    Ext.Events.Tick:Subscribe(function()
        if Ext.Utils.GetGameState() ~= "Running" then return end
        ticks = ticks + 1
        if job then
            if knows(job.Hero, job.Spell) == job.Want then
                answer(job.Sequence, job.Action, true, {Spell = job.Spell})
                job = nil
            elseif ticks >= job.Due then
                answer(job.Sequence, job.Action, false, job.Want and ("BG3 did not add " .. job.Spell) or
                    ("the hero still has " .. job.Spell .. ": it comes from elsewhere (class, race, an item), not from learn"))
                job = nil
            end
            return
        end
        if ticks % 10 ~= 0 then return end
        local okj, c = pcall(Ext.Json.Parse, Ext.IO.LoadFile(COMMAND) or "")
        if not okj or type(c) ~= "table" or c.RunId ~= runId or math.type(c.Sequence) ~= "integer" or c.Sequence <= sequence then return end
        sequence = c.Sequence
        local ok, started, value = pcall(begin, c)
        if not ok then
            answer(c.Sequence, c.Action, false, tostring(started))
        elseif started then
            started.Due = ticks + SETTLE
            job = started
        else
            answer(c.Sequence, c.Action, true, value)
        end
    end)
end
return M
