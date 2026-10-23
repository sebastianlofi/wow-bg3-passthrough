-- Server-side, read-only encounter survey for a disposable save.
-- Every 90 ticks: the nearest characters with Health around the host (name, position, HP,
-- dead/enemy/combat flags). On request (survey-command.json): one engine path query from
-- the host to a surveyed character, released afterwards, or a ground-height grid (Heights).
-- Osiris use is queries only.
-- It never moves actors, changes factions, spawns anything or edits rules.
local M = {}
local Hero = Ext.Require("Hero.lua")
local EXPECTED_REGION = Ext.Require("Hero.lua").REGION -- host level (Hero.lua, host.json)
local RADIUS, LIMIT = 80.0, 32

local function vec(v) return {v[1], v[2], v[3]} end
local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function flag(fn, ...)
    local v = q(fn, ...)
    if v == nil then return nil end
    return v == 1 or v == true
end
local function hdist(a, b)
    local dx, dz = a[1] - b[1], a[3] - b[3]
    return math.sqrt(dx * dx + dz * dz)
end

function M.Survey(host)
    local origin = vec(Ext.Entity.Get(host).Transform.Transform.Translate)
    local rows = {}
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("Health")) do
        local uuid = q(function() return tostring(e.Uuid.EntityUuid) end)
        local pos = q(function() return vec(e.Transform.Transform.Translate) end)
        if uuid and pos and uuid ~= host and flag(Osi.IsCharacter, uuid) then
            local d = hdist(origin, pos)
            if d <= RADIUS then
                rows[#rows + 1] = {Uuid = uuid, Position = pos, Distance = d, Dy = pos[2] - origin[2]}
            end
        end
    end
    table.sort(rows, function(a, b) return a.Distance < b.Distance end)
    while #rows > LIMIT do table.remove(rows) end
    for _, r in ipairs(rows) do
        local e = Ext.Entity.Get(r.Uuid)
        r.Name = q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(r.Uuid)) end)
        r.Template = q(function() return Osi.GetTemplate(r.Uuid) end)
        r.Hp = q(function() return {e.Health.Hp, e.Health.MaxHp} end)
        r.Dead = flag(Osi.IsDead, r.Uuid)
        r.Enemy = flag(Osi.IsEnemy, host, r.Uuid)
        r.InCombat = flag(Osi.IsInCombat, r.Uuid)
        r.Faction = q(Osi.GetFaction, r.Uuid)
        r.CanFight = flag(Osi.CanFight, r.Uuid)
        r.OnStage = flag(Osi.IsOnStage, r.Uuid)
    end
    return {Origin = origin, Characters = rows}
end

