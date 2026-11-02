-- WoW arena in BG3 through the wowbg3 native plugin (plugins, replaces runtime patch
-- 0003; see the collision-hooks notes). Runs in both contexts; each acts on its own
-- grid and physics scene. Driven by wow-bg3/arena-command.json (one command per
-- sequence, never retried):
--   Stamp   {MinX, MinZ, AnchorY, Void, Layer, Tolerance, Seconds}  tiles from wow-bg3/arena-tiles.bin;
--                                            Void: into the out-of-bounds layer (subgrids at Layer =
--                                            their Translate.y) far from the ship
--   Restore {}                               put the grid back (and rays and camera off)
--   Rays    {Void, Mask, SkipContexts, Seconds}  WoW triangles from wow-bg3/arena-triangles.bin in this
--                                            context's raycasts; Void ignores BG3's own static hits;
--                                            SkipContexts (bits) leaves those query contexts to BG3
--   RaysOff {}
--   Camera  {Eye, Target, Seconds}           client: BG3's camera looks from Eye at Target
--   Follow  {Wow, Bg3 | Anchor, Yaw, Fov, Mirror, Seconds}  client: BG3's camera follows benilla's
--                                            (wow-bg3/link/camera.bin), WoW point Wow on BG3 point Bg3;
--                                            Anchor puts the WoW player on the disposable character
--   CameraOff {}                             BG3's own camera back
--   Limits  {Side, Span}                     raise the arena size limits (tiles per side, metres)
--   Pool    {Layer, Layers, Count}           world mode's treadmill: take whole plain subgrids of the
--                                            out-of-bounds layer (snapshots; native-<side>-pool.json);
--                                            Layers: several layers' heights at once (plugin 0.2.9)
--   Place   {Cells = {{K, MinX, MinZ, BaseY}...}, Seconds}  move pool subgrid K to that 64 m square
--                                            with WoW ground from wow-bg3/section-<K>.bin (Restore and
--                                            leaving the level put every pool subgrid back)
--   PickRegion {Seconds}                     client: BG3's cursor picks characters anywhere in the void
--                                            (the level's region map emptied, plugin op 28); Restore
--   PickRegionOff                            client: the map back, for BG3's own level (a trip to camp)
--                                            puts it back
--   SpatialGrid {Rebuild, Targets}          BG3's spatial grid on this side (square, flags, the AI grid
--                                            bounds it would take); Rebuild asks the engine to rebuild
--                                            it (plugin op 30); Targets (uuids): Active and position
--   Status  {}                               plugin, stamp, rays and camera state
-- Read-only diagnosis: CameraProbe, CameraStats, CountOn/CountRead, GridSurvey, Columns {From, To,
-- Step, Top} (vertical rays), SceneCounts (calls per physics scene slot).
-- Guarantees: restore when the game leaves Running (save, load, menu), when Seconds run
-- out (at most 900), and before any new stamp. Guards: the disposable character, the
-- expected region (server), and an arena centred within 25 m of that character.
local M = {}
local N = Ext.Require("WowNative.lua")
local Hero = Ext.Require("Hero.lua")
local REGION = Ext.Require("Hero.lua").REGION -- host level (Hero.lua, host.json)
local MAX_SECONDS, RADIUS, VOID_RADIUS = 900, 25.0, 400.0
-- The actions only the client carries out: the server answers "client only" before any check of its own, so a
-- caller waits for the client's result alone (WorldMode.lua). Waiting for the server's word on them tied them to a
-- server that can be busy for seconds: in the 0.1.3013.59 test the server ran no tick for 8 to 13 s after a
-- move's teleport, the client had done Lead within 0.1 s, and the move failed "Lead: no result".
M.ClientOnly = {Lead = true, PickRegionOff = true, CameraOff = true, CameraStats = true, CameraProbe = true,
    CountOn = true, CountRead = true}

local function now()
    local ok, ms = pcall(Ext.Utils.MonotonicTime)
    return ok and type(ms) == "number" and ms / 1000.0 or os.clock()
end

-- The active camera controller's address (the Camera component's Controller pointer).
function M.ControllerAddress()
    local e = M.ActiveCameraEntity()
    local handle = assert(tonumber(tostring(e):match("Entity %((%x+)%)"), 16), "No camera entity handle")
    local field = assert(Ext._Internal.FieldAddress(handle, "Camera", "Controller"))
    return assert(Ext._Internal.Peek(field, 8), "Unreadable controller pointer")
end

function M.ActiveCameraEntity()
    local first
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("Camera")) do
        local ok, c = pcall(function() return e.Camera end)
        if ok and c and c.Active == true then
            if c.AcceptsInput == true then return e end
            first = first or e
        end
    end
    return assert(first, "No active camera")
end

