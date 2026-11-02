-- Server-side, read-only monitor for the legality tests on a disposable save.
-- Records Osiris turn/cast events with a resource snapshot of the host at each event.
-- It never changes actors, rules or requests.
local M = {}
local Hero = Ext.Require("Hero.lua")
-- The party: each member's HP, death, DOWNED and combat in the report (LinkLive.lua).
local Party = Ext.Require("Party.lua")
local LISTEN = {
    TurnStarted = 1, TurnEnded = 1, EnteredForceTurnBased = 1, LeftForceTurnBased = 1,
    CombatStarted = 1, CombatEnded = 1, EnteredCombat = 2, LeftCombat = 2,
    UsingSpell = 5, CastSpell = 5, CastedSpell = 5, CastSpellFailed = 5,
    UsingSpellOnTarget = 6, UsingSpellAtPosition = 8, StatusApplied = 4, StatusRemoved = 4,
    AttackedBy = 7, Died = 1,
}
-- Environment objects flood these; keep only events that mention the host.
local HOST_ONLY = {EnteredForceTurnBased = true, LeftForceTurnBased = true, StatusApplied = true, StatusRemoved = true}
local names = {}
local function resourceName(id)
    local key = tostring(id)
    if names[key] == nil then
        local ok, res = pcall(function() return Ext.StaticData.Get(id, "ActionResource").Name end)
        names[key] = ok and tostring(res) or key
    end
    return names[key]
end
local function try(fn)
    local ok, v = pcall(fn)
    if ok then return v end
    return {Error = tostring(v)}
end
function M.Snapshot(host)
    local e = host and Ext.Entity.Get(host)
    if not e then return {Error = "host entity unavailable"} end
    return {
        Position = try(function() local p = e.Transform.Transform.Translate; return {p[1], p[2], p[3]} end),
        Hp = try(function() return {e.Health.Hp, e.Health.MaxHp} end),
        Turn = try(function()
            local t = e.TurnBased
            return {Active = t.IsActiveCombatTurn, CanAct = t.CanActInCombat, RequestedEnd = t.RequestedEndTurn,
                Completed = t.TurnActionsCompleted}
        end),
        Resources = try(function()
            local rows = {}
            for id, levels in pairs(e.ActionResources.Resources) do
                for _, r in ipairs(levels) do
                    assert(#rows < 64, "resource limit")
                    rows[#rows + 1] = {Name = resourceName(id), Level = r.Level, Amount = r.Amount, Max = r.MaxAmount}
                end
            end
            table.sort(rows, function(a, b) return a.Name .. a.Level < b.Name .. b.Level end)
            return rows
        end),
        CastRequests = try(function()
            local s = Ext.System.ServerCastRequest
            return {Network = #s.NetworkStartRequests, Osiris = #s.OsirisCastRequests}
        end),
    }
end
function M.Start(runId)
    -- BG3's menu entries on WoW's mobs and NPCs, started here so
    -- Probe.lua needs no change.
    local okI, errI = pcall(function() Ext.Require("Interact.lua").Start(runId) end)
    if not okI then Ext.Log.Print("WOW-BG3 interact: " .. tostring(errI)) end
    local okS, errS = pcall(function() Ext.Require("StatusFx.lua").Start() end)
    if not okS then Ext.Log.Print("WOW-BG3 statusfx: " .. tostring(errS)) end
    local ticks, events, listeners, seq = 0, {}, {}, 0
    local function host()
        local ok, h = pcall(Osi.GetHostCharacter)
        return ok and h or nil
    end
    for name, arity in pairs(LISTEN) do
        local ok, err = pcall(Ext.Osiris.RegisterListener, name, arity, "after", function(...)
            local args = {}
            local mentionsHost = false
            for i = 1, select("#", ...) do
                args[i] = tostring((select(i, ...)))
                if Hero.Is(args[i]) then mentionsHost = true end
            end
            if HOST_ONLY[name] and not mentionsHost then return end
            seq = seq + 1
            -- The hero (Hero.lua: the character the player controls) is snapshotted.
            events[#events + 1] = {Seq = seq, Tick = ticks, Event = name, Args = args,
                Host = M.Snapshot(Hero.Uuid())}
            if #events > 128 then table.remove(events, 1) end
        end)
        listeners[name] = ok and true or tostring(err)
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 15 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local report = {RunId = runId, Tick = ticks, Listeners = listeners, Events = events}
        local ok, err = pcall(function()
            local h = host()
            report.Host = h or ""
            -- Hero: who the harness drives; Disposable kept for older readers.
            local hero = Hero.Uuid()
            report.Hero = hero or ""
            report.Disposable = hero ~= nil and Ext.Entity.Get(hero) ~= nil
            report.Region = report.Disposable and Osi.GetRegion(hero) or ""
            if report.Disposable then report.State = M.Snapshot(hero) end
            local okP, party = pcall(Party.Snapshot)
            if okP then report.Party = party end
        end)
        report.OK, report.Error = ok, ok and "" or tostring(err)
        Ext.IO.SaveFile("wow-bg3/m3-actions.json", Ext.Json.Stringify(report))
    end)
end
return M
