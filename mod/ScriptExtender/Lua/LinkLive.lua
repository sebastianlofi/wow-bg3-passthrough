-- Live BG3 backend for the link test on a disposable save (client context).
-- A WoW-role peer (wow-bg3-link live-wow) writes wow-bg3/link/start.json and then talks
-- protocol v1 over wow-bg3/link/{wow,bg3}.json. Every action goes through the game's normal
-- player path: hotbar commands, the real pointer (runtime patch 0002) and the game's own
-- picking. Results come from the engine: the view model's turn flags (Force Turn-Based),
-- the server monitor's TurnStarted and cast events (real combat), the host's position and
-- Health. No Osiris calls, cast flags or rule edits. In real combat it selects the
-- disposable character through the portrait command, ends turns through the view model's
-- EndTurn command when the game's CanEndTurn flags allow it, and passes the turns of other
-- characters this player controls after the host ends its turn.
-- The bridge-to-world mapping is a plain offset that puts the WoW hero on the host; it is
-- not a calibrated basis. Do not run manual action-command.py pointer actions meanwhile.
--
-- Void fights: when start.json carries Void, the encounter is set
-- up by this module from the Begin message alone, through the same command files the
-- harness tools use (Arena and StandIns, one sequence each): the WoW arena (already written
-- to wow-bg3/arena-tiles.bin and arena-triangles.bin by the WoW side) is stamped into the
-- out-of-bounds layer and put into both physics scenes, the disposable character is
-- teleported onto it, one stand-in per WoW enemy is spawned from the given root template,
-- given the given faction and joined to the host, and BG3's camera follows benilla's when
-- its camera stream is present. Mapping (BG3 renders left-handed): world = Centre +
-- (-x, y, z) of the bridge point, with the Begin basis' WoW origin on Centre. Turns of
-- stand-ins go back to WoW as Acted plus Effect. Cleanup (or an abort) removes the
-- stand-ins, returns the host and restores the grid and scenes.
--
-- World mode (WorldMode.lua started): Tav already stands on WoW ground in the void, the
-- arena follows her and BG3's camera leads. The WoW side cuts the fight with world mode's
-- mapping (link/world.json), so a void fight here only spawns and joins the stand-ins: no stamp,
-- rays, teleport or follow camera, and cleanup leaves Tav, the arena and the camera alone.
--
-- World fights (the decisions in the world-fights notes): one to eight
-- enemies per encounter, and Join adds more while it runs (spawned, scaled and joined to the
-- running combat; Roster answers with joins counted). Each stand-in's template and the WoW
-- numbers it is scaled from come from wow-bg3/link/standins.json (the bridge writes it before
-- Begin and before each Join). Scaling is BG3 setup, not a rule change: the stand-in keeps its
-- template's own BG3 max HP (no HP scaling) at the WoW mob's HP fraction (set
-- in the tick that creates it), and with ratio = Tav's BG3 max HP / the WoW hero's max HP it gets
-- DamageBonus(WoW average hit x ratio - its attack's average) (Osiris AddBoosts; BG3 still rolls
-- hits and damage). Roster reports every combatant's BG3 HP before
-- Ready, after each Join and whenever HP changed without an Effect; a missed stand-in attack
-- still sends an Effect (unchanged HP) so WoW plays the miss.
--
-- Mirrored mobs: in world mode a WoW mob near Tav already has a BG3 stand-in
-- (Mirrors.lua, scaled at spawn). Begin and Join adopt it (no spawn, no scaling; it is joined to
-- Tav's combat unless it is in it already), and cleanup leaves it to Mirrors.lua. When BG3's combat
-- is already running at Begin (Tav attacked a mirror, or one spotted her), setup does not wait for
-- a first TurnStarted: the turns are read from the latest TurnStarted of a combatant on.
--
-- Live stand-ins: WoW models walked 1.0 to 1.2 s
-- behind BG3 (positions reached benilla through mirror-status.json and the bridge about 3 times a
-- second) and sometimes faced away from Tav. During a world-mode fight every tick writes
-- wow-bg3/link/standins.bin straight for benilla: "SIN1", seq u32, n u32, BG3's clock f64 (ms,
-- Ext.Utils.MonotonicTime), then per enemy guid hi u32, guid lo u32, WoW x, y, z (yards),
-- WoW orientation (radians), flags u32 (1 dead, 2 moving) from the stand-in's client Transform
-- (what BG3 draws), then seq again (torn-read check). fight.txt's actor lines stay the fallback.
local M = {}
local N = Ext.Require("WowNative.lua")
local ArenaMath = Ext.Require("Arena.lua")
local Link = Ext.Require("Link.lua")
local Adapter = Ext.Require("LinkAdapter.lua")
local Probe = Ext.Require("ActionProbe.lua")
local World = Ext.Require("WorldMode.lua")
local Hero = Ext.Require("Hero.lua") -- the hero: the character the player controls
-- standins.bin's seq, rising across fights; mod-wide (client), as MirrorsClient.lua writes the file
-- too for robbed NPCs searching outside a fight. WOWBG3SinAt: when this file wrote it last.
WOWBG3SinSeq = WOWBG3SinSeq or 0
local DIR = "wow-bg3/link/"
-- Protocol spell id -> {hotbar prototype, targeting}.
local SPELLS = {Dash = {"Shout_Dash", "self"}, Disengage = {"Shout_Disengage", "self"},
    FireBolt = {"Projectile_FireBolt", "target"}, Attack = {"Target_MainHandAttack", "target"}}
local AIM_TOLERANCE = 1.5 -- metres between the requested and the picked ground point
local SETTLE, NO_MOVE, CAST_WAIT = 45, 120, 240 -- ticks
local MAX_ENEMIES, SPAWN_RADIUS = 50, 20.0 -- stand-ins per encounter (bridge and server: 50); metres from Tav for a spawn
local LIFT = 0.8 -- metres above a target's feet to aim at its body

local function hdist(a, b)
    local dx, dz = a[1] - b[1], a[3] - b[3]
    return math.sqrt(dx * dx + dz * dz)
end
local function hotbar() return Probe.FindUnique("HotBar", "HotBar").DataContext end
local function player() return hotbar().CurrentPlayer end
local function assigned(uuid)
    local list = player().AssignedCharacters
    for i = 1, 16 do
        local c = list[i]
        if c == nil then break end
        if c.EntityUUID == uuid then return c end
    end
end
-- The disposable character's own view model (turn flags), whoever is selected.
local function hostVM() return assert(assigned(Hero.Uuid()), "The hero is not assigned to this player") end
local function selected() return player().SelectedCharacter end
local function hostSelected() return selected().EntityUUID == Hero.Uuid() end
local function hostTurn()
    local c = hostVM()
    return c.InTurnBasedMode == true and c.IsCurrentTurn == true
end
-- The portrait's own SelectCharacter command, as a portrait click does.
local function selectCharacter(uuid)
    local dc = Probe.FindUnique("PlayerPortraits", "PlayerPortraits").DataContext
    local c = assert(assigned(uuid), "Character is not assigned to this player")
    assert(dc.SelectCharacter:CanExecute(c), "SelectCharacter unavailable")
    dc.SelectCharacter:Execute(c)
end
-- The view model's EndTurn command (what the End Turn key invokes), only when the game's
-- own flags allow it. The End Turn button's cached IsEnabled is unreliable in this setup.
local function endTurnNow()
    local dc, p = hotbar(), player()
    if p.IsCurrentTurn ~= true or p.CanEndTurn ~= true then return nil, "game reports the turn cannot end" end
    if p.SelectedCharacter.PlayerCharacterProperties.CanEndTurn ~= true then return nil, "selected character cannot end its turn" end
    if not dc.EndTurn:CanExecute(nil) then return nil, "EndTurn command unavailable" end
    dc.EndTurn:Execute(nil)
    return true