-- Every active camera's controller address (Camera component's Controller pointer), in entity order.
function M.ActiveControllers()
    local out = {}
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("Camera")) do
        local ok, c = pcall(function() return e.Camera end)
        if ok and c and c.Active == true then
            local handle = tonumber(tostring(e):match("Entity %((%x+)%)"), 16)
            local field = handle and Ext._Internal.FieldAddress(handle, "Camera", "Controller")
            local addr = field and Ext._Internal.Peek(field, 8)
            -- BG3's character preview cameras (inventory,
            -- character sheet: ecl::SpectatorCameraBehavior, set up near the world's origin, no input) are
            -- active while their panel is open; the lead followed one and WoW's world went away.
            local preview = false
            if c.AcceptsInput ~= true then
                local okn, names = pcall(function() return e:GetAllComponentNames(false) end)
                for _, n in ipairs(okn and names or {}) do
                    if n:find("SpectatorCameraBehavior", 1, true) then preview = true break end
                end
            end
            if addr and addr ~= 0 then out[#out + 1] = {Address = addr, Input = c.AcceptsInput == true, Preview = preview} end
        end
    end
    return out
end

local function sub(a, b) return {a[1] - b[1], a[2] - b[2], a[3] - b[3]} end
local function dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
local function cross(a, b) return {a[2] * b[3] - a[3] * b[2], a[3] * b[1] - a[1] * b[3], a[1] * b[2] - a[2] * b[1]} end
local function unit(a) local l = math.sqrt(dot(a, a)); return {a[1] / l, a[2] / l, a[3] / l} end
local function lookAt(eye, target, rh)
    local f = unit(sub(target, eye))
    local up = {0, 1, 0}
    local s, u, back
    if rh then
        s = unit(cross(f, up)); u = cross(s, f); back = {-f[1], -f[2], -f[3]}
    else
        s = unit(cross(up, f)); u = cross(f, s); back = f
    end
    return {s[1], u[1], back[1], 0, s[2], u[2], back[2], 0, s[3], u[3], back[3], 0,
        -dot(s, eye), -dot(u, eye), -dot(back, eye), 1}
end
-- BG3's view convention, then the requested view in it (r48: +Z forward, left-handed).
function M.LookAt(current, eye, target, focus)
    assert(type(current) == "table" and #current == 16, "No BG3 view to match")
    local r = {{current[1], current[5], current[9]}, {current[2], current[6], current[10]}, {current[3], current[7], current[11]}}
    local t = {current[13], current[14], current[15]}
    local e = {-(t[1] * r[1][1] + t[2] * r[2][1] + t[3] * r[3][1]),
               -(t[1] * r[1][2] + t[2] * r[2][2] + t[3] * r[3][2]),
               -(t[1] * r[1][3] + t[2] * r[2][3] + t[3] * r[3][3])}
    for _, rh in ipairs({true, false}) do
        local f = rh and {-r[3][1], -r[3][2], -r[3][3]} or r[3]
        local rebuilt = lookAt(e, {e[1] + f[1], e[2] + f[2], e[3] + f[3]}, rh)
        local err = 0
        for i = 1, 16 do err = err + math.abs(rebuilt[i] - current[i]) end
        if err < 1e-2 and dot(f, sub(focus, e)) > 0 then return lookAt(eye, target, rh), rh and "RH" or "LH" end
    end
    error("BG3's view matches neither look-at convention")
end

function M.Start(runId)
    local client = Ext.IsClient()
    local side = client and "client" or "server"
    local resultName = "wow-bg3/arena-" .. side .. "-result.json"
    local ticks, sequence, stamp, rays, camera, pool = 0, 0, nil, nil, nil, nil
    -- A load starts this module again with the same RunId: the command already in the file is the
    -- previous session's, done (WorldMode re-ran the old Start after a
    -- load, and the bridge's own Start then failed "the pool is in use"). Taken as done here, at start.
    pcall(function()
        local raw = Ext.IO.LoadFile("wow-bg3/arena-command.json")
        local ok, c = pcall(Ext.Json.Parse, raw or "")
        if ok and type(c) == "table" and c.RunId == runId and math.type(c.Sequence) == "integer" then sequence = c.Sequence end
    end)

    local function readJson(name)
        local raw = Ext.IO.LoadFile("wow-bg3/" .. name)
        local ok, v = pcall(Ext.Json.Parse, raw or "")
        return ok and v or raw
    end
    local function raysOff(reason)
        if not rays then return nil end
        N.Call(N.Op.RaysStats)
        local stats = readJson("native-" .. side .. "-rays.json")
        N.Call(N.Op.RaysOff)
        local out = {Reason = reason, Held = now() - rays.At, Stats = stats}
        rays = nil
        Ext.Log.Print("WOW-BG3 arena " .. side .. " rays off: " .. Ext.Json.Stringify(out))
        return out
    end
    local function cameraOff(reason)
        if not camera then return nil end
        local st = N.Call(N.Op.CamStats)
        N.Call(N.Op.CamOff)
        local out = {Reason = reason, Held = now() - camera.At, Calls = st.R[1], Served = st.R[2],
            FovServed = st.R[3], FollowFrames = st.R[4]}
        camera = nil
        Ext.Log.Print("WOW-BG3 arena camera off: " .. Ext.Json.Stringify(out))
        return out
    end
    -- BG3's cursor keeps a character it hit only if the level's region map (a 4 m grid of region
    -- numbers over the tutorial's AI grid) at the character's position equals the player's region
    -- (the region trigger box the player's character stands in, 0 for none). The map lookup wraps
    -- rows west and east of the grid, so in the void Tav was picked or not by an unrelated cell,
    -- and never above the box's top (y 155.7). PickRegion empties the map on the client (plugin op
    -- 28): every lookup reads 0, Tav's region in the void (see the m4b-world-mode notes).
    -- The level's region triggers are never moved (an earlier fix moved them onto Tav, which set her
    -- region to the box's and broke picking wherever the wrapped cell read 0).
    local pick = nil
    local function pickOff(reason)
        if not pick then return nil end
        local ok, r = pcall(N.Call, N.Op.PickRegion, 0)
        local out = {Reason = reason, Held = now() - pick.At, Back = ok and r.Err == 0 and r.R[1] == 0,
            Dropped = ok and r.R[1] == 2, Error = not ok and tostring(r) or nil}
        pick = nil
        Ext.Log.Print("WOW-BG3 arena " .. side .. " picking region map: " .. Ext.Json.Stringify(out))
        return out
    end
    -- World mode's step height: BG3 paths only
    -- between neighbour tiles whose heights differ by less than the path's StepHeight, which comes
    -- from the walker's template (MovementStepUpHeight, 0.6 m for Tav and the wolf). WoW's own rule
    -- is two numbers (the 1.12 client's, from benilla's character controller): a surface is walkable
    -- while its normal is within 50 degrees of up, and over anything steeper the player climbs only
    -- a 1 yd rise (STEP_UP_HEIGHT; 2 yd for everything that is not the player). wow_arena.py
    -- keeps the stamp to the 50 degree rule, so the ground BG3 is given is already what a WoW
    -- player may stand on, and terrain never offers neighbour tiles more than 0.5 * tan 50
    -- = 0.60 m apart (measured over seven squares). STEP_UP 1.0 m is the client's 1 yd rounded up:
    -- the 9 percent it is generous by is spent only where a building meets the ground (stair treads
    -- and floor joins, where our 0.5 m sampling makes the step look bigger than the client sees it;
    -- 70 such pairs in the Northshire abbey square, 42 in the Goldshire inn's, none on terrain).
    -- While the pool is out (world mode),
    -- the templates of the characters near Tav (Tav, the fights' stand-ins) take STEP_UP; poolOff
    -- puts every one back. A template that will not take the value is left alone (logged once).
    local STEP_UP = 1.0
    local steps, stepsFailed = {}, false
    local function stepOf(t)
        local v = t.MovementStepUpHeight
        if type(v) == "number" then return v end
        return v.Value
    end
    local function setStep(t, value)
        if type(t.MovementStepUpHeight) == "number" then t.MovementStepUpHeight = value
        else t.MovementStepUpHeight.Value = value end
    end
    local function raiseSteps()
        if stepsFailed then return end
        local host = Hero.Entity()
        local hp = host and host.Transform.Transform.Translate
        if not hp then return end
        for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent(client and "ClientCharacter" or "ServerCharacter")) do
            local okT, t = pcall(function()
                local q = e.Transform.Transform.Translate
                if (q[1] - hp[1]) ^ 2 + (q[3] - hp[3]) ^ 2 > 200 * 200 then return nil end
                return (client and e.ClientCharacter or e.ServerCharacter).Template
            end)
            local key = okT and t and tostring(t.Id) or nil
            if key and not steps[key] then
                local old = stepOf(t)
                if type(old) == "number" and old < STEP_UP then
                    local ok, err = pcall(setStep, t, STEP_UP)
                    if ok and math.abs(stepOf(t) - STEP_UP) < 1e-3 then
                        steps[key] = {Template = t, Old = old, Name = tostring(t.Name)}
                        Ext.Log.Print(string.format("WOW-BG3 arena %s: %s steps up to %.1f m in world mode (was %.2f)", side,
                            tostring(t.Name), STEP_UP, old))
                    else
                        stepsFailed = true
                        Ext.Log.Print("WOW-BG3 arena " .. side .. ": template step height not writable: " .. tostring(err))
                        return
                    end
                end
            end
        end
    end
    local function lowerSteps()
        local back = 0
        for _, st in pairs(steps) do
            if pcall(setStep, st.Template, st.Old) then back = back + 1 end
        end
        steps, stepsFailed = {}, false
        return back
    end
    local function poolOff(reason)
        if not pool then return nil end
        local okS, steppedBack = pcall(lowerSteps)
        local r = N.Call(N.Op.PoolRestore)
        local out = {Reason = reason, Held = now() - pool.At, Back = r.R[1], Occupied = r.R[2], Dropped = r.R[3],
            StepHeightsBack = okS and steppedBack or tostring(steppedBack)}
        pool = nil
        Ext.Log.Print("WOW-BG3 arena " .. side .. " pool back: " .. Ext.Json.Stringify(out))
        return out
    end
    -- BG3 answers "who is near X" (the server activating characters round each player, which the
    -- client needs before it builds a character's visual, sight and picking) from a spatial grid of
    -- 12.73 m cells whose square the engine takes from the AI grid's bounds when the level loads; a
    -- query wholly outside the square finds nothing. Tav's void lies 1.1 km west of the level, so
    -- stand-ins there were never activated (client stubs without Visual or Sight, positions frozen).
    -- Place keeps the AI grid's bounds round the squares; gridFit then has the engine rebuild its
    -- grid (plugin op 30: the engine's own level-load flags) whenever those bounds leave the grid's
    -- square, and Restore shrinks it back to the level's.
    local CELL = 12.727922
    local spatial = nil
    local function gridSystem()
        local name = client and "ecl::spatial_grid::UpdateSystem" or "esv::spatial_grid::UpdateSystem"
        local p = Ext._Internal.SystemProbe(name)
        assert(type(p) == "table" and p.Ok and math.type(p.System) == "integer" and p.System ~= 0, "no " .. name)
        return p.System
    end
    local function gridSquare()
        for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("SpatialGrid")) do
            local ok, g = pcall(function() local d = e.SpatialGrid.Grid; return {d.MinPos[1], d.MinPos[3], d.Size} end)
            if ok then return g end
        end
        return nil
    end
    local function gridCall(rebuild, reason)
        local a1, a2, a3 = N.Pieces(gridSystem())
        local out = N.Must(N.Op.SpatialGrid, a1, a2, a3, rebuild and 1 or 2)
        local r = {Flags = out.R[1], Bounds = {out.F[1], out.F[2], out.F[3], out.F[4]}, Square = gridSquare()}
        if rebuild then
            spatial = spatial or {At = now(), Rebuilds = 0}
            spatial.Rebuilds = spatial.Rebuilds + 1
            spatial.Bounds = r.Bounds
            r.Rebuild, r.Reason = true, reason
            Ext.Log.Print("WOW-BG3 arena " .. side .. " spatial grid rebuild: " .. Ext.Json.Stringify(r))
        end
        return r
    end
    local function gridFit(reason)
        local r = gridCall(false)
        local b, sq = r.Bounds, r.Square
        local span = sq and sq[3] * CELL
        if sq and b[1] >= sq[1] and b[2] >= sq[2] and b[3] <= sq[1] + span and b[4] <= sq[2] + span then
            r.Covered = true
            return r
        end
        return gridCall(true, reason)
    end
    -- After the pool is back (the AI grid's bounds are the level's again).
    local function gridBack(reason)
        if not spatial then return nil end
        local r = gridCall(true, reason)
        spatial = nil
        return r
    end
    local function restore(reason)
        local okc, cam = pcall(cameraOff, reason)
        local okr, off = pcall(raysOff, reason)
        local okp, back = pcall(poolOff, reason)
        local okk, picked = pcall(pickOff, reason)
        local okg, regrid = pcall(gridBack, reason)
        if not stamp then return {Stamped = false, Reason = reason, Rays = okr and off or tostring(off), Camera = okc and cam or tostring(cam),
            Pool = okp and back or tostring(back), PickRegion = okk and picked or tostring(picked),
            SpatialGrid = okg and regrid or tostring(regrid)} end
        local r = N.Call(N.Op.Restore)
        local out = {Restored = r.R[1], Changed = r.R[2], Dropped = r.R[3], Reason = reason,
            Held = now() - stamp.At, Rays = okr and off or tostring(off), Camera = okc and cam or tostring(cam),
            Pool = okp and back or tostring(back), PickRegion = okk and picked or tostring(picked),
            SpatialGrid = okg and regrid or tostring(regrid)}
        stamp = nil
        Ext.Log.Print("WOW-BG3 arena " .. side .. " restore: " .. Ext.Json.Stringify(out))
        return out
    end
    local function write(ok, value)
        Ext.IO.SaveFile(resultName, Ext.Json.Stringify({RunId = runId, Sequence = sequence, Side = side,
            OK = ok, Value = ok and value or tostring(value)}))
    end
    local function installCamera(inner)
        local controller = M.ControllerAddress()
        if inner then controller = assert(Ext._Internal.Peek(controller + 0x300, 8), "No inner controller") end
        local out = N.Must(N.Op.CamInstall, N.Pieces(controller))
        camera = {At = now(), Controller = string.format("%x", controller), Address = controller, Inner = inner == true,
            OwnFov = out.F[1], Source = 0, Refused = {}}
    end
    -- Leading follows the camera BG3 renders (WoW's camera mimics BG3's action
    -- camera during casts and attacks): an active camera other than the game camera the plugin hooked
    -- leads while it is there (plugin LeadSource: its pose, field of view and projection go to
    -- bg3cam.bin, with the eye clearance on top), else the game camera. A camera the plugin refuses
    -- is not asked again.
    local function followRenderCamera()
        local want = 0
        for _, c in ipairs(M.ActiveControllers()) do
            if c.Address ~= camera.Address and not c.Preview and not camera.Refused[c.Address] then want = c.Address break end
        end
        if want == camera.Source then return end
        local out = want == 0 and N.Call(N.Op.LeadSource, 0, 0, 0) or N.Call(N.Op.LeadSource, N.Pieces(want))
        if out.Err == 0 then
            camera.Source = want
            Ext.Log.Print(string.format("WOW-BG3 arena client: lead follows %s (%x)", want == 0 and "the game camera"
                or "another camera", want))
        else
            camera.Refused[want] = true
            Ext.Log.Print(string.format("WOW-BG3 arena client: camera %x not followed: %s", want, tostring(out.Why)))
        end
    end
    local function currentView()
        local st = N.Must(N.Op.CamStats)
        return st.F, st
    end

    -- The server's beat (plugin op 70): its stall watchdog logs where the server thread is when the beats stop
    -- while the pool is out (re-origins stopped the server for 4 to 13 s with nothing in the log). An older
    -- plugin refuses the op: no more beats then. Unarmed (no pool, or a load or a save) it beats once to say so.
    local beating, beatArmed = not client, false
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if beating then
            local armed = pool ~= nil and Ext.Require("Session.lua").Live()
            if armed or beatArmed then
                local okBeat, beat = pcall(N.Call, N.Op.Beat, armed and 1 or 0)
                if okBeat and beat.Err ~= 0 then beating = false end
                beatArmed = armed
            end
        end
        if stamp or rays or camera or pool or pick then
            if not Ext.Require("Session.lua").Live() then -- Running, or held under the loading screen
                -- A save (Save, Paused) keeps the level and the moved squares: hold, never restore.
                if not Ext.Require("Session.lua").Held() then
                    restore("game state " .. tostring(Ext.Utils.GetGameState()))
                end
            elseif stamp and now() > stamp.Until then
                restore("time limit")
            elseif pool and now() > pool.Until then
                restore("time limit")
            elseif rays and now() > rays.Until then
                pcall(raysOff, "time limit")
            elseif camera and now() > camera.Until then
                pcall(cameraOff, "time limit")
            elseif pick and now() > pick.Until then
                pcall(pickOff, "time limit")
            elseif pick and ticks % 60 == 47 then
                -- Kept empty (the plugin empties it again if the engine refilled it); the player's
                -- region is reported (0 in the void; not 0 means Tav stands in the tutorial's box).
                local ok, r = pcall(N.Call, N.Op.PickRegion, 1)
                if ok and r.Err == 0 then
                    if r.R[4] ~= pick.PlayerRegion and r.R[4] ~= 0 then
                        Ext.Log.Print("WOW-BG3 arena client: Tav is in region " .. r.R[4] .. ": BG3 picks only there")
                    end
                    pick.PlayerRegion = r.R[4]
                elseif ok then
                    Ext.Log.Print("WOW-BG3 arena client picking region map: " .. tostring(r.Why))
                end
            elseif rays and ticks % 60 == 31 then
                -- The shape synthetic WoW hits name must still be in the level (plugin ProxyCheck).
                local ok, r = pcall(N.Call, N.Op.ProxyCheck)
                if ok and r.R[1] == 2 then
                    Ext.Log.Print("WOW-BG3 arena " .. side .. ": no proxy shape left, rays off")
                    rays = nil
                end
            end
        end
        if pool and ticks % 30 == 13 and Ext.Require("Session.lua").Live() then
            local ok, err = pcall(raiseSteps)
            if not ok and ticks % 300 == 13 then Ext.Log.Print("WOW-BG3 arena " .. side .. ": step heights: " .. tostring(err)) end
        end
        if client and camera and camera.Lead and Ext.Require("Session.lua").Live() then
            local ok, err = pcall(followRenderCamera)
            if not ok and ticks % 300 == 0 then Ext.Log.Print("WOW-BG3 arena client: lead source: " .. tostring(err)) end
        end
        if ticks % 15 ~= 0 or not Ext.Require("Session.lua").Live() then return end
        local raw = Ext.IO.LoadFile("wow-bg3/arena-command.json")
        if not raw or #raw > 4096 then return end
        local parsed, c = pcall(Ext.Json.Parse, raw)
        if not parsed or type(c) ~= "table" or c.RunId ~= runId then return end
        if math.type(c.Sequence) ~= "integer" or c.Sequence <= sequence or c.Sequence > 4096 then return end
        sequence = c.Sequence
        local ok, value = pcall(function()
            if c.Action == "Restore" then return restore("command") end
            if c.Action == "Status" then
                local info, why = N.Info()
                local st = info and N.Call(N.Op.Status) or nil
                return {Plugin = info or why, Native = st and {Tiles = st.R[1], Rays = st.R[2] == 1, Camera = st.R[3] == 1,
                    FollowFrames = st.R[4], FollowAge = st.F[1], FollowStale = st.F[2]} or nil,
                    Stamped = stamp ~= nil, Stamp = stamp, Rays = rays, Camera = camera, Pool = pool, PickRegion = pick,
                    Now = now()}
            end
            if c.Action == "GridSurvey" then
                -- Read-only: this context's subgrids, patches and tiles (native-<side>-grid.json/.bin).
                local out = N.Must(N.Op.GridSurvey)
                return {Subgrids = out.R[1], Patches = out.R[2], Tiles = out.R[3]}
            end
            if c.Action == "Columns" then
                -- Read-only: vertical rays on a grid (native-<side>-columns.json).
                local f, t = c.From, c.To
                assert(type(f) == "table" and #f == 2 and type(t) == "table" and #t == 2, "From and To [x, z] required")
                local out = N.Must(N.Op.Columns, f[1], f[2], tonumber(c.Step) or 8, t[1], t[2], tonumber(c.Top) or 100)
                return {Columns = out.R[1], WithHits = out.R[2]}
            end
            if c.Action == "SceneCounts" then
                N.Must(N.Op.SceneCounts)
                return readJson("native-" .. side .. "-scene-counts.json")
            end
            if c.Action == "Limits" then
                local out = N.Must(N.Op.Limits, tonumber(c.Side) or 0, tonumber(c.Span) or 0)
                return {Side = out.R[1], Span = out.F[1]}
            end
            if c.Action == "Pool" then
                -- Snapshots only; the subgrids move with Place.
                -- Ids: exactly the subgrids in wow-bg3/pool-ids.txt (WorldMode.lua writes the ones both
                -- sides qualify, so the server and the client move the same subgrids).
                -- Layers (plugin 0.2.9): several out-of-bounds layers at once,
                -- their heights in wow-bg3/pool-layers.txt (both sides write the same text).
                local many = 0
                if type(c.Layers) == "table" and #c.Layers > 0 then
                    local parts = {}
                    for i, y in ipairs(c.Layers) do
                        assert(type(y) == "number" and y == y, "Layers must be numbers")
                        parts[i] = string.format("%.3f", y)
                    end
                    Ext.IO.SaveFile("wow-bg3/pool-layers.txt", table.concat(parts, " "))
                    many = 1
                end
                local out = N.Must(N.Op.Pool, tonumber(c.Layer) or -15.262, math.tointeger(c.Count) or 6, c.Ids and 1 or 0, many)
                return {Count = out.R[1], Pool = readJson("native-" .. side .. "-pool.json")}
            end
            if c.Action == "Bounds" then
                -- Read-only: the AI grid's bounds (tiles) and min corner (metres), as the plugin sees them.
                local out = N.Must(N.Op.Bounds)
                return {Min = {out.R[1], out.R[2]}, Max = {out.R[3], out.R[4]}, WorldMin = {out.F[1], out.F[2], out.F[3]},
                    PoolFoundThem = out.F[4] == 1, MatchSubgrids = out.F[5] == 1}
            end
            if c.Action == "RaysOff" then return raysOff("command") or {Rays = false} end
            if c.Action == "PickRegionOff" then
                if not client then return {Side = "server", PickRegion = "client only"} end
                return pickOff("command") or {PickRegion = false}
            end
            if c.Action == "Extend" then
                -- World mode keeps the arena for the whole session: push every time limit out.
                local seconds = assert(tonumber(c.Seconds), "Seconds required")
                assert(seconds > 0 and seconds <= MAX_SECONDS, "Seconds out of range")
                if stamp then stamp.Until = now() + seconds end
                if rays then rays.Until = now() + seconds end
                if camera then camera.Until = now() + seconds end
                if pool then pool.Until = now() + seconds end
                if pick then pick.Until = now() + seconds end
                return {Stamp = stamp ~= nil, Rays = rays ~= nil, Camera = camera ~= nil, Pool = pool ~= nil,
                    PickRegion = pick ~= nil, Until = now() + seconds}
            end
            if c.Action == "Lead" then
                if not client then return {Side = "server", Lead = "client only"} end
                -- BG3 owns the camera; benilla follows (plugin writes link/bg3cam.bin). Needs the
                -- camera hooked and a basis (Follow sets both; Basis below without following).
                if not camera then installCamera(false) end
                local w, b = c.Wow, c.Bg3
                if type(w) == "table" and type(b) == "table" then
                    N.Must(N.Op.Basis, w[1], w[2], w[3], b[1], b[2], b[3])
                    N.Must(N.Op.Follow, tonumber(c.Yaw) or 0, 0, 0, 1)  -- basis mirror on, following off
                end
                local out = N.Must(N.Op.Lead, c.On == false and 0 or 1)
                camera.Source = 0  -- the plugin leads from the game camera again (LeadSource)
                camera.Until = now() + (tonumber(c.Seconds) or MAX_SECONDS)
                camera.Lead = c.On ~= false
                return {Lead = camera.Lead, Frames = out.R[1]}
            end
            if c.Action == "CameraProbe" then
                if not client then return {Side = "server", Camera = "client only"} end
                local controller = M.ControllerAddress()
                local function dump(object, nslots, nwords)
                    local vmt = assert(Ext._Internal.Peek(object, 8), "Unreadable vtable")
                    local slots = {}
                    for i = 0, nslots - 1 do
                        local fn = Ext._Internal.Peek(vmt + 8 * i, 8)
                        local bytes = {}
                        for k = 0, 23 do
                            local b = fn and Ext._Internal.Peek(fn + k, 1)
                            bytes[#bytes + 1] = b and string.format("%02x", b) or "??"
                        end
                        slots[#slots + 1] = string.format("%2d %x %s", i, fn or 0, table.concat(bytes, " "))
                    end
                    local words = {}
                    for k = 0, nwords - 1 do
                        local w = Ext._Internal.Peek(object + 4 * k, 4)
                        words[#words + 1] = w and string.format("%08x", w) or "????????"
                    end
                    return {Address = string.format("%x", object), Vtable = string.format("%x", vmt), Slots = slots, Words = words}
                end
                local inner = Ext._Internal.Peek(controller + 0x300, 8)
                return {Wrapper = dump(controller, 24, 0xd0), Inner = inner and inner ~= 0 and dump(inner, 32, 0x100) or nil}
            end
            if c.Action == "CountOn" then
                if not client then return {Side = "server", Count = "client only"} end
                local object = M.ControllerAddress()
                if c.Inner then object = assert(Ext._Internal.Peek(object + 0x300, 8), "No inner object") end
                local slots = math.tointeger(c.Slots) or 19
                local a, b, d = N.Pieces(object)
                N.Must(N.Op.CountOn, a, b, d, slots)
                return {Counting = string.format("%x", object), Slots = slots, At = now()}
            end
            if c.Action == "CountRead" then
                if not client then return {Side = "server", Count = "client only"} end
                N.Must(N.Op.CountRead)
                local counts = readJson("native-count.json")
                N.Call(N.Op.CountOff)
                return {Counts = counts, At = now()}
            end
            if c.Action == "CameraStats" then
                if not client then return {Side = "server", Camera = "client only"} end
                local view, st = currentView()
                return {Camera = camera, Calls = st.R[1], Served = st.R[2], FovServed = st.R[3], FollowFrames = st.R[4], View = view}
            end
            if c.Action == "CameraOff" then
                if not client then return {Side = "server", Camera = "client only"} end
                return cameraOff("command") or {Camera = false}
            end
            local host = assert(Hero.Entity(), "No hero (Hero.lua)")
            if not client then assert(Osi.GetRegion(Hero.Uuid()) == REGION, "Hero not in " .. REGION) end
            local seconds = assert(tonumber(c.Seconds), "Seconds required")
            assert(seconds > 0 and seconds <= MAX_SECONDS, "Seconds out of range")
            if c.Action == "Camera" then
                if not client then return {Side = "server", Camera = "client only"} end
                local eye, target = c.Eye, c.Target
                assert(type(eye) == "table" and #eye == 3 and type(target) == "table" and #target == 3, "Eye and Target required")
                local p = host.Transform.Transform.Translate
                for _, q in ipairs({eye, target}) do
                    local dx, dy, dz = q[1] - p[1], q[2] - p[2], q[3] - p[3]
                    assert(math.sqrt(dx * dx + dy * dy + dz * dz) <= 60, "Camera points too far from the disposable character")
                end
                if not camera then installCamera(c.Inner) end
                local current = currentView()
                local hp = host.Transform.Transform.Translate
                local view, convention = M.LookAt(current, eye, target, {hp[1], hp[2], hp[3]})
                N.SaveView(view)
                N.Must(N.Op.CamPose, 1)
                camera.Until = now() + seconds
                camera.Eye, camera.Target, camera.Convention = eye, target, convention
                Ext.Log.Print("WOW-BG3 arena camera: " .. Ext.Json.Stringify(camera))
                return {Camera = camera, BG3View = current, View = view}
            end
            if c.Action == "Follow" then
                if not client then return {Side = "server", Follow = "client only"} end
                local w, b = c.Wow, c.Bg3
                local p = host.Transform.Transform.Translate
                if c.Anchor then
                    -- The WoW player (from benilla's camera record) lands on the disposable character.
                    local raw = assert(Ext.IO.LoadFile("wow-bg3/link/camera.bin"), "No camera stream from benilla")
                    assert(#raw >= 96 and raw:sub(1, 4) == "WBC1", "Camera stream has no player record")
                    local px, py, pz = string.unpack("<fff", raw, 73)
                    local flags = string.unpack("<I4", raw, 89)
                    assert(flags & 1 == 1, "benilla has no placed player yet")
                    w, b = {px, py, pz}, {p[1], p[2], p[3]}
                end
                assert(type(w) == "table" and #w == 3 and type(b) == "table" and #b == 3, "Wow and Bg3 points required")
                local dx, dz = b[1] - p[1], b[3] - p[3]
                assert(math.sqrt(dx * dx + dz * dz) <= 60, "Bridge origin too far from the disposable character")
                if not camera then installCamera(false) end
                N.Must(N.Op.Basis, w[1], w[2], w[3], b[1], b[2], b[3])
                N.Must(N.Op.Follow, tonumber(c.Yaw) or 0, 1, c.Fov and 1 or 0, c.Mirror and 1 or 0)
                camera.Until = now() + seconds
                camera.Follow = {Wow = w, Bg3 = b, Yaw = tonumber(c.Yaw) or 0, Fov = c.Fov == true, Mirror = c.Mirror == true}
                Ext.Log.Print("WOW-BG3 arena follow: " .. Ext.Json.Stringify(camera))
                return {Camera = camera}
            end
            if c.Action == "PickRegion" then
                if not client then return {Side = "server", PickRegion = "client only"} end
                local out = N.Must(N.Op.PickRegion, 1)
                pick = pick or {At = now()}
                pick.Until = now() + seconds
                pick.Cells, pick.Width, pick.PlayerRegion = out.R[2], out.R[3], out.R[4]
                Ext.Log.Print("WOW-BG3 arena client picking region map emptied: " .. Ext.Json.Stringify(pick))
                return {PickRegion = pick, Scale = out.F[1], Origin = {out.F[2], out.F[3]}}
            end
            if c.Action == "SpatialGrid" then
                local r = gridCall(c.Rebuild == true, "command")
                r.State = spatial
                r.Targets = {}
                for _, uuid in ipairs(type(c.Targets) == "table" and c.Targets or {}) do
                    local e = type(uuid) == "string" and Ext.Entity.Get(uuid)
                    r.Targets[#r.Targets + 1] = e and {Uuid = uuid, Active = e:HasRawComponent("eoc::ActiveComponent"),
                        Presence = client and e:HasRawComponent("ecl::level::PresenceComponent") or nil,
                        Position = (function() local t = e.Transform.Transform.Translate; return {t[1], t[2], t[3]} end)()}
                        or {Uuid = tostring(uuid), Exists = false}
                end
                return r
            end
            if c.Action == "PlaceCheck" then
                -- Whether this side would take each cell's pool subgrid now: Place refuses one that
                -- something stands on (an entity registered on its tiles or attached to it), unless the cell
                -- is the spot it already holds (retiled in place). WorldMode asks both sides before a Place,
                -- so a square moves on both or on neither.
                assert(type(c.Cells) == "table" and #c.Cells >= 1 and #c.Cells <= 12, "Cells (1 to 12) required")
                local busy, seen = {}, {}
                for _, cell in ipairs(c.Cells) do
                    local k = assert(math.tointeger(cell[1]), "a cell is {K, MinX, MinZ, BaseY, Serial}")
                    -- Bound first: assert returns its message too, so as the call's last argument it became
                    -- argument 4 ("plugin argument 4 is not a number") and every check failed.
                    local minX, minZ = tonumber(cell[2]), tonumber(cell[3])
                    assert(minX and minZ, "a cell's MinX and MinZ are numbers")
                    local out = N.Must(N.Op.PoolFree, k, minX, minZ)
                    if out.R[1] ~= 1 then
                        busy[#busy + 1] = k
                        seen[#seen + 1] = {K = k, Tiles = out.R[2], Attached = out.R[3], Entities = out.R[4],
                            First = {out.F[1], out.F[2]}}
                    end
                end
                if #seen > 0 then Ext.Log.Print("WOW-BG3 arena " .. side .. " place check: " .. Ext.Json.Stringify(seen)) end
                return {Busy = busy, Seen = seen}
            end
            if c.Action == "Place" then
                -- World mode's treadmill: pool subgrids to 64 m squares of WoW ground near Tav (or, at
                -- Start, near the void spot she is about to be teleported to: within 3 km).
                assert(type(c.Cells) == "table" and #c.Cells >= 1 and #c.Cells <= 12, "Cells (1 to 12) required")
                local p = host.Transform.Transform.Translate
                local placed = {}
                -- Tracked before the first move, so a refused cell still leaves a pool to restore.
                pool = pool or {At = now()}
                pool.Until = now() + seconds
                for _, cell in ipairs(c.Cells) do
                    local k, minX, minZ, baseY = math.tointeger(cell[1]), tonumber(cell[2]), tonumber(cell[3]), tonumber(cell[4])
                    local serial = math.tointeger(cell[5]) or 0  -- tiles file section-<K>-<serial>.bin (0: section-<K>.bin)
                    assert(k and minX and minZ and baseY, "a cell is {K, MinX, MinZ, BaseY, Serial}")
                    assert(serial >= 0 and serial < 4096, "Serial out of range")
                    local dx, dz = minX + 32 - p[1], minZ + 32 - p[3]
                    assert(math.sqrt(dx * dx + dz * dz) <= 3000, "Cell too far from the disposable character")
                    local out = N.Must(N.Op.Place, k, minX, minZ, baseY, serial)
                    placed[#placed + 1] = {K = k, Patches = out.R[1]}
                end
                pool.Cells = c.Cells
                -- BG3's spatial grid must cover the squares, or nothing near Tav is found there.
                local okg, fit = pcall(gridFit, "place")
                return {Placed = placed, Until = pool.Until, SpatialGrid = okg and fit or tostring(fit)}
            end
            if c.Action == "Rays" then
                local tris = assert(Ext.IO.LoadFile("wow-bg3/arena-triangles.bin"), "No arena triangles")
                assert(#tris > 0 and #tris % 36 == 0, "Arena triangles must be records of 9 floats")
                -- The plugin swaps the triangle set atomically: no moment without WoW ground.
                local before = rays and {Replaced = true, Held = now() - rays.At} or nil
                local mask = math.tointeger(c.Mask) or 0
                -- Contexts left to BG3 (bit c = context c). Default none: the client's cursor picking
                -- and its cast-position search share context 1, and both must see WoW geometry
                -- (skipping it sends casts that the server then refuses; see m4-void-arena.md).
                local skip = math.tointeger(c.SkipContexts) or 0
                assert(skip >= 0 and skip < (1 << 24), "SkipContexts out of range")
                local out = N.Must(N.Op.Rays, c.Void == true and 1 or 0, mask, skip)
                rays = {At = now(), Until = now() + seconds, Triangles = out.R[1], Void = c.Void == true, Mask = mask,
                    SkipContexts = skip}
                Ext.Log.Print("WOW-BG3 arena " .. side .. " rays: " .. Ext.Json.Stringify(rays))
                return {Previous = before, Rays = rays}
            end
            assert(c.Action == "Stamp", "Unknown arena action")
            local tiles = assert(Ext.IO.LoadFile("wow-bg3/arena-tiles.bin"), "No arena tiles")
            local n = math.floor(math.sqrt(#tiles / 5) + 0.5)
            assert(n * n * 5 == #tiles, "Arena tiles must be n*n records of 5 bytes")
            local minX, minZ, anchorY = tonumber(c.MinX), tonumber(c.MinZ), tonumber(c.AnchorY)
            assert(minX and minZ and anchorY, "MinX, MinZ and AnchorY required")
            local p = host.Transform.Transform.Translate
            local dx, dz = minX + n * 0.25 - p[1], minZ + n * 0.25 - p[3]
            -- A void stamp goes into empty out-of-bounds tiles away from the ship, where the
            -- party is then teleported; a normal stamp stays around the disposable character.
            assert(math.sqrt(dx * dx + dz * dz) <= (c.Void and VOID_RADIUS or RADIUS), "Arena centre too far from the disposable character")
            local before
            if c.Keep and stamp then
                -- World mode: only the tiles go back before the new stamp; rays and the
                -- camera stay, so the character standing on WoW ground never loses it.
                local r = N.Call(N.Op.Restore)
                before = {Restored = r.R[1], Changed = r.R[2], Kept = true}
                stamp = nil
            else
                before = restore("new stamp")
            end
            local out = N.Must(N.Op.Stamp, minX, minZ, anchorY, tonumber(c.Tolerance) or (c.Void and 15 or 4), c.Void and 1 or 0,
                tonumber(c.Layer) or 0)
            stamp = {At = now(), Until = now() + seconds, Side = n, MinX = minX, MinZ = minZ, Void = c.Void == true,
                AnchorY = anchorY, Stamped = out.R[1], NoTile = out.R[2], OutOfRange = out.R[3], Bytes = #tiles}
            Ext.Log.Print("WOW-BG3 arena " .. side .. " stamped: " .. Ext.Json.Stringify(stamp))
            return {Previous = before, Stamp = stamp}
        end)
        write(ok, value)
    end)
end
return M
