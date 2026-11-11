-- The party ("via commands add both Shadowheart and Astarion to my party
-- for testing; we will eventually figure out how to add/remove companions within the WoW map").
-- BG3's companions join the hero's party the BG3 way and travel with the hero in world mode; WoW draws
-- them like the hero (TavPlan.lua figures, the capture layer, benilla 0033), the WoW server never hears
-- of them (client-only followers, the party-design notes), and in BG3 fights they are ordinary BG3
-- party members. Server context.
--
-- Driven by wow-bg3/party-command.json {RunId, Sequence, Action, Name} (link/tools/party.sh; one job at
-- a time, never retried), result in wow-bg3/party-result.json {RunId, Sequence, OK, Value}:
--   List              the party (hero first) and the companions this tool knows, with where they are
--   Add {Name}        recruit a companion BG3's way: brought into the hero's level (Osiris TeleportTo,
--                     which moves a global character between levels: Astarion waits in WLD_Main_A),
--                     on stage, Shadowheart out of her pod with the tutorial's own procs and its
--                     recruitment rows, then BG3's own recruitment after a dialog,
--                     PROC_GLO_PartyMembers_CheckAdd (MakePlayer, RegisterAsCompanion, faction, DB_Players,
--                     BG3's party limit), next to the hero and chained to the hero's group
--                     (AttachToPartyGroup), so BG3's own party following moves them
--   Remove {Name}     BG3's dismiss: the tutorial's own PROC_TUT_Misc_TutorialCompanion_RemoveCompanion for
--                     a tutorial companion while the goal holding it (TUT_Misc) runs, else
--                     PROC_GLO_PartyMembers_Remove (force, gold to the hero). Nothing is done by hand: when
--                     BG3's story would refuse, Remove refuses too and says why;
--                     then back where Add found it (wow-bg3/party-home.json): same level by
--                     TeleportToPosition, another level through a companion standing there (TeleportTo),
--                     else off stage where it stands
--   Gather {Position, Upkeep}  (WorldMode.lua) every companion next to the hero (or Position) and
--                     chained, each with its summons (BG3's teleport takes them along); Upkeep true while
--                     world mode runs (below), false at its Stop
--   Refill            ("a command to refill my resources (spell slots, rage, etc)")
--                     BG3's own full restore, Osiris RestoreParty on the hero (every party member's HP and
--                     action resources), then ResetCooldowns on each member; anything still under its
--                     maximum after that (reported as TopUp) is set to it in the ActionResources component.
--                     In fights too (it refills the turn's actions as well)
--   Pool              (Progression.lua) the WoW level, party slots, level-ups earned, given
--                     and left, each member's BG3 level
--   LevelUp {Name, Count}  Count level-ups (1 by default) from the pool to that member (NAME, or
--                     "hero"): the XP of its next level each time, so BG3's Level Up button comes up Count
--                     times in a row and the class choices stay the player's
--   Catchup           every level-up left handed out evenly across the party, in rotation to whoever has
--                     had the fewest; reports what each member got
--   Reset {Name}      Withers' respec for that member; its level-ups go back to the pool once BG3's
--                     respec screen is finished
--   (Level {Level}, raised everyone's XP to a BG3 level: retired, all BG3 XP comes
--   from the WoW levels now)
-- Add refuses when the WoW level has not opened another party slot (WoW levels 1 to 4, one each).
-- Upkeep (world mode): members range freely (a treadmill per member keeps ground under each), and nobody
-- is ever put back: a fall is BG3's own, and world_bridge.py's fall alarm logs one that nothing can stop.
-- Help: a member down after a fight stays down until the player uses BG3's Help, as in BG3.
-- Status: wow-bg3/party.json (on change within 10 ticks, else every 300): {RunId, Tick, Hero, Members =
-- [{Uuid, Name, Hero, Hp, MaxHp, Dead, Downed, InCombat, Combat, Pos, Region, OnStage}], Summons = the party's
-- summons (M.Summons: [{Uuid, Name, Summoner, Dead, OnStage, Pos, Region}]), Upkeep}.
-- Read by TavPlan.lua/WorldMode.lua (the figures WoW draws), Mirrors.lua and ActionMonitor.lua (the
-- party in fights) and the tools.
local M = {}
local Hero = Ext.Require("Hero.lua")
local Prog = Ext.Require("Progression.lua") -- WoW levels to BG3 levels
local REGION = Hero.REGION -- host level (Hero.lua, host.json)
local TUT_REGION = "TUT_Avernus_C" -- the tutorial level; the tutorial's own goals and procs run only there

-- The companions this tool recruits (BG3's origin companions; a name or its first letters).
local KNOWN = {
    {"shadowheart", "S_Player_ShadowHeart_3ed74f06-3c60-42dc-83f6-f034cb47c679"},
    {"astarion", "S_Player_Astarion_c7c13742-bacd-460a-8f65-f864fe41f255"},
    {"gale", "S_Player_Gale_ad9af97d-75da-406a-ae13-7071c563f604"},
    {"laezel", "S_Player_Laezel_58a69333-40bf-8358-1d17-fff240d7fb12"},
    {"wyll", "S_Player_Wyll_c774d764-4a17-48dc-b470-32ace9ce447d"},
    {"karlach", "S_Player_Karlach_2c76687d-93a2-477b-8b18-8a14b549304c"},
    {"jaheira", "S_Player_Jaheira_91b6b200-7d00-4d62-8dc9-99e8339dfa1a"},
    {"minsc", "S_Player_Minsc_0de603c5-42e2-4811-9dad-f652de080eba"},
    {"halsin", "S_GLO_Halsin_7628bc0e-52b8-42a7-856a-13a6fd413323"},
    {"minthara", "S_GOB_DrowCommander_25721313-0c15-4935-8176-9f134385451b"},
}
-- Shadowheart's tutorial recruitment (TUT_TransformChamber.txt, its "Shadowheart Tutorial Recruitment"
-- region after the freed dialog, and the pod procs it frees her with).
local SH = "S_Player_ShadowHeart_3ed74f06-3c60-42dc-83f6-f034cb47c679"
local SH_FLAG = "TUT_TransformChamber_State_RecruitedShadowheart_2c531120-0297-4e8d-9783-762f5ea67c66"
local SH_FOLLOWER = "TUT_TransformChamber_ShadowheartFollower_5c77b381-e606-12b7-1379-89db17e4a6d5"
local SH_INPARTY = "ShadowHeart_InParty_95ca3833-09d0-5772-b16a-c7a5e9208fe5"
local SH_FACTION = "Origin_ShadowHeart_901cd370-86ff-b538-e1e8-574c84135ca0"
local MAX_REACHED = "GEN_MaxPlayerCountReached_823b5064-8aa4-c0b7-1b8c-657b46987ccd"
local NULL = "NULL_00000000-0000-0000-0000-000000000000"

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function uuidOf(s) return Hero.UuidOf(tostring(s)) end
local function rows(db, n)
    local ok, r = pcall(function()
        if n == 1 then return Osi[db]:Get(nil) elseif n == 2 then return Osi[db]:Get(nil, nil) end
    end)
    return ok and r or {}
end
local function pos(u)
    local ok, x, y, z = pcall(Osi.GetPosition, u)
    if ok and type(x) == "number" then return {x, y, z} end
end
local function nameOf(u) return q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(u)) end) or u end

-- The hero's party: the hero first, then every other DB_Players character in the hero's party (the
-- companions), by UUID. Cached per tick.
local tick, cacheTick, cache = 0, -1, nil
-- The cache's clock (this module runs once per side: Ext.Require keeps it), also before Start.
Ext.Events.Tick:Subscribe(function() tick = tick + 1 end)
function M.Party()
    if cacheTick == tick and cache then return cache end
    local hero = Hero.Uuid()
    local list = {}
    if hero then
        list[1] = hero
        for _, r in ipairs(rows("DB_Players", 1)) do
            local u = uuidOf(r[1])
            if u and u ~= hero and q(Osi.IsInPartyWith, u, hero) == 1 then list[#list + 1] = u end
        end
    end
    cache, cacheTick = list, tick
    return list
end
-- Whether a UUID or GUIDSTRING names a member of the hero's party (the hero included).
function M.IsMember(s)
    if type(s) ~= "string" then return false end
    for _, u in ipairs(M.Party()) do if s:find(u, 1, true) then return true end end
    return false
end
-- Whether it names a companion (a party member who is not the hero).
function M.IsCompanion(s)
    local hero = Hero.Uuid()
    return M.IsMember(s) and not (hero and s:find(hero, 1, true))
end
local function combatOf(u)
    if q(Osi.IsInCombat, u) ~= 1 then return nil end
    return q(Osi.CombatGetGuidFor, u)
end
-- The party's side: every member and every summon of a member (a familiar, a spirit companion, a raised
-- dead). Each fights in BG3 as itself: a fight it starts is its own, whoever else is far away.
-- By UUID, members first. Cached per tick.
local fightersTick, fighters = -1, nil
function M.Fighters()
    if fightersTick == tick and fighters then return fighters end
    local list, seen = {}, {}
    for _, u in ipairs(M.Party()) do list[#list + 1] = u; seen[u] = true end
    pcall(function()
        for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("IsSummon")) do
            local okU, u = pcall(function() return e.Uuid.EntityUuid end)
            local okS, by = pcall(function() return e.IsSummon.Summoner.Uuid.EntityUuid end)
            local okO, owner = pcall(function() return e.IsSummon.Owner.Uuid.EntityUuid end)
            if okU and u and not seen[u] and ((okS and seen[by]) or (okO and seen[owner])) then
                list[#list + 1] = u
                seen[u] = true
            end
        end
    end)
    fighters, fightersTick = list, tick
    return list
end
-- Whether a UUID or GUIDSTRING names one of the party's side.
function M.IsFighter(s)
    if type(s) ~= "string" then return false end
    for _, u in ipairs(M.Fighters()) do if s:find(u, 1, true) then return true end end
    return false
end
-- The party's BG3 combat: the first of the party's side in one (the hero's, a companion's or a summon's,
-- with a stand-in it attacked or one that spotted it).
function M.Combat()
    for _, u in ipairs(M.Fighters()) do
        local c = combatOf(u)
        if c then return c end
    end
    return nil
end
-- Who a stand-in fights when something outside BG3's own flow joins it to a combat (M.Opponent):
-- the one of the party's side that last attacked it. target uuid -> {Who, At}.
local ATTACKER_KEEP = 30000 -- ms: an attack this long ago no longer says who it fights
local attackers = {}
function M.NoteAttack(target, who)
    if target and who then attackers[target] = {Who = who, At = Ext.Utils.MonotonicTime()} end
end
function M.ForgetAttacks(target) attackers[target] = nil end
-- The one of the party's side a stand-in fights: its attacker; else the nearest in a BG3 combat (a pack
-- that joins someone's fight); else the nearest (WoW's aggro on whoever walked by). Never the hero by
-- title: BG3 takes whoever is named into the combat, however far away they are.
function M.Opponent(target)
    local a = attackers[target]
    if a and Ext.Utils.MonotonicTime() - a.At <= ATTACKER_KEEP and Ext.Entity.Get(a.Who) and q(Osi.IsDead, a.Who) ~= 1 then
        return a.Who
    end
    local at = pos(target)
    local best, bestD, bestIn
    for _, u in ipairs(M.Fighters()) do
        local p = q(Osi.IsDead, u) ~= 1 and pos(u)
        if p then
            local d = at and (p[1] - at[1]) ^ 2 + (p[3] - at[3]) ^ 2 or 0
            local inCombat = combatOf(u) ~= nil
            if not best or (inCombat and not bestIn) or (inCombat == bestIn and d < bestD) then
                best, bestD, bestIn = u, d, inCombat
            end
        end
    end
    return best or Hero.Uuid()
end
local function member(u, hero)
    local e = Ext.Entity.Get(u)
    local h = q(function() return {e.Health.Hp, e.Health.MaxHp} end) or {0, 0}
    return {Uuid = u, Name = nameOf(u), Hero = u == hero, Hp = h[1], MaxHp = h[2], Dead = q(Osi.IsDead, u) == 1,
        Downed = q(Osi.HasActiveStatus, u, "DOWNED") == 1, InCombat = q(Osi.IsInCombat, u) == 1,
        Combat = combatOf(u), Pos = pos(u), Region = q(Osi.GetRegion, u), OnStage = q(Osi.IsOnStage, u) == 1}
end
function M.Snapshot()
    local hero = Hero.Uuid()
    local out = {}
    for _, u in ipairs(M.Party()) do out[#out + 1] = member(u, hero) end
    return out
end
-- The party's summons (a familiar, a spirit companion, a raised dead): every eoc::summon::IsSummon whose
-- Summoner (or Owner) is a party member. They are not in DB_Players, so M.Party() never lists them, but
-- the player walks them like members (a Find Familiar spider, say), so world_bridge.py
-- gives each its own treadmill squares from this list. [{Uuid, Name, Summoner, Dead, OnStage, Pos, Region}]
function M.Summons()
    local members = {}
    for _, u in ipairs(M.Party()) do members[u] = true end
    local out = {}
    pcall(function()
        for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("IsSummon")) do
            local okU, u = pcall(function() return e.Uuid.EntityUuid end)
            local okS, by = pcall(function() return e.IsSummon.Summoner.Uuid.EntityUuid end)
            local okO, owner = pcall(function() return e.IsSummon.Owner.Uuid.EntityUuid end)
            local of = (okS and members[by] and by) or (okO and members[owner] and owner) or nil
            if okU and u and of and not members[u] then
                out[#out + 1] = {Uuid = u, Name = nameOf(u), Summoner = of, Dead = q(Osi.IsDead, u) == 1,
                    OnStage = q(Osi.IsOnStage, u) == 1, Pos = pos(u), Region = q(Osi.GetRegion, u)}
            end
        end
    end)
    table.sort(out, function(a, b) return a.Uuid < b.Uuid end)
    return out
end

local function resolve(name)
    assert(type(name) == "string" and #name >= 2, "Name required (e.g. shadowheart, astarion)")
    local n = name:lower():gsub("[^%a]", "")
    for _, k in ipairs(KNOWN) do
        if k[1] == n or k[1]:sub(1, #n) == n then return {Key = k[1], Guid = k[2], Uuid = uuidOf(k[2])} end
    end
    error("unknown companion '" .. name .. "' (known: shadowheart, astarion, gale, laezel, wyll, karlach, jaheira, minsc, halsin, minthara)", 0)
end

-- Where Add found each companion (Remove puts it back), kept across BG3 runs.
local HOME_FILE = "wow-bg3/party-home.json"
local function homes()
    local raw = Ext.IO.LoadFile(HOME_FILE)
    local ok, v = pcall(Ext.Json.Parse, raw or "")
    return ok and type(v) == "table" and v or {}
end
local function saveHomes(h) Ext.IO.SaveFile(HOME_FILE, Ext.Json.Stringify(h)) end

-- Next to the hero (or `at`): beside and behind it, slot i (0, 1, 2...), on the ground.
local function placeNear(u, at, i)
    local side = (i % 2 == 0) and 1 or -1
    local row = math.floor(i / 2)
    local p = {at[1] + side * 1.4, at[2] + 0.5, at[3] + 1.2 + row * 1.2}
    -- Osiris: source, x, y, z, event, linked characters, party followers, summons, leave combat, snap. Its
    -- summons come too (BG3's own teleport of them): a familiar left where the save had it is on no ground.
    Osi.TeleportToPosition(u, p[1], p[2], p[3], "", 0, 0, 1, 0, 1)
    return p
end

function M.Start(runId)
    local okp, errp = pcall(Prog.Start, runId)
    if not okp then Ext.Utils.Print("WOW-BG3 progression unavailable: " .. tostring(errp)) end
    local sequence, jobs = 0, {}
    -- bg3le builds the Lua states again at every load: the command already in the file was carried out
    -- by the previous state, so its Sequence counts as done.
    do
        local okc, c = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/party-command.json") or "")
        if okc and type(c) == "table" and c.RunId == runId and math.type(c.Sequence) == "integer" then sequence = c.Sequence end
    end
    local upkeep, lastKey, lastWrite = false, nil, -1000
    local function wait(n)
        local due = tick + n
        while tick < due do coroutine.yield() end
    end
    local function waitFor(fn, n, what)
        local due = tick + n
        while not fn() do
            if tick > due then error(what, 0) end
            coroutine.yield()
        end
    end
    local function heroOrFail()
        local hero = Hero.Uuid()
        assert(hero and Ext.Entity.Get(hero), "no hero (no character controlled)")
        return hero
    end
    local function inDbPlayers(u)
        for _, r in ipairs(rows("DB_Players", 1)) do if uuidOf(r[1]) == u then return true end end
        return false
    end

    -- Out of the tutorial's pod (TUT_TransformChamber.txt: PROC_TUT_TransformChamber_FreeShadowheart and
    -- the state part of PROC_TUT_TransformChamber_MoveShadowheart, without its kneel and dialog), then the
    -- tutorial's recruitment rows (its DialogEnded region for the freed dialog with the recruited flag).
    local function tutorialShadowheart(hero, steps)
        local inPod = q(Osi.HasActiveStatus, SH, "TUT_SHADOWHEARTINPOD") == 1 or q(Osi.HasActiveStatus, SH, "TUT_CHARACTERINPOD") == 1
        if inPod then
            pcall(Osi.PROC_TUT_TransformChamber_FreeShadowheart)
            pcall(Osi.StopAnimation, SH, 1)
            pcall(Osi.RemoveStatus, SH, "TUT_SHADOWHEARTINPOD", "")
            pcall(Osi.RemoveStatus, SH, "TUT_CHARACTERINPOD", "")
            pcall(Osi.PROC_SetInvulnerable, SH, 0)
            pcall(Osi.PROC_TUT_TransformChamber_RemoveChairHack)
            steps[#steps + 1] = "out of her pod (the tutorial's own procs)"
        end
        if REGION == TUT_REGION and q(Osi.GetRegion, SH) == REGION then
            pcall(Osi.SetFlag, SH_FLAG, hero)
            pcall(Osi.PROC_RemoveDialog, SH)
            pcall(Osi.DB_Dialogs, SH, SH_FOLLOWER)
            pcall(function() Osi.DB_OriginInPartyDialog:Delete(SH, SH_INPARTY) end)
            pcall(Osi.DB_OriginInPartyDialog, SH, SH_FOLLOWER)
            pcall(Osi.DB_TUT_Misc_TutorialCompanion_OldFaction, SH, SH_FACTION)
            steps[#steps + 1] = "the tutorial's recruitment rows (recruited flag, follower dialog)"
        end
    end

    local function add(c)
        local hero = heroOrFail()
        local who = resolve(c.Name)
        local u = who.Uuid
        assert(u ~= hero, nameOf(u) .. " is the hero")
        assert(Ext.Entity.Get(u), nameOf(u) .. " does not exist in this game")
        assert(not M.IsMember(u), nameOf(u) .. " is already in the party")
        assert(q(Osi.IsDead, u) ~= 1, nameOf(u) .. " is dead")
        assert(q(Osi.IsInCombat, hero) ~= 1, "not during a fight")
        local refusal = Prog.AddRefusal(#M.Party())
        if refusal then error(refusal, 0) end
        local hasFaction = false
        for _, r in ipairs(rows("DB_GLO_PartyMembers_DefaultFaction", 2)) do
            if uuidOf(r[1]) == u then hasFaction = true end
        end
        assert(hasFaction, nameOf(u) .. " has no companion setup in this game (DB_GLO_PartyMembers_DefaultFaction)")
        assert(q(Osi.GetFlag, MAX_REACHED, NULL) ~= 1,
            string.format("the party is full (BG3's limit, %s)", tostring(q(Osi.GetMaxPartySize))))
        local steps = {}
        -- Where it is now, for Remove (not a spot in world mode's void).
        local here, region = pos(u), q(Osi.GetRegion, u)
        local h = homes()
        if here and region and not (region == REGION and Hero.InWorld(here)) then
            h[u] = {Region = region, Pos = here, OnStage = q(Osi.IsOnStage, u) == 1, Name = nameOf(u)}
            saveHomes(h)
        end
        local heroRegion = q(Osi.GetRegion, hero)
        if region ~= heroRegion then
            -- A global character moves between levels with TeleportTo (Astarion: WLD_Main_A).
            Osi.TeleportTo(u, hero, "", 0, 0, 0, 1, 1)
            waitFor(function() return q(Osi.GetRegion, u) == heroRegion end, 300, "BG3 did not bring " .. nameOf(u) .. " into " .. tostring(heroRegion))
            steps[#steps + 1] = string.format("brought from %s into %s (TeleportTo)", tostring(region), tostring(heroRegion))
        end
        if q(Osi.IsOnStage, u) ~= 1 then
            Osi.SetOnStage(u, 1)
            steps[#steps + 1] = "on stage"
        end
        if u == uuidOf(SH) then tutorialShadowheart(hero, steps) end
        -- BG3's own recruitment (what a recruitment dialog's end runs, _GLO_Shared_PartyMembers.txt).
        Osi.PROC_GLO_PartyMembers_CheckAdd(u, hero)
        waitFor(function() return inDbPlayers(u) end, 60, "BG3 did not recruit " .. nameOf(u) .. " (PROC_GLO_PartyMembers_CheckAdd)")
        steps[#steps + 1] = "recruited (PROC_GLO_PartyMembers_CheckAdd: DB_Players, faction " .. tostring(q(Osi.GetFaction, u)) .. ")"
        wait(2)
        local at = pos(hero)
        local n = #M.Party() - 2
        local p = placeNear(u, at, math.max(0, n))
        wait(5)
        Osi.AttachToPartyGroup(u, hero)
        steps[#steps + 1] = string.format("next to the hero at %.1f %.1f %.1f, chained to the hero's group", p[1], p[2], p[3])
        cacheTick = -1
        return {Added = nameOf(u), Uuid = u, Steps = steps, Party = M.Snapshot(), MaxPartySize = q(Osi.GetMaxPartySize)}
    end

    -- Whether a story goal runs (Osiris SysIsActive, as BG3's own savegame patches ask).
    local function goalRunning(name)
        local v = q(Osi.SysIsActive, name)
        return v == true or v == 1
    end
    -- What in BG3's story would stop its forced dismiss, before anything of it runs (it hands over the
    -- gold and detaches first, then PROC_GLO_PartyMembers_MakeNPC needs these rows; _GLO_Shared_PartyMembers.txt).
    -- An avatar (another player's character) takes the story's own avatar rules: not judged here.
    local function dismissRefusal(u)
        local function has(db, n)
            for _, r in ipairs(rows(db, n)) do if uuidOf(r[1]) == u then return true end end
            return false
        end
        if has("DB_Avatars", 1) then return nil end
        if not has("DB_GLO_PartyMembers_OriginalAlignment", 2) then return "no DB_GLO_PartyMembers_OriginalAlignment row" end
        if not has("DB_PartyMembers", 1) then return "no DB_PartyMembers row" end
        return nil
    end

    -- Whether BG3's party keeps u in a group of its own, not the hero's (chained members share the hero's view).
    local function apart(u, hero)
        local a = q(function() return Ext.Entity.Get(u).PartyMember.ViewUuid end)
        local b = q(function() return Ext.Entity.Get(hero).PartyMember.ViewUuid end)
        return a ~= nil and b ~= nil and a ~= b
    end

    local function remove(c)
        local hero = heroOrFail()
        local who = resolve(c.Name)
        local u = who.Uuid
        assert(u ~= hero, nameOf(u) .. " is the hero")
        assert(M.IsMember(u), nameOf(u) .. " is not in the party")
        assert(q(Osi.IsInCombat, hero) ~= 1 and q(Osi.IsInCombat, u) ~= 1, "not during a fight")
        local steps = {}
        local tut = false
        for _, r in ipairs(rows("DB_TUT_Misc_TutorialCompanion_OldFaction", 2)) do
            if uuidOf(r[1]) == u then tut = true end
        end
        -- Which dismiss (both BG3's own). The tutorial's proc is a rule of goal TUT_Misc: with that goal
        -- not running (it starts on loading TUT_Avernus_C and ends with the tutorial) the proc has no
        -- rules and does nothing, which is what "BG3 did not dismiss Shadowheart" was in BGO_Main_A.
        local function gone() return not inDbPlayers(u) end
        local inTutorial = tut and goalRunning("TUT_Misc")
        if not inTutorial then
            local why = dismissRefusal(u)
            if why then error(string.format("BG3 would not dismiss %s: %s", nameOf(u), why), 0) end
        end
        -- Off the hero's group first. BG3's dismiss detaches the companion itself and sends it to camp, but a
        -- detach takes hold on the next tick (each one's PartyMember.ViewUuid, PartyView), and in the void BG3's
        -- walk out of sight ends at once: its TeleportTo to camp, linked characters left at their default, took
        -- the whole group along (measured: hero and companions 2988 m in one tick; a detach and a linked
        -- teleport in one tick do it in the void and on any ground, a detach before it moves the companion alone).
        Osi.DetachFromPartyGroup(u)
        waitFor(function() return apart(u, hero) end, 30, "BG3 did not detach " .. nameOf(u) .. " from the hero's group")
        if inTutorial then
            Osi.PROC_TUT_Misc_TutorialCompanion_RemoveCompanion(u, "Dismissed", "WOWBG3")
            steps[#steps + 1] = "dismissed (PROC_TUT_Misc_TutorialCompanion_RemoveCompanion)"
        else
            Osi.PROC_GLO_PartyMembers_Remove(u, hero, 1, 0)
            steps[#steps + 1] = "dismissed (PROC_GLO_PartyMembers_Remove" .. (tut and ", a tutorial companion with TUT_Misc not running" or "") .. ")"
        end
        -- On the log's clock, against Camp.lua's jump lines (which of these teleports took the hero along).
        Ext.Utils.Print("WOW-BG3 party: Remove " .. nameOf(u) .. ": " .. steps[#steps])
        if not pcall(waitFor, gone, 60, "") then
            error(string.format("BG3 did not dismiss %s (tried: %s)", nameOf(u), table.concat(steps, "; ")), 0)
        end
        cacheTick = -1
        wait(2)
        if Prog.CampOn() then
            -- Back to camp, as BG3's own dismiss does (Camp.lua's switch aside: nowhere else is home now).
            Ext.Utils.Print("WOW-BG3 party: Remove " .. nameOf(u) .. ": PROC_DismissToCamp")
            pcall(Osi.PROC_DismissToCamp, u)
            steps[#steps + 1] = "back to camp (PROC_DismissToCamp)"
            return {Removed = nameOf(u), Uuid = u, Steps = steps, Party = M.Snapshot()}
        end
        local home = homes()[u]
        local region = q(Osi.GetRegion, u)
        if home and type(home.Pos) == "table" and #home.Pos == 3 then
            if home.Region ~= region then
                -- Another level: through a character standing there (TeleportTo moves global characters).
                local anchor
                for _, k in ipairs(KNOWN) do
                    local a = uuidOf(k[2])
                    if a ~= u and not M.IsMember(a) and Ext.Entity.Get(a) and q(Osi.GetRegion, a) == home.Region then anchor = a; break end
                end
                if anchor then
                    Osi.TeleportTo(u, anchor, "", 0, 0, 0, 1, 1)
                    local okw = pcall(waitFor, function() return q(Osi.GetRegion, u) == home.Region end, 300, "")
                    if okw then steps[#steps + 1] = "taken back to " .. home.Region .. " (TeleportTo " .. nameOf(anchor) .. ")" end
                end
            end
            if q(Osi.GetRegion, u) == home.Region then
                Osi.TeleportToPosition(u, home.Pos[1], home.Pos[2], home.Pos[3], "", 0, 0, 0, 1, 1)
                if home.OnStage == false then Osi.SetOnStage(u, 0) end
                steps[#steps + 1] = string.format("back where Add found it: %s %.1f %.1f %.1f%s", home.Region, home.Pos[1],
                    home.Pos[2], home.Pos[3], home.OnStage == false and " (off stage)" or "")
            else
                Osi.SetOnStage(u, 0)
                steps[#steps + 1] = "no way back to " .. tostring(home.Region) .. ": off stage where it stands"
            end
        else
            Osi.SetOnStage(u, 0)
            steps[#steps + 1] = "no home recorded: off stage where it stands"
        end
        return {Removed = nameOf(u), Uuid = u, Steps = steps, Party = M.Snapshot()}
    end

    -- Companions at camp (settings.conf `companions`, needs `camp`): every companion not yet with the
    -- party waits at camp once per save, the way BG3's own "fullcamp" does it (GLO_Camp): out of any
    -- tutorial pod, PROC_ORI_SetupCamp (part of the team, on stage, to its camp spot, from another level
    -- too) and PROC_DismissToCamp, with its in-party dialog, whose "join me" BG3 itself runs
    -- (OriginAddToParty, PROC_GLO_PartyMembers_CheckAdd). Withers is BG3's own move to camp.
    local function toCamp(k)
        local u = uuidOf(k[2])
        if not Ext.Entity.Get(u) or M.IsMember(u) or q(Osi.IsDead, u) == 1 then return end
        if not Prog.Once("camp:" .. k[1]) then return end
        if u == uuidOf(SH) then tutorialShadowheart(heroOrFail(), {}) end
        pcall(Osi.PROC_ORI_SetupCamp, u, 1)
        pcall(Osi.PROC_DismissToCamp, u)
        -- Their faction of the story's moment (Minsc's Act 3 gang fights the hero on sight): their own instead.
        for _, r in ipairs(rows("DB_GLO_PartyMembers_DefaultFaction", 2)) do
            if uuidOf(r[1]) == u then pcall(Osi.SetFaction, u, r[2]) end
        end
        local dlg = q(function() return Osi.DB_OriginInPartyDialog:Get(k[2], nil) end)
        if dlg and dlg[1] then
            pcall(Osi.PROC_GLO_PartyMembers_SetInpartyDialog, u, dlg[1][2])
            pcall(Osi.SetHasDialog, u, 1)
        end
        Ext.Utils.Print(string.format("WOW-BG3 party: %s waits at camp (%s)", nameOf(u), tostring(q(Osi.GetRegion, u))))
    end
    local campStarted = false
    local function campStart()
        if campStarted or not Prog.CampOn() or not Ext.Require("Camp.lua").On() then return end
        local hero = Hero.Uuid()
        local active = q(function() return Osi.DB_ActiveCamp:Get(nil) end)
        if not hero or not active or not active[1] then return end -- the story has not set its camp yet
        -- In a level of our own the story knows its camp only in the camp's level (Camp.lua): there, then.
        if q(Osi.IsInCombat, hero) == 1 or (not Hero.OWN and Ext.Require("Camp.lua").InTrip()) then return end
        campStarted = true
        for _, k in ipairs(KNOWN) do
            local ok, err = pcall(toCamp, k)
            if not ok then Ext.Utils.Print("WOW-BG3 party: to camp failed: " .. tostring(err)) end
        end
        if Prog.Once("camp:withers") then
            local ok, err = pcall(function()
                Osi.PROC_GLO_Jergal_MoveToCamp()
                Osi.PROC_GLO_Jergal_Appear()
            end)
            Ext.Utils.Print("WOW-BG3 party: Withers " .. (ok and "at camp" or ("to camp failed: " .. tostring(err))))
        end
    end

    -- Every companion next to the hero (or c.Position) and chained (world mode's Start and Stop).
    local function gather(c)
        local hero = heroOrFail()
        if c.Upkeep ~= nil then upkeep = c.Upkeep == true end
        local at = (type(c.Position) == "table" and #c.Position == 3) and c.Position or pos(hero)
        local heroRegion = q(Osi.GetRegion, hero)
        Ext.Utils.Print("WOW-BG3 party: Gather at " .. (at and string.format("%.1f %.1f %.1f", at[1], at[2], at[3]) or "?")
            .. " (upkeep " .. tostring(upkeep) .. ")")
        local done = {}
        for i, u in ipairs(M.Party()) do
            if u ~= hero then
                if q(Osi.GetRegion, u) ~= heroRegion then
                    -- source, target, event, linked characters, party followers, summons, leave combat, snap
                    Osi.TeleportTo(u, hero, "", 0, 0, 1, 1, 1)
                    pcall(waitFor, function() return q(Osi.GetRegion, u) == heroRegion end, 300, "")
                end
                if q(Osi.IsOnStage, u) ~= 1 then Osi.SetOnStage(u, 1) end
                placeNear(u, at, i - 2)
                done[#done + 1] = nameOf(u)
            end
        end
        if #done > 0 then
            wait(5)
            for _, u in ipairs(M.Party()) do if u ~= hero then pcall(Osi.AttachToPartyGroup, u, hero) end end
        end
        return {Gathered = done, Upkeep = upkeep, At = at}
    end

    local function list()
        local out = {Party = M.Snapshot(), Known = {}, MaxPartySize = q(Osi.GetMaxPartySize),
            Full = q(Osi.GetFlag, MAX_REACHED, NULL) == 1, Upkeep = upkeep}
        for _, k in ipairs(KNOWN) do
            local u = uuidOf(k[2])
            if Ext.Entity.Get(u) then
                out.Known[#out.Known + 1] = {Key = k[1], Name = nameOf(u), Region = q(Osi.GetRegion, u),
                    InParty = M.IsMember(u), Dead = q(Osi.IsDead, u) == 1, OnStage = q(Osi.IsOnStage, u) == 1}
            end
        end
        return out
    end

    -- Refill (see the header).
    local function resourceName(id)
        local ok, n = pcall(function() return Ext.StaticData.Get(id, "ActionResource").Name end)
        return ok and tostring(n) or tostring(id)
    end
    local function low(u)
        local out = {}
        local e = Ext.Entity.Get(u)
        local ok = pcall(function()
            for id, levels in pairs(e.ActionResources.Resources) do
                for _, r in ipairs(levels) do
                    if r.Amount < r.MaxAmount then
                        out[#out + 1] = string.format("%s%s %g/%g", resourceName(id),
                            (r.Level or 0) > 0 and (" " .. r.Level) or "", r.Amount, r.MaxAmount)
                    end
                end
            end
        end)
        if not ok then out[#out + 1] = "(resources unreadable)" end
        table.sort(out)
        return out
    end
    local function topUp(u)
        local e = Ext.Entity.Get(u)
        local n = 0
        pcall(function()
            for _, levels in pairs(e.ActionResources.Resources) do
                for _, r in ipairs(levels) do
                    if r.Amount < r.MaxAmount then r.Amount = r.MaxAmount; n = n + 1 end
                end
            end
            if n > 0 then e:Replicate("ActionResources") end
        end)
        return n
    end
    local function refill()
        local hero = heroOrFail()
        local party = M.Party()
        local before = {}
        for _, u in ipairs(party) do before[u] = {Low = low(u), Hp = member(u, hero).Hp} end
        local okr, err = pcall(Osi.RestoreParty, hero)
        assert(okr, "RestoreParty failed: " .. tostring(err))
        -- With camp off this stands in for a long rest (settings.conf), so vendors restock on it.
        if WOWBG3Inventory and not Ext.Require("Camp.lua").On() then pcall(WOWBG3Inventory.Restock) end
        wait(10)
        local out = {}
        for _, u in ipairs(party) do
            pcall(Osi.ResetCooldowns, u)
            local still = low(u)
            local n = #still > 0 and topUp(u) or 0
            if n > 0 then wait(2) end
            local m = member(u, hero)
            out[#out + 1] = {Name = m.Name, Hero = m.Hero, HpBefore = before[u].Hp, Hp = m.Hp, MaxHp = m.MaxHp,
                LowBefore = before[u].Low, TopUp = still, LowAfter = low(u)}
        end
        return {Refilled = out}
    end

    -- Pool, LevelUp, Reset (see the header; the rules and the save live in Progression.lua).
    -- A member by name: "hero", the hero's or a member's display name, or a companion this tool knows
    -- (a name's first letters do).
    local function memberOf(name)
        if type(name) ~= "string" or #name < 2 then error("Name required (hero, or a party member such as shadowheart)", 0) end
        local hero = heroOrFail()
        local n = name:lower():gsub("[^%a]", "")
        if n == "hero" then return hero end
        for _, u in ipairs(M.Party()) do
            local dn = tostring(nameOf(u)):lower():gsub("[^%a]", "")
            if dn == n or dn:sub(1, #n) == n then return u end
        end
        local ok, who = pcall(resolve, name)
        if ok then
            if not M.IsMember(who.Uuid) then error(nameOf(who.Uuid) .. " is not in the party", 0) end
            return who.Uuid
        end
        error("no party member '" .. name .. "'", 0)
    end
    local function pool()
        Prog.RefreshWowLevel()
        local s = Prog.Status(M.Party())
        s.PartySize = #M.Party()
        return s
    end
    local function levelUp(c)
        local u = memberOf(c.Name)
        if q(Osi.IsInCombat, u) == 1 then error("not during a fight", 0) end
        local n = math.tointeger(c.Count) or 1
        if n < 1 or n > 60 then error("how many level-ups: 1 to 60", 0) end
        local r = n > 1 and Prog.LevelUpMany(u, n, wait) or Prog.LevelUp(u, wait)
        r.Pool = pool()
        return r
    end
    -- Every level-up left, evenly across the party (the pool, the cap and the class choices as ever).
    local function catchup()
        local party = M.Party()
        for _, u in ipairs(party) do
            if q(Osi.IsInCombat, u) == 1 then error("not during a fight", 0) end
        end
        local r = Prog.Catchup(party, wait)
        r.Pool = pool()
        return r
    end
    local function reset(c)
        local r = Prog.Reset(memberOf(c.Name))
        r.Pool = pool()
        return r
    end

    local function writeStatus(force)
        local snap, summons = M.Snapshot(), M.Summons()
        local key = Ext.Json.Stringify({snap, summons})
        if not force and key == lastKey and tick - lastWrite < 300 then return end
        lastKey, lastWrite = key, tick
        Ext.IO.SaveFile("wow-bg3/party.json", Ext.Json.Stringify({RunId = runId, Tick = tick, Hero = Hero.Uuid(),
            Members = snap, Summons = summons, Upkeep = upkeep}))
    end

    -- Origin portraits: BG3's own re-render has its coloured backdrop since the capture layer keeps
    -- `void` off portrait renders (testing saw Tav's respec
    -- portrait come back on green with void on), so the old swap back to the root template's icon
    -- (originPortraits) is gone.

    -- BG3's own dismiss line in a companion's dialog: the rule at that dialog's end runs the same dismiss as
    -- remove above, so the companion leaves the hero's group as soon as the line is chosen.
    pcall(Ext.Osiris.RegisterListener, "DB_GLO_PartyMembers_DismissAfterDialog", 2, "after", function(_, member)
        local u = uuidOf(tostring(member))
        if u then pcall(Osi.DetachFromPartyGroup, u) end
    end)

    Ext.Events.Tick:Subscribe(function()
        if Ext.Utils.GetGameState() ~= "Running" then
            if Ext.Require("Session.lua").Held() then return end -- a save: keep the upkeep state
            upkeep = false
            return
        end
        local job = jobs[1]
        if job then
            local ok, value = coroutine.resume(job.co)
            if not ok or coroutine.status(job.co) == "dead" then
                table.remove(jobs, 1)
                Ext.IO.SaveFile("wow-bg3/party-result.json", Ext.Json.Stringify({RunId = runId, Sequence = job.seq,
                    Action = job.name, OK = ok, Value = ok and value or tostring(value)}))
                Ext.Utils.Print("WOW-BG3 party: " .. job.name .. (ok and " done" or (" failed: " .. tostring(value))))
                cacheTick = -1
                pcall(writeStatus, true)
            end
        end
        if tick % 10 == 3 then pcall(writeStatus, false) end
        if tick % 90 == 31 and not jobs[1] then pcall(campStart) end
        if tick % 10 ~= 0 then return end
        local raw = Ext.IO.LoadFile("wow-bg3/party-command.json")
        local okj, c = pcall(Ext.Json.Parse, raw or "")
        if not okj or type(c) ~= "table" or c.RunId ~= runId or math.type(c.Sequence) ~= "integer" or c.Sequence <= sequence then return end
        sequence = c.Sequence
        local fn = ({List = list, Add = add, Remove = remove, Gather = gather, Refill = refill, Pool = pool,
            LevelUp = levelUp, Catchup = catchup, Reset = reset})[c.Action]
        if not fn then
            Ext.IO.SaveFile("wow-bg3/party-result.json", Ext.Json.Stringify({RunId = runId, Sequence = c.Sequence,
                Action = tostring(c.Action), OK = false, Value = c.Action == "Level"
                    and "Level is retired: BG3 levels come from the WoW levels now (party.sh pool, party.sh levelup NAME)"
                    or "unknown action (List, Add, Remove, Gather, Refill, Pool, LevelUp, Catchup, Reset)"}))
            return
        end
        jobs[#jobs + 1] = {name = c.Action, seq = c.Sequence, co = coroutine.create(function() return fn(c) end)}
    end)
end
return M
