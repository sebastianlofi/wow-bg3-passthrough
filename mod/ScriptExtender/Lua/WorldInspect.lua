-- Bounded diagnostics for a disposable save. No actor/rule mutations.
local M = {}
local Hero = Ext.Require("Hero.lua")
local function vec(v) return {v[1], v[2], v[3]} end
local function capture(out, key, fn)
    local ok, value = pcall(fn)
    out[key] = {OK = ok, Value = ok and value or tostring(value)}
end
function M.Snapshot(host)
    assert(Hero.Is(host), "Not the hero")
    local entity = assert(Ext.Entity.Get(host), "Host entity unavailable")
    local out = {Host = host}
    capture(out, "Position", function() return vec(entity.Transform.Transform.Translate) end)
    capture(out, "Health", function() return {Hp = entity.Health.Hp, MaxHp = entity.Health.MaxHp} end)
    capture(out, "Components", function() return entity:GetAllComponentNames(true) end)
    capture(out, "Turn", function()
        local t = entity.TurnBased
        return {Active = t.IsActiveCombatTurn, CanAct = t.CanActInCombat,
            RequestedEnd = t.RequestedEndTurn, Completed = t.TurnActionsCompleted}
    end)
    capture(out, "Spells", function()
        local rows = {}
        for i, spell in ipairs(entity.SpellBook.Spells) do
            assert(i <= 256, "Spell limit")
            rows[#rows + 1] = {Prototype = spell.Id.Prototype, Originator = spell.Id.OriginatorPrototype,
                SourceType = tostring(spell.Id.SourceType), Source = tostring(spell.Id.Source),
                Charges = spell.NumCharges, Used = spell.UsedCharges}
        end
        return rows
    end)
    capture(out, "Resources", function()
        local rows = {}
        for id, levels in pairs(entity.ActionResources.Resources) do
            for _, resource in ipairs(levels) do
                assert(#rows < 128, "Resource limit")
                rows[#rows + 1] = {Id = tostring(id), Level = resource.Level,
                    Amount = resource.Amount, MaxAmount = resource.MaxAmount}
            end
        end
        return rows
    end)
    capture(out, "CastRequests", function()
        local s = assert(Ext.System.ServerCastRequest, "Cast request system unavailable")
        return {Network = #s.NetworkStartRequests, Osiris = #s.OsirisCastRequests}
    end)
    return out
end
function M.Paths(host)
    assert(Hero.Is(host) and Osi.GetRegion(host) == "TUT_Avernus_C", "Unexpected save/region")
    local entity = assert(Ext.Entity.Get(host))
    local origin = vec(entity.Transform.Transform.Translate)
    local out = {Origin = origin, Queries = {}}
    for _, offset in ipairs({{1,0}, {-1,0}, {0,1}, {0,-1}}) do
        local target = {origin[1]+offset[1], origin[2], origin[3]+offset[2]}
        local row = {Target = target}
        capture(row, "Heights", function() return Ext.Level.GetHeightsAt(target[1],target[3]) end)
        local path
        capture(row, "Path", function()
            path = assert(Ext.Level.BeginPathfindingImmediate(entity, target), "No path allocated")
            local found = Ext.Level.FindPath(path)
            local value = {Found = found, Complete = path.SearchComplete, ErrorCause = path.ErrorCause,
                DestinationReached = path.DestinationReached, Nodes = {}}
            for i, node in ipairs(path.Nodes) do
                assert(i <= 256, "Path node limit")
                value.Nodes[#value.Nodes + 1] = {Position = vec(node.Position), Distance = node.Distance}
            end
            return value
        end)
        if path then capture(row, "Release", function() Ext.Level.ReleasePath(path); return true end) end
        out.Queries[#out.Queries + 1] = row
    end
    out.FinalPosition = vec(entity.Transform.Transform.Translate)
    return out
end
return M
