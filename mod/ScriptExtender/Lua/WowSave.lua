-- WowSave (server, plug-and-play plan "Save sync"): the WoW side of
-- every BG3 save and load. the save-sync notes has the whole flow.
--
-- Save: on the switch to Save (F5, an autosave, the menu) a fresh snapshot id goes into the save as a
-- persistent user variable on every player character (Ext.Vars, written by bg3le during the save; the
-- mod's PersistentVars would need a mod UUID, which the unpacked harness mod has not), and
-- wow-bg3/wowsave-request.json asks the lifecycle watcher (link/tools/lifecycle.py) for a snapshot of
-- every party member's WoW character under that id.
-- Load: bg3le rebuilds the Lua states on every load, so this module starts fresh in the loaded
-- session. wow-bg3/lifecycle.json tells the watcher what the session is: its token (Hero.SESSION),
-- the game state (Loading while BG3 is not in a running level), the loaded save's snapshot id (the
-- host character's variable) and the party with each member's WoW character (variable WowBg3Char,
-- kept in the save) and race, class and body (for a WoW character still to be made). The watcher logs
-- the WoW characters out, restores that snapshot, makes missing characters, logs them in, and writes
-- wow-bg3/wowchars.json, whose names this module stores on the members (so the next save keeps them).
-- Mid-fight save (decided): a load drops the stand-ins the save carried (they are
-- temporary copies of WoW mobs whose WoW fight ended when the WoW character logged out for the
-- restore); BG3's fight ends with them. See save-sync.md.
local M = {}
local Hero = Ext.Require("Hero.lua")
local Party = Ext.Require("Party.lua")

local VAR, CHARVAR = "WowBg3Save", "WowBg3Char"
local OPTS = {Server = true, Client = false, SyncToClient = false, Persistent = true}
pcall(Ext.Vars.RegisterUserVariable, VAR, OPTS)
pcall(Ext.Vars.RegisterUserVariable, CHARVAR, OPTS)
-- Game states of a session in play (Session.lua's held ones and Running); anything else is a load,
-- an unload or the menu: the WoW side logs out.
local LIVE = {Running = true, Save = true, Paused = true, GameMasterPause = true}

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    return ok and v or nil
end
local function uuidOf(s) return Hero.UuidOf(tostring(s)) end
local function players()
    local out = {}
    for _, row in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
        local u = uuidOf(row[1])
        if u then out[#out + 1] = u end
    end
    return out
end
local function getVar(u, name)
    local e = q(Ext.Entity.Get, u)
    local v = e and q(function() return e.Vars[name] end)
    return type(v) == "string" and v or nil
end
local function setVar(u, name, v)
    local e = q(Ext.Entity.Get, u)
    return e ~= nil and pcall(function() e.Vars[name] = v end)
end
local function nameOf(u) return q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(u)) end) end
local function readJson(name)
    local raw = Ext.IO.LoadFile(name)
    if not raw or #raw > 65536 then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

-- The party (hero first) with what the watcher needs per member.
local function members()
    local out = {}
    for _, u in ipairs(Party.Party()) do
        local m = Hero.Body(u)
        m.Uuid, m.Name, m.Hero, m.Char = u, nameOf(u), #out == 0, getVar(u, CHARVAR)
        m.Dead = q(Osi.IsDead, u) == 1
        -- Down (DOWNED or 0 HP) and HP: at a load the lifecycle brings each WoW
        -- character to this state (alive at BG3's HP fraction, or dead in place), whatever the snapshot had.
        local e = q(Ext.Entity.Get, u)
        local h = e and q(function() return {e.Health.Hp, e.Health.MaxHp} end)
        if h then m.Hp, m.MaxHp = h[1], h[2] end
        m.Down = not m.Dead and (q(Osi.HasActiveStatus, u, "DOWNED") == 1 or (h ~= nil and h[1] <= 0))
        out[#out + 1] = m
    end
    return out
end

-- Stand-ins in a save (mid-fight saves). At the save, the stand-ins world mode owns (Mirrors.lua's
-- WOWBG3MirrorOwned: every mirrored WoW mob's temporary BG3 copy, fight members included) are listed in
-- the save itself (user variable WowBg3StandIns on the host character). After a load those very
-- characters are removed (Osiris RequestDeleteTemporary, as StandIns.lua's Clear does), once per
-- session and before world mode starts again; nothing else is touched.
local SIVAR = "WowBg3StandIns"
pcall(Ext.Vars.RegisterUserVariable, SIVAR, OPTS)
local function listStandIns()
    local out = {}
    for u in pairs(WOWBG3MirrorOwned or {}) do
        if type(u) == "string" and u:find("^%x+%-%x+%-%x+%-%x+%-%x+$") then out[#out + 1] = u end
    end
    table.sort(out)
    return table.concat(out, ",")
end
local function dropSavedStandIns(host)
    local list = getVar(host, SIVAR) or ""
    local listed, gone = 0, {}
    for u in list:gmatch("[%x%-]+") do
        listed = listed + 1
        if q(Ext.Entity.Get, u) and q(Osi.IsPlayer, u) ~= 1 then
            pcall(Osi.SetOnStage, u, 0)
            local ok = pcall(Osi.RequestDeleteTemporary, u)
            gone[#gone + 1] = {Uuid = u, Name = nameOf(u), OK = ok}
        end
    end
    if listed > 0 then
        Ext.Log.Print(string.format("WOW-BG3 wowsave: the save listed %d stand-in(s); %d still there, removed", listed, #gone))
    end
    return gone
end

function M.Start(runId)
    local seq, known = 0, nil
    local state = tostring(Ext.Utils.GetGameState())
    local loadedId, dropped, chars = nil, nil, {}
    local function request(action, id, extra)
        seq = seq + 1
        local body = {RunId = runId, Session = Hero.SESSION, Seq = seq, Action = action, Id = id,
            Time = Ext.Utils.MonotonicTime()}
        for k, v in pairs(extra or {}) do body[k] = v end
        Ext.IO.SaveFile("wow-bg3/wowsave-request.json", Ext.Json.Stringify(body))
        Ext.Log.Print("WOW-BG3 wowsave: " .. action .. " " .. id)
    end
    local function status()
        local live = LIVE[state] == true
        local body = {RunId = runId, Session = Hero.SESSION, State = state, Loading = not live,
            Time = Ext.Utils.MonotonicTime(), SaveId = loadedId, NewGame = Ext.Require("NewGame.lua").Enabled(),
            StandInsDropped = dropped and #dropped or nil}
        if state == "Running" then
            body.Members = members()
            local h = Hero.Uuid()
            body.Region = h and q(Osi.GetRegion, h) or nil
        end
        Ext.IO.SaveFile("wow-bg3/lifecycle.json", Ext.Json.Stringify(body))
    end
    Ext.Events.GameStateChanged:Subscribe(function(e)
        state = tostring(e.ToState)
        -- A load in this same Lua state: the session's knowledge starts over (Hero.lua makes a new token).
        if state == "LoadSession" then known, loadedId, dropped, chars = nil, nil, nil, {} end
        if state == "Save" then
            local id = string.format("%x%04x", math.floor(Ext.Utils.MonotonicTime()), math.random(0, 0xffff))
            for _, u in ipairs(players()) do setVar(u, VAR, id) end
            local host = uuidOf(q(Osi.GetHostCharacter) or "")
            if host then setVar(host, SIVAR, listStandIns()) end
            known = id
            request("snapshot", id, {Members = members()})
        end
        status()
    end)
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 30 ~= 0 then return end
        state = tostring(Ext.Utils.GetGameState())
        if state == "Running" then
            -- The loaded save's id (the host character's variable), once per session.
            local host = uuidOf(q(Osi.GetHostCharacter) or "")
            local id = host and getVar(host, VAR)
            if id and id ~= known then
                known, loadedId = id, id
                request("restore", id)
                status()
            end
            -- Stand-ins a mid-fight save carried: once per session, before world mode starts.
            if not dropped and host then dropped = dropSavedStandIns(host) end
            -- WoW character names from the watcher, kept on the members for the next save.
            local w = readJson("wow-bg3/wowchars.json")
            -- Only this session's names (an origin keeps its uuid across
            -- playthroughs, so another session's file named the wrong character).
            if w and w.RunId == runId and w.Session == Hero.SESSION and type(w.Chars) == "table" then
                for u, name in pairs(w.Chars) do
                    if type(name) == "string" and name:find("^%a+$") and chars[u] ~= name and getVar(u, CHARVAR) ~= name then
                        if setVar(u, CHARVAR, name) then
                            chars[u] = name
                            Ext.Log.Print("WOW-BG3 wowsave: " .. tostring(nameOf(u)) .. " is WoW character " .. name)
                        end
                    end
                end
            end
        end
        if ticks % 60 == 0 or not LIVE[state] then status() end
    end)
    status()
    -- Death, BG3 rules (Death.lua): started with save sync, its watcher is the same tool.
    local okD, errD = pcall(function() Ext.Require("Death.lua").Start(runId) end)
    if not okD then Ext.Log.Print("WOW-BG3 death unavailable: " .. tostring(errD)) end
end

return M
