-- Stand-in numbers from the balance research (a balance simulator run outside this repository): a WoW mob's BG3 stats follow BG3's own enemy curves
-- at the effective level of its own WoW level, and its HP follows what a party of four deals in a turn at that
-- level. Nothing of the player's party goes in, not its level and not its size (playtest feedback): a mob is what it is
-- for good, and WoW's zones are the order to go through them. Switch: `scaling` in settings.conf,
-- the launcher's wow-bg3/scaling.json {On, Difficulty}; off, Mirrors.lua keeps HP = WoW ^ 0.65.
--
-- Fights last what BG3's own do (about 3.5 rounds on average, all four members played) for one mob of the party's
-- own level; a pack is as many of those as it is. Nothing limits a pull: every hostile stand-in chained within 40 yd
-- of a mob in the fight joins it (Mirrors.lua; BG3's own ally join does the same).
--
-- The level table is Progression.lua's curve (M.CURVE, 4x18 at WoW 60): effective level of a WoW level W
-- = 1 + (W - 4) x (Cap - 1) / 56.
--
-- From the WoW mob only its level, max HP, hit and the map are read. Rank comes from its HP against a
-- normal mob of its level (a rare is a normal; vmangos' rank is not in the mirror plan).
--
-- HP = M.NORMAL_BG3_HP at the mob's level x member-turns of the rank against a normal's x (mob's WoW HP / its rank's usual) ^ 0.5.
-- Each mob's numbers are set when it first appears and never change.
-- AC, to-hit, DC and saves: BG3's Combatant curve at E, plus the rank's. Damage: a share of a member's HP
-- at E. One attack a turn: the boss's extra attacks and area ability of the research are in that one hit.
-- All BG3 boosts, all applied by Mirrors.lua.
local M = {}

-- A Fighter, Cleric, Rogue, Wizard party, levels 1 to 18:
-- A normal mob's BG3 HP by WoW level, solved offline against the party a player has at that level (the slots at WoW 1 to 4,
-- then four; tools/balance-sim): one mob for a solo mage about 3 rounds; from WoW 4, four against 2 comfortable, 4 a bit of a
-- challenge, 6 harder but doable. It only rises. Pairs of level, HP; straight between them.
M.NORMAL_BG3_HP = {1, 10, 2, 20, 3, 28, 4, 37, 5, 38, 8, 39, 10, 41, 13, 50, 15, 63, 18, 82, 20, 88, 23, 90, 25, 91, 28, 100, 30, 108,
    35, 133, 40, 143, 45, 151, 50, 156, 55, 161, 60, 166}
function M.NormalHp(level)
    local t = M.NORMAL_BG3_HP
    if level >= t[#t - 1] then return t[#t] + (level - t[#t - 1]) end
    for i = 1, #t - 2, 2 do
        if level <= t[i + 2] then return t[i + 1] + (t[i + 3] - t[i + 1]) * (level - t[i]) / (t[i + 2] - t[i]) end
    end
end
M.NORMAL_TURNS = 6
-- A level 1 mage has about 8 HP and fights alone: a lighter hit at WoW 1, so a single mob is seldom the end of it.
M.FIRST_LEVEL_DAMAGE = 0.67
M.MEMBER_HP = {10, 17, 24, 31, 38, 45, 52, 59, 66, 73, 80, 99, 107, 115, 123, 131, 139, 147}
-- WoW: spawn-weighted median HP and hit of normal open-world mobs for levels 1 to 63 (vmangos), spikes
-- from a few odd mobs taken out.
M.NORMAL_HP = {42, 48, 71, 78, 102, 120, 138, 146, 177, 187, 222, 253, 274, 300, 328, 342, 386, 402, 449, 484, 533, 574,
    617, 641, 713, 726, 788, 868, 950, 1003, 1107, 1192, 1221, 1250, 1342, 1469, 1536, 1570, 1678, 1865, 1902, 1981,
    2060, 2099, 2218, 2399, 2488, 2532, 2673, 2769, 2981, 3031, 3188, 3241, 3441, 3644, 3760, 3876, 3998, 4059, 4170, 4370, 4497}
M.NORMAL_HIT = {2.1, 2.1, 3.3, 3.9, 7.0, 7.2, 10, 11.2, 13.6, 14, 14.5, 18.4, 21.6, 21.6, 22.5, 26.4, 28.8, 29.8, 30.1, 33.3, 33.9,
    36.3, 37.1, 37.5, 39.3, 41.4, 42.3, 46.8, 49.4, 49.4, 52.2, 52.2, 52.2, 55, 57.9, 59.3, 60.5, 62.7, 63.2, 66.7, 68.9, 73.2,
    77.4, 80.6, 86.8, 90, 91.8, 93, 96.1, 98.2, 100.3, 101.5, 104.9, 105.9, 109.4, 110.4, 115, 115, 118.5, 120.9, 123.2, 200.8, 204.4}
-- The playtest measure for four against normal mobs of their level: 2 comfortable, 4 a bit of a challenge, 6 harder but
-- doable (simulator: party HP lost about 13%, 27%, 52%, won 99% or more).
-- Rank: member-turns to kill at party size 4, the WoW HP (x normal) it is meant for, AC, to-hit, DC above the
-- curve, and damage: the share of a member's HP an enemy turn deals is 30% (easing off past level 8, as BG3's
-- enemies do) x (WoW hit / normal's) ^ 0.35 within
-- Low and High.
M.RANKS = {
    normal = {Turns = 6, Rel = 1, AC = 0, Hit = 0, DC = 0, Low = 0, High = 0.60},
    elite = {Turns = 10.5, Rel = 2.7, AC = 1, Hit = 1, DC = 1, Low = 0, High = 0.60},
    dungeon_trash = {Turns = 7.5, Rel = 2.7, AC = 1, Hit = 1, DC = 1, Low = 0, High = 0.60},
    dungeon_boss = {Turns = 19.5, Rel = 8, AC = 3, Hit = 2, DC = 2, Low = 0.49, High = 0.60},
    raid_trash = {Turns = 9, Rel = 6, AC = 2, Hit = 2, DC = 2, Low = 0, High = 0.60},
    raid_boss = {Turns = 27, Rel = 80, AC = 4, Hit = 3, DC = 3, Low = 0.62, High = 0.62},
}
M.TURNS_CAP = 36
-- BG3's AI needs an average hit of 1 (Boosts). Below that the shortfall comes off to-hit, about 9 points for all of it.
M.MISS = 9
M.DIFFICULTY = {
    explorer = {HP = 0.7, Hit = -1, DC = -1, Damage = 0.8},
    balanced = {HP = 1, Hit = 0, DC = 0, Damage = 1},
    tactician = {HP = 1.3, Hit = 2, DC = 2, Damage = 1},
    honour = {HP = 1.5, Hit = 2, DC = 2, Damage = 1.15},
}
M.DUNGEONS = {[33] = 1, [34] = 1, [36] = 1, [43] = 1, [47] = 1, [48] = 1, [70] = 1, [90] = 1, [109] = 1, [129] = 1, [189] = 1, [209] = 1,
    [229] = 1, [230] = 1, [289] = 1, [329] = 1, [349] = 1, [389] = 1, [429] = 1}
M.RAIDS = {[249] = 1, [309] = 1, [409] = 1, [469] = 1, [509] = 1, [531] = 1, [533] = 1}
-- The stand-in templates at level 1 (bridge.rs STANDINS, read from BG3's stats): AC, to-hit, spell DC.
-- AC is read from the stand-in itself when BG3 gives it (it may be dressed).
M.TEMPLATES = {wolf = {13, 3, 11}, boar = {11, 3, 11}, bear = {11, 4, 11}, spider = {14, 5, 11}, rat = {10, -2, 10},
    zombie = {8, 5, 10}, goblin = {13, 3, 10}, human = {16, 5, 11}}
M.BASE_SAVE = 1

local function lerp(t, x)
    x = math.max(1, math.min(#t, x))
    local i = math.floor(x)
    local j = math.min(#t, i + 1)
    return t[i] + (t[j] - t[i]) * (x - i)
end
local function at(t, level) return t[math.max(1, math.min(#t, math.floor(level + 0.5)))] end
local function round(x) return math.floor(x + 0.5) end

function M.Effective(w, cap) return math.min(18, 1 + math.max(0, w - 4) * (cap - 1) / 56) end
function M.RankOf(rel, map)
    if rel >= 25 then return "raid_boss" end
    if M.RAIDS[map] then return "raid_trash" end
    if rel >= 5 then return "dungeon_boss" end
    if rel >= 1.6 then return M.DUNGEONS[map] and "dungeon_trash" or "elite" end
    return "normal"
end

-- The numbers for one mob. c: Level, WowMaxHp, WowDamage (WoW), Map, Cap, Difficulty. No game calls:
-- tools/test_scaling.py runs this.
function M.Compute(c)
    local cap = c.Cap or 18
    local d = M.DIFFICULTY[c.Difficulty] or M.DIFFICULTY.balanced
    local E = M.Effective(c.Level, cap)
    local rel = c.WowMaxHp / at(M.NORMAL_HP, c.Level)
    local reld = (c.WowDamage or 0) / at(M.NORMAL_HIT, c.Level)
    local rank = M.RankOf(rel, c.Map)
    local r = M.RANKS[rank]
    local turns = math.min(M.TURNS_CAP, r.Turns * (rel / r.Rel) ^ 0.5)
    local hp = M.NormalHp(c.Level) * turns / M.NORMAL_TURNS * d.HP
    local share = math.max(r.Low, math.min(r.High, 0.30 * (1 - 0.035 * math.max(0, E - 8)) * math.max(reld, 0.01) ^ 0.35))
    local damage = share * lerp(M.MEMBER_HP, E) * d.Damage * (c.Level <= 1 and M.FIRST_LEVEL_DAMAGE or 1)
    return {E = E, Rank = rank, Rel = rel, MaxHp = math.max(1, round(hp)), Damage = damage,
        AC = math.max(12, round(10.7 + 0.5 * E)) + r.AC,
        ToHit = math.max(3, round(2.2 + 0.57 * E)) + r.Hit + d.Hit - round(M.MISS * math.max(0, 1 - damage)),
        DC = round(8.4 + 0.63 * E) + r.DC + d.DC,
        Save = 1 + math.floor(E * 0.4) + r.AC // 2}
end
-- The boost string for those numbers over a stand-in of kind `kind`: base max HP, base attack average, and
-- its AC if BG3 gave one. MIN_AVG_HIT: BG3's AI does not attack with an expected damage of about 0.
local MIN_AVG_HIT = 1
function M.Boosts(n, kind, baseMaxHp, baseDamage, baseAC)
    local t = M.TEMPLATES[kind] or M.TEMPLATES.human
    local out = {}
    local function add(f, v) if v ~= 0 then out[#out + 1] = string.format(f, v) end end
    add("IncreaseMaxHP(%d)", n.MaxHp - baseMaxHp)
    local bonus = math.max(-200, math.min(200, math.max(round(n.Damage - baseDamage), math.ceil(MIN_AVG_HIT - baseDamage))))
    add("DamageBonus(%d)", bonus)
    add("AC(%d)", n.AC - (baseAC or t[1]))
    add("RollBonus(Attack,%d)", n.ToHit - t[2])
    add("SpellSaveDC(%d)", n.DC - t[3])
    add("RollBonus(SavingThrow,%d)", n.Save - M.BASE_SAVE)
    return table.concat(out, ";"), bonus
end

-- From here on the game's side: the switch, read once per Lua session.
local settings = nil
local function read()
    if settings == nil then
        local ok, c = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/scaling.json") or "")
        settings = ok and type(c) == "table" and c or {}
    end
    return settings
end
function M.On() return read().On == true end
function M.Difficulty()
    local d = tostring(read().Difficulty or "balanced"):lower()
    return M.DIFFICULTY[d] and d or "balanced"
end
-- What Compute takes besides the mob: neither changes during a game.
function M.Fixed()
    local Progression = Ext.Require("Progression.lua")
    return {Cap = Progression.CURVE and Progression.CURVE.Cap or 18, Difficulty = M.Difficulty()}
end
-- What BG3's templates carry that a stand-in is made without. Undead Fortitude (BG3's zombies: at 0 HP a saving
-- throw puts them back to 1) is off for now, by the maintainer's word (2026-11-12): a WoW zombie dies at 0. BG3 gives
-- it as the status UNDEAD_FORTITUDE, which carries the passive (measured: RemovePassive alone leaves it; with the
-- status removed the passive is gone a moment later and stays gone).
M.WithoutStatuses = {"UNDEAD_FORTITUDE"}
M.WithoutPassives = {"UndeadFortitude"}
-- Server: takes them off a stand-in. Called when it is made and again when its health is first set, a tick or
-- more later (Mirrors.lua, StandIns.lua): BG3 may lay a template's statuses on after the character exists.
function M.Without(uuid)
    for _, status in ipairs(M.WithoutStatuses) do pcall(Osi.RemoveStatus, uuid, status) end
    for _, passive in ipairs(M.WithoutPassives) do pcall(Osi.RemovePassive, uuid, passive) end
end

return M