end
-- ActionMonitor (server context) rewrites this file in place, so a read can catch it half
-- written: parse failures are retried a few times, then read as no report.
local function monitor()
    for _ = 1, 4 do
        local raw = Ext.IO.LoadFile("wow-bg3/m3-actions.json")
        if not raw then return nil end
        local ok, v = pcall(Ext.Json.Parse, raw)
        if ok and type(v) == "table" then return v end
    end
    return nil
end
local function lastSeq(report)
    local n = 0
    for _, e in ipairs((report and report.Events) or {}) do if e.Seq > n then n = e.Seq end end
    return n
end
-- The party: a downed hero is no WoW death while a companion still stands (BG3's
-- party can help her up), so the hero's HP reads 1 for the WoW side until then; the hero dead, or
-- every companion down too, is the WoW death as before (ActionMonitor's Party). Now any
-- standing companion counts, in BG3's combat or not (one just revived by a scroll is not flagged in
-- combat yet; BG3 itself shows no game over while anyone stands).
local function standing(m)
    return type(m) == "table" and m.Dead ~= true and m.Downed ~= true and (tonumber(m.Hp) or 0) > 0
end
local function partyHolds(r)
    if type(r) ~= "table" or type(r.Party) ~= "table" then return false end
    local heroDead, stands = false, false
    for _, m in ipairs(r.Party) do
        if type(m) == "table" then
            if m.Hero == true then heroDead = m.Dead == true
            elseif standing(m) then stands = true end
        end
    end
    return stands and not heroDead
end
-- Whether any party member stands (nil: the monitor has no party yet).
local function partyStands(r)
    if type(r) ~= "table" or type(r.Party) ~= "table" or #r.Party == 0 then return nil end
    for _, m in ipairs(r.Party) do
        if standing(m) then return true end
    end
    return false
end
-- Ticks a stand-in must have been out of BG3's combat before the fight counts as over without a
-- victory (BG3 ends a combat when its sides lose each other; a LeftCombat can come and go).
local LEFT_GRACE = 90
local function health(uuid)
    if uuid == Hero.Uuid() then
        local r = monitor()
        local hp = r and r.State and r.State.Hp
        if type(hp) == "table" and math.type(hp[1]) == "integer" then
            if hp[1] <= 0 and partyHolds(r) then return 1, hp[2] end
            return hp[1], hp[2]
        end
    end
    local ok, hp, max = pcall(function()
        local h = Ext.Entity.Get(uuid).Health
        return h.Hp, h.MaxHp
    end)
    if ok and math.type(hp) == "integer" and math.type(max) == "integer" and max > 0 then return hp, max end
    return nil
end
local function findSlot(character, prototype)
    local props = character.PlayerCharacterProperties
    for _, bar in ipairs({"CommonHotBar", "ClassHotBar"}) do
        local list = props[bar].SlotList
        for i = 1, 64 do
            local slot = list[i]
            if slot == nil then break end
            if slot.Content and slot.Content.PrototypeID == prototype then return slot end
        end
    end
end

-- Protocol ids allow [A-Za-z0-9_-] up to 64 characters.
local function protocolId(s)
    s = tostring(s or ""):gsub("[^%w_%-]", "_"):sub(1, 64)
    return s ~= "" and s or "none"
end
local function readJson(name)
    local raw = Ext.IO.LoadFile("wow-bg3/" .. name)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

