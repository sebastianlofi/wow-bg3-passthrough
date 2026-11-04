-- Server-side stand-in setup for the action tests on a disposable save (approved
-- design: Osiris spawn calls for stand-in setup only, never as proof of legal actions).
-- Driven by wow-bg3/stand-in-command.json (one command per sequence, never retried):
--   Spawn    {From = UUID | Template = ID, Position = {x,y,z}, Name, Percent, Guid}
--                                                temporary copy of an existing character's template,
--                                                or of a character root template (a BG3 wolf for
--                                                WoW's Young Wolf); Name: the WoW mob's name shown in
--                                                BG3 (Names.lua); Percent: its hit points
--                                                as a percentage of its max, set in the tick that
--                                                creates it (BG3 applies it when it fills Health, the
--                                                next tick; checked then: the WoW mob's
--                                                fraction, so BG3 never shows another value);
--                                                Guid: the WoW mob's, for a fight's stand-in of a mob
--                                                with no mirror (WOWBG3FightSpawned: Portraits.lua
--                                                gives it the mob's portrait and level)
--   Templates {Name}                             read-only: character root templates whose name
--                                                contains Name (at most 24)
--   Visible  {Target, Visible = true|false}      model visibility only (Osiris SetVisible);
--                                                also allowed on the disposable character
--   Faction  {Target, From | Faction}            copy another character's faction, or set a
--                                                faction id read earlier from Inspect (setup)
--   Join     {Target, With}                      enter the combat of With (setup); also a mirrored mob's
--                                                stand-in (Mirrors.lua), which a fight adopts
--   TempHostile {Target, With}                   a mirrored friendly NPC's stand-in the WoW fight holds
--                                                (WoW's rules pulled it in, its
--                                                creature group) turns on With the way BG3 turns
--                                                someone it attacked: Osiris SetRelationTemporaryHostile
--                                                (then Join as usual), never a faction change
--   Remove   {Target}                            delete a stand-in this module created
--   Clear    {Centre, Evict}                     world mode's Stop: delete every stand-in
--                                                (this module's and Mirrors.lua's) and every other
--                                                non-player character within 200 m of Centre (one
--                                                carried in a save made in world mode); Left = how
--                                                many still exist (the Stop waits for 0 before the
--                                                ground goes home: one left on a square marks the
--                                                square's home tiles taken and it never qualifies
--                                                again); Evict also teleports those 500 m away;
--                                                Near: what stands there (characters and items)
--   Inspect  {Target}                            read-only state of a created stand-in (also its
--                                                server Transform's rotation and where it looks:
--                                                Facing = its rotated -z (x, z))
--   Face     {Target, Dir = {x, z}}              harness (facing research): a created
--                                                stand-in looks along Dir (its -z, through the
--                                                Steering component, as Mirrors.lua turns them)
--   Teleport {Target, Position, Near, Snap}      Osiris TeleportToPosition of the disposable character
--                                                or a created stand-in to the void arena (approved
--                                                design, void step 4); the first teleport
--                                                records where it stood
--   Away     {}                                  world mode: every other player character (from
--                                                Osiris DB_Players) outside the hero's party goes off
--                                                stage for the session (SetOnStage 0), recorded; the
--                                                hero's companions come along (Party.lua Gather)
--   Back     {}                                  those characters back on stage
--   GameOver {Hold}                              world mode: Hold = true while world
--                                                mode runs, false at Stop: BG3's game over waits
--                                                (see the action)
--   Music    {Event}                             world mode: one of BG3's own music
--                                                events, for everyone (Osiris MusicPlayGeneral, as
--                                                BG3's story plays music); only Music_Off: BG3's
--                                                exploration music off, its combat music stays
--   Scale    {Target, DamageBonus}               world fights (setup of a created stand-in):
--                                                a BG3 DamageBonus boost (may be negative; Osiris
--                                                AddBoosts), so its hits do the WoW mob's damage scaled.
--                                                MaxHp: its new total max HP
--                                                (round(WoW max HP ^ 0.65)), an IncreaseMaxHP boost of
--                                                the difference to its max HP now; Spells:
--                                                extra abilities (Osiris AddSpell, SCALE_SPELLS only)
--   Health   {Target, Percent}                   a created stand-in's hit points as a percentage of its
--                                                maximum (Osiris SetHitpointsPercentage, setup)
--   Return   {Target}                            back to that recorded spot (before any cleanup); the
--                                                disposable character also gets back the hit points
--                                                it had at its first teleport (a void fight's damage
--                                                and DOWNED are undone: SetHitpointsPercentage,
--                                                RemoveStatus)
-- Errands (world mode), a file of their own so they never wait behind or
-- clobber a command above: wow-bg3/errand.json (WorldMode.lua, client, from benilla's
-- link/tavgo.bin) {RunId, Seq, Go, Position = {x,y,z} BG3, Stop}. Go: the disposable character runs
-- there (Osiris CharacterMoveToPosition), only out of combat and within 80 m; she is stopped
-- (Osiris PurgeOsirisQueue) when an errand ends with Stop, when she enters combat, or after 20 s.
-- BG3 ignores the player's own clicks while an Osiris move runs on her (harness), so
-- WorldMode.lua ends it with Stop at the press of any click, and this file is read every tick while
-- an errand runs (every third otherwise). Status in wow-bg3/errand-status.json.
-- Guards: expected region, at most eight live stand-ins, spawn positions within 30 m of the
-- disposable character, teleports within 3 km of its recorded home, and every target must
-- be the disposable character (Visible, Teleport, Return) or a stand-in this module created.
local M = {}
local Harness = Ext.Require("Harness.lua")
local Names = Ext.Require("Names.lua")
-- BG3's own healing of a hurt NPC out of combat, on the stand-ins made here: off, so one spawned at
-- a percentage keeps it (Mirrors.lua says where that healing comes from); true leaves BG3's alone.
M.SelfHeal = false
-- The music events world mode may play (Music action).
local MUSIC_EVENTS = {Music_Off = true}
-- BG3's normal fight themes (MUSIC.bnk's fight switch keys on one state, and these events set it), one drawn
-- for each BG3 combat in world mode. Left out: the fights' own, bosses and set pieces (Ketheric, who shares
-- Fight_Theme_1616_Dark_On, Myrkul, Gortash, Raphael, the Netherbrain, Moonrise, the Bhaalists, High Hall, the
-- hag, the Goblin den, the tutorial's end, cinematics). World mode's Music_Off arms it.
local FIGHT_THEMES = {
    "Fight_Theme_03_On", "Fight_Theme_03_Dark_On", "Fight_Theme_04_12_On", "Fight_Theme_06_II_On",
    "Fight_Theme_06_Dark_On", "Fight_Theme_1616_On", "Fight_Theme_916_On", "Fight_Theme_916_Dark_On",
    "Fight_Theme_916_Quartet_On", "Fight_Theme_916_Quartet_Horror_On", "Fight_Theme_98_On",
    "Fight_Theme_Percussive_On", "Fight_Theme_Percussive_02_On", "Fight_Theme_Uni_On", "Fight_Theme_WMPA_On",
    "Fight_Theme_WMPA_Dark_On", "Set_02_Fight_On", "Set_06_Fight_On",
}
local fightThemesOn, lastFightTheme = false, nil
local function drawFightTheme()
    local i = math.random(#FIGHT_THEMES)
    if FIGHT_THEMES[i] == lastFightTheme then i = i % #FIGHT_THEMES + 1 end
    lastFightTheme = FIGHT_THEMES[i]
    local ok, err = pcall(Osi.MusicPlayGeneral, lastFightTheme)
    Ext.Utils.Print("WOW-BG3 fight music: " .. lastFightTheme .. (ok and "" or (" failed: " .. tostring(err))))
end
pcall(Ext.Osiris.RegisterListener, "CombatStarted", 1, "before", function()
    if fightThemesOn then drawFightTheme() end
end)
-- Scale's Spells: the ones a stand-in may get (spiders).
local SCALE_SPELLS = {Target_Web_Spider = true, Target_EtherealJaunt = true}
local Hero = Ext.Require("Hero.lua") -- the hero: the character the player controls
local REGION = "TUT_Avernus_C"
-- VOID_RADIUS: world mode puts Tav in the void well away from the tutorial (about 1.2 km).
local MAX, RADIUS, VOID_RADIUS = 60, 30.0, 3000.0
-- A home in the void: a save made in world mode loads with the hero in the void, so the
-- spot recorded at her first teleport was the void and Stop's Return left her (and the party
-- gathered on her) standing on the squares as they went home; their home tiles then stayed taken and
-- world mode could not start again (tele.sh). A spot farther than LEVEL_REACH from the tutorial's
-- floor is recorded as LEVEL_SPOT instead: Tav's own spot on the nautiloid (harness).
local LEVEL_SPOT, LEVEL_REACH = Ext.Require("Hero.lua").SPOT, 600.0 -- host level spot (Hero.lua, host.json)
local function inVoid(p)
    local dx, dz = p[1] - LEVEL_SPOT[1], p[3] - LEVEL_SPOT[3]
    return math.sqrt(dx * dx + dz * dz) > LEVEL_REACH
end

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
local function uuidOf(guidstring)
    return type(guidstring) == "string" and guidstring:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil
end

function M.Start(runId)
    local ticks, sequence, created, home, hpHome, away = 0, 0, {}, {}, {}, {}
    -- A load starts this module again with the same RunId: the command already in the file is the
    -- previous session's, done (WorldMode re-ran the old Start after a
    -- load, and the bridge's own Start then failed "the pool is in use"). Taken as done here, at start.
    pcall(function()
        local raw = Ext.IO.LoadFile("wow-bg3/stand-in-command.json")
        local ok, c = pcall(Ext.Json.Parse, raw or "")
        if ok and type(c) == "table" and c.RunId == runId and math.type(c.Sequence) == "integer" then sequence = c.Sequence end
    end)
    local gameOverHeld = {} -- defeat causes this module added to DB_GameOver_DefeatCausesNotGameOver
    -- Mirrored mobs' stand-ins (Mirrors.lua, world fights) may join a fight too.
    local function live(uuid)
        assert(type(uuid) == "string" and (created[uuid] or (WOWBG3MirrorOwned and WOWBG3MirrorOwned[uuid])),
            "Not a stand-in created by this module or a mirror")
        return uuid
    end
    -- Spawn's Percent: set in the creating tick (Health reads -1/-1 there; BG3 fills
    -- it the next tick and applies the call), checked in the first tick Health has a max, set again
    -- if BG3 shows another value (it rounds a percentage up).
    local hpDue = {}
    local function healthDue()
        for uuid, d in pairs(hpDue) do
            local h = q(function() local c = Ext.Entity.Get(uuid).Health; return {c.Hp, c.MaxHp} end)
            if h and type(h[2]) == "number" and h[2] > 0 then
                hpDue[uuid] = nil
                if h[1] ~= math.max(1, math.min(h[2], math.ceil(h[2] * d.Percent / 100 - 1e-6))) then
                    pcall(Osi.SetHitpointsPercentage, uuid, d.Percent * 1.0)
                end
            elseif ticks - d.Tick > 90 then
                hpDue[uuid] = nil
            end
        end
    end
    local function count()
        local n = 0
        for uuid in pairs(created) do if Ext.Entity.Get(uuid) then n = n + 1 end end
        return n
    end
    local function inspect(uuid)
        local e = Ext.Entity.Get(uuid)
        local out = {Uuid = uuid, Exists = e ~= nil}
        if not e then return out end
        out.Position = q(function() return vec(e.Transform.Transform.Translate) end)
        q(function()
            local r = e.Transform.Transform.RotationQuat
            local x, y, z, w = r[1], r[2], r[3], r[4]
            out.Rotation = {x, y, z, w}
            out.Facing = {-2 * (x * z + w * y), -(1 - 2 * (x * x + y * y))}
        end)
        out.Hp = q(function() return {e.Health.Hp, e.Health.MaxHp} end)
        out.Name = q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(uuid)) end)
        out.Template = q(Osi.GetTemplate, uuid)
        out.Faction = q(Osi.GetFaction, uuid)
        out.Enemy = flag(Osi.IsEnemy, Hero.Uuid(), uuid)
        out.InCombat = flag(Osi.IsInCombat, uuid)
        out.Dead = flag(Osi.IsDead, uuid)
        out.OnStage = flag(Osi.IsOnStage, uuid)
        out.CombatGuid = q(Osi.CombatGetGuidFor, uuid)
        out.Turn = q(function()
            local t = e.TurnBased
            return {Active = t.IsActiveCombatTurn, CanAct = t.CanActInCombat}
        end)
        out.Components = q(function()
            local names, keep = e:GetAllComponentNames(false), {}
            for _, n in ipairs(names) do
                if n:find("Visual") or n:find("Invisib") or n:find("Visib") or n:find("Combat") then keep[#keep + 1] = n end
            end
            return keep
        end)
        return out
    end
    -- Errands (see the header).
    local errandSeq, errand, errandLog = 0, nil, {}
    local ERRAND_TICKS = 600
    local function errandStatus(what)
        errandLog[#errandLog + 1] = {Tick = ticks, What = what, Seq = errandSeq}
        if #errandLog > 12 then table.remove(errandLog, 1) end
        Ext.IO.SaveFile("wow-bg3/errand-status.json", Ext.Json.Stringify({RunId = runId, Seq = errandSeq,
            Active = errand ~= nil, Errand = errand, Log = errandLog,
            At = q(function() return vec(Ext.Entity.Get(Hero.Uuid()).Transform.Transform.Translate) end)}))
    end
    local function stopErrand(why, stop)
        if stop then
            local ok, err = pcall(Osi.PurgeOsirisQueue, Hero.Uuid(), 1)
            if not ok then why = why .. " (purge failed: " .. tostring(err) .. ")" end
        end
        errand = nil
        errandStatus((stop and "stopped: " or "ended: ") .. why)
    end
    local function errandTick()
        if errand then
            if flag(Osi.IsInCombat, Hero.Uuid()) then stopErrand("combat", true)
            elseif ticks - errand.Tick > ERRAND_TICKS then stopErrand("20 s", true)
            else
                local here = q(function() return vec(Ext.Entity.Get(Hero.Uuid()).Transform.Transform.Translate) end)
                if here and math.sqrt((here[1] - errand.Position[1]) ^ 2 + (here[3] - errand.Position[3]) ^ 2) < 0.4 then
                    stopErrand("arrived", false)
                end
            end
        end
        local raw = Ext.IO.LoadFile("wow-bg3/errand.json")
        if not raw or #raw > 2048 then return end
        local parsed, e = pcall(Ext.Json.Parse, raw)
        if not parsed or type(e) ~= "table" or e.RunId ~= runId or math.type(e.Seq) ~= "integer" or e.Seq <= errandSeq then return end
        errandSeq = e.Seq
        if e.Go ~= true then
            local who = e.Why and tostring(e.Why) or (e.Refused and ("refused: " .. tostring(e.Refused))) or "benilla ended it"
            if errand then stopErrand(who, e.Stop == true)
            else errandStatus("nothing to end" .. (e.Refused and (" (refused: " .. tostring(e.Refused) .. ")") or "")) end
            return
        end
        local p = e.Position
        local ok, err = pcall(function()
            assert(type(p) == "table" and #p == 3, "Position required")
            for i = 1, 3 do assert(type(p[i]) == "number" and p[i] == p[i], "Position must be numbers") end
            Hero.Assert()
            assert(not flag(Osi.IsInCombat, Hero.Uuid()), "in combat")
            local here = vec(Ext.Entity.Get(Hero.Uuid()).Transform.Transform.Translate)
            assert(math.sqrt((p[1] - here[1]) ^ 2 + (p[3] - here[3]) ^ 2) <= 80, "too far")
            -- Osiris: character, x, y, z, movement, arrival event, move id
            Osi.CharacterMoveToPosition(Hero.Uuid(), p[1], p[2], p[3], "Run", "WOWBG3_Errand", 0)
        end)
        if ok then
            errand = {Seq = e.Seq, Position = vec(p), Tick = ticks, Guid = e.Guid}
            errandStatus("going")
        else
            errand = nil
            errandStatus("refused: " .. tostring(err))
        end
    end

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if next(hpDue) then pcall(healthDue) end
        if (errand or ticks % 3 == 0) and Ext.Utils.GetGameState() == "Running" then
            local ok, err = pcall(errandTick)
            if not ok and ticks % 300 == 0 then Ext.Log.Print("WOW-BG3 errand: " .. tostring(err)) end
        end
        if ticks % 15 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local raw = Ext.IO.LoadFile("wow-bg3/stand-in-command.json")
        if not raw or #raw > 4096 then return end
        local parsed, c = pcall(Ext.Json.Parse, raw)
        if not parsed or type(c) ~= "table" or c.RunId ~= runId then return end
        if math.type(c.Sequence) ~= "integer" or c.Sequence <= sequence or c.Sequence > 4096 then return end
        sequence = c.Sequence
        local ok, value = pcall(function()
            Hero.Assert()
            if c.Action == "Spawn" then
                assert(count() < MAX, "Stand-in limit reached")
                local p = c.Position
                assert(type(p) == "table" and #p == 3, "Position required")
                assert(c.Percent == nil or (type(c.Percent) == "number" and c.Percent > 0 and c.Percent <= 100),
                    "Percent must be in (0, 100]")
                local origin = vec(Ext.Entity.Get(Hero.Uuid()).Transform.Transform.Translate)
                local dx, dz = p[1] - origin[1], p[3] - origin[3]
                assert(math.sqrt(dx * dx + dz * dz) <= RADIUS, "Position too far from the disposable character")
                local template
                if c.Template ~= nil then
                    assert(type(c.Template) == "string" and #c.Template <= 128, "Template must be a string")
                    local t = assert(Ext.Template.GetRootTemplate(uuidOf(c.Template) or c.Template), "Unknown root template")
                    assert(t.TemplateType == "character", "Template is not a character template")
                    template = t.Name .. "_" .. t.Id
                else
                    assert(type(c.From) == "string" and #c.From == 36 and Ext.Entity.Get(c.From), "From must be an existing character")
                    template = assert(Osi.GetTemplate(c.From), "No template for From")
                end
                local made = Osi.CreateAt(template, p[1], p[2], p[3], 1, 0, "")
                local uuid = assert(uuidOf(made), "CreateAt returned no character: " .. tostring(made))
                created[uuid] = true
                if type(c.Guid) == "string" then
                    WOWBG3FightSpawned = WOWBG3FightSpawned or {}
                    WOWBG3FightSpawned[uuid] = c.Guid
                end
                -- Tagged for Cleanup.lua: a stand-in a save carried goes after a load.
                pcall(function() Ext.Require("Cleanup.lua").Mark(uuid) end)
                -- BG3 heals a hurt NPC out of combat by itself about 4 s after its HP dropped
                -- (the story's self-healing; Mirrors.lua, HP guard): off for a stand-in before its
                -- health is set, the way BG3's own story does it for a character it sets lower.
                if M.SelfHeal ~= true then pcall(Osi.PROC_SelfHealing_Disable, made) end
                -- A vision cone on every mob (Mirrors.lua, spawn): BG3's own flag, off in some templates.
                pcall(Osi.SetCanSpotSneakers, uuid, 1)
                if c.Percent ~= nil and c.Percent < 100 then
                    pcall(Osi.SetHitpointsPercentage, uuid, c.Percent * 1.0)
                    hpDue[uuid] = {Percent = c.Percent, Tick = ticks}
                end
                local named
                if c.Name ~= nil then
                    assert(type(c.Name) == "string" and #c.Name <= 128, "Name must be a string")
                    named = {Names.Apply(uuid, c.Name)}
                end
                return {Created = uuid, Template = template, Named = named, State = inspect(uuid)}
            elseif c.Action == "Visible" then
                -- The disposable character may also be hidden, for clean effect captures.
                local uuid = c.Target == Hero.Uuid() and Hero.Uuid() or live(c.Target)
                Osi.SetVisible(uuid, c.Visible and 1 or 0)
                return inspect(uuid)
            elseif c.Action == "Faction" then
                local uuid = live(c.Target)
                if c.Faction ~= nil then
                    assert(type(c.Faction) == "string" and #c.Faction <= 128 and uuidOf(c.Faction), "Faction must be a faction id")
                    Osi.SetFaction(uuid, c.Faction)
                else
                    assert(type(c.From) == "string" and Ext.Entity.Get(c.From), "From must be an existing character")
                    Osi.SetFaction(uuid, Osi.GetFaction(c.From))
                end
                return inspect(uuid)
            elseif c.Action == "Templates" then
                assert(type(c.Name) == "string" and #c.Name >= 3 and #c.Name <= 64, "Name required")
                local out, total = {}, 0
                for id, t in pairs(Ext.Template.GetAllRootTemplates()) do
                    total = total + 1
                    local okt, kind = pcall(function() return t.TemplateType end)
                    if okt and kind == "character" and #out < 24 and tostring(t.Name):lower():find(c.Name:lower(), 1, true) then
                        out[#out + 1] = {Id = tostring(id), Name = tostring(t.Name)}
                    end
                end
                return {Total = total, Matches = out}
            elseif c.Action == "Join" then
                local uuid = live(c.Target)
                assert(type(c.With) == "string" and Ext.Entity.Get(c.With), "With must be an existing character")
                Osi.EnterCombat(uuid, c.With)
                return inspect(uuid)
            elseif c.Action == "TempHostile" then
                local uuid = live(c.Target)
                assert(type(c.With) == "string" and Ext.Entity.Get(c.With), "With must be an existing character")
                Osi.SetCanJoinCombat(uuid, 1)
                Osi.SetRelationTemporaryHostile(uuid, c.With)
                return inspect(uuid)
            elseif c.Action == "Remove" then
                local uuid = live(c.Target)
                assert(created[uuid], "Mirrors remove their own stand-ins")
                if WOWBG3FightSpawned then WOWBG3FightSpawned[uuid] = nil end
                Osi.RequestDeleteTemporary(uuid)
                return {Requested = uuid}
            elseif c.Action == "Clear" then
                local left, asked = 0, 0
                WOWBG3FightSpawned = {}
                local all = {}
                for uuid in pairs(created) do all[uuid] = true end
                for uuid in pairs(WOWBG3MirrorOwned or {}) do all[uuid] = true end
                for uuid in pairs(all) do
                    if Ext.Entity.Get(uuid) then
                        left = left + 1
                        if pcall(Osi.RequestDeleteTemporary, uuid) then asked = asked + 1 end
                    end
                end
                -- What else is still in the void around Centre (tele.sh: the server's squares
                -- went home with something on them): characters and items within 200 m, for the report.
                local near = {}
                if type(c.Centre) == "table" and type(c.Centre[1]) == "number" then
                    for _, comp in ipairs({"ServerCharacter", "ServerItem"}) do
                        for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent(comp)) do
                            local okp, p = pcall(function() return vec(e.Transform.Transform.Translate) end)
                            -- A stand-in that walked off the old place's squares comes along too. The level's own
                            -- people are over 2 km away.
                            local reach = comp == "ServerCharacter" and 1000 or 200
                            if okp and p and (p[1] - c.Centre[1]) ^ 2 + (p[3] - c.Centre[3]) ^ 2 < reach * reach and #near < 40 then
                                local id = q(function() return e.Uuid.EntityUuid end)
                                local row = {Kind = comp, Uuid = id, Pos = p,
                                    Name = id and q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(id)) end),
                                    Template = id and q(function() return Osi.GetTemplate(id) end)}
                                -- A character in the void that is no player is a stand-in nobody owns any
                                -- more: one carried in a save made in world mode (harness: a
                                -- Young Wolf, its server tiles kept two squares from going home clean).
                                if id and comp == "ServerCharacter" and q(Osi.IsPlayer, id) ~= 1 then
                                    row.Deleted = pcall(Osi.RequestDeleteTemporary, id)
                                    if c.Evict == true then
                                        -- Still there after a few tries: off the squares, 500 m further on.
                                        row.Evicted = pcall(Osi.TeleportToPosition, id, c.Centre[1], c.Centre[2], c.Centre[3] + 500, "", 0, 0, 0, 0, 0)
                                    end
                                    left = left + 1
                                end
                                near[#near + 1] = row
                            end
                        end
                    end
                end
                return {Left = left, Asked = asked, Near = near}
            elseif c.Action == "ClearSurfaces" then
                -- World mode stops (a hearthstone, a graveyard port, /wowbg3 tp): BG3's surfaces stay at
                -- their BG3 coordinates while the WoW point those coordinates stand for changes, so a
                -- Grease patch would show up on the new place's ground (its inn's floor).
                -- The void's ground is the placed pool subgrids alone: the plugin's op 60 hands every tile of them that
                -- holds a ground or cloud surface to BG3's own cell removal (as Place does before it moves one).
                -- It was Osi.RemoveSurfaceLayer round the hero, 130 m: BG3 makes that a removal action over every
                -- tile in the circle, surface or not, and adds each to the SurfaceManager's changed cells after a
                -- linear search of that list (bg3+0x389b290 from the action's Update bg3+0x389ac70), so n tiles
                -- cost n * n / 2 compares: the server ran no tick for 1 to 7 s after each Start, move and Stop
                -- (0.1.3013.61's stall trace: every stack there, every stall within 0.6 s of this call).
                local u = c.Target  -- the hero, not one of the stand-ins live() is for
                assert(type(u) == "string", "ClearSurfaces needs a Target")
                local N = Ext.Require("WowNative.lua")
                local out = N.Call(N.Op.PoolSurfaces, -1, 1)
                if out.Err == 61 then return {Cleared = u, Pool = false} end -- no pool out: no void ground to clear
                if out.Err ~= 0 then error("wowbg3: " .. tostring(out.Why or out.Err), 0) end
                return {Cleared = u, Ground = out.R[1], Cloud = out.R[2], Subgrids = out.R[3]}
            elseif c.Action == "Teleport" or c.Action == "Return" then
                local uuid = (c.Target == Hero.Uuid() or (c.Action == "Return" and type(c.Target) == "string" and home[c.Target]))
                    and c.Target or live(c.Target)
                local here = vec(Ext.Entity.Get(uuid).Transform.Transform.Translate)
                local p
                if c.Action == "Return" then
                    p = assert(home[uuid], "Nothing to return: not teleported")
                else
                    p = c.Position
                    assert(type(p) == "table" and #p == 3, "Position required")
                    for i = 1, 3 do assert(type(p[i]) == "number" and p[i] == p[i], "Position must be numbers") end
                    -- Measured from what the teleport is relative to: Near, the void spot world mode works around
                    -- (its Start and a move send their Centre), else the hero's home or position. The hero's home
                    -- is where the save left her, and BG3's camp (about 384, 6, -752) lies 2.99 to 3.05 km from the
                    -- Centre: a camp save's Start passed this by 11 m, from camp's east side it fails.
                    local anchor, what = home[Hero.Uuid()] or vec(Ext.Entity.Get(Hero.Uuid()).Transform.Transform.Translate),
                        "the disposable character's home"
                    local near = nil
                    if c.Near ~= nil then
                        local n = c.Near
                        assert(type(n) == "table" and #n == 3 and tonumber(n[1]) and tonumber(n[3]), "Near is a point {X, Y, Z}")
                        near = {tonumber(n[1]), tonumber(n[2]) or 0, tonumber(n[3])}
                        anchor, what = near, "Near"
                    end
                    local dx, dz = p[1] - anchor[1], p[3] - anchor[3]
                    assert(math.sqrt(dx * dx + dz * dz) <= VOID_RADIUS, "Teleport target too far from " .. what)
                    if not home[uuid] then
                        -- In the void: near world mode's Centre when the caller says where that is (as Camp.lua tells
                        -- a void jump), else LEVEL_REACH from the level's spot. That far test calls the east of camp
                        -- void (x 420, -790 is 606 m from the spot), and Stop would then send the hero to LEVEL_SPOT.
                        local void
                        if near then
                            void = math.sqrt((here[1] - near[1]) ^ 2 + (here[3] - near[3]) ^ 2) < Ext.Require("Camp.lua").VOID_REACH
                        else
                            void = inVoid(here)
                        end
                        home[uuid] = void and {LEVEL_SPOT[1], LEVEL_SPOT[2], LEVEL_SPOT[3]} or here
                        hpHome[uuid] = q(function() local h = Ext.Entity.Get(uuid).Health; return {h.Hp, h.MaxHp} end)
                    end
                end
                -- Osiris: source, x, y, z, event, linked characters, party followers, summons, leave combat, snap to ground.
                -- The hero's summons come along, BG3's default (its story helpers pass summons 1): with 0 a familiar
                -- stayed where the save had it, off WoW's ground, at every world mode Start after a load.
                local summons = uuid == Hero.Uuid() and 1 or 0
                Osi.TeleportToPosition(uuid, p[1], p[2], p[3], "", 0, 0, summons, 0, c.Snap == false and 0 or 1)
                -- On the log's clock: the server stopped for seconds soon after some of these (plugin op 70's trace).
                Ext.Log.Print(string.format("WOW-BG3 stand-ins: %s %s to %.1f %.1f %.1f, snap %s", c.Action,
                    uuid == Hero.Uuid() and "the hero" or uuid, p[1], p[2], p[3], tostring(c.Snap ~= false)))
                local healed
                if c.Action == "Return" then
                    home[uuid] = nil
                    local before = hpHome[uuid]
                    hpHome[uuid] = nil
                    local now = q(function() local h = Ext.Entity.Get(uuid).Health; return {h.Hp, h.MaxHp} end)
                    if uuid == Hero.Uuid() and before and now and now[1] < before[1] and before[2] > 0 then
                        healed = {Before = before, After = now}
                        healed.Downed = flag(Osi.HasActiveStatus, uuid, "DOWNED")
                        if healed.Downed then healed.RemoveDowned = q(Osi.RemoveStatus, uuid, "DOWNED", "") ~= nil end
                        healed.Set = pcall(Osi.SetHitpointsPercentage, uuid, 100.0 * before[1] / before[2])
                        healed.Now = q(function() local h = Ext.Entity.Get(uuid).Health; return {h.Hp, h.MaxHp} end)
                    end
                end
                return {Target = uuid, From = here, To = p, Home = home[uuid], Healed = healed}
            elseif c.Action == "Away" then
                local moved = {}
                for _, row in pairs(Osi.DB_Players:Get(nil) or {}) do
                    local uuid = uuidOf(type(row) == "table" and row[1] or row)
                    -- The hero's companions come along (Party.lua Gather): only player
                    -- characters outside the hero's party go off stage.
                    if uuid and uuid ~= Hero.Uuid() and not away[uuid] and flag(Osi.IsOnStage, uuid) ~= false
                        and flag(Osi.IsInPartyWith, uuid, Hero.Uuid()) ~= true then
                        Osi.SetOnStage(uuid, 0)
                        away[uuid] = true
                        moved[#moved + 1] = uuid
                    end
                end
                return {Away = moved}
            elseif c.Action == "Back" then
                local back = {}
                for uuid in pairs(away) do
                    if Ext.Entity.Get(uuid) then Osi.SetOnStage(uuid, 1); back[#back + 1] = uuid end
                    away[uuid] = nil
                end
                return {Back = back}
            elseif c.Action == "GameOver" then
                -- World mode: the WoW character's life decides the hero's (Mirrors.lua
                -- raises a dead or downed hero when WoW revives; a lost fight kills the WoW character).
                -- BG3 shows its game over when every character of DB_Players is defeated (Shared
                -- GLO_GameOverLogic.txt), so a party of the hero alone (a new game; Tav's test save also
                -- has Lae'zel there) would end the game at the first lost fight. The story's own list of
                -- defeat causes that are no game over (DB_GameOver_DefeatCausesNotGameOver; BG3 puts
                -- TempPetrified and TempBanished there) holds Dead and Downed while world mode runs.
                -- BG3's story never lists Dead or Downed itself, so a save made in world mode (which
                -- carries the two rows) gets them taken out again at the next Stop too.
                local changed = {}
                for _, cause in ipairs({"Dead", "Downed"}) do
                    local has = #(Osi.DB_GameOver_DefeatCausesNotGameOver:Get(cause) or {}) > 0
                    if c.Hold == true then
                        if not has then
                            Osi.DB_GameOver_DefeatCausesNotGameOver(cause)
                            changed[#changed + 1] = "+" .. cause
                        end
                        gameOverHeld[cause] = true
                    elseif c.Hold ~= true and gameOverHeld[cause] then
                        Osi.DB_GameOver_DefeatCausesNotGameOver:Delete(cause)
                        gameOverHeld[cause] = nil
                        changed[#changed + 1] = "-" .. cause
                    end
                end
                return {Held = c.Hold == true, Changed = changed}
            elseif c.Action == "Music" then
                -- World mode: outside BG3's fights WoW plays its own music
                -- (benilla 0030). BG3's music (MUSIC.bnk, one Wwise music switch) picks exploration
                -- music by the state Music_Set (the area's set: global, Underdark, the acts...) and
                -- fight music by MUSIC_Type = fight, whose branch also holds for Music_Set None.
                -- BG3's own event Music_Off sets Music_Set and every per-area set to None: no
                -- exploration music, fight music as before; sound effects, voices and ambience are
                -- other buses (from an analysis of BG3's sound banks). The fight switch itself keys on
                -- no Music_Set, so FIGHT_THEMES above work with it off.
                assert(MUSIC_EVENTS[c.Event], "Music: Event must be Music_Off")
                Osi.MusicPlayGeneral(c.Event)
                fightThemesOn = true
                return {Event = c.Event}
            elseif c.Action == "Scale" then
                local uuid = live(c.Target)
                local h = Ext.Entity.Get(uuid).Health
                local before = {h.Hp, h.MaxHp}
                -- A mob that joined a fight with no mirror (LinkLive.lua), with `scaling` on: the rules Mirrors.lua uses.
                local Scaling = Ext.Require("Scaling.lua")
                if type(c.Rules) == "table" and Scaling.On() and (tonumber(c.Rules.Level) or 0) > 0 and (tonumber(c.Rules.WowMaxHp) or 0) > 0 then
                    local r, fixed = c.Rules, Scaling.Fixed()
                    local n = Scaling.Compute({Level = tonumber(r.Level), WowMaxHp = tonumber(r.WowMaxHp), WowDamage = tonumber(r.WowDamage),
                        Map = r.Map, Cap = fixed.Cap, Difficulty = fixed.Difficulty})
                    local text = Scaling.Boosts(n, r.Kind, h.MaxHp, tonumber(r.BaseDamage) or 0, nil)
                    if text ~= "" then Osi.AddBoosts(uuid, text, "WOWBG3_SCALE", uuid) end
                    return {Target = uuid, Before = before, Boosts = text, MaxHp = n.MaxHp, Rules = true}
                end
                local boosts = {}
                local function int(v) return type(v) == "number" and v == math.floor(v) and v or nil end
                local maxHp = int(c.MaxHp)
                if maxHp ~= nil and maxHp ~= h.MaxHp and h.MaxHp > 0 then
                    assert(maxHp >= 1 and maxHp <= 5000, "MaxHp out of range")
                    boosts[#boosts + 1] = string.format("IncreaseMaxHP(%d)", maxHp - h.MaxHp)
                end
                local bonus = int(c.DamageBonus)
                if bonus ~= nil and bonus ~= 0 then
                    assert(math.abs(bonus) <= 200, "DamageBonus out of range")
                    boosts[#boosts + 1] = string.format("DamageBonus(%d)", bonus)
                end
                if #boosts > 0 then Osi.AddBoosts(uuid, table.concat(boosts, ";"), "WOWBG3_SCALE", uuid) end
                local spells = {}
                for _, s in ipairs(type(c.Spells) == "table" and c.Spells or {}) do
                    assert(SCALE_SPELLS[s], "Spells: not an allowed spell")
                    if Osi.HasSpell(uuid, s) ~= 1 then Osi.AddSpell(uuid, s, 0, 1) end
                    spells[#spells + 1] = s
                end
                return {Target = uuid, Before = before, Boosts = boosts, Spells = spells}
            elseif c.Action == "Health" then
                local uuid = live(c.Target)
                assert(type(c.Percent) == "number" and c.Percent > 0 and c.Percent <= 100, "Percent must be in (0, 100]")
                Osi.SetHitpointsPercentage(uuid, c.Percent * 1.0)
                return inspect(uuid)
            elseif c.Action == "Inspect" then
                return inspect(c.Target == Hero.Uuid() and Hero.Uuid() or live(c.Target))
            elseif c.Action == "Face" then
                local uuid = live(c.Target)
                local d = c.Dir
                assert(type(d) == "table" and type(d[1]) == "number" and type(d[2]) == "number", "Dir = {x, z} required")
                local st = assert(Ext.Entity.Get(uuid).Steering, "no Steering component")
                st.TargetRotation = math.atan(-d[1], -d[2])
                st.RequestSteering = 1
                return inspect(uuid)
            elseif c.Action == "Osi" then
                -- Harness only (effects research): one Osiris call, e.g. ApplyStatus on Tav.
                assert(Harness.On, "harness actions are off (WOW_BG3_HARNESS=1)")
                assert(type(c.Fn) == "string" and type(Osi[c.Fn]) ~= "nil", "unknown Osiris function")
                local r = table.pack(Osi[c.Fn](table.unpack(c.Args or {})))
                return {Results = r}
            end
            error("Unknown stand-in action")
        end)
        local list = {}
        for uuid in pairs(created) do list[#list + 1] = {Uuid = uuid, Exists = Ext.Entity.Get(uuid) ~= nil} end
        Ext.IO.SaveFile("wow-bg3/stand-in-result.json", Ext.Json.Stringify({RunId = runId, Sequence = sequence,
            OK = ok, Value = ok and value or tostring(value), Created = list}))
    end)
end
return M
