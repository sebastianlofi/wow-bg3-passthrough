-- Read-only private-world API checks (registration success is not event-delivery proof); also starts
-- the server modules TurnHold.lua, Cleanup.lua and CampOff.lua.
local M = {}
local inspect = Ext.Require("WorldInspect.lua")
local Hero = Ext.Require("Hero.lua")
function M.Start(runId)
    -- Server modules started from here: turn-based holds outside combat (TurnHold.lua) and
    -- no stand-in outliving its session (Cleanup.lua), no camp in world mode (CampOff.lua).
    for _, name in ipairs({"TurnHold.lua", "Cleanup.lua", "CampOff.lua"}) do
        local ok, err = pcall(function() Ext.Require(name).Start(runId) end)
        if not ok then Ext.Log.Print("WOW-BG3 " .. name .. ": " .. tostring(err)) end
    end
    local ticks, samples, events = 0, 0, {}
    local listeners = {}
    for name, arity in pairs({TurnStarted = 1, TurnEnded = 1, LevelGameplayStarted = 2}) do
        local ok, err = pcall(Ext.Osiris.RegisterListener, name, arity, "after", function(...)
            events[#events + 1] = {Event = name, Args = {...}}
            if #events > 32 then table.remove(events, 1) end
        end)
        listeners[name] = {Registered = ok, Error = ok and "" or tostring(err)}
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 150 ~= 0 or samples >= 30 or Ext.Utils.GetGameState() ~= "Running" then return end
        samples = samples + 1
        local report = {RunId = runId, Sample = samples, Listeners = listeners, Events = events}
        local ok, err = pcall(function()
            report.SizeAudit = Ext._Internal.SizeAudit()
            local host = Osi.GetHostCharacter()
            report.Host = host or ""
            if host then
                report.Region = Osi.GetRegion(host)
                if Hero.Is(host) then
                    report.HostState = inspect.Snapshot(host)
                    if samples == 3 and report.Region == "TUT_Avernus_C" then
                        local pathOK, paths = pcall(inspect.Paths, host)
                        Ext.IO.SaveFile("wow-bg3/m3-paths.json", Ext.Json.Stringify({RunId = runId,
                            OK = pathOK, Value = pathOK and paths or tostring(paths)}))
                    end
                end
            end
            local actors = Ext.Entity.GetAllEntitiesWithComponent("Health")
            report.HealthEntityCount = #actors
            report.Actors = {}
            for i = 1, math.min(#actors, 16) do
                local entity = actors[i]
                local state = {Entity = tostring(entity), Hp = entity.Health.Hp, MaxHp = entity.Health.MaxHp}
                local uuidOK, uuid = pcall(function() return tostring(entity.Uuid.EntityUuid) end)
                if uuidOK then state.Uuid = uuid end
                local posOK, pos = pcall(function() return entity.Transform.Transform.Translate end)
                if posOK and pos then state.Position = {pos[1], pos[2], pos[3]} end
                report.Actors[#report.Actors + 1] = state
            end
        end)
        report.OK, report.Error = ok, ok and "" or tostring(err)
        Ext.IO.SaveFile("wow-bg3/m3-world.json", Ext.Json.Stringify(report))
    end)
end
return M