-- A goal point (world) within 25 m of the host, for arena path checks.
local function near(origin, p, limit)
    assert(type(p) == "table" and #p == 3, "Point must be {x, y, z}")
    local dx, dz = p[1] - origin[1], p[3] - origin[3]
    assert(math.sqrt(dx * dx + dz * dz) <= limit, "Point too far from the disposable character")
    return {p[1], p[2], p[3]}
end

function M.Path(host, target)
    local entity = assert(Ext.Entity.Get(host), "Host entity unavailable")
    local origin = vec(entity.Transform.Transform.Translate)
    local goal
    if type(target) == "table" then
        goal = near(origin, target, 25.0)
    else
        local t = assert(Ext.Entity.Get(target), "Target entity unavailable")
        goal = vec(t.Transform.Transform.Translate)
    end
    local path = assert(Ext.Level.BeginPathfindingImmediate(entity, goal), "No path allocated")
    local out = {Origin = origin, Goal = goal}
    local ok, err = pcall(function()
        out.Found = Ext.Level.FindPath(path)
        out.Complete, out.ErrorCause, out.DestinationReached = path.SearchComplete, path.ErrorCause, path.DestinationReached
        local length, prev, nodes = 0.0, nil, {}
        for i, node in ipairs(path.Nodes) do
            assert(i <= 512, "Path node limit")
            local p = vec(node.Position)
            if prev then
                local dx, dy, dz = p[1] - prev[1], p[2] - prev[2], p[3] - prev[3]
                length = length + math.sqrt(dx * dx + dy * dy + dz * dz)
            end
            prev = p
            nodes[#nodes + 1] = p
        end
        out.Length, out.NodeCount, out.Last, out.Nodes = length, #path.Nodes, prev, nodes
    end)
    Ext.Level.ReleasePath(path)
    assert(ok, err)
    out.FinalPosition = vec(entity.Transform.Transform.Translate)
    return out
end

function M.Start(runId)
    local ticks, sequence = 0, 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 15 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        -- Survey around the hero (Hero.lua).
        local host = Hero.Uuid()
        if not host or not Ext.Entity.Get(host) or q(Osi.GetRegion, host) ~= EXPECTED_REGION then return end
        if ticks % 90 == 0 then
            local ok, value = pcall(M.Survey, host)
            Ext.IO.SaveFile("wow-bg3/m3-survey.json", Ext.Json.Stringify({RunId = runId, Tick = ticks,
                OK = ok, Value = ok and value or tostring(value)}))
        end
        local raw = Ext.IO.LoadFile("wow-bg3/survey-command.json")
        if not raw or #raw > 4096 then return end
        local parsed, command = pcall(Ext.Json.Parse, raw)
        if not parsed or type(command) ~= "table" or command.RunId ~= runId then return end
        if math.type(command.Sequence) ~= "integer" or command.Sequence <= sequence or command.Sequence > 4096 then return end
        sequence = command.Sequence
        local ok, value = pcall(function()
            if command.Action == "Tiles" then
                -- Read-only nav tiles (0.5 m) on a grid around the disposable character.
                local r = tonumber(command.Radius) or 6
                assert(r > 0 and r <= 8, "Radius too large")
                local o = vec(Ext.Entity.Get(host).Transform.Transform.Translate)
                if command.Centre then o = near(o, command.Centre, 25.0) end
                local rows = {}
                for dz = -r, r, 0.5 do
                    for dx = -r, r, 0.5 do
                        local t = q(Ext.Level.GetTileDebugInfo, {o[1] + dx, o[2], o[3] + dz})
                        if t then
                            rows[#rows + 1] = {dx, dz, q(function() return t.Flags end), t.MinHeight, t.MaxHeight,
                                t.SubgridId, t.TileX, t.TileY, t.MetaDataIndex}
                        else
                            rows[#rows + 1] = {dx, dz}
                        end
                    end
                end
                return {Origin = o, Radius = r, Tiles = rows}
            end
            if command.Action == "Rays" then
                -- Read-only physics rays and Osiris line-of-sight from the disposable character's
                -- eye height to each listed character and to fixed offsets around it.
                local o = vec(Ext.Entity.Get(host).Transform.Transform.Translate)
                local eye = {o[1], o[2] + 1.5, o[3]}
                local targets = {}
                for _, uuid in ipairs(type(command.Targets) == "table" and command.Targets or {}) do
                    local e = Ext.Entity.Get(uuid)
                    if e then
                        local p = vec(e.Transform.Transform.Translate)
                        targets[#targets + 1] = {Name = uuid, Point = {p[1], p[2] + 1.0, p[3]}, Uuid = uuid}
                    end
                end
                for i, point in ipairs(type(command.Points) == "table" and command.Points or {}) do
                    assert(i <= 8, "At most eight points")
                    local p = near(o, point, 25.0)
                    targets[#targets + 1] = {Name = "point" .. i, Point = p}
                end
                for a = 0, 315, 45 do
                    local rad = math.rad(a)
                    targets[#targets + 1] = {Name = "dir" .. a, Point = {o[1] + 10 * math.cos(rad), o[2] + 1.5, o[3] + 10 * math.sin(rad)}}
                end
                local out = {Eye = eye, Rays = {}}
                for _, t in ipairs(targets) do
                    local row = {Name = t.Name, Point = t.Point, Casts = {}}
                    -- Physics type (1 static, 4 dynamic) and include groups to compare.
                    for _, c in ipairs({{1, 0x10}, {1, 0x800}, {5, 0x10 | 0x20 | 0x400 | 0x800}, {5, 0xFFFFFFFF}}) do
                        local cast = {Type = c[1], Include = c[2]}
                        cast.Any = q(Ext.Level.RaycastAny, eye, t.Point, c[1], c[2], 0, 0)
                        local hit = q(Ext.Level.RaycastClosest, eye, t.Point, c[1], c[2], 0, 0)
                        if hit then
                            cast.Hit = q(function()
                                return {Position = vec(hit.Position), Distance = hit.Distance,
                                    Group = tostring(hit.PhysicsGroup)}
                            end) or q(function() return tostring(hit) end)
                        end
                        row.Casts[#row.Casts + 1] = cast
                    end
                    if t.Uuid then
                        row.OsiLos = q(Osi.HasLineOfSight, host, t.Uuid)
                        row.OsiCanSee = q(Osi.CanSee, host, t.Uuid)
                    end
                    out.Rays[#out.Rays + 1] = row
                end
                return out
            end
            if command.Action == "Heights" then
                -- Read-only ground heights on a square grid around the disposable character.
                local r, step = tonumber(command.Radius) or 12, tonumber(command.Step) or 1
                assert(r > 0 and r <= 30 and step >= 0.5 and (2 * r / step + 1) ^ 2 <= 4096, "Grid too large")
                local o = vec(Ext.Entity.Get(host).Transform.Transform.Translate)
                local rows = {}
                for dz = -r, r, step do
                    for dx = -r, r, step do
                        local h = q(Ext.Level.GetHeightsAt, o[1] + dx, o[3] + dz)
                        local list = {}
                        if type(h) == "table" then for i = 1, math.min(#h, 4) do list[i] = h[i] end end
                        rows[#rows + 1] = {dx, dz, list}
                    end
                end
                return {Origin = o, Radius = r, Step = step, Points = rows}
            end
            assert(command.Action == "Path", "Unknown survey action")
            if command.Goal then return M.Path(host, command.Goal) end
            assert(type(command.Target) == "string" and #command.Target == 36, "Target must be a UUID")
            return M.Path(host, command.Target)
        end)
        Ext.IO.SaveFile("wow-bg3/survey-result.json", Ext.Json.Stringify({RunId = runId, Sequence = sequence,
            OK = ok, Value = ok and value or tostring(value)}))
    end)
end
return M
