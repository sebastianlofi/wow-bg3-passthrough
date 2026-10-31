-- Stand-in numbers from the balance research (docs/agents-20261031/balance-research.md in the private
-- workspace; the simulator is tools/balance-sim there): a WoW mob's BG3 stats follow BG3's own enemy curves
-- at an effective level, and its HP follows what a party deals in a turn. Switch: `scaling` in settings.conf,
-- the launcher's wow-bg3/scaling.json {On, Difficulty}; off, Mirrors.lua keeps HP = WoW ^ 0.65.
--
-- Fights last what BG3's own do (about 3.5 rounds on average, all four members played), and the usual fight
-- is a pack: every hostile stand-in chained within 40 yd of a mob in the fight joins it (Mirrors.lua; BG3's own
-- ally join does the same, a Defias pull at Moonbrook grew from 2 to 14). Nothing limits a pull but the stand-ins
-- there are. A mob's stats know its camp, the mobs so chained: a camp's mobs are each weaker, in HP, damage and
-- effective level, so a camp of 14 or 30 is one fight. A lone mob is one strong mob, about 3 rounds.
--
-- The level table is Progression.lua's curve (M.CURVE, 4x18 at WoW 60): effective level of a WoW level W
-- = 1 + (W - 4) x (Cap - 1) / 56. A party below that for its own WoW level (no level mod, level-ups not
-- spent, the curve's switch off) pulls every mob down by the same gap; a party above it is left strong.
--
-- From the WoW mob only its level, max HP, hit and the map are read. Rank comes from its HP against a
-- normal mob of its level (a rare is a normal; vmangos' rank is not in the mirror plan).
--
-- HP = party damage a member-turn at E x member-turns of the rank x (mob's WoW HP / its rank's usual) ^ 0.5
-- x the camp's and the party size's share. A smaller party's camp counts for more (CrowdOf).
-- AC, to-hit, DC and saves: BG3's Combatant curve at E, plus the rank's. Damage: a share of a member's HP
-- at E. One attack a turn: the boss's extra attacks and area ability of the research are in that one hit.
-- All BG3 boosts, all applied by Mirrors.lua.
local M = {}

-- At-will damage per member-turn against BG3's AC of that level, and a member's HP, for levels 1 to 18:
-- a Fighter, Cleric, Rogue, Wizard party, from the simulator.
M.DPR = {7.5, 7.3, 7.8, 8.3, 15.7, 14.6, 15.7, 16.0, 19.5, 20.6, 24.5, 23.1, 25.7, 26.5, 27.1, 25.6, 28.6, 26.8}
M.MEMBER_HP = {10, 17, 24, 31, 38, 45, 52, 59, 66, 73, 80, 99, 107, 115, 123, 131, 139, 147}
-- WoW: spawn-weighted median HP and hit of normal open-world mobs for levels 1 to 63 (vmangos), spikes
-- from a few odd mobs taken out.
M.NORMAL_HP = {42, 48, 71, 78, 102, 120, 138, 146, 177, 187, 222, 253, 274, 300, 328, 342, 386, 402, 449, 484, 533, 574,
    617, 641, 713, 726, 788, 868, 950, 1003, 1107, 1192, 1221, 1250, 1342, 1469, 1536, 1570, 1678, 1865, 1902, 1981,
    2060, 2099, 2218, 2399, 2488, 2532, 2673, 2769, 2981, 3031, 3188, 3241, 3441, 3644, 3760, 3876, 3998, 4059, 4170, 4370, 4497}
M.NORMAL_HIT = {2.1, 2.1, 3.3, 3.9, 7.0, 7.2, 10, 11.2, 13.6, 14, 14.5, 18.4, 21.6, 21.6, 22.5, 26.4, 28.8, 29.8, 30.1, 33.3, 33.9,
    36.3, 37.1, 37.5, 39.3, 41.4, 42.3, 46.8, 49.4, 49.4, 52.2, 52.2, 52.2, 55, 57.9, 59.3, 60.5, 62.7, 63.2, 66.7, 68.9, 73.2,
    77.4, 80.6, 86.8, 90, 91.8, 93, 96.1, 98.2, 100.3, 101.5, 104.9, 105.9, 109.4, 110.4, 115, 115, 118.5, 120.9, 123.2, 200.8, 204.4}
-- Rank: member-turns to kill at party size 4, the WoW HP (x normal) it is meant for, AC, to-hit, DC above the
-- curve, and damage: the share of a member's HP an enemy turn deals is 30% (easing off past level 8, as BG3's
-- enemies do) x (WoW hit / normal's) ^ 0.35 within
-- Low and High.
M.RANKS = {
    normal = {Turns = 8, Rel = 1, AC = 0, Hit = 0, DC = 0, Low = 0, High = 0.60},
    elite = {Turns = 14, Rel = 2.7, AC = 1, Hit = 1, DC = 1, Low = 0, High = 0.60},
    dungeon_trash = {Turns = 10, Rel = 2.7, AC = 1, Hit = 1, DC = 1, Low = 0, High = 0.60},
    dungeon_boss = {Turns = 26, Rel = 8, AC = 3, Hit = 2, DC = 2, Low = 0.49, High = 0.60},
    raid_trash = {Turns = 12, Rel = 6, AC = 2, Hit = 2, DC = 2, Low = 0, High = 0.60},
    raid_boss = {Turns = 36, Rel = 80, AC = 4, Hit = 3, DC = 3, Low = 0.62, High = 0.62},
}
M.TURNS_CAP = 48
-- By party members present, 1 to 4.
M.SIZE_HP = {0.33, 0.58, 0.8, 1}
M.SIZE_DAMAGE = {0.4, 0.65, 0.85, 1}
-- By the mob's camp, 1 to 8. Past 8 the mobs weaken fast enough that the whole camp stays one fight: a member kills
-- at most about one a turn, and every mob left alive attacks. HP stops at about a hit's worth. Levels: effective
-- levels off AC, to-hit, DC and saves per mob over one, at most CAMP_LEVELS.
M.CAMP_HP = {1, 0.75, 0.62, 0.55, 0.5, 0.46, 0.43, 0.4}
M.CAMP_DAMAGE = {1, 0.85, 0.72, 0.65, 0.6, 0.55, 0.5, 0.46}
M.CAMP_LEVEL, M.CAMP_LEVELS = 0.6, 3
local function between(t, k)
    k = math.max(1, k)
    local a = math.floor(k)
    return a >= #t and t[#t] or t[a] + (t[a + 1] - t[a]) * (k - a)
end
function M.CampHp(k) return k <= 8 and between(M.CAMP_HP, k) or math.max(0.1, 0.4 * (8 / k) ^ 1.2) end
function M.CampDamage(k) return k <= 8 and between(M.CAMP_DAMAGE, k) or 0.46 * (8 / k) ^ 1.1 end
-- A smaller party meets the same camp with fewer to kill and to spread the attacks.
function M.CrowdOf(camp, members) return camp * (4 / members) ^ 0.5 end
-- BG3's AI needs an average hit of 1 (Boosts). Below that the shortfall comes off to-hit, about 9 points for all of it.
M.MISS = 9
-- The reach: 40 yd chained.
M.REACH_M = 36.6
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
-- WoW's gray line for a player level (MaNGOS XP::GetGrayLevel).
function M.GrayLine(p)
    if p <= 5 then return 0 end
    if p <= 39 then return p - 5 - p // 10 end
    if p <= 59 then return p - 1 - p // 5 end
    return p - 9
end
function M.RankOf(rel, map)
    if rel >= 25 then return "raid_boss" end
    if M.RAIDS[map] then return "raid_trash" end
    if rel >= 5 then return "dungeon_boss" end
    if rel >= 1.6 then return M.DUNGEONS[map] and "dungeon_trash" or "elite" end
    return "normal"
end

-- The numbers for one mob. c: Level, WowMaxHp, WowDamage (WoW), Map, Members, Camp, WowLevel, PartyLevel (the
-- party's mean BG3 level), Cap, Difficulty. No game calls: tools/test_scaling.py runs this.
function M.Compute(c)
    local members = math.max(1, math.min(4, c.Members or 1))
    local cap = c.Cap or 18
    local camp = c.Camp or 1
    local d = M.DIFFICULTY[c.Difficulty] or M.DIFFICULTY.balanced
    local gap = c.PartyLevel and math.min(0, c.PartyLevel - M.Effective(c.WowLevel, cap)) or 0
    local E = math.max(1, M.Effective(c.Level, cap) + gap)
    local rel = c.WowMaxHp / at(M.NORMAL_HP, c.Level)
    local reld = (c.WowDamage or 0) / at(M.NORMAL_HIT, c.Level)
    local rank = M.RankOf(rel, c.Map)
    local r = M.RANKS[rank]
    local turns = math.min(M.TURNS_CAP, r.Turns * (rel / r.Rel) ^ 0.5)
    local crowd = M.CrowdOf(camp, members)
    local hp = lerp(M.DPR, E) * turns * M.SIZE_HP[members] * M.CampHp(crowd) * d.HP
    local share = math.max(r.Low, math.min(r.High, 0.30 * (1 - 0.035 * math.max(0, E - 8)) * math.max(reld, 0.01) ^ 0.35))
    local damage = share * lerp(M.MEMBER_HP, E) * M.SIZE_DAMAGE[members] * M.CampDamage(crowd) * d.Damage
    -- Level against the player: WoW's own gap, softened.
    local under = c.WowLevel - c.Level
    if under > 0 then
        local f = math.max(0.25, 1 - 0.07 * under)
        hp, damage = hp * f, damage * f
    end
    local over = math.max(0, -under)
    hp, damage = hp * (1 + 0.06 * over), damage * (1 + 0.06 * over)
    local steps = over // 3
    if c.Level <= M.GrayLine(c.WowLevel) then
        hp = math.min(hp, 0.6 * lerp(M.DPR, M.Effective(c.WowLevel, cap) + gap))
    end
    local Es = math.max(1, E - math.min(M.CAMP_LEVELS, M.CAMP_LEVEL * (math.max(1, camp) - 1)))
    return {E = E, Rank = rank, Rel = rel, MaxHp = math.max(1, round(hp)), Damage = damage,
        AC = math.max(12, round(10.7 + 0.5 * Es)) + r.AC + steps,
        ToHit = math.max(3, round(2.2 + 0.57 * Es)) + r.Hit + d.Hit + steps - round(M.MISS * math.max(0, 1 - damage)),
        DC = round(8.4 + 0.63 * Es) + r.DC + d.DC,
        Save = 1 + math.floor(Es * 0.4) + r.AC // 2}
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

-- The camp of one mob: mobs is a list of {Pos = {x, y, z}, Hostile} (BG3 metres), each mob reaching on from the
-- last.
function M.CampOf(me, mobs)
    local seen, todo, n = {[me] = true}, {me}, 1
    while #todo > 0 do
        local a = table.remove(todo)
        for _, b in ipairs(mobs) do
            if not seen[b] and b.Hostile and type(b.Pos) == "table" and type(a.Pos) == "table" then
                local dx, dz = a.Pos[1] - b.Pos[1], a.Pos[3] - b.Pos[3]
                if dx * dx + dz * dz <= M.REACH_M * M.REACH_M then
                    seen[b], n = true, n + 1
                    todo[#todo + 1] = b
                end
            end
        end
    end
    return n
end

-- From here on the game's side: the switch, read once per Lua session, and what the party is now.
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
-- Members present, the hero's WoW level, the party's mean BG3 level, the curve's cap. Cached for a second;
-- Key says when stand-ins should be redone.
local snap, snapAt = nil, -1000
function M.Party(ticks)
    if snap and ticks - snapAt < 30 then return snap end
    local Party, Progression = Ext.Require("Party.lua"), Ext.Require("Progression.lua")
    local list = Party.Party()
    local sum, n = 0, 0
    for _, u in ipairs(list) do
        local ok, l = pcall(Osi.GetLevel, u)
        if ok and type(l) == "number" then sum, n = sum + l, n + 1 end
    end
    local w = Progression.WowLevel()
    local mean = n > 0 and sum / n or nil
    snap, snapAt = {Members = math.max(1, #list), WowLevel = w, PartyLevel = mean, Cap = Progression.CURVE and Progression.CURVE.Cap or 18,
        Difficulty = M.Difficulty(), Key = string.format("%d:%d:%.2f", #list, w, mean or 0)}, ticks
    return snap
end
return M