local function newBackend(actors, void, runId, session)
    local B, events, job, releases = {}, {}, nil, {}
    local ticks, started, lastTurn, origin, heroBridge = 0, false, false, nil, nil
    local mode, seen, byUuid, passing, passed, pendingTurns = nil, 0, {}, false, {}, {}
    -- Void fights: setup/teardown coroutines, stand-ins made here, BG3-run turn being watched.
    local setup, teardown, made, npc, following, holding, finished = nil, nil, {}, nil, false, false, false
    local rest, enemies, heroWait, heroShown, torn = {}, {}, 0, 0, false
    -- World mode: each enemy's last BG3 combat event ({In, At}; none yet = in), and when the encounter
    -- was first seen active.
    local combatState, activeSince = {}, nil
    local inWorld = false -- this fight runs inside world mode
    -- World fights: stand-in specs (standins.json), WoW GUIDs per actor id, joins, HP reported.
    local specs, heroWowMax, guids, scaled, adopted = {}, nil, {}, {}, {}
    local joinQueue, joining, joinsDone, reported = {}, nil, 0, {}
    local function later(fn) events[#events + 1] = fn end
    local function toWorld(p)
        if void then return {void.Centre[1] - p[1], void.Centre[2] + p[2], void.Centre[3] + p[3]} end
        return {origin[1] + p[1] - heroBridge[1], origin[2] + p[2] - heroBridge[2], origin[3] + p[3] - heroBridge[3]}
    end
    local function toBridge(w)
        if void then return {void.Centre[1] - w[1], w[2] - void.Centre[2], w[3] - void.Centre[3]} end
        return {heroBridge[1] + w[1] - origin[1], heroBridge[2] + w[2] - origin[2], heroBridge[3] + w[3] - origin[3]}
    end
    local function positionOf(uuid)
        local ok, p = pcall(function()
            local t = Ext.Entity.Get(uuid).Transform.Transform.Translate
            return {t[1], t[2], t[3]}
        end)
        return ok and p or nil
    end
    local function loadSpecs()
        local v = readJson("link/standins.json")
        if not (v and v.Session == session and type(v.StandIns) == "table") then return false end
        for id, sp in pairs(v.StandIns) do if type(sp) == "table" then specs[id] = sp end end
        if type(v.HeroWowMaxHp) == "number" and v.HeroWowMaxHp > 0 then heroWowMax = v.HeroWowMaxHp end
        return true
    end
    -- A spawn point at most SPAWN_RADIUS from Tav (horizontally): the treadmill keeps at least
    -- 24 m of WoW ground around her. Now (a treadmill per party member,
    -- members any distance apart) from the party member nearest the point, so a companion's fight far
    -- from the hero gets its joining enemies by that companion, on its ground (party.json, Party.lua).
    local function spawnPoint(w)
        local h = Probe.HostPosition()
        local pj = readJson("party.json")
        if type(pj) == "table" and type(pj.Members) == "table" then
            local best
            for _, m in ipairs(pj.Members) do
                local p = type(m) == "table" and m.Pos
                if type(p) == "table" and #p == 3 and m.Dead ~= true then
                    local d = (w[1] - p[1]) ^ 2 + (w[3] - p[3]) ^ 2
                    if not best or d < best then best, h = d, {p[1], p[2], p[3]} end
                end
            end
        end
        local dx, dz = w[1] - h[1], w[3] - h[3]
        local d = math.sqrt(dx * dx + dz * dz)
        if d <= SPAWN_RADIUS then return w end
        local k = SPAWN_RADIUS / d
        return {h[1] + dx * k, math.max(w[2], h[2]), h[3] + dz * k}
    end

    -- Command files (same protocol as bg3/tools/action-command.py; never retried).
    local function wait(n)
        local due = ticks + n
        while ticks < due do coroutine.yield() end
    end
    local function waitFor(test, limit, why)
        local due = ticks + limit
        while not test() do
            if ticks > due then error(why, 0) end
            coroutine.yield()
        end
    end
    local function command(stem, c, results)
        local old = readJson(stem .. ".json")
        local seq = (old and old.RunId == runId and math.type(old.Sequence) == "integer") and old.Sequence + 1 or 1
        c.RunId, c.Sequence = runId, seq
        Ext.IO.SaveFile("wow-bg3/" .. stem .. ".json", Ext.Json.Stringify(c))
        local got = {}
        waitFor(function()
            for _, name in ipairs(results) do
                if not got[name] then
                    local r = readJson(name)
                    if r and r.RunId == runId and r.Sequence == seq then got[name] = r end
                end
            end
            for _, name in ipairs(results) do if not got[name] then return false end end
            return true
        end, 300, c.Action .. ": no result")
        for _, name in ipairs(results) do
            if got[name].OK ~= true then error(c.Action .. " failed: " .. tostring(got[name].Value), 0) end
        end
        return got[results[1]].Value, got
    end
    local function arena(c) return command("arena-command", c, {"arena-server-result.json", "arena-client-result.json"}) end
    local function standIn(c) return command("stand-in-command", c, {"stand-in-result.json"}) end
    local function waitUntil(test, limit)
        local due = ticks + limit
        while not test() do
            if ticks > due then return false end
            coroutine.yield()
        end
        return true
    end
    -- The WoW mob's HP as a percentage of its max (standins.json WowHp / WowMaxHp), at least 1;
    -- also the WoW HP and max HP.
    local function wowPercent(sp)
        local wowMax = sp and tonumber(sp.WowMaxHp) or 0
        if wowMax <= 0 then return nil end
        local wowHp = math.min(tonumber(sp.WowHp) or wowMax, wowMax)
        return math.max(1.0, math.min(100.0, 100.0 * wowHp / wowMax)), wowHp, wowMax
    end
    -- The hit points BG3 gives a character of maxHp for a percentage (SetHitpointsPercentage): rounded
    -- up, at least 1 (harness: 50% of 11 is 6, 40% is 5).
    local function bg3Hp(maxHp, percent)
        return math.max(1, math.min(maxHp, math.ceil(maxHp * percent / 100 - 1e-6)))
    end
    -- A stand-in's BG3 max HP: round(WoW max HP ^ 0.65) (Mirrors.lua the same).
    local function standInMaxHp(wowMax) return math.max(1, math.floor(wowMax ^ 0.65 + 0.5)) end
    -- The stand-in's numbers, recorded per actor in status.json (Scaled). Health: max HP
    -- standInMaxHp (Scale's IncreaseMaxHP) at the WoW mob's fraction. Damage ("keep WoW's ratios"): the average hit is
    -- the WoW mob's times Tav's BG3 max HP / the WoW hero's max HP.
    local function scaleStandIn(id, uuid)
        local sp, info = specs[id], {}
        scaled[id] = info
        local _, tavMax = health(Hero.Uuid())
        if not sp then info.Skipped = "no stand-in spec"; return end
        info.Kind, info.Name = sp.Kind, sp.Name
        if not (tavMax and heroWowMax) then info.Skipped = "no hero numbers"; return end
        local percent, wowHp, wowMax = wowPercent(sp)
        if not percent then info.Skipped = "no WoW HP"; return end
        local ratio = tavMax / heroWowMax
        local base, baseMax = health(uuid)
        local maxHp = standInMaxHp(wowMax)
        local hp = bg3Hp(maxHp, percent)
        local bonus = 0
        if type(sp.WowDamage) == "number" and type(sp.BaseDamage) == "number" then
            bonus = math.floor(sp.WowDamage * ratio - sp.BaseDamage + 0.5)
            -- Floor as in Mirrors.lua: at an average hit near 0 BG3's AI never attacks.
            local floor = math.ceil(1 - sp.BaseDamage)
            if bonus < floor then bonus, info.Floored = floor, true end
        end
        info.Ratio, info.WowHp, info.WowDamage, info.MaxHp, info.Hp, info.DamageBonus =
            ratio, {wowHp, wowMax}, sp.WowDamage, maxHp, hp, bonus
        info.Health, info.Base = "wow^0.65", {base, baseMax}
        -- Spiders (a showcase): BG3's spider web and the phase spider's teleport.
        local spells = sp.Kind == "spider" and {"Target_Web_Spider", "Target_EtherealJaunt"} or nil
        info.Boosts = standIn({Action = "Scale", Target = uuid, DamageBonus = bonus, MaxHp = maxHp, Spells = spells}).Boosts
        pcall(Ext.Require("Abilities.lua").Grant, uuid, sp.Entry)
        info.MaxTook = waitUntil(function() return select(2, health(uuid)) == maxHp end, 90)
        if base ~= hp then
            standIn({Action = "Health", Target = uuid, Percent = percent})
            info.HpTook = waitUntil(function() return health(uuid) == hp end, 90)
        end
        info.Got = {health(uuid)}
    end
    -- Mirrors.lua's report (link/mirror-status.json): a mirrored mob's stand-in, Tav's combat.
    local function mirrorStatus() return readJson("link/mirror-status.json") end
    local function mirrorOf(guid)
        local st = mirrorStatus()
        local m = st and type(st.Mobs) == "table" and st.Mobs[guid]
        if type(m) == "table" and type(m.Uuid) == "string" and m.Dead ~= true and Ext.Entity.Get(m.Uuid) then return m end
    end
    -- Spawn, scale and (unless setup joins later) join one stand-in for a protocol actor; in world
    -- mode a mirrored mob's existing stand-in is adopted instead.
    local function addStandIn(a, joinCombat)
        local mirror = inWorld and mirrorOf(a.wow_guid)
        if mirror then
            local uuid = mirror.Uuid
            actors[a.id], byUuid[uuid], guids[a.id] = uuid, a.id, a.wow_guid
            adopted[uuid] = mirror.InCombat == true and "in combat" or "joins"
            scaled[a.id] = {Adopted = uuid, Kind = specs[a.id] and specs[a.id].Kind, Name = specs[a.id] and specs[a.id].Name,
                Scale = mirror.Scale, Hp = {mirror.Hp, mirror.MaxHp}}
            -- A neutral mob in the fight was provoked in WoW: BG3 only lets a hostile character
            -- join (Osiris EnterCombat did nothing for a neutral stand-in in the harness).
            -- A friendly NPC WoW's rules pulled in (its creature group) turns
            -- on Tav the way BG3 turns someone attacked: a temporary hostile relation, its own
            -- faction kept (the one Tav attacked is in BG3's combat already, its crime flow did it).
            if mirror.InCombat ~= true and mirror.Friendly == true then
                standIn({Action = "TempHostile", Target = uuid, With = Hero.Uuid()})
                scaled[a.id].Provoked = "temporarily hostile"
            elseif mirror.InCombat ~= true and mirror.Hostile ~= true and void.Faction then
                standIn({Action = "Faction", Target = uuid, Faction = void.Faction})
                scaled[a.id].Provoked = true
            end
            if joinCombat and mirror.InCombat ~= true then standIn({Action = "Join", Target = uuid, With = Hero.Uuid()}) end
            rest[a.id] = positionOf(uuid)
            enemies[#enemies + 1] = a
            return uuid
        end
        local sp = specs[a.id]
        local template = (sp and sp.Template) or (void.Templates and void.Templates[a.id])
        assert(type(template) == "string", "no stand-in template for " .. a.id)
        local w = spawnPoint(toWorld(a.position))
        -- Its HP at the WoW fraction from the tick that creates it.
        local percent = wowPercent(sp)
        local v = standIn({Action = "Spawn", Template = template, Position = {w[1], w[2] + 1, w[3]},
            Name = sp and type(sp.Name) == "string" and sp.Name ~= "" and sp.Name or nil,
            Percent = percent and percent < 100 and percent or nil})
        local uuid = assert(v.Created, "spawn returned no stand-in")
        made[#made + 1], actors[a.id], byUuid[uuid], guids[a.id] = uuid, uuid, a.id, a.wow_guid
        wait(30)
        scaleStandIn(a.id, uuid)
        if scaled[a.id] then scaled[a.id].Uuid = uuid end -- Silence.lua: its BG3 sounds off too
        if void.Faction then standIn({Action = "Faction", Target = uuid, Faction = void.Faction}) end
        if joinCombat then standIn({Action = "Join", Target = uuid, With = Hero.Uuid()}) end
        rest[a.id] = positionOf(uuid)
        enemies[#enemies + 1] = a
        return uuid
    end
    -- standins.bin (see the header): every enemy where BG3 draws it, for benilla, each tick.
    local sinLast = {} -- uuid -> last client position (moving flag)
    local function guidParts(g)
        g = tostring(g or ""):gsub("^0[xX]", "")
        if #g == 0 or #g > 16 or not g:match("^%x+$") then return nil end
        g = string.rep("0", 16 - #g) .. g
        return tonumber(g:sub(1, 8), 16), tonumber(g:sub(9, 16), 16)
    end
    local function publishStandIns()
        if not (inWorld and World.ToWow and enemies[1]) then return end
        local parts, n = {}, 0
        for _, a in ipairs(enemies) do
            local uuid = actors[a.id]
            local hi, lo = guidParts(guids[a.id])
            local t = hi and uuid and Ext.Entity.Get(uuid)
            t = t and t.Transform and t.Transform.Transform
            if t then
                local p, r = t.Translate, t.RotationQuat
                local w = World.ToWow({p[1], p[2], p[3]})
                -- A character faces along its client Transform's rotated -z (harness: a
                -- wolf running at Tav moved at -52.7 degrees while its +z pointed at 127.3); WoW
                -- faces (cos o, sin o), and the mapping turns a BG3 direction (dx, dz) into WoW's
                -- (-dz, dx). With (fx, fz) the rotated +z, the facing is (-fx, -fz): o = atan(-fx, fz).
                local x, y, z, qw = r[1], r[2], r[3], r[4]
                local fx, fz = 2 * (x * z + qw * y), 1 - 2 * (x * x + y * y)
                local o = math.atan(-fx, fz)
                local last = sinLast[uuid]
                local moving = last and (math.abs(p[1] - last[1]) + math.abs(p[3] - last[3]) > 0.005)
                sinLast[uuid] = {p[1], p[2], p[3]}
                local hp = health(uuid)
                local flags = ((hp and hp <= 0) and 1 or 0) | (moving and 2 or 0)
                parts[#parts + 1] = string.pack("<I4I4ffffI4", hi, lo, w[1], w[2], w[3], o, flags)
                n = n + 1
            end
        end
        if n == 0 then return end
        WOWBG3SinSeq = WOWBG3SinSeq + 1
        WOWBG3SinAt = Ext.Utils.MonotonicTime()
        Ext.IO.SaveFile(DIR .. "standins.bin", string.pack("<c4I4I4d", "SIN1", WOWBG3SinSeq, n, Ext.Utils.MonotonicTime() * 1.0)
            .. table.concat(parts) .. string.pack("<I4", WOWBG3SinSeq))
    end
    -- Every combatant as BG3 has it, for Roster (and what WoW was told last).
    local function rosterActors()
        local list = {}
        local function add(id, uuid)
            local hp, max = health(uuid)
            local p = positionOf(uuid)
            if hp and p and guids[id] then
                hp = math.max(0, math.min(hp, max))
                list[#list + 1] = {id = id, wow_guid = guids[id], position = toBridge(p), hp = hp, max_hp = max}
                reported[id] = hp
            end
        end
        add("hero", Hero.Uuid())
        for _, a in ipairs(enemies) do add(a.id, actors[a.id]) end
        return list
    end
    -- BG3's own camera for hero actions: its cursor picking ignores an imposed rotation.
    local holdSince = 0
    -- Following pauses for the action (the plugin's CamHold alone left Fire Bolt clicks
    -- without a cast in s4 to s6) and resumes with the same basis afterwards.
    local function hold(on)
        if not following or holding == on then return end
        local ok = pcall(N.Call, N.Op.Follow, 0, on and 0 or 1, on and 0 or 1, 1)
        holding = ok and on
        if holding then holdSince = ticks end
    end
    -- Frames for BG3's own camera to come back before projecting a point under it.
    local function cameraSettling() return following and (not holding or ticks - holdSince < 45) end
    -- With following paused, BG3's own camera may not show the point (s10: the wolf off the
    -- safe screen area). Pose the camera 12 m back from the point along BG3's own forward:
    -- the rotation stays BG3's, so the game's picking (which uses the controller's own
    -- rotation) agrees with the projection. Following, when resumed, replaces the pose.
    local function aimAt(point)
        if not following then return true end
        local ok, err = pcall(function()
            local v = N.Must(N.Op.CamStats).F
            local fwd = {v[3], v[7], v[11]}
            local eye = {point[1] - fwd[1] * 12, point[2] - fwd[2] * 12, point[3] - fwd[3] * 12}
            local view = ArenaMath.LookAt(v, eye, {eye[1] + fwd[1], eye[2] + fwd[2], eye[3] + fwd[3]}, point)
            N.SaveView(view)
            N.Must(N.Op.CamPose, 1)
        end)
        holdSince = ticks
        return ok, err
    end
    -- The server's view of the host's turn (ActionMonitor snapshot). Void fights poll this
    -- instead of the UI view model: reading Noesis view models while BG3 rebuilds them at a
    -- turn change crashed the game once.
    local function serverTurn()
        local r = monitor()
        local t = r and r.State and r.State.Turn
        return type(t) == "table" and t.Active == true
    end
    local function reject(action, reason, detail)
        job = nil
        later(function(ad) return ad:rejected(action, reason, string.sub(detail or "", 1, 256)) end)
        return true
    end
    local function mouse(kind, button, down, x, y) Ext._Internal.InjectMouse(kind, button, down, x, y) end
    -- Button releases run a few frames after the press, even after an abort.
    local function click(button, x, y)
        mouse(1, button, true, x, y)
        releases[#releases + 1] = {ticks + 3, function() mouse(1, button, false, x, y) end}
    end
    local function cancelPreview()
        local ok, casting = pcall(function() return selected().PlayerCharacterProperties.IsCasting end)
        if ok and casting == true then
            local w, h = Probe.ScreenSize(hotbar())
            mouse(0, 0, false, w // 2, h // 2)
            click(3, w // 2, h // 2)
        end
    end

    local function beginVoid(m)
        local hero, foes = nil, {}
        loadSpecs()
        for _, a in ipairs(m.actors) do
            guids[a.id] = a.wow_guid
            if a.id == "hero" then hero = a
            elseif type(void.Templates[a.id]) ~= "string" and not (specs[a.id] and type(specs[a.id].Template) == "string") then
                return nil, "actor " .. a.id .. " has no stand-in template"
            else foes[#foes + 1] = a end
        end
        if not hero then return nil, "no hero actor" end
        if #foes < 1 or #foes > MAX_ENEMIES then return nil, "void fights take one to fifty enemies" end
        local b = m.basis
        if b.yaw ~= 0 or b.bridge_origin[1] ~= 0 or b.bridge_origin[2] ~= 0 or b.bridge_origin[3] ~= 0 then
            return nil, "void fights need a zero bridge origin and yaw"
        end
        actors = {hero = Hero.Uuid()}
        byUuid = {[Hero.Uuid()] = "hero"}
        origin, heroBridge = toWorld(hero.position), hero.position
        setup = coroutine.create(function(ad)
            inWorld = World.Running and World.Running() or false
            if not inWorld then
                local layer = tonumber(void.Layer) or -15.262
                arena({Action = "Stamp", Void = true, MinX = void.MinCorner[1], MinZ = void.MinCorner[2],
                    AnchorY = void.Centre[2], Layer = layer, Tolerance = 60, Seconds = 900})
                arena({Action = "Rays", Void = true, Mask = 2096, SkipContexts = 0, Seconds = 900})
                local at = toWorld(hero.position)
                standIn({Action = "Teleport", Target = Hero.Uuid(), Position = {at[1], at[2] + 1, at[3]}})
                waitFor(function()
                    local p = Probe.HostPosition()
                    return hdist(p, at) < 1.5 and math.abs(p[2] - at[2]) < 3
                end, 300, "teleport did not arrive")
            end
            if not inWorld then wait(30) end
            rest.hero = Probe.HostPosition()
            for _, a in ipairs(foes) do addStandIn(a, false) end
            wait(10)
            -- BG3's combat may already run (world mode: Tav attacked a mirror, or one spotted her).
            local st = inWorld and mirrorStatus()
            local already = st and st.Tav and st.Tav.InCombat == true
            local report, entered = monitor(), nil
            seen = lastSeq(report)
            if already then
                -- Its turns so far: every event since Tav last entered combat.
                for _, e in ipairs((report and report.Events) or {}) do
                    if e.Event == "EnteredCombat" and Hero.Is(tostring(e.Args[1])) then entered = e.Seq end
                end
                if entered then seen = entered end
            end
            for _, uuid in ipairs(made) do standIn({Action = "Join", Target = uuid, With = Hero.Uuid()}) end
            for uuid, how in pairs(adopted) do
                if how == "joins" then standIn({Action = "Join", Target = uuid, With = Hero.Uuid()}) end
            end
            if not inWorld and void.Follow ~= false and Ext.IO.LoadFile("wow-bg3/link/camera.bin") then
                local w = b.wow_origin
                arena({Action = "Follow", Wow = {w[1], w[2], w[3]}, Bg3 = void.Centre, Mirror = true, Fov = true,
                    Seconds = 900})
                following = true
            end
            -- Combat has started once the engine reports a turn for a mapped combatant.
            if not already then waitFor(function()
                if ticks % 15 ~= 0 then return false end
                for _, e in ipairs((monitor() or {}).Events or {}) do
                    if e.Seq > seen and e.Event == "TurnStarted" then
                        for uuid in pairs(byUuid) do
                            if tostring(e.Args[1]):find(uuid, 1, true) then return true end
                        end
                    end
                end
                return false
            end, 600, "combat did not start") end
            -- The new combat's turns arrive as TurnStarted events (seen was taken before Join).
            mode, lastTurn = "combat", true
            if not hostSelected() then selectCharacter(Hero.Uuid()) end
            if not ad:roster(0, rosterActors()) then return end
            return ad:ready()
        end)
        started = true
        return true, "deferred"
    end

    -- Teardown: stand-ins removed, host back home, grid and scenes restored (each step tried).
    local function newTeardown(done)
        return coroutine.create(function(ad)
            hold(false)
            local problems = {}
            local function try(fn, ...)
                local ok, err = pcall(fn, ...)
                if not ok then problems[#problems + 1] = tostring(err) end
            end
            for _, uuid in ipairs(made) do try(standIn, {Action = "Remove", Target = uuid}) end
            made = {}
            if not inWorld then
                try(standIn, {Action = "Return", Target = Hero.Uuid()})
                wait(30)
                try(arena, {Action = "Restore"})
            end
            following, started, setup, torn = false, false, nil, true
            if done then
                if #problems > 0 then return ad:fail("cleanup: " .. table.concat(problems, "; ")) end
                return ad:cleaned()
            end
        end)
    end

    function B.begin(m)
        if void then return beginVoid(m) end
        local hero
        for _, a in ipairs(m.actors) do
            if a.id == "hero" then hero = a
            elseif not actors[a.id] then return nil, "actor " .. a.id .. " has no BG3 character mapping" end
        end
        if not hero then return nil, "no hero actor" end
        for id, uuid in pairs(actors) do
            if not Ext.Entity.Get(uuid) then return nil, "mapped character for " .. id .. " unavailable" end
        end
        local c = hostVM()
        if c.InTurnBasedMode ~= true then return nil, "host is not in turn-based mode" end
        -- Force Turn-Based has no TurnStarted events, so it uses the view model's turn flag;
        -- real combat uses the engine's TurnStarted events for every mapped combatant.
        mode = c.CharacterMode == "InFTB" and "ftb" or "combat"
        origin, heroBridge = Probe.HostPosition(), {hero.position[1], hero.position[2], hero.position[3]}
        actors.hero = Hero.Uuid()
        for id, uuid in pairs(actors) do byUuid[uuid] = id end
        seen = lastSeq(monitor())
        if not hostSelected() then selectCharacter(Hero.Uuid()) end
        started = true
        return true
    end

    -- World fights: more enemies join the running encounter. Spawns run one Join at a time
    -- beside the turns (poll); Roster answers each with joins counted, and an actor missing from
    -- it (spawn refused, stand-in limit) did not join.
    function B.join(m)
        if not void then return nil, "joins need a void fight" end
        for _, a in ipairs(m.actors) do guids[a.id] = a.wow_guid end
        joinQueue[#joinQueue + 1] = m.actors
        return true
    end

    function B.move(m)
        if m.actor ~= "hero" then return nil, "only the host moves" end
        job = {kind = "move", m = m, stage = "aim", at = ticks}
        return true
    end

    function B.cast(m)
        if m.actor ~= "hero" then return nil, "only the host casts" end
        if not SPELLS[m.spell] then return nil, "spell not allowlisted for the live test" end
        if not actors[m.target] then return nil, "unknown target" end
        job = {kind = "cast", m = m, stage = "start", at = ticks}
        return true
    end

    function B.end_turn()
        if void then
            -- The view model's turn flags lag the engine's turn; keep trying for 150 ticks.
            job = {kind = "end", at = ticks}
            return true
        end
        if not hostTurn() then return nil, "not the host's turn" end
        if not hostSelected() then return nil, "host is not selected" end
        local ok, why = endTurnNow()
        if not ok then return nil, why end
        -- Other characters this player controls pass their turns so the round can continue.
        passing, passed = true, {}
        return true
    end

    function B.cleanup()
        job, passing = nil, false
        cancelPreview()
        if void then
            if setup then return nil, "setup still running" end
            teardown = newTeardown(true)
            return true
        end
        if mode == "ftb" and hostVM().InTurnBasedMode == true then
            local ok, button = pcall(Probe.FindUnique, "HotBar", "ExitFTBButton")
            if not (ok and button.IsEnabled ~= false and button.IsVisible == true and button.Command
                and button.Command:CanExecute(button.CommandParameter)) then
                return nil, "cannot leave turn-based mode through the HotBar"
            end
            button.Command:Execute(button.CommandParameter)
        end
        -- Force Turn-Based: Cleaned once the engine reports the host out of turn-based mode.
        -- Real combat: BG3's fight continues; this backend stops driving it. Stand-in
        -- removal belongs to the stand-in harness.
        local due = ticks + 150
        local function wait(ad)
            local tb = mode == "ftb" and hostVM().InTurnBasedMode == true
            if tb and ticks < due then later(wait); return true end
            started = false
            if tb then return ad:fail("host still in turn-based mode after cleanup") end
            return ad:cleaned()
        end
        later(wait)
        return true
    end

    function B.abort()
        local engaged = setup ~= nil or started
        job, events, started, passing, setup = nil, {}, false, false, nil
        pcall(cancelPreview)
        -- A void fight is always undone, also after an abort (drain runs it).
        if void and not teardown and not torn and engaged then teardown = newTeardown(false) end
    end
    local function stepTeardown(ad)
        if not teardown then return end
        local ok, err = coroutine.resume(teardown, ad)
        if not ok then
            teardown = nil
            Ext.Log.Print("WOW-BG3 link void teardown error: " .. tostring(err))
        elseif coroutine.status(teardown) == "dead" then
            teardown = nil
        end
    end
    -- Releases must still fire after an abort; the driver keeps polling until they drain.
    function B.drain()
        ticks = ticks + 1
        while releases[1] and releases[1][1] <= ticks do pcall(table.remove(releases, 1)[2]) end
        if teardown then pcall(stepTeardown, {fail = function() end, cleaned = function() end}) end
        return #releases == 0 and not teardown
    end

    -- Returns true once the disposable character is selected for an action.
    local function ensureHost()
        if hostSelected() then return true end
        if not job.selecting then selectCharacter(Hero.Uuid()); job.selecting = ticks end
        if ticks - job.selecting > 60 then error("could not select the disposable character") end
        return false
    end

    local function stepMove(ad)
        local m = job.m
        if job.stage == "aim" then
            if not hostTurn() then
                return reject(m.action, "not_your_turn", "view model: IsCurrentTurn=" .. tostring(hostVM().IsCurrentTurn))
            end
            if cameraSettling() then return true end
            if void and following and not job.aimed then
                job.aimed = true
                aimAt(toWorld(m.target))
                return true
            end
            if cameraSettling() then return true end
            if not ensureHost() then return true end
            if selected().PlayerCharacterProperties.IsCasting == true then cancelPreview(); return true end
            local world = toWorld(m.target)
            local w, h = Probe.ScreenSize(hotbar())
            local ok, s = pcall(Probe.Project, world, w, h)
            if not ok then return reject(m.action, "cannot_aim", tostring(s)) end
            local x, y = math.floor(s.X + 0.5), math.floor(s.Y + 0.5)
            local safe = Probe.Safe(w, h)
            if x < safe.x0 or x > safe.x1 or y < safe.y0 or y > safe.y1 then
                return reject(m.action, "cannot_aim", "point off the safe screen area")
            end
            mouse(0, 0, false, x, y)
            job.x, job.y, job.world, job.stage, job.at = x, y, world, "check", ticks
        elseif job.stage == "check" and ticks - job.at >= 10 then
            local pick = Probe.PickInfo()
            if pick.Character1 or pick.Character2 or pick.Item1 or pick.Item2 then
                return reject(m.action, "cannot_aim", "picking found an object, not ground")
            end
            local p = pick.Position
            if type(p) ~= "table" or hdist(p, job.world) > AIM_TOLERANCE then
                return reject(m.action, "cannot_aim", "picked ground too far from the target")
            end
            click(1, job.x, job.y)
            local start = Probe.HostPosition()
            job.stage, job.at, job.samples, job.lastMove = "watch", ticks, {start}, nil
        elseif job.stage == "watch" then
            if (ticks - job.at) % 5 ~= 0 then return true end
            local p = Probe.HostPosition()
            local last = job.samples[#job.samples]
            if math.abs(p[1] - last[1]) + math.abs(p[2] - last[2]) + math.abs(p[3] - last[3]) > 0.05 then
                if #job.samples < 255 then job.samples[#job.samples + 1] = p else job.samples[255] = p end
                job.lastMove = ticks
            end
            if job.lastMove and ticks - job.lastMove > SETTLE then
                local points = {}
                for i, q in ipairs(job.samples) do points[i] = toBridge(q) end
                local action = m.action
                job = nil
                return ad:path(action, points) and ad:action_done(action)
            end
            if not job.lastMove and ticks - job.at > NO_MOVE then
                return reject(m.action, "no_effect", "click issued; the host did not move")
            end
        end
        return true
    end

    local function stepCast(ad)
        local m = job.m
        local prototype, targeting = SPELLS[m.spell][1], SPELLS[m.spell][2]
        if job.stage == "start" then
            if not hostTurn() then
                return reject(m.action, "not_your_turn", "view model: IsCurrentTurn=" .. tostring(hostVM().IsCurrentTurn))
            end
            if not ensureHost() then return true end
            local c = selected()
            local slot = findSlot(c, prototype)
            if not slot then return reject(m.action, "unavailable", "no hotbar slot for " .. prototype) end
            -- The hotbar's CanUse can lag the turn start by a few frames; give it 45 ticks.
            if slot.CanUse ~= true and ticks - job.at < 45 then return true end
            if slot.CanUse ~= true then return reject(m.action, "unavailable", prototype .. " CanUse=false") end
            local dc = hotbar()
            if not dc.UseSlotCommand:CanExecute(slot) then
                return reject(m.action, "unavailable", prototype .. " UseSlotCommand cannot execute")
            end
            job.baseline = lastSeq(monitor())
            dc.UseSlotCommand:Execute(slot)
            job.stage, job.at = "preview", ticks
        elseif job.stage == "preview" and ticks - job.at >= 10 and not cameraSettling() then
            if void and following and targeting == "target" and not job.aimed then
                job.aimed = true
                local ok, p = pcall(function() return Ext.Entity.Get(actors[m.target]).Transform.Transform.Translate end)
                if ok then aimAt({p[1], p[2] + LIFT, p[3]}) end
                return true
            end
            if selected().PlayerCharacterProperties.IsCasting ~= true then
                return reject(m.action, "no_effect", "no targeting preview opened")
            end
            local w, h = Probe.ScreenSize(hotbar())
            local x, y = w // 2, h // 2
            if targeting == "target" then
                local ok, s = pcall(Probe.Project, (function()
                    local p = Ext.Entity.Get(actors[m.target]).Transform.Transform.Translate
                    return {p[1], p[2] + LIFT, p[3]}
                end)(), w, h)
                if not ok then cancelPreview(); return reject(m.action, "cannot_aim", tostring(s)) end
                x, y = math.floor(s.X + 0.5), math.floor(s.Y + 0.5)
                local safe = Probe.Safe(w, h)
                if x < safe.x0 or x > safe.x1 or y < safe.y0 or y > safe.y1 then
                    cancelPreview()
                    return reject(m.action, "cannot_aim", "target off the safe screen area")
                end
            end
            mouse(0, 0, false, x, y)
            job.x, job.y, job.stage, job.at = x, y, "confirm", ticks
        elseif job.stage == "confirm" and ticks - job.at >= 10 then
            if targeting == "target" then
                local pick = Probe.PickInfo()
                if (pick.Character1 or pick.Character2) ~= actors[m.target] then
                    cancelPreview()
                    return reject(m.action, "cannot_aim", "picking does not show the target")
                end
            end
            click(1, job.x, job.y)
            job.stage, job.at = "await", ticks
        elseif job.stage == "await" then
            if (ticks - job.at) % 15 ~= 0 then return true end
            local report = monitor()
            for _, e in ipairs((report and report.Events) or {}) do
                if e.Seq > job.baseline and Hero.Is(tostring(e.Args[1])) then
                    -- In a void fight the click can end up as another action (s9: the Fire Bolt
                    -- preview closed and the click made a main-hand attack); report what the
                    -- engine actually did.
                    local same = e.Args[2] == prototype
                    if e.Event == "CastSpellFailed" and same then
                        return reject(m.action, "no_effect", "engine reported CastSpellFailed")
                    elseif e.Event == "CastedSpell" and (same or void) then
                        local hp, max = health(actors[m.target])
                        if not hp then return reject(m.action, "no_effect", "cast completed; target health unreadable") end
                        local action = m.action
                        job = nil
                        local used = same and m.spell or protocolId(e.Args[2])
                        reported[m.target] = math.max(hp, 0)
                        return ad:effect(action, m.target, math.max(hp, 0), max, used) and ad:action_done(action)
                    end
                end
            end
            if ticks - job.at > CAST_WAIT then
                local okc, casting = pcall(function() return selected().PlayerCharacterProperties.IsCasting end)
                local pick = Probe.PickInfo()
                cancelPreview()
                return reject(m.action, "no_effect", string.format(
                    "no cast event from the engine (preview open %s, hold %s, picked %s)",
                    tostring(okc and casting), tostring(holding), tostring(pick.Character1 or pick.Character2)))
            end
        end
        return true
    end

    local function idOf(arg)
        for uuid, id in pairs(byUuid) do
            if tostring(arg):find(uuid, 1, true) then return id, uuid end
        end
    end
    -- The BG3-run turn being watched ends: report its path, spell, target and damage.
    local function flushNpc(ad)
        local t = npc
        npc = nil
        if not t then return true end
        local uuid = actors[t.id]
        local here = positionOf(uuid)
        if here then t.samples[#t.samples + 1] = here end
        rest[t.id] = here or rest[t.id]
        local points, last = {}, nil
        for _, q in ipairs(t.samples) do
            if not last or math.abs(q[1] - last[1]) + math.abs(q[2] - last[2]) + math.abs(q[3] - last[3]) > 0.05 then
                if #points < 256 then points[#points + 1] = toBridge(q) end
                last = q
            end
        end
        if #points == 1 then points = {} end
        local action = t.id .. "-" .. ad.turn
        local spell = protocolId(t.spell or "none")
        if not ad:acted(action, spell, t.target or "none", points) then return nil end
        -- A miss still reports the aimed target (HP unchanged), so WoW plays the attack at once.
        if t.target and spell ~= "none" then t.hit[t.target] = true end
        for id in pairs(t.hit) do
            local hp, max = health(actors[id])
            if hp then
                reported[id] = math.max(hp, 0)
                if not ad:effect(action, id, math.max(hp, 0), max, spell) then return nil end
            end
        end
        return true
    end
    local function checkFinished(ad)
        if finished or ad.phase ~= "active" or ad.pending or npc then return end
        activeSince = activeSince or ticks
        local hp = health(Hero.Uuid())
        local over, outcome
        if inWorld then
            -- World mode: the fight ends when BG3's does,
            -- never earlier. Victory: every enemy down. Defeat: no party member stands (BG3's game
            -- over; a dead or downed hero alone is not it while a companion fights on, and the WoW
            -- side keeps the encounter while a party bot lives). Otherwise, once no living enemy has
            -- been in BG3's combat for LEFT_GRACE ticks (the sides lost each other), it ends as
            -- fled: WoW sends those mobs home while their stand-ins, out of BG3's combat, follow them.
            -- Ending sooner left stand-ins fighting in BG3 while their WoW mobs walked home, and
            -- stand-ins still in BG3's combat never followed their mobs again.
            local alive, fighting = 0, 0
            for _, a in ipairs(enemies) do
                local ehp = health(actors[a.id])
                if ehp and ehp > 0 then
                    alive = alive + 1
                    local st = combatState[a.id]
                    if not st or st.In or ticks - st.At < LEFT_GRACE then fighting = fighting + 1 end
                end
            end
            local stands = partyStands(monitor())
            if alive == 0 then
                over, outcome = true, "victory"
            elseif stands == false then
                over, outcome = true, "defeat"
            elseif fighting == 0 and ticks - activeSince >= LEFT_GRACE then
                over, outcome = true, "fled"
            end
        else
            over = hp and hp <= 0
            if not over then
                over = true
                for _, a in ipairs(enemies) do
                    local ehp = health(actors[a.id])
                    if ehp and ehp > 0 then over = false end
                end
            end
            outcome = (hp and hp <= 0) and "defeat" or "victory"
        end
        if not over then return end
        -- The last blow may have come from a direct BG3 action (world mode): WoW hears every final
        -- HP (the kill gives the server's credit) before the fight is over.
        for id, uuid in pairs(actors) do
            local h = health(uuid)
            if h and reported[id] ~= nil and math.max(h, 0) ~= reported[id] then
                return ad:roster(joinsDone, rosterActors())
            end
        end
        finished = true
        Ext.Utils.Print("WOW-BG3 fight over: " .. outcome .. (inWorld and " (BG3's combat)" or ""))
        return ad:finished(outcome)
    end
    -- Engine turn events for mapped combatants (real combat only). In void fights the turns
    -- of stand-ins are watched and reported as Acted; deaths finish the encounter.
    local function turnEvents(ad)
        if npc and ticks % 5 == 0 then
            local p = positionOf(actors[npc.id])
            local last = npc.samples[#npc.samples]
            if p and (not last or math.abs(p[1] - last[1]) + math.abs(p[3] - last[3]) > 0.05) and #npc.samples < 400 then
                npc.samples[#npc.samples + 1] = p
            end
        end
        if ticks % 10 ~= 0 or job then return end
        local report = monitor()
        for _, e in ipairs((report and report.Events) or {}) do
            if e.Seq > seen then
                seen = e.Seq
                -- World mode: which enemies BG3 still has in a combat (checkFinished).
                if inWorld and (e.Event == "EnteredCombat" or e.Event == "LeftCombat") then
                    local cid = idOf(e.Args[1])
                    if cid and cid ~= "hero" then combatState[cid] = {In = e.Event == "EnteredCombat", At = ticks} end
                end
                if e.Event == "TurnStarted" then
                    local id = idOf(e.Args[1])
                    if id then pendingTurns[#pendingTurns + 1] = id end
                elseif void and npc then
                    local caster = idOf(e.Args[1])
                    if e.Event == "TurnEnded" and caster == npc.id then
                        npc.ended = true
                    elseif e.Event == "UsingSpellOnTarget" and caster == npc.id then
                        npc.spell, npc.target = e.Args[3], idOf(e.Args[2])
                    elseif (e.Event == "UsingSpell" or e.Event == "UsingSpellAtPosition") and caster == npc.id then
                        npc.spell = npc.spell or e.Args[2]
                    elseif e.Event == "AttackedBy" and (idOf(e.Args[3]) == npc.id or idOf(e.Args[2]) == npc.id) then
                        local target = idOf(e.Args[1])
                        if target then npc.hit[target] = true; npc.target = npc.target or target end
                    end
                end
            end
        end
        if npc and (npc.ended or pendingTurns[1]) and not ad.pending then
            if not flushNpc(ad) then return end
        end
        if void then checkFinished(ad) end
        if finished then return end
        -- Fallback: the view model shows the host's turn but no TurnStarted came (90 ticks).
        if void and not npc and not ad.pending and ad.turn_actor ~= "hero" and not pendingTurns[1] and serverTurn() then
            heroWait = heroWait + 10
            if heroWait >= 90 then pendingTurns[1], heroWait = "hero", 0 end
        else
            heroWait = 0
        end
        while pendingTurns[1] and not ad.pending and not npc do
            -- TurnStarted for the host comes before its view model and hotbar are rebuilt for
            -- the turn; touching them meanwhile crashed BG3 twice (Noesis, s4 and s7). Announce
            -- the turn once the server has shown it for 2.5 s.
            if pendingTurns[1] == "hero" and void then
                local now = Ext.Utils.MonotonicTime()
                if not serverTurn() then heroShown = 0; return end
                if heroShown == 0 then heroShown = now end
                if now - heroShown < 2500 then return end
                heroShown = 0
            end
            local id = table.remove(pendingTurns, 1)
            if id == "hero" then passing = false end
            if not ad:turn_started(id) then return end
            if void and id ~= "hero" then
                npc = {id = id, samples = {rest[id] or positionOf(actors[id])}, hit = {}}
            end
        end
    end

    -- After the host ends its turn, other characters this player controls pass theirs
    -- (each at most once per host turn), through the same EndTurn command.
    local function passOthers()
        if not passing or ticks % 15 ~= 0 then return end
        -- World mode: the companions are BG3 party members and play their own turns
        -- with BG3's controls, as in BG3.
        if World.Running and World.Running() then return end
        local list = player().AssignedCharacters
        for i = 1, 16 do
            local c = list[i]
            if c == nil then break end
            local uuid = c.EntityUUID
            if uuid ~= Hero.Uuid() and c.IsCurrentTurn == true and not passed[uuid] then
                if selected().EntityUUID ~= uuid then selectCharacter(uuid); return end
                passed[uuid] = true
                endTurnNow()
                return
            end
        end
    end

    function B.poll(ad)
        ticks = ticks + 1
        if not torn then pcall(publishStandIns) end
        while releases[1] and releases[1][1] <= ticks do pcall(table.remove(releases, 1)[2]) end
        local now = events
        events = {}
        for _, e in ipairs(now) do
            local ok, err = pcall(e, ad)
            if not ok then pcall(cancelPreview); return ad:fail("live backend error: " .. tostring(err)) end
        end
        if setup then
            local ok, err = coroutine.resume(setup, ad)
            if not ok then
                setup = nil
                return ad:fail("void setup: " .. tostring(err))
            end
            if coroutine.status(setup) == "dead" then setup = nil end
            return
        end
        if teardown then return stepTeardown(ad) end
        if job then
            hold(true)
        else
            hold(false)
        end
        if not started or ad.phase ~= "active" then return end
        local ok, err = pcall(function()
            if mode == "ftb" then
                -- One Turn per start of the host's turn (false -> true) in the view model.
                local current = hostTurn()
                if current and not lastTurn then
                    if not ad.pending then ad:turn_started("hero"); lastTurn = true end
                elseif not current then
                    lastTurn = false
                end
            else
                -- The host's turn may already be running when the encounter begins.
                if not lastTurn then
                    lastTurn = true
                    if hostTurn() then pendingTurns[#pendingTurns + 1] = "hero" end
                end
                turnEvents(ad)
                passOthers()
                if void then
                    -- Joins, one at a time, beside the turns.
                    if not joining and joinQueue[1] then
                        local list = table.remove(joinQueue, 1)
                        joining = coroutine.create(function(a)
                            loadSpecs()
                            for _, actor in ipairs(list) do
                                local okj, errj = pcall(addStandIn, actor, true)
                                if not okj then
                                    scaled[actor.id] = scaled[actor.id] or {}
                                    scaled[actor.id].JoinFailed = tostring(errj)
                                end
                            end
                            joinsDone = joinsDone + 1
                            return a:roster(joinsDone, rosterActors())
                        end)
                    end
                    if joining then
                        local okc, errc = coroutine.resume(joining, ad)
                        if not okc then joining = nil; error("join: " .. tostring(errc), 0) end
                        if coroutine.status(joining) == "dead" then joining = nil end
                    elseif ticks % 30 == 0 and not job and not npc and not ad.pending and ad.phase == "active" then
                        -- HP that changed without an Effect (a direct BG3 action): Roster.
                        for id, uuid in pairs(actors) do
                            local hp = health(uuid)
                            if hp and reported[id] ~= nil and math.max(hp, 0) ~= reported[id] then
                                ad:roster(joinsDone, rosterActors())
                                break
                            end
                        end
                    end
                end
            end
            if job then
                if job.kind == "move" then stepMove(ad)
                elseif job.kind == "end" then
                    -- The fight may already be over (the last enemy died): nothing to end.
                    local alive = false
                    for _, a in ipairs(enemies) do
                        local ehp = health(actors[a.id])
                        if ehp and ehp > 0 then alive = true end
                    end
                    if not alive then job = nil
                    elseif ticks % 10 == 0 then
                        if not hostSelected() then selectCharacter(Hero.Uuid())
                        else
                            local ok = endTurnNow()
                            -- Only the host fights in the void: nobody else's turn to pass.
                            if ok then job = nil
                            elseif ticks - job.at > 150 then job = nil; ad:fail("BG3 did not let the turn end") end
                        end
                    end
                else stepCast(ad) end
            end
        end)
        if not ok then
            job = nil
            pcall(cancelPreview)
            ad:fail("live backend error: " .. tostring(err))
        end
    end
    function B.scaled() return scaled end
    return B
end

-- Whether the bridge runs a fight in this session (Cleanup.lua's mid-fight save rule reads it; a loaded
-- session starts with none, whatever BG3's combat the save carried).
M.FightLive = false

function M.Start(runId)
    local ticks, active, seenSession, status, draining = 0, nil, {}, {}, nil
    local function writeStatus(extra)
        status.RunId, status.Tick = runId, ticks
        for k, v in pairs(extra or {}) do status[k] = v end
        Ext.IO.SaveFile(DIR .. "status.json", Ext.Json.Stringify(status))
    end
    local function stop(why)
        if active then pcall(active.backend.abort, why); draining = active.backend end
        active = nil
        M.FightLive = false
        writeStatus({Active = false, Stopped = tostring(why)})
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if draining and draining.drain() then draining = nil end
        if Ext.Utils.GetGameState() ~= "Running" then
            -- A save (Save, Paused) keeps the session and the fight: hold the link, never stop it.
            if active and not Ext.Require("Session.lua").Held() then stop("game left Running") end
            return
        end
        if not active then
            if ticks % 30 ~= 0 then return end
            local raw = Ext.IO.LoadFile(DIR .. "start.json")
            if not raw or #raw > 4096 then return end
            local parsed, s = pcall(Ext.Json.Parse, raw)
            if not parsed or type(s) ~= "table" or s.RunId ~= runId or type(s.Session) ~= "string"
                or not s.Session:find("^%x+$") or #s.Session ~= 32 or seenSession[s.Session] then return end
            seenSession[s.Session] = true
            local actors = {}
            for id, uuid in pairs(type(s.Actors) == "table" and s.Actors or {}) do
                if Link.id_ok(id) and type(uuid) == "string" and #uuid == 36 and id ~= "hero" then actors[id] = uuid end
            end
            local t0 = Ext.Utils.MonotonicTime()
            local endpoint, err = Link.endpoint(s.Session, "bg3", 0, 10000)
            if not endpoint then writeStatus({Active = false, Stopped = tostring(err)}); return end
            local void
            if type(s.Void) == "table" then
                local v = s.Void
                local function vec(t, n)
                    if type(t) ~= "table" or #t ~= n then return false end
                    for i = 1, n do if type(t[i]) ~= "number" or t[i] ~= t[i] then return false end end
                    return true
                end
                if not (vec(v.Centre, 3) and vec(v.MinCorner, 2) and type(v.Templates) == "table") then
                    writeStatus({Active = false, Stopped = "invalid Void setup"}); return
                end
                void = {Centre = v.Centre, MinCorner = v.MinCorner, Layer = v.Layer, Templates = v.Templates,
                    Faction = type(v.Faction) == "string" and v.Faction or nil, Follow = v.Follow}
            end
            local backend = newBackend(actors, void, runId, s.Session)
            active = {endpoint = endpoint, backend = backend, adapter = Adapter.new(Link, backend), t0 = t0, delivered = 0}
            M.FightLive = true
            status = {Session = s.Session, Messages = {}}
            writeStatus({Active = true})
            return
        end
        local a = active
        local now = math.floor(Ext.Utils.MonotonicTime() - a.t0)
        local ok, err = a.endpoint:tick(now)
        if not ok then return stop(err) end
        local raw = Ext.IO.LoadFile(DIR .. "wow.json")
        if raw and Link.decode(raw) then
            local got, why = a.endpoint:receive(raw, now, function(m)
                local accepted, reason = a.adapter:apply(m)
                if #status.Messages < 64 then
                    status.Messages[#status.Messages + 1] = {In = m.type, Action = m.action, Accepted = accepted == true,
                        Why = reason}
                end
                return accepted, reason
            end)
            if got == nil then return stop(why) end
            if got then a.delivered = a.delivered + 1 end
        end
        a.backend.poll(a.adapter)
        if a.backend.scaled and ticks % 30 == 0 then status.Scaled = a.backend.scaled() end
        if not a.endpoint:busy() and not a.adapter:idle() then
            local m = a.adapter:next()
            if #status.Messages < 64 then
                status.Messages[#status.Messages + 1] = {Out = m.type, Action = m.action, Reason = m.reason, Detail = m.detail}
            end
            local offered, oerr = a.endpoint:offer(m)
            if not offered then return stop(oerr) end
        end
        local bytes, ferr = a.endpoint:frame()
        if not bytes then return stop(ferr) end
        Ext.IO.SaveFile(DIR .. "bg3.json", bytes)
        if ticks % 30 == 0 then writeStatus({Active = true, Phase = a.adapter.phase, Delivered = a.delivered}) end
        if a.adapter.phase == "done" and a.adapter:idle() and not a.endpoint:busy() then
            a.doneAt = a.doneAt or now
            if now - a.doneAt > 1000 then stop("encounter done") end
        end
    end)
end
return M
