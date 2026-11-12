-- Mirrored WoW mobs (world fights; server context). In world mode every attackable WoW
-- creature near the hero has a BG3 stand-in all the time, at the mob's position and facing on the
-- treadmill ground, so BG3's own targeting, attacks, throws, spells, sight cones, stealth and
-- turn-based mode work on WoW's mobs. WoW keeps drawing the mobs: the capture layer's "void" keep
-- list (TavPlan.PublishKeep) leaves the stand-ins out of the composite.
--
-- Driven by wow-bg3/link/mirror.json (the fight bridge, bridge/src/mirror.rs, every 250 ms):
--   {Seq, Session, Active, HeroWowMaxHp, Hostile = faction, Neutral = faction, Fight = {guid...},
--    Mobs = {{Guid, Template, Kind, Hostile, Pos = {x,y,z} BG3, Dir = {fx,fz} BG3 forward, WowHp,
--             WowMaxHp, WowDamage, BaseDamage, Name}...}}
-- Each listed mob gets one stand-in (Osiris CreateAt of the template, the hostile or neutral
-- faction) with its template's own BG3 max HP at the WoW mob's HP fraction (set in the tick that
-- creates it) and a DamageBonus boost as the world-fights notes says (BG3 still rolls
-- everything), kept where the mob is (TeleportToPosition with
-- snap, turned to its facing) and at its HP fraction while out of BG3 combat. Mobs in the
-- running fight (Fight) are never moved, healed or removed: BG3 runs them. Unlisted stand-ins are
-- removed. Writes wow-bg3/link/mirror-status.json: per guid its stand-in uuid, BG3 HP, dead, and
-- whether it is in Tav's BG3 combat; Tav's HP, combat, turn-based mode (Force Turn-Based) and
-- sneaking. Nothing here is saved; leaving the level or a new bridge session starts over.
-- Guards: the authorized disposable save; spawns only on ground the treadmill placed.
--
-- Fixes from playtesting:
-- - Issue 20 (a stand-in healed to full when Tav opened with her own attack): the out-of-combat HP
--   sync ran between BG3 applying her damage and the AttackedBy event. Now a stand-in Tav aimed at
--   (UsingSpellOnTarget) is not synced for AIM_GUARD ticks, BG3's HP is only raised when the WoW
--   fraction itself rose since the last sync (SyncFrac; a drop BG3 has and WoW has not is kept),
--   and the heal right after scaling runs only while the stand-in is unhurt and out of combat.
-- - Issue 14 (mob attacks played 1.3 to 1.5 s late, 0-damage hits as misses): every attack a
--   stand-in lands on Tav, and every hit of Tav's on a stand-in, goes to benilla at once in
--   link/strikes.bin (see Strikes below); fight.txt's strike lines stay the fallback.
-- - Issue 8 (BG3 names): each stand-in shows its WoW mob's name (Names.lua).
-- More playtest fixes:
-- - Facing: a BG3 character faces along its Transform's rotated -z (found in playtesting, checked
--   on the server's Transform and by BG3's own sight here), but idle stand-ins were turned so +z
--   matched the WoW mob's facing: each looked the opposite way of its mob, so BG3's sight, stealth
--   and vision cones ran backwards. face() now turns -z to the mob's facing, forward() reads -z.
--   M.FaceAxis = 1 (research knob) restores the old +z behaviour.
-- - Tav's casts with the time (wow-bg3/link/casts.txt, the last CAST_KEEP lines, and the SE log),
--   so a problem the player sees at a time maps to the cast and to the other logs: wall clock (the
--   PC's local time), CLOCK_MONOTONIC ms (the layer's and benilla's pacing clock), event (begins, aims,
--   ends, failed), spell, target, BG3's story action id (and for a failure her turn then).
-- Further playtest fixes:
-- - Issue 22 (out of combat the stand-ins trailed the WoW models by about 0.4 s and moved in jumps):
--   positions came benilla mobs.txt (every 12 frames), the bridge's mirror.json (every 300 ms) and
--   this file's step (every 10 server ticks, a teleport once 0.35 m off). Now benilla writes
--   link/wowmobs.bin every frame (each mob where WoW draws it, its velocity, held by a fight or not)
--   and follow() places every idle stand-in each server tick: at the WoW mob's position now (the
--   record moved on by its velocity for its age), with BG3's own TeleportToPosition once M.LiveMove
--   off, so BG3's rules (sight, reach, surfaces, the AI grid) see it where the model is. What the
--   player sees and picks is the client's (MirrorsClient.lua places the client copy every frame).
--   mirror.json's positions stay the fallback without a fresh record, and the spawn point.
-- - Issue 23 (the stand-in faced elsewhere than its model): it turned only after a stop, in
--   12 degree steps, and only on the server (BG3's Steering turns the server copy; the client copy
--   kept its spawn or last teleport rotation, so the vision cone pointed there). Now follow() turns
--   the server copy whenever the model's facing moves M.LiveTurnDeg, while it walks too, and
--   MirrorsClient.lua sets the client copy's rotation every frame.
-- - Issue 27 (a WoW revive left Tav dead): mirror.json's HeroState (benilla's own body: alive,
--   dead, ghost) is mirrored onto Tav at its changes: the WoW character coming back to life
--   brings her back at the WoW HP fraction, the way BG3 does for her state: a dead character with
--   BG3's own Resurrect (then the HP once the Resurrected event comes), a downed one (DOWNED, 0 HP:
--   what a player's character is after a lost fight) as BG3's Help does, DOWNED removed and HP
--   given (Resurrect does nothing to a downed character; harness); the WoW character dying while
--   she lives (a WoW-side death, outside any fight) takes her down the way BG3 takes a player's
--   character down: damage to 0 HP (Osiris ApplyDamage), which leaves her DOWNED (BG3 never lets a
--   player's own character just die: Osiris Die did nothing to her, and her fight deaths leave her
--   DOWNED at 0 HP too, which is what a playtest saw). Down counts as dead here (IsDead, DOWNED or
--   0 HP). Her death in a fight kills the WoW character as before (bridge `hero`). The WoW character
--   dead while she stays up after all (in BG3's combat, or a 0 HP moment she came back from) is revived
--   in place: BG3 decides who lives (Death.lua ReviveWow, asked again until WoW reports it alive).
-- Health (a middle ground for a party of 4): max HP = round(WoW max HP ^
-- HP_CURVE 0.65), an IncreaseMaxHP boost at the scale step (a level 1 wolf 42 -> 11, a level 24
-- wolf 714 -> 72, a level 60 elite 30000 -> 813), then the WoW fraction again. Before that
-- a stand-in kept its template's BG3 health (a Young Wolf 11). The old scaling (WoW HP x the hero's BG3 max HP / the WoW
-- hero's max HP) made Gale's Young Wolf 3 HP, and as it ran a second after the spawn, the hover showed
-- the wolf's health drop 11 -> 3 as she came near. The WoW HP fraction is set (Osiris
-- SetHitpointsPercentage) in the tick of CreateAt: Health reads -1/-1 then, BG3 fills it the next tick
-- and the call takes effect there, so the stand-in never has another HP, on the server or on BG3's
-- client (harness); healthDue checks it in that first filled tick. BG3 rounds a percentage
-- up (50% of 11 is 6, 40% is 5), and the out-of-combat sync compares whole HP the same way (bg3Hp).
-- The damage scaling (DamageBonus at the hero's ratio, the MIN_AVG_HIT floor) is unchanged.
-- Friendly NPCs: vendors, quest givers, guards and villagers get
-- stand-ins too (mirror.json Friendly; Guard for WoW's guards), with the neutral faction and, like
-- neutral mobs, unable to join a combat until Tav attacks that one. Attacking one runs BG3's own
-- flow for attacking someone who is not hostile (the world-fights notes
-- "Friendly NPCs"): the hit is BG3's Assault crime, and the victim reacts by its reaction tag:
-- NOT_MESSING_AROUND (given here to all but guards) plays the narrator's line (dialog
-- NGB_Attack_Assault: "Your violence hasn't gone unnoticed. It's about to be returned in kind.");
-- a guard (BG3's GUARD tag, from the template) calls "To arms!" (GEB_AD_AttackHelp); then BG3 makes
-- it temporarily hostile (TEMPORARILY_HOSTILE) and the combat starts, which the bridge turns into
-- the WoW fight (server patch 0006 turns the NPC on the player). Who else fights is WoW's rule
-- (its creature group only; never guards or bystanders, server patch 0006): every friendly
-- stand-in's crime reactions stay blocked (BlockNewCrimeReactions: no bystander barks,
-- confrontations or arrests) except the one Tav aims at, which is ready to react for
-- FRIENDLY_AIM ticks after her last aim or hit.
-- Help for a friendly NPC (a playtest request): a party member who hits a friendly (green) WoW NPC
-- has WoW's guards within M.GuardRadius and every other friendly NPC within M.HelpRadius on them as
-- well: each turns temporarily hostile to that attacker the way BG3's own victim does
-- (SetRelationTemporaryHostile), may join a combat and enters the attacker's; the bridge joins them to
-- the WoW fight as provoked NPCs (`!GUID`, server patch 0006), and after the fight they go back to
-- peaceful by the same reset as the victim. Neutral (yellow) WoW mobs never call each other (group
-- aggro below) and hostile (red) ones join as they always did: only a friendly victim calls for help.
-- The call lasts while the victim is alive and in its fight: every step, friendly NPCs and guards that
-- are near the victim or its attacker by now (the fight moved, a guard walked by, a stand-in spawned
-- since) join too. Before, only those near at the moment of a hit did.
local M = {}
local Names = Ext.Require("Names.lua")
local Hero = Ext.Require("Hero.lua") -- the hero: the character the player controls
-- The party: the hero's companions are BG3 party members. Their attacks provoke a
-- stand-in like the hero's (WoW's rule: a mob joins when you attack it), a stand-in's attack on one plays
-- early (strike flag 16), and the fight is the party's BG3 combat: a stand-in that spots a companion
-- starts it as one that spots the hero does. Party.lua's lists.
local Party = Ext.Require("Party.lua")
local Scaling = Ext.Require("Scaling.lua")
local Death = Ext.Require("Death.lua")
local KILL_LANDS, REVIVE_AGAIN = 60, 150 -- ticks: ApplyDamage reaching her; between asks for a WoW revive
local STRIKE_PARTY = 16
local REGION = Ext.Require("Hero.lua").REGION -- host level (Hero.lua, host.json)
local DIR = "wow-bg3/link/"
-- Which WoW creatures get stand-ins is the bridge's list, WoW's own visible set (no range or count
-- here: was 60 stand-ins within 42 m of a party member, the bridge's old 45 yd); nearest first.
-- A dead stand-in stays as BG3's corpse this long after the death (ticks; about a WoW corpse's life)
-- while it is on placed ground, though WoW's mob list (living creatures only) drops it: BG3's
-- own loot window opens on it (Inventory.lua fills it with the WoW loot; benilla 0060 sends the click
-- to BG3, the bridge's `c GUID`). Deleted at once, it left only WoW's corpse, and WoW's loot window.
local CORPSE_KEEP = 9000
-- Per party member: the bridge mirrors the mobs round every member (each has
-- its own treadmill), so a spawn may be near any member, and only on ground the treadmill placed
-- (wow-bg3/floor.json): a stand-in spawned past the ground's edge fell and was made again and again
-- (so spawns stay on placed ground).
-- HP guard: a hurt stand-in out of every fight showed full HP for under 0.4 s
-- about 4.5 s after its spawn. Every server tick, a stand-in out of combat that Tav never hurt and whose HP
-- rose above its WoW fraction without this file asking is set back in that same tick (HpGuard), and the
-- first rises are logged with the statuses it had (HpRises in mirror-status.json) so the cause shows.
-- The cause is BG3's own and was measured: an NPC out of combat whose HP dropped heals itself about
-- 4 s later (BG3's story _Disturbance_NPCRestoration: HitpointsChanged -> the shout
-- Shout_RegainHP_Peace_NPC -> the status REGAINHP_PEACE_NPC -> SetHitpointsPercentage 100), and for
-- a stand-in that drop is its own spawn. BG3 switches the same thing off for a character its story
-- sets to a lower HP (PROC_SetHitpointsPercentage_BlockSelfHealing -> PROC_SelfHealing_Disable), and
-- every stand-in made here does that at its spawn: WoW owns a mob's health, BG3 never raises it.
-- M.SelfHeal = true leaves BG3's peace healing on. The guard stays as the backstop, and with the
-- cause gone it should never fire (HpGuard 0, HpRises empty in mirror-status.json).
-- Outfit: BG3 draws a stand-in in its own screens (the trade screen of a WoW vendor, examine), and
-- the human template comes with a sword and a shield but no clothes, so a vendor stood there naked.
-- Every friendly human stand-in is given BG3's own plain robe at its spawn (the stats entry in
-- M.Outfit, made from that entry's root template and worn through Osiris Equip). The robe's armour
-- class is 10 + Dexterity, what the character has with nothing on, so no number of the mob's
-- changes; it is deleted with the stand-in. M.Outfit = nil leaves them as BG3 made them.
local MOVE, TURN = 0.35, math.rad(12) -- metres and radians before a stand-in is moved or turned
local SCALE_AFTER, SCALE_WAIT = 30, 90 -- ticks after a spawn before scaling; ticks to see it take
local GONE_AFTER = 90 -- ticks without a mirror.json before every stand-in goes
local AIM_GUARD = 150 -- ticks after Tav aimed at a stand-in before its HP is synced to WoW again
-- Strikes: link/strikes.bin, rewritten whole at each strike:
--   "STK1", epoch u32 (new per BG3 run), seq u32, n u32, then n x (guid hi u32, guid lo u32,
--   id u32 (rising), flags u32, damage u32, BG3 time f64 ms), then seq again (torn-read check).
-- flags: 1 hit (AttackedBy with a damage type), 2 wound (Tav hit the stand-in; guid is the
-- stand-in's mob), 4 lethal (the stand-in is at 0 HP), 8 miss reported at the cast's end
-- (CastedSpell without an AttackedBy), 16 the victim is a companion, not the hero (benilla plays
-- the swing with BG3's own facing, no victim). Without the wound bit the guid's stand-in attacked Tav or
-- (16) a companion, 32 a thrown weapon (benilla plays its throw, not its swing), 64 with the wound bit: the party's attack
-- missed the stand-in (BG3's MissedBy; benilla plays WoW's dodge). "Tav" in the wound bit is any party member.
local STRIKE_WOUND, STRIKE_DODGE = 2, 64
local STRIKES_KEEP = 16
local MISS_DELAY = 6 -- ticks after CastedSpell before an attack with no AttackedBy counts as a miss
-- Friendly NPCs (see the header): BG3's crime reaction tag for all but guards, and how long (ticks)
-- a friendly stand-in Tav aimed at or hit stays ready to react and to join her combat.
local TAG_NOT_MESSING_AROUND = "542b58f0-42ad-4157-a28e-27434e0e7b18"
local FRIENDLY_AIM = 300

-- Stand-in uuid -> WoW guid. StandIns.lua accepts these as join targets (LinkLive adopts them);
-- a mod-wide global, so every file of this mod in the server context sees the same table.
WOWBG3MirrorOwned = WOWBG3MirrorOwned or {}
M.Owned = WOWBG3MirrorOwned
function M.Owns(uuid) return type(uuid) == "string" and M.Owned[uuid] ~= nil end
-- Facing: the ways tried in order (Osiris ToTransform left the rotation at identity), each
-- checked on the stand-in's rotation a second later; one that turns nothing three times in a row is
-- dropped for the next. Research knobs: the mode in use and a yaw offset in degrees.
local FACE_MODES = {"Steer", "Quat", "ToTransform"}
local FACE_CHECK = 30 -- ticks after a turn before its result is read
M.FaceMode, M.YawOffset, M.FaceStats = FACE_MODES[1], 0.0, {}
-- Which Transform axis a BG3 character looks along: -1 its rotated -z (BG3's), 1 its +z (before
-- an earlier version, wrong: stand-ins looked away from where their WoW mob looks).
M.FaceAxis = -1
local CAST_KEEP = 200
-- Live placement: a wowmobs.bin record this young (ms) places its stand-in;
-- LIVE_LEAD_MAX caps how far (s) a record is moved on by its velocity. The server copy is teleported
-- once M.LiveMove metres off and turned once its wanted facing moved M.LiveTurnDeg (or BG3's turn has
-- not reached it after FACE_CHECK ticks).
local LIVE_FRESH_MS, LIVE_LEAD_MAX = 250, 0.2
local YD = 0.9144
-- Research knobs: Live false goes back to mirror.json's positions alone; LiveMove (m) and LiveTurnDeg
-- are the teleport and turn thresholds; LiveLeadMs moves the server copy's target on by that much
-- more of the mob's motion (the time until the client applies a replicated server change).
M.Live, M.LiveMove, M.LiveTurnDeg, M.LiveLeadMs = true, 0.25, 4, 0
-- Metres a stand-in may stand over or under its creature before the grid's floor there is taken for another's
-- (see follow: the placement without the snap).
M.LiveHeight = 2.5
-- Help for a friendly NPC (see the header): metres round the victim; 0 turns a kind off.
M.GuardRadius, M.HelpRadius = 30.0, 10.0
-- Base movement of every stand-in, percent of its template's (scale step; 100 leaves it).
M.MovementPct = 150
-- BG3's own healing of a hurt NPC out of combat, on stand-ins (see the header, HP guard): off, so a
-- stand-in keeps its WoW mob's health; true leaves BG3's self-healing alone.
M.SelfHeal = false
-- The plain outfit a friendly human stand-in wears: a BG3 stats entry, made from its root template
-- and worn at the spawn. nil leaves the stand-in as BG3's own template made it. Off (seen in a live test):
-- the robe never reached the stand-in (made in the world, ToInventory and Equip in the same tick do
-- nothing, so a loose robe lay by every friendly human), and even a robe equipped by hand does not
-- show: this template draws no equipped body armour in BG3's screens (its own scale mail is equipped and
-- invisible too). The fix is a template that comes dressed, not an outfit.
M.Outfit = nil

local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end
local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function pos(uuid)
    return q(function() local t = Ext.Entity.Get(uuid).Transform.Transform.Translate; return {t[1], t[2], t[3]} end)
end
local function hp(uuid)
    return q(function() local h = Ext.Entity.Get(uuid).Health; return {h.Hp, h.MaxHp} end)
end
local function hdist(a, b) return math.sqrt((a[1] - b[1]) ^ 2 + (a[3] - b[3]) ^ 2) end
-- The HP BG3 gives a character of maxHp for the fraction f with SetHitpointsPercentage: rounded up,
-- at least 1 (harness).
local function bg3Hp(maxHp, f) return math.max(1, math.min(maxHp, math.ceil(maxHp * f - 1e-6))) end
-- A stand-in's BG3 max HP for its WoW mob's (see the header, Health; LinkLive.lua the same).
local HP_CURVE = 0.65
-- Abilities a stand-in kind gets on top of its template's (a showcase): the
-- giant spider template only bites and jumps; BG3's spider web (Target_Web_Spider) and the phase
-- spider's teleport (Target_EtherealJaunt, a bonus action) added with Osiris AddSpell.
local KIND_SPELLS = {spider = {"Target_Web_Spider", "Target_EtherealJaunt"}}
local Abilities = Ext.Require("Abilities.lua") -- the switch `mob_abilities`
local function standInMaxHp(wowMax) return math.max(1, math.floor(wowMax ^ HP_CURVE + 0.5)) end
-- BG3's forward direction (x, z) of an entity, from its rotation quaternion: the rotated -z axis
-- (M.FaceAxis), where a BG3 character looks and walks.
local function forward(uuid)
    return q(function()
        local r = Ext.Entity.Get(uuid).Transform.Transform.RotationQuat
        local x, y, z, w = r[1], r[2], r[3], r[4]
        local s = M.FaceAxis
        return {s * 2 * (x * z + w * y), s * (1 - 2 * (x * x + y * y))}
    end)
end
local function yawOf(d) return d and math.floor(math.deg(math.atan(d[1], d[2])) + 0.5) or nil end
local function angle(a, b)
    local d = math.atan(a[1], a[2]) - math.atan(b[1], b[2])
    while d > math.pi do d = d - 2 * math.pi end
    while d < -math.pi do d = d + 2 * math.pi end
    return math.abs(d)
end
local function readJson(name)
    local raw = Ext.IO.LoadFile(DIR .. name)
    if not raw or #raw > 262144 then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end
-- The outfit (M.Outfit) on a stand-in: BG3's own item, made at its feet, put in its inventory and
-- worn. Returns the item's uuid (kept with the stand-in, so it goes when the stand-in goes).
local function dressUp(uuid)
    local st = M.Outfit and Ext.Stats.Get(M.Outfit)
    local root = st and st.RootTemplate
    if type(root) ~= "string" or root == "" then return nil, "no outfit" end
    local x, y, z = Osi.GetPosition(uuid)
    if not x then return nil, "no position" end
    local item = uuidOf(Osi.CreateAt(root, x, y, z, 0, 0, ""))
    if not item then
        -- As for a stand-in: CreateAt refuses a point outside the level's grid, so the item is made
        -- at the hero's feet instead; it goes into the stand-in's inventory either way.
        local hx, hy, hz = Osi.GetPosition(Hero.Uuid())
        item = hx and uuidOf(Osi.CreateAt(root, hx, hy, hz, 0, 0, ""))
    end
    if not item then return nil, "CreateAt gave no item" end
    Osi.ToInventory(item, uuid, 1, 0, 0)
    Osi.Equip(uuid, item)
    return item
end
local function templateName(t)
    local rt = Ext.Template.GetRootTemplate(uuidOf(t) or t)
    assert(rt and rt.TemplateType == "character", "not a character root template: " .. tostring(t))
    return rt.Name .. "_" .. rt.Id
end

function M.Start(runId)
    local ticks, lastSeen, session = 0, nil, nil
    local mirrors = {} -- guid -> {Uuid, Born, Stage, Want (the mob), Scale, Error}
    local ftb, problems = false, {}
    -- Live placement: benilla's wowmobs.bin, world mode's mapping (world.json).
    local live, liveSeq, liveT = {}, nil, 0
    local world, worldAt = nil, -1000
    M.LiveStats = {Teleports = 0, Turns = 0, Ticks = 0, Records = 0, Stale = 0}
    -- Issue 27: the WoW hero's state as last seen in mirror.json, and what was mirrored onto Tav.
    local heroState = nil
    M.Life = {Resurrected = 0, Killed = 0}
    -- The WoW HP fraction to give her once BG3's resurrection has landed (it restores full HP a
    -- moment after the Resurrect call, over a SetHitpointsPercentage made right after it).
    local reviveFrac, reviveAt = nil, 0
    local killedAt, wowReviveAt = -KILL_LANDS, -REVIVE_AGAIN
    -- BG3's non-lethal hit: on BG3's side the mob lies knocked out at 1 HP, for WoW it is a kill (LinkLive.lua reads this list,
    -- so the bridge sends HP 0: WoW's corpse, loot, XP and respawn). It counts as dead here, and goes as a corpse does.
    M.KnockedOut = {}
    Ext.IO.SaveFile("wow-bg3/link/knocked-out.json", "{}")
    pcall(Ext.Osiris.RegisterListener, "StatusApplied", 4, "after", function(o, status)
        if status ~= "KNOCKED_OUT" then return end
        local u = uuidOf(o)
        if u and (M.Owned[u] or (WOWBG3FightSpawned and WOWBG3FightSpawned[u])) then
            M.KnockedOut[u] = true
            Ext.IO.SaveFile("wow-bg3/link/knocked-out.json", Ext.Json.Stringify(M.KnockedOut))
            pcall(Osi.SetHitpointsPercentage, u, 0.1) -- rounds up to 1 HP, what LinkLive.lua looks for
        end
    end)
    pcall(Ext.Osiris.RegisterListener, "Resurrected", 1, "after", function(c)
        if reviveFrac and Hero.Is(tostring(c)) then
            pcall(Osi.SetHitpointsPercentage, Hero.Uuid(), 100.0 * reviveFrac)
            M.Life.HpSet = string.format("%.0f%% at Resurrected", 100 * reviveFrac)
            reviveFrac = nil
        end
    end)
    local function host() return Hero.Entity() end
    -- Turn-based mode: the IsInFTB tag component on Tav, or the Osiris events.
    local function inFtb()
        local e = host()
        return (e and q(function() return e.IsInFTB ~= nil end)) == true or ftb
    end
    -- Stand-ins Tav has hurt (a kill outside a fight is hers only then).
    local hitByTav = {}
    -- Group aggro as in WoW: a neutral WoW mob fights only when Tav
    -- attacks it herself. BG3 would pull every neutral stand-in near the one she attacked (one
    -- shared faction, TEMPORARILY_HOSTILE), so neutral stand-ins cannot join a combat
    -- (SetCanJoinCombat 0) until Tav targets or hurts that one. Hostile mobs keep BG3's joining.
    local provoked = {}
    local function provoke(uuid)
        if uuid and M.Owned[uuid] and not provoked[uuid] then
            provoked[uuid] = true
            pcall(Osi.SetCanJoinCombat, uuid, 1)
        end
    end
    -- Friendly NPCs (see the header): uuid -> tick of Tav's last aim or hit; its crime reaction is
    -- on (BG3's victim flow) while it is listed here.
    local friendlyAimed = {}
    local function friendlyOf(uuid)
        local g = uuid and M.Owned[uuid]
        local m = g and mirrors[g]
        return m ~= nil and m.Friendly == true
    end
    -- keep: ticks more than FRIENDLY_AIM before its reactions are blocked again (a theft's).
    local function aimFriendly(uuid, keep)
        if not friendlyOf(uuid) then return end
        if not friendlyAimed[uuid] then pcall(Osi.BlockNewCrimeReactions, uuid, 0) end
        friendlyAimed[uuid] = math.max(friendlyAimed[uuid] or 0, ticks + (keep or 0))
    end
    -- BG3's crime flow on friendly stand-ins (playtest reports: robbed NPCs shouted and stood still;
    -- then, with a scripted search walk in place, NPCs wandered after merely spotting her hide, a
    -- pickpocket caught red-handed walked off instead of the fight the narrator announced, and nobody
    -- showed the thief marker or asked her anything). BG3 runs it all itself (the caught dialog, the
    -- fight, the search with its marker, the interrogation) once the stand-in is let: when BG3 registers
    -- a crime against a friendly stand-in (CrimeIsRegistered), it and the friendly stand-ins within
    -- THEFT_RADIUS are BG3's: follow() no longer pins them to their WoW mobs, their crime reactions are
    -- unblocked (aimFriendly), the victim and the guards may join a combat (provoke), mirror-status.json
    -- says Searching (MirrorsClient.lua streams them, benilla 0067 walks their WoW models along). Once
    -- BG3 is done with one (no crime for M.CrimeQuiet ticks and it has stood still for M.CrimeIdle) it
    -- walks back to its post (where it stood when the crime came), and is WoW's again once there for
    -- 60 ticks, or after M.SearchMax ticks; a fight takes over from it as from any idle stand-in.
    -- M.QuietCrimes are left to BG3 with the stand-ins pinned: being seen sneaking registers a
    -- "Sneaking" crime (harness: 10 of them around one pickpocket, none a theft), which BG3 answers with
    -- a look and a bark, not a search; searching on it made the NPCs wander (a playtest report).
    M.QuietCrimes = {Sneaking = true}
    local searching = {} -- stand-in uuid -> {Last, Post, MovedAt, At, Returning, Home, StepAt, Steps, Turn}
    M.SearchMax, M.SearchHome, M.CrimeQuiet, M.CrimeIdle = 5400, 1.5, 600, 450
    -- The search itself is BG3's crime flow: nothing here walks a searcher (AGENTS.md, no imitations). Mirrors only let go of it.
    M.CrimeKinds = {}
    local function search(uuid)
        if not friendlyOf(uuid) then return end
        local s = searching[uuid] or {}
        s.Last, s.Home, s.Returning, s.MovedAt = ticks, nil, nil, ticks
        -- Its post: where it stood when the crime came (pinned to its WoW mob until then; while BG3 has
        -- it, benilla draws the mob where BG3 walks it, so WoW's records follow).
        s.Post = s.Post or pos(uuid)
        if not searching[uuid] then M.Searches = (M.Searches or 0) + 1 end
        searching[uuid] = s
    end
    -- One tick of a stand-in BG3's crime flow has: once BG3 is done with it, the walk back to its post.
    -- In a dialog (DB_DialogSpeakers: BG3's interrogation, or anyone's dialog while the hero speaks)
    -- BG3 is not done: it stands still there, which counted as idle, and it walked off mid-interrogation
    -- (a playtest report).
    local speakersAt, speakers = -1, {}
    local hiddenInDialog = {} -- stand-in uuid -> true while a dialog hides it (the tick below)
    local function inDialog(u)
        if speakersAt ~= ticks then
            speakersAt, speakers = ticks, {}
            for _, row in ipairs(q(function() return Osi.DB_DialogSpeakers:Get(nil, nil, nil) end) or {}) do
                local sp = uuidOf(tostring(row[2]))
                if sp then speakers[sp] = true end
            end
        end
        return speakers[u] == true
    end
    local function searchStep(m)
        local s = searching[m.Uuid]
        local at = s and pos(m.Uuid)
        if not at then return end
        -- What the searcher knows, every 2 s or so: BG3's questioning needs her to see the thief.
        if ticks % 60 == 0 then
            local hero = Hero.Uuid()
            local hp_ = pos(hero)
            local okS, sneaking = pcall(Osi.HasActiveStatus, hero, "SNEAKING")
            local okC, sees = pcall(Osi.CanSee, m.Uuid, hero)
            Ext.Utils.Print(string.format("WOW-BG3 search: %s %.1f m from him, sees him %s, he sneaks %s, dialog %s, back %s, tick %d",
                m.Uuid, hp_ and hdist(at, hp_) or -1, okC and tostring(sees) or "?", okS and tostring(sneaking) or "?",
                tostring(inDialog(m.Uuid)), tostring(s.Returning == true), ticks))
        end
        if inDialog(m.Uuid) then
            s.Last, s.MovedAt = ticks, ticks
            if s.Returning then
                s.Returning = nil
                pcall(Osi.CharacterMoveToPosition, m.Uuid, at[1], at[2], at[3], "Walk", "", 1) -- stay put
            end
            return
        end
        if not s.At or hdist(at, s.At) > 0.3 then s.At, s.MovedAt = at, ticks end
        if not s.Returning and s.Post and ticks - s.Last > M.CrimeQuiet and ticks - s.MovedAt > M.CrimeIdle then
            s.Returning = true
            if hdist(at, s.Post) > M.SearchHome then
                pcall(Osi.CharacterMoveToPosition, m.Uuid, s.Post[1], s.Post[2], s.Post[3], "Walk", "", 1)
            end
        end
    end
    local function searchDone(m, want)
        local s = searching[m.Uuid]
        if not s then return true end
        want = want and s.Post
        if want then
            local at = pos(m.Uuid)
            if at and hdist(at, want) <= M.SearchHome then s.Home = s.Home or ticks else s.Home = nil end
        end
        if ticks - s.Last > M.SearchMax or (s.Returning and s.Home and ticks - s.Home > 60) then
            searching[m.Uuid] = nil
            return true
        end
        return false
    end
    M.Searching = searching
    -- A friendly victim's guards and neighbours on its attacker (see the header). Each one goes the
    -- victim's own way (provoke, aimFriendly), so the same reset makes it peaceful after the fight.
    -- Victims still calling (see the header): uuid -> {Attacker, Tick of the last hit}.
    local calling = {}
    local function callHelp(victim, attacker)
        local vp = pos(victim)
        if not vp or not attacker then return end
        calling[victim] = {Attacker = attacker, Tick = ticks}
        local ap = pos(attacker)
        for _, m in pairs(mirrors) do
            local u = m.Uuid
            local r = m.Guard and M.GuardRadius or M.HelpRadius
            if u ~= victim and m.Friendly and not m.Dead and r > 0 and Ext.Entity.Get(u) then
                local p = pos(u)
                if p and (hdist(p, vp) <= r or (ap and hdist(p, ap) <= r)) then
                    provoke(u)
                    aimFriendly(u)
                    if q(Osi.IsInCombat, u) ~= 1 then
                        pcall(Osi.SetRelationTemporaryHostile, u, attacker)
                        pcall(Osi.EnterCombat, u, attacker)
                        M.HelpCalled = (M.HelpCalled or 0) + 1
                    end
                end
            end
        end
    end
    -- Stand-ins Tav aimed at (tick of her UsingSpellOnTarget): BG3 applies her damage before the
    -- AttackedBy event, so the HP sync leaves them alone for AIM_GUARD ticks.
    local aimed = {}
    local function aimedRecently(uuid) return aimed[uuid] ~= nil and ticks - aimed[uuid] < AIM_GUARD end
    -- Strikes (see STRIKES_KEEP): the last few, each with a rising id.
    local strikeLog, strikeId, strikeSeq = {}, 0, 0
    local strikeEpoch = math.random(1, 0x7fffffff)
    -- A stand-in's attack on Tav in flight: {At = tick of UsingSpellOnTarget, Struck, Done = tick of CastedSpell}.
    local attacking = {}
    local function guidParts(g)
        g = tostring(g or ""):gsub("^0[xX]", "")
        if #g == 0 or #g > 16 or not g:match("^%x+$") then return nil end
        g = string.rep("0", 16 - #g) .. g
        return tonumber(g:sub(1, 8), 16), tonumber(g:sub(9, 16), 16)
    end
    local function strike(uuid, flags, damage)
        local hi, lo = guidParts(M.Owned[uuid])
        if not hi then return end
        strikeId = strikeId + 1
        local dmg = math.max(0, math.min(0x7fffffff, math.floor(tonumber(damage) or 0)))
        strikeLog[#strikeLog + 1] = string.pack("<I4I4I4I4I4d", hi, lo, strikeId, flags, dmg, Ext.Utils.MonotonicTime() * 1.0)
        if #strikeLog > STRIKES_KEEP then table.remove(strikeLog, 1) end
        strikeSeq = strikeSeq + 1
        Ext.IO.SaveFile(DIR .. "strikes.bin", string.pack("<c4I4I4I4", "STK1", strikeEpoch, strikeSeq, #strikeLog)
            .. table.concat(strikeLog) .. string.pack("<I4", strikeSeq))
        M.Strikes = (M.Strikes or 0) + 1
    end
    -- Theft (a playtest report: a robbed merchant never noticed): pickpocketing a friendly stand-in
    -- lets BG3's own crime flow run, the victim's discovery ("someone stole from me") and the search by
    -- it and the friendly NPCs and guards near it, for THEFT_KEEP ticks.
    local THEFT_KEEP, THEFT_RADIUS = 3600, 15.0
    for _, ev in ipairs({{"RequestPickpocket", 2}, {"StartPickpocket", 3}}) do
        pcall(Ext.Osiris.RegisterListener, ev[1], ev[2], "after", function(thief, npc)
            local n = uuidOf(tostring(npc))
            if not (n and friendlyOf(n)) then return end
            aimFriendly(n, THEFT_KEEP)
            local np = pos(n)
            for _, m in pairs(mirrors) do
                local p = m.Friendly and m.Uuid ~= n and pos(m.Uuid)
                if p and np and hdist(p, np) <= THEFT_RADIUS then aimFriendly(m.Uuid, THEFT_KEEP) end
            end
            M.Pickpockets = (M.Pickpockets or 0) + 1
        end)
    end
    local okInv, errInv = pcall(Ext.Osiris.RegisterListener, "OnCrimeInvestigatorSwitchedState", 4, "after", function(crime, who, from, to)
        Ext.Utils.Print(string.format("WOW-BG3 crime %s: %s, %s to %s, tick %d", tostring(crime), tostring(who), tostring(from), tostring(to), ticks))
    end)
    if not okInv then Ext.Utils.Print("WOW-BG3 crime: no investigator states in the log: " .. tostring(errInv)) end
    for _, ev in ipairs({{"DialogStarted", 2}, {"DialogEnded", 2}, {"DialogRequestFailed", 2}}) do
        pcall(Ext.Osiris.RegisterListener, ev[1], ev[2], "after", function(a, b)
            Ext.Utils.Print(string.format("WOW-BG3 crime: %s %s %s, tick %d", ev[1], tostring(a), tostring(b), ticks))
        end)
    end
    -- A push on a stand-in is BG3's (a playtest shove: "skipped being pushed"): the live hold teleported it back
    -- at once. BG3 registers a shove's crime only when the force ends, too late for `searching` to let go of it.
    local forced = {}
    pcall(Ext.Osiris.RegisterListener, "ForceMoveStarted", 3, "after", function(_, target)
        local t = uuidOf(tostring(target))
        if not (t and M.Owned[t]) then return end
        forced[t] = true
        if friendlyOf(t) then search(t) end
    end)
    pcall(Ext.Osiris.RegisterListener, "ForceMoveEnded", 3, "after", function(_, target)
        local t = uuidOf(tostring(target))
        if t then forced[t] = nil end
    end)
    -- The crime BG3 registers (victim, type, id, evidence, up to four criminals): a friendly victim and
    -- its friendly neighbours search (see `searching`).
    M.CrimeListener = select(2, pcall(Ext.Osiris.RegisterListener, "CrimeIsRegistered", 8, "after", function(victim, kind)
        local v = uuidOf(tostring(victim))
        if not (v and friendlyOf(v)) then return end
        M.LastCrime = {Victim = v, Kind = tostring(kind), Tick = ticks}
        M.CrimeKinds[tostring(kind)] = (M.CrimeKinds[tostring(kind)] or 0) + 1
        -- BG3's guard questions only a thief she can see; the logs could not say why none ever came.
        local hero = Hero.Uuid()
        local okS, sneaking = pcall(Osi.HasActiveStatus, hero, "SNEAKING")
        local okC, sees = pcall(Osi.CanSee, v, hero)
        Ext.Utils.Print(string.format("WOW-BG3 crime: %s on %s, tick %d, thief sneaking %s, sees him %s", tostring(kind), v, ticks,
            okS and tostring(sneaking) or "?", okC and tostring(sees) or "?"))
        if M.QuietCrimes[tostring(kind)] then return end
        search(v)
        provoke(v)
        aimFriendly(v, M.SearchMax)
        local vp = pos(v)
        for _, m in pairs(mirrors) do
            local p = m.Friendly and m.Uuid ~= v and not m.Dead and pos(m.Uuid)
            if p and vp and hdist(p, vp) <= THEFT_RADIUS then
                search(m.Uuid)
                aimFriendly(m.Uuid, M.SearchMax)
                if m.Guard then provoke(m.Uuid) end
            end
        end
    end))
    -- AttackedBy (defender, attacker owner, attacker, damage type, damage, cause, story action).
    pcall(Ext.Osiris.RegisterListener, "AttackedBy", 7, "after", function(defender, owner, attacker, damageType, damage)
        local uuid = uuidOf(tostring(defender))
        local kind = tostring(damageType or "")
        local typed = kind ~= "" and kind ~= "None"
        if uuid and M.Owned[uuid] and (Party.IsFighter(tostring(owner)) or Party.IsFighter(tostring(attacker))) then
            hitByTav[uuid] = true
            provoke(uuid)
            aimFriendly(uuid)
            local by = uuidOf(tostring(attacker))
            if not (by and Party.IsFighter(by)) then by = uuidOf(tostring(owner)) end
            Party.NoteAttack(uuid, by)
            if friendlyOf(uuid) then pcall(callHelp, uuid, by) end
            local h = hp(uuid)
            pcall(strike, uuid, 2 | (typed and 1 or 0) | ((h and h[1] <= 0) and 4 or 0), damage)
        elseif uuid and Party.IsFighter(uuid) then
            local a = uuidOf(tostring(attacker))
            if not (a and M.Owned[a]) then a = uuidOf(tostring(owner)) end
            if a and M.Owned[a] then
                local thrown = attacking[a] and attacking[a].Thrown and 32 or 0
                if attacking[a] then attacking[a].Struck = true end
                pcall(strike, a, (typed and 1 or 0) | (uuid ~= Hero.Uuid() and STRIKE_PARTY or 0) | thrown, damage)
            end
        end
    end)
    -- BG3's own event for an attack roll that missed (defender, attacker owner, attacker, story action; Larian's goals read it
    -- so too, GLO_DaisyDreams_PostEA): a save or a resist does not raise it. The stand-in's WoW model dodges.
    -- A miss on the party needs nothing here: every party member is drawn with BG3's own figure and BG3's reaction.
    local okMiss, errMiss = pcall(Ext.Osiris.RegisterListener, "MissedBy", 4, "after", function(defender, owner, attacker)
        local uuid = uuidOf(tostring(defender))
        if uuid and M.Owned[uuid] and (Party.IsFighter(tostring(owner)) or Party.IsFighter(tostring(attacker))) then
            pcall(strike, uuid, STRIKE_WOUND | STRIKE_DODGE, 0)
        end
    end)
    if not okMiss then Ext.Utils.Print("WOW-BG3 strikes: no MissedBy, a miss plays no dodge: " .. tostring(errMiss)) end
    -- Before the action lands (attack, spell, Shove, Throw at it): it may join the combat.
    pcall(Ext.Osiris.RegisterListener, "UsingSpellOnTarget", 6, "after", function(caster, target, spell)
        local c, t = uuidOf(tostring(caster)), uuidOf(tostring(target))
        if c and Party.IsFighter(c) then
            provoke(t)
            aimFriendly(t)
            if t and M.Owned[t] then
                aimed[t] = ticks
                Party.NoteAttack(t, c)
            end
        elseif c and M.Owned[c] and t and Party.IsFighter(t) then
            local okB, base = pcall(function() return Ext.Require("Abilities.lua").BaseOf(tostring(spell)) end)
            local name = okB and base or tostring(spell)
            attacking[c] = {At = ticks, Struck = false, Party = t ~= Hero.Uuid(), Thrown = name:find("Javelin") ~= nil or name:find("Throw") ~= nil}
        end
    end)
    -- The end of a stand-in's attack on Tav: with no AttackedBy it missed (sent MISS_DELAY ticks later).
    pcall(Ext.Osiris.RegisterListener, "CastedSpell", 5, "after", function(caster)
        local c = uuidOf(tostring(caster))
        if c and attacking[c] and not attacking[c].Done then attacking[c].Done = ticks end
    end)
    -- Tav's casts with the time (see the header).
    local castLog = {}
    local function castNote(event, spell, target, action)
        -- SE has no os library: Ext.Timer.ClockTime is the PC's local wall clock, not UTC (benilla's log is
        -- UTC; the two differed by the PC's time zone in 0.1.3013.61).
        local okd, wall = pcall(function() return Ext.Timer.ClockTime() end)
        local okm, mono = pcall(Ext.Utils.MonotonicTime)
        local line = string.format("%s local mono %.0f %s %s%s%s", okd and tostring(wall) or "?", okm and mono or -1, event,
            tostring(spell or "-"), target and (" on " .. tostring(target)) or "",
            action ~= nil and (" (story action " .. tostring(action) .. ")") or "")
        castLog[#castLog + 1] = line
        if #castLog > CAST_KEEP then table.remove(castLog, 1) end
        Ext.IO.SaveFile(DIR .. "casts.txt", table.concat(castLog, "\n") .. "\n")
        Ext.Utils.Print("WOW-BG3 cast: " .. line)
    end
    local function isTav(c) return Hero.Is(tostring(c)) end
    -- UsingSpell, CastedSpell and CastSpellFailed: (caster, spell, spell type, element, story action id).
    pcall(Ext.Osiris.RegisterListener, "UsingSpell", 5, "after", function(caster, spell, _, _, action)
        if isTav(caster) then pcall(castNote, "begins", spell, nil, action) end
    end)
    pcall(Ext.Osiris.RegisterListener, "UsingSpellOnTarget", 6, "after", function(caster, target, spell)
        if isTav(caster) and not isTav(target) then pcall(castNote, "aims", spell, target) end
    end)
    pcall(Ext.Osiris.RegisterListener, "UsingSpellAtPosition", 8, "after", function(caster, x, y, z, spell)
        if isTav(caster) then pcall(castNote, "aims", spell, string.format("%.1f %.1f %.1f", x or 0, y or 0, z or 0)) end
    end)
    pcall(Ext.Osiris.RegisterListener, "CastedSpell", 5, "after", function(caster, spell, _, _, action)
        if isTav(caster) then pcall(castNote, "ends", spell, nil, action) end
    end)
    -- A failed cast is not always a failed click: BG3 also fails Target_MainHandAttack by itself when the
    -- hero's turn ends or starts (its default-attack preview dropped; m3-native-gate.md, story action 0).
    -- 0.1.3013.61: all five "failed" lines of a test fell on turn changes and every attack click cast, but
    -- the line could not say so. Now it carries the story action id and her turn then (yes, ending: she
    -- asked to end it, no: not hers, -: she is in no fight).
    local function turnNow()
        if q(Osi.IsInCombat, Hero.Uuid()) ~= 1 then return "-" end
        local ok, t = pcall(function() return Ext.Entity.Get(Hero.Uuid()).TurnBased end)
        if not ok or not t then return "?" end
        if not t.IsActiveCombatTurn then return "no" end
        return t.RequestedEndTurn and "ending" or "yes"
    end
    pcall(Ext.Osiris.RegisterListener, "CastSpellFailed", 5, "after", function(caster, spell, _, _, action)
        if isTav(caster) then
            local okt, turn = pcall(turnNow)
            pcall(castNote, "failed", spell, nil, tostring(action) .. ", her turn " .. (okt and turn or "?"))
        end
    end)
    local function missedAttacks()
        for c, a in pairs(attacking) do
            if a.Struck or ticks - a.At > 600 then
                attacking[c] = nil
            elseif a.Done and ticks - a.Done >= MISS_DELAY then
                attacking[c] = nil
                pcall(strike, c, 8 | (a.Party and STRIKE_PARTY or 0) | (a.Thrown and 32 or 0), 0)
            end
        end
    end
    for _, ev in ipairs({"EnteredForceTurnBased", "LeftForceTurnBased"}) do
        pcall(Ext.Osiris.RegisterListener, ev, 1, "after", function(o)
            if Hero.Is(tostring(o)) then ftb = ev == "EnteredForceTurnBased" end
        end)
    end
    local function note(text)
        problems[#problems + 1] = text
        if #problems > 16 then table.remove(problems, 1) end
    end
    -- Spawn and removal counts per mob (stand-ins at the ground's edge were made again and
    -- again): Churn in mirror-status.json, the mobs whose stand-in was made more than once.
    M.Spawns, M.Removes, M.Churn = 0, 0, {}
    local function remove(guid)
        local m = mirrors[guid]
        mirrors[guid] = nil
        if not m then return end
        M.Removes = M.Removes + 1
        M.Owned[m.Uuid] = nil
        M.KnockedOut[m.Uuid] = nil
        hitByTav[m.Uuid] = nil
        Party.ForgetAttacks(m.Uuid)
        provoked[m.Uuid] = nil
        friendlyAimed[m.Uuid] = nil
        aimed[m.Uuid], attacking[m.Uuid] = nil, nil
        if m.Outfit and Ext.Entity.Get(m.Outfit) then pcall(Osi.RequestDelete, m.Outfit) end
        if Ext.Entity.Get(m.Uuid) then pcall(Osi.RequestDeleteTemporary, m.Uuid) end
    end
    local function removeAll()
        for guid in pairs(mirrors) do remove(guid) end
    end
    -- The WoW mob's name on its stand-in; tried again while benilla has no name yet.
    local function nameOf(m, mob)
        if m.Named or (m.NameTries or 0) >= 3 or type(mob.Name) ~= "string" or mob.Name == "" then return end
        m.NameTries = (m.NameTries or 0) + 1
        local how, info = Names.Apply(m.Uuid, mob.Name)
        if how then m.Named, m.NameText = how, info else m.NameError = info end
    end
    -- The WoW mob's HP fraction (mirror.json WowHp / WowMaxHp), at least 1%.
    local function wowFraction(mob)
        local mx = tonumber(mob.WowMaxHp) or 0
        if mx <= 0 then return nil end
        return math.max(0.01, math.min(1.0, (tonumber(mob.WowHp) or mx) / mx))
    end
    -- A new stand-in's health (see the header), checked in the first server tick its Health has
    -- a max (HpInit: ticks after the spawn, HP, max): not at the WoW fraction (HpDue) yet, it is set
    -- again there (HpInit[4] "set again"; not seen in the harness).
    local function healthDue(m)
        Scaling.Without(m.Uuid) -- again once BG3 has filled the character in (see Scaling.lua)
        local h = hp(m.Uuid)
        if not (h and type(h[2]) == "number" and h[2] > 0) then
            if ticks - m.Born > 90 then m.HpWait, m.HpInit = nil, "no Health after 90 ticks" end
            return
        end
        m.HpInit = {ticks - m.Born, h[1], h[2]}
        if m.HpDue and h[1] ~= bg3Hp(h[2], m.HpDue) then
            pcall(Osi.SetHitpointsPercentage, m.Uuid, 100.0 * m.HpDue)
            m.HpInit[4] = "set again"
        end
        m.HpWait, m.HpDue = nil, nil
    end
    local function spawn(mob)
        local t = templateName(mob.Template)
        local p = mob.Pos
        local made = Osi.CreateAt(t, p[1], p[2] + 1, p[3], 1, 0, "")
        if not uuidOf(made) then
            -- CreateAt refuses points outside the level's grid (far from the void arena's centre);
            -- BG3 creates it at Tav's feet and teleports it there (default far centre).
            local x, y, z = Osi.GetPosition(Hero.Uuid())
            made = x and Osi.CreateAt(t, x, y + 1, z, 1, 0, "")
            if uuidOf(made) then
                pcall(Osi.TeleportToPosition, uuidOf(made), p[1], p[2] + 0.5, p[3], "", 0, 0, 0, 0, 1)
            end
        end
        local uuid = assert(uuidOf(made), "CreateAt returned no character: " .. tostring(made))
        Scaling.Without(uuid) -- passives of BG3's template a stand-in does without (Undead Fortitude)
        -- Its health from the start: the template's own max HP at the WoW fraction, set in this
        -- tick (BG3 applies it when it fills Health, the next tick; healthDue checks).
        local frac = wowFraction(mob)
        local born = hp(uuid)
        -- BG3 heals a hurt NPC out of combat by itself (see the header, HP guard): off for every
        -- stand-in before its health is set, the way BG3's own story does it. The whole GUIDSTRING
        -- CreateAt gave, since the story keeps the character in a database of its own.
        if M.SelfHeal ~= true then pcall(Osi.PROC_SelfHealing_Disable, made) end
        -- A vision cone on every mob: BG3 draws none for a template with SpotSneakers off (its boar,
        -- rat and other animals), so the stand-in gets BG3's own flag whatever its template.
        pcall(Osi.SetCanSpotSneakers, uuid, 1)
        if frac and frac < 1 then pcall(Osi.SetHitpointsPercentage, uuid, 100.0 * frac) end
        for _, s in ipairs(KIND_SPELLS[mob.Kind] or {}) do pcall(Osi.AddSpell, uuid, s, 0, 1) end
        pcall(Abilities.Grant, uuid, mob.Entry)
        local plan = readJson("mirror.json") or {}
        local faction = mob.Hostile and plan.Hostile or plan.Neutral
        if type(faction) == "string" and uuidOf(faction) then Osi.SetFaction(uuid, faction) end
        M.Owned[uuid] = mob.Guid
        if not mob.Hostile then pcall(Osi.SetCanJoinCombat, uuid, 0) end
        local friendly = mob.Friendly == true
        local outfit, outfitError
        if friendly then
            -- BG3's crime reactions off until Tav attacks this one (see the header).
            pcall(Osi.BlockNewCrimeReactions, uuid, 1)
            if mob.Guard ~= true then pcall(Osi.SetTag, uuid, TAG_NOT_MESSING_AROUND) end
            -- Clothes on the ones BG3 shows in its own screens (see the header, Outfit): the human
            -- template, which is the one that comes bare.
            if mob.Kind == "human" then
                local ok, item, why = pcall(dressUp, uuid)
                if ok then outfit, outfitError = item, why else outfitError = tostring(item) end
            end
        end
        M.Spawns = M.Spawns + 1
        M.Churn[mob.Guid] = (M.Churn[mob.Guid] or 0) + 1
        mirrors[mob.Guid] = {Uuid = uuid, Born = ticks, Stage = "spawned", Want = mob, Hostile = mob.Hostile,
            Friendly = friendly, Guard = friendly and mob.Guard == true, SpawnHp = born, SpawnFrac = frac,
            Outfit = outfit, OutfitError = outfitError,
            HpWait = true, HpDue = frac and frac < 1 and frac or nil}
        healthDue(mirrors[mob.Guid])
        nameOf(mirrors[mob.Guid], mob)
    end
    -- Damage ("keep WoW's ratios"): the average hit is the WoW mob's times Tav's BG3 max HP / the WoW
    -- hero's max HP (a DamageBonus boost; BG3 still rolls hits and damage). The average hit never drops
    -- under MIN_AVG_HIT: BG3's AI does not attack with an expected damage of about 0. Health: max HP
    -- standInMaxHp(WoW max HP) (IncreaseMaxHP; the "scaled" stage sets the WoW fraction after).
    local MIN_AVG_HIT = 1
    -- The switch `scaling` (Scaling.lua): the stand-in's numbers from BG3's curves at its own level, set once.
    local function scaleByRules(m, info)
        local sp = m.Want
        local base = m.ScaleBase or hp(m.Uuid)
        local wowMax, level = tonumber(sp.WowMaxHp) or 0, tonumber(sp.Level) or 0
        if not base or base[2] <= 0 or wowMax <= 0 or level <= 0 then info.Skipped = "no numbers"; return end
        m.ScaleBase = base
        local fixed = Scaling.Fixed()
        local n = Scaling.Compute({Level = level, WowMaxHp = wowMax, WowDamage = tonumber(sp.WowDamage), Map = world and world.Map,
            Cap = fixed.Cap, Difficulty = fixed.Difficulty})
        local okAc, ac = pcall(function() return Ext.Entity.Get(m.Uuid).Stats.ArmorClass end)
        -- The stand-in's AC only before our boost is on it.
        if m.ScaleAC == nil then m.ScaleAC = okAc and type(ac) == "number" and ac or false end
        local boosts, bonus = Scaling.Boosts(n, sp.Kind, base[2], tonumber(sp.BaseDamage) or 0, m.ScaleAC or nil)
        -- The same string again changes nothing: taken off and put on in one tick, a stand-in was left without it.
        if boosts ~= m.ScaleBoosts then
            if m.ScaleBoosts then pcall(Osi.RemoveBoosts, m.Uuid, m.ScaleBoosts, 0, "WOWBG3_SCALE", m.Uuid) end
            if boosts ~= "" then Osi.AddBoosts(m.Uuid, boosts, "WOWBG3_SCALE", m.Uuid) end
        end
        m.ScaleBoosts, m.ScaleKey = boosts, true
        info.MaxHp, info.DamageBonus, info.Base, info.Boosts, info.BaseAC = n.MaxHp, bonus, base, boosts, m.ScaleAC
        info.Rank, info.E, info.Rel, info.Damage, info.AC, info.ToHit, info.DC = n.Rank, n.E, n.Rel, n.Damage, n.AC, n.ToHit, n.DC
        info.Health = "rules"
    end
    local function scale(m, heroWowMax)
        local sp, info = m.Want, {}
        m.Scale = info
        -- Movement (a playtest request): every stand-in walks M.MovementPct of its template's base
        -- movement a turn (BG3's own multiplier, as Dash's 200).
        if M.MovementPct and M.MovementPct ~= 100 and not m.ScaleKey then
            local b = string.format("ActionResourceMultiplier(Movement,%d,0)", M.MovementPct)
            local okm = pcall(Osi.AddBoosts, m.Uuid, b, "WOWBG3_MOVE", m.Uuid)
            info.Movement = okm and M.MovementPct or "failed"
        end
        if Scaling.On() then return scaleByRules(m, info) end
        local tav = hp(Hero.Uuid())
        local wowMax = tonumber(sp.WowMaxHp) or 0
        if not (tav and heroWowMax and heroWowMax > 0 and wowMax > 0) then info.Skipped = "no numbers"; return end
        local ratio = tav[2] / heroWowMax
        local bonus = 0
        if type(sp.WowDamage) == "number" and type(sp.BaseDamage) == "number" then
            bonus = math.floor(sp.WowDamage * ratio - sp.BaseDamage + 0.5)
            -- Floor: at an average hit near 0 BG3's AI never attacks (a Young Wolf
            -- at DamageBonus(-7) only dashed); keep the average hit at MIN_AVG_HIT or more.
            local floor = math.ceil(MIN_AVG_HIT - sp.BaseDamage)
            if bonus < floor then bonus, info.Floored = floor, true end
        end
        bonus = math.max(-200, math.min(200, bonus))
        local base = hp(m.Uuid) or {0, 0}
        local maxHp = standInMaxHp(wowMax)
        local boosts = {}
        if base[2] > 0 and maxHp ~= base[2] then
            boosts[#boosts + 1] = string.format("IncreaseMaxHP(%d)", maxHp - base[2])
        else
            maxHp = base[2]
        end
        if bonus ~= 0 then boosts[#boosts + 1] = string.format("DamageBonus(%d)", bonus) end
        if #boosts > 0 then Osi.AddBoosts(m.Uuid, table.concat(boosts, ";"), "WOWBG3_SCALE", m.Uuid) end
        info.Ratio, info.MaxHp, info.DamageBonus, info.Base, info.Boosts = ratio, maxHp, bonus, base, boosts
        info.Health = "wow^" .. HP_CURVE
    end
    -- Facing (see FACE_MODES): the shared Steering component's target rotation (BG3 turns the
    -- character itself), the server Transform's rotation written and replicated, or Osiris
    -- ToTransform (degrees about +Y). Yaw a turns BG3's +z axis to (sin a, cos a), so its -z (where
    -- the character looks, M.FaceAxis) to -(sin a, cos a): a = atan(-dir) for a look along dir.
    local function nextFaceMode(why)
        note("face " .. tostring(M.FaceMode) .. ": " .. tostring(why))
        local nextMode = "none"
        for i, f in ipairs(FACE_MODES) do
            if f == M.FaceMode then nextMode = FACE_MODES[i + 1] or "none" end
        end
        M.FaceMode = nextMode
    end
    local function face(m, dir)
        if M.FaceMode == "none" or type(dir) ~= "table" then return end
        local s = M.FaceAxis
        local yaw = math.atan(s * dir[1], s * dir[2]) + math.rad(M.YawOffset)
        local mode = M.FaceMode
        local ok, err = pcall(function()
            local e = Ext.Entity.Get(m.Uuid)
            if mode == "Steer" then
                local st = assert(e.Steering, "no Steering component")
                st.TargetRotation = yaw
                st.RequestSteering = 1
            elseif mode == "Quat" then
                e.Transform.Transform.RotationQuat = {0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2)}
                e:Replicate("Transform")
            else
                local p = assert(pos(m.Uuid), "no position")
                Osi.ToTransform(m.Uuid, p[1], p[2], p[3], 0.0, math.deg(yaw), 0.0)
            end
        end)
        if not ok then nextFaceMode(err); return end
        m.FaceCheck = {At = ticks, Dir = dir, Mode = mode}
    end
    -- A turn's result: did the stand-in's forward reach the wanted direction?
    local function checkFace(m)
        local c = m.FaceCheck
        if ticks - c.At < FACE_CHECK then return end
        m.FaceCheck = nil
        local f = forward(m.Uuid)
        if c.Mode ~= M.FaceMode or not f then return end
        local st = M.FaceStats[c.Mode] or {Ok = 0, Bad = 0}
        M.FaceStats[c.Mode] = st
        if angle(f, c.Dir) <= TURN then st.Ok = st.Ok + 1 else st.Bad = st.Bad + 1 end
        -- Wanted and reached yaw in degrees (a constant gap would mean another yaw convention).
        st.Last = {math.floor(math.deg(math.atan(c.Dir[1], c.Dir[2])) + 0.5), math.floor(math.deg(math.atan(f[1], f[2])) + 0.5)}
        if st.Ok == 0 and st.Bad >= 3 then nextFaceMode("the stand-in did not turn") end
    end
    local function combatOf(uuid)
        if q(Osi.IsInCombat, uuid) ~= 1 then return nil end
        return q(Osi.CombatGetGuidFor, uuid)
    end

    -- link/wowmobs.bin (benilla 0025): "WMB1", seq u32, n u32, CLOCK_MONOTONIC ms f64, n x (guid hi
    -- u32, guid lo u32, WoW x, y, z, orientation, velocity x, y, z f32, flags u32: 1 held by a BG3
    -- fight, 2 moving), seq again.
    local function readLive()
        local raw = Ext.IO.LoadFile(DIR .. "wowmobs.bin")
        if not raw or #raw < 24 or raw:sub(1, 4) ~= "WMB1" then return end
        local seq, n, t = string.unpack("<I4I4d", raw, 5)
        if seq == liveSeq or n > 512 or #raw ~= 24 + 40 * n or string.unpack("<I4", raw, 21 + 40 * n) ~= seq then return end
        local out = {}
        for i = 0, n - 1 do
            local hi, lo, x, y, z, o, vx, vy, vz, fl = string.unpack("<I4I4fffffffI4", raw, 21 + 40 * i)
            out[string.format("%08x%08x", hi, lo)] = {x, y, z, o, vx, vy, vz, fl}
        end
        live, liveSeq, liveT = out, seq, t
        M.LiveStats.Records = n
    end
    local function toBg3(w)
        local C, W = world.Centre, world.WowOrigin
        return {C[1] + (w[2] - W[2]) * YD, C[2] + (w[3] - W[3]) * YD, C[3] - (w[1] - W[1]) * YD}
    end
    -- The WoW mob now, as world mode's BG3 point and forward (x, z): its record moved on by its
    -- velocity for the record's age. Nil without a fresh record or for a mob a fight holds.
    local function liveWant(guid)
        if not (M.Live and world and liveSeq) then return nil end
        local r = live[guid]
        local age = Ext.Utils.MonotonicTime() - liveT
        if not r or age > LIVE_FRESH_MS or (r[8] & 1) ~= 0 then return nil end
        local dt = math.max(0.0, math.min(LIVE_LEAD_MAX, (age + M.LiveLeadMs) / 1000))
        local p = toBg3({r[1] + r[5] * dt, r[2] + r[6] * dt, r[3] + r[7] * dt})
        -- WoW faces (cos o, sin o); the mapping turns it into BG3's (sin o, -cos o).
        return p, {math.sin(r[4]), -math.cos(r[4])}
    end
    -- Every server tick: each idle stand-in where its WoW mob is now, facing its way.
    local function follow()
        M.LiveStats.Ticks = M.LiveStats.Ticks + 1
        if ticks - worldAt >= 30 then
            worldAt = ticks
            local w = readJson("world.json")
            world = (w and w.Active == true and type(w.Centre) == "table" and type(w.WowOrigin) == "table") and w or nil
        end
        readLive()
        if not world then return end
        -- A stand-in in a BG3 dialog is hidden (Osiris SetVisible 0) until it ends: the dialog camera
        -- comes close, and over the WoW model there its BG3 eyes and eyelashes still showed (a playtest
        -- report; harness: the caught-stealing dialog on Duokna).
        if ticks % 5 == 0 then
            for u in pairs(hiddenInDialog) do
                if not inDialog(u) then
                    hiddenInDialog[u] = nil
                    pcall(Osi.SetVisible, u, 1)
                    Ext.Utils.Print(string.format("WOW-BG3 dialog: %s out, tick %d", u, ticks))
                end
            end
        end
        for guid, m in pairs(mirrors) do
            if ticks % 5 == 0 and not hiddenInDialog[m.Uuid] and inDialog(m.Uuid) then
                hiddenInDialog[m.Uuid] = true
                pcall(Osi.SetVisible, m.Uuid, 0)
                M.DialogHides = (M.DialogHides or 0) + 1
                -- What it holds, for the shine on a shield in a dialog (no recording; layer.log's "void:" lines).
                Ext.Utils.Print(string.format("WOW-BG3 dialog: %s in, hand %s, off hand %s, tick %d", m.Uuid,
                    tostring(q(Osi.GetEquippedItem, m.Uuid, "MeleeMainHand")),
                    tostring(q(Osi.GetEquippedItem, m.Uuid, "MeleeOffHand")), ticks))
            end
            local p, dir = liveWant(guid)
            m.LiveAt = p and ticks or m.LiveAt
            -- A stand-in in BG3's crime flow is BG3's (see `searching`), not after its WoW mob (whose
            -- record benilla marks held meanwhile, so p is nil then).
            if searching[m.Uuid] and not searchDone(m, true) then
                if not m.InFight and not m.Dead and q(Osi.IsInCombat, m.Uuid) ~= 1 then searchStep(m) end
                p = nil
            end
            if p and not m.InFight and not m.Dead and not forced[m.Uuid] then
                -- BG3 runs a stand-in in its combat: checked every tick, a fight can start any time.
                if q(Osi.IsInCombat, m.Uuid) ~= 1 and q(Osi.IsDead, m.Uuid) ~= 1 then
                    local at = pos(m.Uuid)
                    if at then
                        -- A stand-in stands where its creature stands, at its height. BG3's snap puts a
                        -- character on the walk grid's floor in that column, and the grid has no tile where
                        -- no walker goes: behind a counter, on a mine's floor under a road while the party is
                        -- above, in a city under a mountain seen from its roof. The snap then took another
                        -- layer in the column (Stormwind's auctioneers 35 m under their hall on the buried
                        -- terrain, Moonbrook's miners on the street over their mine, Ironforge's guards on the
                        -- peak: 6 of 50, 10 of 37, 12 of 38 stand-ins), out of reach of every click and
                        -- path, or standing unseen beside the party. Such a stand-in is placed without the
                        -- snap, onto WoW's own face there, which BG3's rays find (measured: all of those
                        -- stood at their creatures and stayed). One that sinks again has nothing of WoW's
                        -- under it, a creature in the air: it goes back to the snap for good, after a fall of
                        -- M.LiveHeight at most.
                        local dy = at[2] - p[2]
                        if m.NoSnap and dy < -M.LiveHeight then
                            m.NoSnap, m.Air = nil, true
                            M.LiveStats.Air = (M.LiveStats.Air or 0) + 1
                            pcall(Osi.TeleportToPosition, m.Uuid, p[1], p[2] + 0.5, p[3], "", 0, 0, 0, 0, 1)
                        elseif not m.Air and not m.NoSnap and math.abs(dy) > M.LiveHeight then
                            m.NoSnap = true
                            M.LiveStats.NoSnap = (M.LiveStats.NoSnap or 0) + 1
                            pcall(Osi.TeleportToPosition, m.Uuid, p[1], p[2] + 0.5, p[3], "", 0, 0, 0, 0, 0)
                        elseif hdist(at, p) > M.LiveMove or (m.NoSnap and dy > M.LiveHeight) then
                            pcall(Osi.TeleportToPosition, m.Uuid, p[1], p[2] + 0.5, p[3], "", 0, 0, 0, 0, m.NoSnap and 0 or 1)
                            M.LiveStats.Teleports = M.LiveStats.Teleports + 1
                        end
                    end
                    local f = m.FaceCheck == nil and forward(m.Uuid)
                    local turnDue = (m.LiveDir == nil or angle(m.LiveDir, dir) > math.rad(M.LiveTurnDeg))
                        or (f and angle(f, dir) > TURN and ticks - (m.LiveTurnAt or 0) > FACE_CHECK)
                    if turnDue then
                        m.LiveDir, m.LiveTurnAt = dir, ticks
                        face(m, dir)
                        M.LiveStats.Turns = M.LiveStats.Turns + 1
                    elseif m.FaceCheck then
                        checkFace(m)
                    end
                end
            end
        end
    end
    -- Issue 27: the WoW character's life mirrored onto Tav at its changes (see the header).
    local function heroLife(plan)
        local st = type(plan.HeroState) == "string" and plan.HeroState or nil
        if not st then return end
        local before = heroState
        heroState = st
        if st == before then
            if st ~= "alive" and ticks - killedAt > KILL_LANDS and ticks - wowReviveAt > REVIVE_AGAIN
                and Death.ReviveWow and Death.ReviveWow() then
                wowReviveAt = ticks
                M.Life.WowRevives = (M.Life.WowRevives or 0) + 1
                M.Life.Last = "WoW " .. st .. " under a Tav BG3 has up: the WoW character goes back up"
                Ext.Utils.Print("WOW-BG3 mirrors: " .. M.Life.Last)
            end
            return
        end
        -- The first state a session sees is recorded only (Death.lua does the
        -- same): at a load BG3 decides who lives and the lifecycle brings each WoW character to BG3's state.
        if before == nil then
            M.Life.Seen = "WoW " .. st .. " at the session's first look"
            return
        end
        -- A player's character goes DOWNED at 0 HP; that is her death here.
        local th = hp(Hero.Uuid())
        local tavDead = q(Osi.IsDead, Hero.Uuid()) == 1 or q(Osi.HasActiveStatus, Hero.Uuid(), "DOWNED") == 1 or (th ~= nil and th[1] <= 0)
        local fighting = type(plan.Fight) == "table" and #plan.Fight > 0
        local life = M.Life
        if st == "alive" and before ~= nil and tavDead then
            local frac = (tonumber(plan.HeroWowHp) or 0) / math.max(1, tonumber(plan.HeroWowMaxHp) or 1)
            frac = math.max(0.01, math.min(1.0, frac > 0 and frac or 1.0))
            local ok, err, how
            if q(Osi.IsDead, Hero.Uuid()) == 1 then
                -- Dead: BG3's resurrection, then the HP once it has landed (Resurrected, or the
                -- fallback in step) - it restores full HP after any HP set made now.
                reviveFrac, reviveAt = frac, ticks
                ok, err = pcall(Osi.Resurrect, Hero.Uuid(), "", 0)
                how = "Resurrect"
            else
                -- Downed: up again as after BG3's Help, at the WoW fraction. (SetHitpointsPercentage
                -- takes two arguments here: with a third, empty one it changes nothing.)
                pcall(Osi.RemoveStatus, Hero.Uuid(), "DOWNED", "")
                ok, err = pcall(Osi.SetHitpointsPercentage, Hero.Uuid(), 100.0 * frac)
                how = "DOWNED removed and HP set"
            end
            life.Resurrected = life.Resurrected + 1
            life.Last = string.format("WoW %s -> alive: %s %s, HP %.0f%%", tostring(before), how, ok and "ok" or tostring(err), 100 * frac)
        elseif st ~= "alive" and (before == nil or before == "alive") and not tavDead and not fighting
            and q(Osi.IsInCombat, Hero.Uuid()) ~= 1 then
            local ok, err = pcall(Osi.ApplyDamage, Hero.Uuid(), math.max(1, (th and th[2] or 1) * 2), "Bludgeoning",
                "NULL_00000000-0000-0000-0000-000000000000")
            killedAt = ticks
            life.Killed = life.Killed + 1
            life.Last = string.format("WoW %s -> %s outside a fight: ApplyDamage %s", tostring(before), st, ok and "ok" or tostring(err))
        else
            life.Seen = string.format("WoW %s -> %s (Tav %s%s)", tostring(before), st, tavDead and "dead" or "alive",
                fighting and ", in a fight" or "")
            return
        end
        Ext.Utils.Print("WOW-BG3 mirrors: " .. life.Last)
    end

    -- HP guard (see the header). m.SyncFrac (or the spawn fraction) is what this file last set.
    M.HpRises, M.HpGuard = {}, 0
    local function statusNames(uuid)
        local out = {}
        pcall(function()
            for _, st in pairs(Ext.Entity.Get(uuid).ServerCharacter.StatusManager.Statuses) do
                out[#out + 1] = tostring(st.StatusId)
            end
        end)
        return out
    end
    local function hpGuard(m)
        if m.Dead or m.InFight or hitByTav[m.Uuid] or aimedRecently(m.Uuid) then return end
        local f = m.SyncFrac or m.SpawnFrac
        if not f or f >= 1 then return end
        local h = hp(m.Uuid)
        if not (h and h[2] > 0) then return end
        -- The scale step (IncreaseMaxHP) changes MaxHp: the next ready step sets the fraction; until the
        -- boost lands, compare against the max the stand-in has now.
        local want = bg3Hp(h[2], f)
        if h[1] <= want then m.GuardHp = h[1]; return end
        if q(Osi.IsInCombat, m.Uuid) == 1 then return end
        M.HpGuard = M.HpGuard + 1
        if #M.HpRises < 24 then
            M.HpRises[#M.HpRises + 1] = {Guid = M.Owned[m.Uuid], Age = ticks - m.Born, Stage = m.Stage, From = m.GuardHp,
                To = h[1], Max = h[2], Want = want, Statuses = statusNames(m.Uuid)}
        end
        pcall(Osi.SetHitpointsPercentage, m.Uuid, 100.0 * f)
        m.GuardHp = want
    end

    -- The party's feet (server positions) and the placed squares (floor.json, world_bridge.py).
    local floorRaw, floorSq = nil, nil
    local function onGround(p)
        local raw = Ext.IO.LoadFile("wow-bg3/floor.json")
        if raw ~= floorRaw then
            floorRaw = raw
            local ok, fl = pcall(Ext.Json.Parse, raw or "")
            floorSq = ok and type(fl) == "table" and type(fl.Squares) == "table" and fl or nil
        end
        if not floorSq then return true end -- no floor list: the radius alone decides, as before
        local c = tonumber(floorSq.Cell) or 64
        local function inAny(x, z)
            for _, sq in ipairs(floorSq.Squares) do
                if x >= sq[1] and x < sq[1] + c and z >= sq[2] and z < sq[2] + c then return true end
            end
            return false
        end
        -- 1 m inside the placed ground as a whole: the margin keeps a stand-in off the outer edge only.
        -- Per square it also struck the seams between two placed squares, and the treadmill starts the
        -- hero where four of them meet: a start zone's quest giver standing on a seam never got a
        -- stand-in (Kaltunk in the Valley of Trials, 0.3 m off it; the 0.1.0 playtest).
        return inAny(p[1], p[3]) and inAny(p[1] - 1, p[3]) and inAny(p[1] + 1, p[3])
            and inAny(p[1], p[3] - 1) and inAny(p[1], p[3] + 1)
    end

    -- The calls that last (see the header): every victim alive and fighting calls again; one out of
    -- combat for CALL_QUIET ticks after its last call (or dead, or gone) stops.
    local CALL_QUIET = 90
    local function keepCalling()
        for victim, c in pairs(calling) do
            local alive = Ext.Entity.Get(victim) ~= nil and q(Osi.IsDead, victim) ~= 1
            if alive and q(Osi.IsInCombat, victim) == 1 then
                callHelp(victim, c.Attacker)
            elseif not alive or ticks - c.Tick > CALL_QUIET then
                calling[victim] = nil
            end
        end
    end
    local function step()
        if next(calling) then pcall(keepCalling) end
        local e = host()
        if not (e and q(Osi.GetRegion, Hero.Uuid()) == REGION) then return end
        local plan = readJson("mirror.json")
        -- A new bridge session keeps the stand-ins (the same guids come back; one may be in BG3's
        -- combat, which the new bridge adopts); unlisted ones go as usual.
        if plan and plan.Session ~= session then session = plan.Session end
        -- A Seq first seen in this Lua state is not fresh: bg3le rebuilds the Lua
        -- states at every load, and mirror.json is the last session's file until the new bridge writes it
        -- (Active true, HeroState "dead" after a wipe); taken as fresh it killed the hero right after the load.
        if plan and math.type(plan.Seq) == "integer" and plan.Seq ~= lastSeen then
            if lastSeen ~= nil then M.LastTick = ticks end
            lastSeen = plan.Seq
        end
        local fresh = plan and plan.Active == true and M.LastTick and ticks - M.LastTick < GONE_AFTER
        local fight = {}
        for _, g in ipairs((plan and plan.Fight) or {}) do fight[g] = true end
        if not fresh then
            -- The bridge stopped or world mode ended: every stand-in goes (fight members too once
            -- the bridge is gone for good).
            for guid in pairs(mirrors) do if not (plan and plan.Active == true and fight[guid]) then remove(guid) end end
            return
        end
        pcall(heroLife, plan)
        if reviveFrac and ticks - reviveAt > 60 and q(Osi.HasActiveStatus, Hero.Uuid(), "DOWNED") ~= 1 and q(Osi.IsDead, Hero.Uuid()) ~= 1 then
            pcall(Osi.SetHitpointsPercentage, Hero.Uuid(), 100.0 * reviveFrac)
            M.Life.HpSet = string.format("%.0f%% after 60 ticks", 100 * reviveFrac)
            reviveFrac = nil
        end
        local want, order = {}, {} -- order: the bridge's, nearest to a party member first
        for _, mob in ipairs(plan.Mobs or {}) do
            if type(mob) == "table" and type(mob.Guid) == "string" and type(mob.Pos) == "table" and #mob.Pos == 3 then
                if not want[mob.Guid] then order[#order + 1] = mob end
                want[mob.Guid] = mob
            end
        end
        M.WantNow = want
        -- The party's BG3 combat: the hero's, else a companion's or a summon's.
        local tavCombat = Party.Combat()
        -- Every combat of the party's side: a stand-in fighting a companion or a summon in
        -- another BG3 combat than the hero's is in the party's fight too, so the bridge pulls it and
        -- its WoW mob is held (before, it fought in BG3 while its WoW mob walked on).
        local partyCombats = {}
        for _, u in ipairs(Party.Fighters()) do
            local c = combatOf(u)
            if c then partyCombats[c] = true end
        end
        -- Gone: unlisted and not in the fight, or the entity vanished. A corpse stays (CORPSE_KEEP).
        for guid, m in pairs(mirrors) do
            if not Ext.Entity.Get(m.Uuid) then
                mirrors[guid] = nil
                M.Owned[m.Uuid] = nil
            elseif not want[guid] and not fight[guid] then
                if m.Dead or q(Osi.IsDead, m.Uuid) == 1 or M.KnockedOut[m.Uuid] then
                    m.Dead = true
                    m.DiedAt = m.DiedAt or ticks
                    -- A corpse someone carries (BG3's own pick-up) is on no ground and does not go while it is
                    -- carried; put down, it lies its time from then (WoW's server keeps its corpse the same way).
                    if q(Osi.IsInInventory, m.Uuid) == 1 then
                        m.Carried = true
                    else
                        if m.Carried then m.Carried, m.DiedAt = nil, ticks end
                        local p = pos(m.Uuid)
                        if ticks - m.DiedAt >= CORPSE_KEEP or not (p and onGround(p)) then remove(guid) end
                    end
                else
                    remove(guid)
                end
            end
        end
        local spawned = false
        -- WoW respawned the mob (the bridge: the server confirmed the kill, the mob left mobs.txt and came
        -- back; never a stand-in whose kill has not landed yet). The corpse, knocked out too, gives way.
        local respawned = {}
        for _, g in ipairs(plan.WowRespawned or {}) do respawned[g] = true end
        for _, mob in ipairs(order) do
            local guid = mob.Guid
            local m = mirrors[guid]
            if m and respawned[guid] and not fight[guid] and (q(Osi.IsDead, m.Uuid) == 1 or M.KnockedOut[m.Uuid]) then
                remove(guid)
                m = nil
            end
            if not m then
                -- One spawn per step spreads the load (nearest first); only on placed ground.
                if not spawned and not fight[guid] and onGround(mob.Pos) then
                    spawned = true
                    local ok, err = pcall(spawn, mob)
                    if not ok then note("spawn " .. guid .. ": " .. tostring(err)) end
                end
            else
                m.Want = mob
                local inFight = fight[guid] == true
                local combat = combatOf(m.Uuid)
                local dead = q(Osi.IsDead, m.Uuid) == 1 or M.KnockedOut[m.Uuid] == true
                -- A neutral one BG3 pulled in without Tav attacking it: back out, not hostile.
                if not m.Hostile and not inFight and not provoked[m.Uuid] and not dead then
                    if q(Osi.HasActiveStatus, m.Uuid, "TEMPORARILY_HOSTILE") == 1 then
                        pcall(Osi.RemoveStatus, m.Uuid, "TEMPORARILY_HOSTILE", "")
                    end
                    if combat then
                        pcall(Osi.LeaveCombat, m.Uuid)
                        m.LeftCombat = (m.LeftCombat or 0) + 1
                        combat = nil -- not reported as in Tav's combat, so the bridge does not pull it
                    end
                end
                if inFight then provoke(m.Uuid) end
                -- A friendly stand-in Tav left alone again without a fight: back to blocked
                -- reactions and out of combats (a heal, a zero-damage warning, a missed aim).
                if friendlyAimed[m.Uuid] and not inFight and not combat and not dead and not searching[m.Uuid]
                    and ticks - friendlyAimed[m.Uuid] > FRIENDLY_AIM then
                    friendlyAimed[m.Uuid] = nil
                    pcall(Osi.BlockNewCrimeReactions, m.Uuid, 1)
                    if provoked[m.Uuid] then
                        provoked[m.Uuid] = nil
                        pcall(Osi.SetCanJoinCombat, m.Uuid, 0)
                    end
                    m.FriendlyReset = (m.FriendlyReset or 0) + 1
                end
                nameOf(m, mob)
                if m.Stage == "spawned" and ticks - m.Born >= SCALE_AFTER then
                    local ok, err = pcall(scale, m, tonumber(plan.HeroWowMaxHp))
                    if not ok then note("scale " .. guid .. ": " .. tostring(err)) end
                    m.Stage, m.ScaledAt = "scaled", ticks
                elseif m.Stage == "scaled" then
                    local h = hp(m.Uuid)
                    local target = m.Scale and m.Scale.MaxHp
                    if not target or (h and h[2] == target) or ticks - m.ScaledAt > SCALE_WAIT then
                        local f = wowFraction(mob)
                        -- The WoW fraction once scaled, only on an unhurt stand-in out of every
                        -- fight (this heal also wiped Tav's opening hit). Unhurt: HP not
                        -- under what it had before the boost (its spawn HP, at the WoW fraction).
                        local base = m.Scale and m.Scale.Base
                        local unhurt = h and h[1] >= math.min(type(base) == "table" and tonumber(base[1]) or h[2], h[2])
                        if f and not dead and unhurt and not hitByTav[m.Uuid] and not inFight and not combat
                            and not aimedRecently(m.Uuid) then
                            pcall(Osi.SetHitpointsPercentage, m.Uuid, 100.0 * f)
                        else
                            m.ScaleHealSkipped = true
                        end
                        m.SyncFrac = f
                        m.Stage = "ready"
                    end
                end
                m.InFight = inFight or combat ~= nil
                -- Out of BG3's combat and the fight: follow the WoW mob (every tick through follow()
                -- while wowmobs.bin is fresh; mirror.json's 300 ms positions only without it).
                local livePlaced = m.LiveAt ~= nil and ticks - m.LiveAt < 5
                if not inFight and not combat and not dead and not searching[m.Uuid] then
                    local p = (not livePlaced) and pos(m.Uuid) or nil
                    if livePlaced then
                        m.TurnDue = false
                    elseif p and hdist(p, mob.Pos) > MOVE then
                        pcall(Osi.TeleportToPosition, m.Uuid, mob.Pos[1], mob.Pos[2] + 0.5, mob.Pos[3], "", 0, 0, 0, 0, 1)
                        m.TurnDue = true
                    elseif m.FaceCheck then
                        checkFace(m)
                    else
                        local f = forward(m.Uuid)
                        if m.TurnDue or (f and type(mob.Dir) == "table" and angle(f, mob.Dir) > TURN) then
                            m.TurnDue = false
                            face(m, mob.Dir)
                        end
                    end
                    -- A stand-in Tav hit keeps BG3's HP: the bridge pulls it (a fight, or the kill).
                    -- Issue 20: not while Tav's attack may still be landing (aimed), and BG3's HP
                    -- goes up only when WoW's own fraction rose since the last sync (regeneration,
                    -- a heal); a drop BG3 has and WoW has not (her hit before its AttackedBy) stays.
                    -- Whole HP as BG3 rounds the fraction (the template's HP, e.g. a wolf's 11,
                    -- cannot show every WoW percent; 0.04 of a fraction before).
                    if m.Stage == "ready" and not hitByTav[m.Uuid] and not aimedRecently(m.Uuid) then
                        local h, f = hp(m.Uuid), wowFraction(mob)
                        if h and f and h[2] > 0 then
                            local want, last = bg3Hp(h[2], f), m.SyncFrac or f
                            if h[1] > want or (h[1] < want and f > last + 0.005) then
                                pcall(Osi.SetHitpointsPercentage, m.Uuid, 100.0 * f)
                                m.SyncFrac = f
                            elseif f < last then
                                m.SyncFrac = f
                            elseif h[1] < want then
                                m.HealsKept = (m.HealsKept or 0) + 1
                            end
                        end
                    end
                end
                m.Combat = combat ~= nil and partyCombats[combat] == true
                m.Dead = dead
            end
        end
        -- Fight members that left the list stay (BG3 runs them) but keep their status fresh.
        for guid, m in pairs(mirrors) do
            if not want[guid] then
                local combat = combatOf(m.Uuid)
                m.InFight = true
                m.Combat = combat ~= nil and partyCombats[combat] == true
                m.Dead = q(Osi.IsDead, m.Uuid) == 1 or M.KnockedOut[m.Uuid] == true
            end
        end
    end

    local function writeStatus()
        local mobs = {}
        for guid, m in pairs(mirrors) do
            local h = hp(m.Uuid) or {0, 0}
            if searching[m.Uuid] then searchDone(m, nil) end -- the time limit holds without WoW records too
            mobs[guid] = {Uuid = m.Uuid, Hp = h[1], MaxHp = h[2], InCombat = m.Combat == true, Dead = m.Dead == true,
                HitByTav = hitByTav[m.Uuid] == true, Stage = m.Stage, Hostile = m.Hostile, Scale = m.Scale,
                Friendly = m.Friendly, Guard = m.Guard, CrimeReady = friendlyAimed[m.Uuid] ~= nil,
                Searching = searching[m.Uuid] ~= nil,
                -- A corpse someone carries (BG3's own pick-up): in an inventory, on no ground.
                Carried = m.Dead == true and q(Osi.IsInInventory, m.Uuid) == 1,
                FriendlyReset = m.FriendlyReset,
                Provoked = provoked[m.Uuid] == true, LeftCombat = m.LeftCombat,
                Named = m.Named, NameText = m.NameText, NameError = m.NameError, Aimed = aimed[m.Uuid] and ticks - aimed[m.Uuid] or nil,
                SpawnHp = m.SpawnHp, SpawnFrac = m.SpawnFrac, HpInit = m.HpInit,
                Outfit = m.Outfit, OutfitError = m.OutfitError,
                HealsKept = m.HealsKept, ScaleHealSkipped = m.ScaleHealSkipped,
                Pos = pos(m.Uuid), Yaw = yawOf(forward(m.Uuid)), WantYaw = m.Want and yawOf(m.Want.Dir) or nil,
                Live = m.LiveAt ~= nil and ticks - m.LiveAt < 5, LiveYaw = m.LiveDir and yawOf(m.LiveDir) or nil}
        end
        local th = hp(Hero.Uuid()) or {0, 0}
        -- InCombat: the party's (the hero or a companion in a BG3 combat; the bridge pulls the mobs in
        -- it), HeroInCombat the hero's own.
        local tav = {Hp = th[1], MaxHp = th[2], InCombat = Party.Combat() ~= nil,
            HeroInCombat = q(Osi.IsInCombat, Hero.Uuid()) == 1, Ftb = inFtb(),
            Sneaking = q(Osi.HasActiveStatus, Hero.Uuid(), "SNEAKING") == 1, Position = pos(Hero.Uuid()),
            -- The member the player controls now (BG3's selected character:
            -- world_bridge.py turns WoW's view onto its WoW character through Far Sight).
            Controlled = uuidOf(tostring(q(Osi.GetHostCharacter) or ""))}
        -- Diagnosis (casts refused in a fight): Tav's spell-cast and interrupt components.
        if tav.InCombat then
            tav.Busy = q(function()
                local out = {}
                for _, n in ipairs(host():GetAllComponentNames(false)) do
                    if n:find("spell_cast") or n:find("interrupt") or n:find("Cast") then out[#out + 1] = n end
                end
                table.sort(out)
                return out
            end)
        end
        -- Stand-ins a fight spawned with no mirror, for LinkLive.lua (which cannot read a far one itself).
        -- InCombat as for a mirror: in a party member's BG3 combat (JoinWatch.lua: a join that failed).
        local fightSpawned, partyCombats = {}, nil
        for uuid in pairs(WOWBG3FightSpawned or {}) do
            if Ext.Entity.Get(uuid) then
                if not partyCombats then
                    partyCombats = {}
                    for _, u in ipairs(Party.Fighters()) do
                        local c = combatOf(u)
                        if c then partyCombats[c] = true end
                    end
                end
                local h = hp(uuid) or {0, 0}
                local combat = combatOf(uuid)
                fightSpawned[uuid] = {Hp = h[1], MaxHp = h[2], Pos = pos(uuid),
                    InCombat = combat ~= nil and partyCombats[combat] == true,
                    Dead = q(Osi.IsDead, uuid) == 1 or M.KnockedOut[uuid] == true}
            end
        end
        Ext.IO.SaveFile(DIR .. "mirror-status.json", Ext.Json.Stringify({RunId = runId, Seq = lastSeen, Tick = ticks,
            Session = session, FaceMode = M.FaceMode, FaceStats = M.FaceStats, Tav = tav, Mobs = mobs, Problems = problems,
            FightSpawned = fightSpawned,
            Strikes = M.Strikes or 0, LiveStats = M.LiveStats, Life = M.Life, HeroState = heroState,
            HpGuard = M.HpGuard, HpRises = M.HpRises, Spawns = M.Spawns, Removes = M.Removes, HelpCalled = M.HelpCalled,
            DialogHides = M.DialogHides,
            Pickpockets = M.Pickpockets,
            Searches = M.Searches, CrimeKinds = M.CrimeKinds, LastCrime = M.LastCrime,
            Respawned = (function()
                local n = 0
                for _, c in pairs(M.Churn) do if c > 1 then n = n + 1 end end
                return n
            end)()}))
    end

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if Ext.Utils.GetGameState() ~= "Running" then
            -- A save (Save, Paused) keeps the stand-ins: hold, never forget them.
            if Ext.Require("Session.lua").Held() then return end
            if next(mirrors) then
                mirrors = {}
                for k in pairs(M.Owned) do M.Owned[k] = nil end
            end
            return
        end
        if next(attacking) then pcall(missedAttacks) end
        if next(mirrors) then
            for _, m in pairs(mirrors) do
                if m.HpWait then pcall(healthDue, m) else pcall(hpGuard, m) end
            end
            local okf, errf = pcall(follow)
            if not okf then note("follow: " .. tostring(errf)) end
        end
        if ticks % 10 ~= 0 then return end
        local ok, err = pcall(step)
        if not ok then note("step: " .. tostring(err)) end
        pcall(writeStatus)
    end)
end
return M
