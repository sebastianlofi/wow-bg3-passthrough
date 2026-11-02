-- Stock.lua: what a WoW vendor has left, as a BG3 trader would. No game calls: tools/test_stock.py runs this.
--
-- BG3's rule (Shared.pak, Story/RawFiles/Goals/_Trade.txt): a trader's goods and gold are generated once and stay
-- as they are until PROC_LongRest allows the next generation. So what is bought stays gone and the purse stays spent.
-- The ledger is this: per vendor since the last long rest, the units bought and the gold it holds.
--   {N = writes, V = {g<wow guid> = {B = {e<entry> = units}, P = purse once touched}}}
-- It lives in BG3's save (Inventory.lua), so a load brings back that save's counts by itself, and a long rest empties it.
local M = {}

-- An unlimited ware (WoW's maxcount 0) starts with what a BG3 trader's table gives (Gustav's TreasureTable.txt, the tables
-- with T_Gold_Trader): one of a piece of gear ("1,1"), and of a consumable commonly three (I_OBJ_Potion_Healing "3,1",
-- the commonest fixed count of potions and kits there). Thrown weapons have a gear row and WoW sells them by the 200: one
-- purchase. Arrows and bullets are not sold: a vendor of them carries BG3's special arrows (below).
M.CONSUMABLES = 3

function M.New() return {N = 0, V = {}} end
-- Keys start with a letter so they stay strings through JSON.
local function key(e) return "e" .. tostring(math.tointeger(e) or e) end
local function vendor(l, guid)
    local v = l.V["g" .. guid]
    if not v then v = {B = {}}; l.V["g" .. guid] = v end
    return v
end

function M.Rest(l) l.V = {}; l.N = l.N + 1; l.R = (l.R or 0) + 1 end
function M.Bought(l, guid, entry, units)
    local b = vendor(l, guid).B
    b[key(entry)] = (b[key(entry)] or 0) + units
    l.N = l.N + 1
end

-- How many a ware starts with after a long rest: WoW's maxcount for a limited one, else as above. row: its items.json row.
-- Only what WoW stacks is a consumable here: a recipe or a bag (kind object, stack 1) is one, as in BG3.
function M.Start(w, row)
    if (w.Max or 0) > 0 then return w.Max end
    row = row or {}
    if row.k == "object" then return math.min(M.CONSUMABLES, row.st or 1) end
    return row.bc or 1
end
-- The wares to show: each with Count, what is left; what is sold out is not listed. A row with its own Count
-- (a pocket: the bridge's, gone from its list when stolen) is not stock of this kind.
function M.Wares(l, guid, wares, rowOf)
    local out, v = {}, l.V["g" .. guid]
    for _, w in ipairs(wares or {}) do
        if w.Count then
            out[#out + 1] = w
        elseif (rowOf(w.Entry) or {}).t ~= M.AMMO then
            local left = M.Start(w, rowOf(w.Entry)) - ((v and v.B[key(w.Entry)]) or 0)
            if left > 0 then
                local r = {}
                for k, x in pairs(w) do r[k] = x end
                r.Count = left
                out[#out + 1] = r
            end
        end
    end
    return out
end

-- ---- BG3's special arrows, in place of WoW's arrows and bullets (his ruling, 2026-11-01) ----
-- BG3 items, not WoW's: BG3's barter takes the gold, WoW's money follows it (Inventory.lua), the save holds them.
-- From Shared.pak's Object.txt: name, root template, ValueLevel, ObjectCategory. Not sold by BG3's tables: those with no
-- category (Piercing, Barbed, Celestial, Fey, Giant, Ooze, Plant slaying).
M.AMMO = 24 -- WoW's inventory type
M.ARROWS = {
    OBJ_ArrowOfAcid = {"0bfb72d2-0e8e-4d16-aeae-3159b0d4b195", 2, "Common"},
    OBJ_ArrowOfFire = {"9a142fae-bcea-4e88-a407-16d3b27468b9", 2, "Common"},
    OBJ_ArrowOfIce = {"2e297d80-823b-4200-926e-6ca23f488090", 2, "Common"},
    OBJ_ArrowOfLightning = {"1b0f095b-cb22-4bf9-aecf-4e5a26504ec8", 2, "Common"},
    OBJ_ArrowOfDarkness = {"844c19ce-9e8f-4119-8856-8fa2029461b1", 5, "Uncommon"},
    OBJ_ArrowOfDetonation = {"a0985797-4ec7-475c-8e11-0361e387f39a", 5, "Uncommon"},
    OBJ_ArrowOfWraithsEmbrace = {"a61c8248-1595-450f-bcc9-2ed1d9853739", 5, "Uncommon"},
    OBJ_ArrowOfAntimagic = {"c9871e4a-633e-4b81-9115-8ffd0f69ee6a", 8, "Rare"},
    OBJ_ArrowOfDispelling = {"ac23602b-ceee-4ef3-a6b8-601e40a5aa6f", 8, "Rare"},
    OBJ_ArrowOfRicochet = {"b11b3f7f-d8ec-41db-93b9-24474aea31e3", 8, "Rare"}, -- on screen: Arrow of Many Targets
    OBJ_ArrowOfTeleportation = {"4cf7bbbb-1d1b-4bec-b82a-cdc3cce33f7a", 8, "Rare"},
    OBJ_ArrowOfConstructSlaying = {"86a4dd98-80b1-4778-b498-e3a49b1dec37", 2, "Slaying"},
    OBJ_ArrowOfBeastSlaying = {"86e46f4a-6789-47ef-ab85-449ca36b1b20", 5, "Slaying"},
    OBJ_ArrowOfElementalSlaying = {"0d81014f-cc7f-4c79-9477-b8cda9738c72", 5, "Slaying"},
    OBJ_ArrowOfFiendSlaying = {"04ba10a0-ca33-47e6-9810-0b7234700c4a", 5, "Slaying"},
    OBJ_ArrowOfMonstrositySlaying = {"d7645507-6c0c-48ff-aa1b-57092d139f22", 5, "Slaying"},
    OBJ_ArrowOfUndeadSlaying = {"ae58df6b-0fc9-48ff-9d36-52bf9bc22204", 5, "Slaying"},
    OBJ_ArrowOfAberrationSlaying = {"1545f451-3fc5-4c1b-970e-572f9ace630c", 10, "Slaying"},
    OBJ_ArrowOfDragonSlaying = {"437835b4-0d13-47b7-8aca-5da4f948783b", 10, "Slaying"},
    OBJ_ArrowOfHumanoidSlaying = {"3c2c6a61-e348-4222-9ba3-58a4d49d558c", 10, "Slaying"},
    OBJ_ArrowOfSmokepowder = {"0788a2bb-c732-4ebb-abf9-2ef150f509be", 10, "Smokepowder"},
}
function M.HasAmmo(wares, rowOf)
    for _, w in ipairs(wares or {}) do
        if (rowOf(w.Entry) or {}).t == M.AMMO then return true end
    end
    return false
end
-- An arrow's value: BG3's ConsumablesArrows (GoldValues.lsx) is 0.8 x BaseProgression at its ValueLevel, a plain weapon's
-- 4.16 x, so a sale pays 0.8 / 4.16 of a plain piece of gear of that level. In copper that is WoW's price of such a piece,
-- at the WoW level of the arrow's ValueLevel; the value is that x Balanced's 2.5, as Prices.Held makes any sale's value.
function M.ArrowValue(name)
    local a = M.ARROWS[name]
    return a and math.floor(0.8 / 4.16 * M.Gear(4 + (a[2] - 1) * 56 / 11) * 2.5 + 0.5)
end
-- What a trader rolls, at BG3 level b (TreasureTable.txt): EquipmentTrader_Ranged_Arrows' count of ST_SpecialArrow, whose
-- odds are Common 2, Uncommon 1 up to level 5, then Common 1, Uncommon 1, Rare 2 (ArrowRare 3, Slaying 1).
-- And no arrow more than 2 levels over b, BG3's own Rare at 6 being ValueLevel 8: that keeps the strong ones for later.
local function weights(b)
    if b <= 5 then return {Common = 4, Uncommon = 2} end
    return {Common = 4, Uncommon = 4, Rare = 6, Slaying = 2}
end
function M.ArrowCount(b)
    return (b <= 4 and 12) or (b <= 6 and 25) or (b <= 10 and 35) or 50
end
function M.ArrowChoices(b)
    local w, out = weights(b), {}
    for name, a in pairs(M.ARROWS) do
        if w[a[3]] and a[2] <= b + 2 then out[#out + 1] = {name, a[3]} end
    end
    table.sort(out, function(x, y) return x[1] < y[1] end)
    return out
end
-- A vendor's arrows since the long rest, rolled once as BG3 generates a trader's goods, from the guid and the rest's number,
-- at the lower of its level and the hero's WoW level: a high vendor in a low hero's town has nothing out of its time.
function M.Arrows(l, guid, level, heroLevel)
    local v = vendor(l, guid)
    if v.A then return v.A end
    local b = math.floor(M.BG3Level(math.min(level or M.MEDIAN_LEVEL, heroLevel or 60)))
    local seed = (l.R or 0) * 31 + 7
    for i = 1, #guid do seed = (seed * 131 + guid:byte(i)) % 2147483647 end
    local function roll(n)
        seed = (seed * 16807) % 2147483647
        return seed % n
    end
    local weight, per, list = weights(b), {}, M.ArrowChoices(b)
    for _, c in ipairs(list) do per[c[2]] = (per[c[2]] or 0) + 1 end
    local total = 0
    for cat in pairs(per) do total = total + weight[cat] end
    local out = {}
    for _ = 1, M.ArrowCount(b) do
        local x, pick = roll(total), nil
        for _, cat in ipairs({"Common", "Uncommon", "Rare", "Slaying"}) do
            if per[cat] and not pick then
                if x < weight[cat] then pick = cat else x = x - weight[cat] end
            end
        end
        local nth = roll(per[pick])
        for _, c in ipairs(list) do
            if c[2] == pick then
                if nth == 0 then out[c[1]] = (out[c[1]] or 0) + 1; break end
                nth = nth - 1
            end
        end
    end
    if b >= 9 then out.OBJ_ArrowOfSmokepowder = 2 end
    v.A = out
    l.N = l.N + 1
    return out
end
function M.SetArrows(l, guid, counts)
    vendor(l, guid).A = counts
    l.N = l.N + 1
end

-- A vendor's purse in BG3 gold (1 gold is 1 copper). A proposal: BG3's data says how many sales a trader can pay for, WoW's
-- how much a sale is worth at the vendor's level.
--   BG3 (Shared.pak TreasureTable.txt, GoldValues.lsx, Object.txt), at BG3 level b:
--     gold    = 12.5 piles (Gold_Trader_Normal 10 to 15, the commonest of BG3's traders) x (4 + b) (ST_Gold_Trader_Part)
--               x 6.5 coins (OBJ_GoldPile 4 to 9)
--     a sale  = 1.3 x 3.2 x BaseProgression(b) (Weapon_1, a plain weapon's value) / 2.5 (Balanced's price rule)
--     sales   = gold / sale: about 24 at b 1, 15 at b 6, 5 at b 12
--   WoW (vmangos item_template): the median sell price of white and green weapons and armour by required level.
--   purse = sales(b) x GEAR(level), b from the mod's level curve (Scaling.lua, cap 12), between whole levels by line.
local BASE = {10, 13, 16, 20, 25, 32, 45, 60, 80, 100, 120, 150}
local GEAR = {64, 64, 275, 620, 1133, 1714, 2601, 3885, 5100, 7345, 10500, 13156, 30469} -- WoW levels 0, 5, 10 ... 60
M.MEDIAN_LEVEL = 30 -- of vendors in vmangos' world, when a server older than patch 0021 gives none

function M.BG3Level(level)
    return math.max(1, math.min(12, 1 + math.max(0, level - 4) * 11 / 56))
end
local function salesAt(b)
    return 12.5 * (4 + b) * 6.5 / (1.3 * 3.2 * BASE[b] / 2.5)
end
function M.Sales(b)
    local i = math.floor(b)
    local x, y = salesAt(i), salesAt(math.min(i + 1, 12))
    return x + (y - x) * (b - i)
end
function M.Gear(level)
    local x = math.max(0, math.min(60, level)) / 5
    local i = math.floor(x)
    local a, c = GEAR[i + 1], GEAR[math.min(i + 2, #GEAR)]
    return a + (c - a) * (x - i)
end
function M.Purse(level)
    level = math.max(5, level or M.MEDIAN_LEVEL) -- required levels under 5 are too few to say anything of their own
    return math.floor(M.Sales(M.BG3Level(level)) * M.Gear(level) / 100 + 0.5) * 100
end
-- What this vendor holds now: its purse from the last long rest as it was last seen, or a full one.
function M.PurseOf(l, guid, level)
    local v = l.V["g" .. guid]
    return (v and v.P) or M.Purse(level)
end
function M.SetPurse(l, guid, gold)
    local v = vendor(l, guid)
    if v.P ~= gold then
        v.P = gold
        l.N = l.N + 1
    end
end

return M
