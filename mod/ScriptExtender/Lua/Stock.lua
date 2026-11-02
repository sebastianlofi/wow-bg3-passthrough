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
-- the commonest fixed count of potions and kits there). Ammunition has a gear row and WoW sells it by the 200: one purchase.
M.CONSUMABLES = 3

function M.New() return {N = 0, V = {}} end
-- Keys start with a letter so they stay strings through JSON.
local function key(e) return "e" .. tostring(math.tointeger(e) or e) end
local function vendor(l, guid)
    local v = l.V["g" .. guid]
    if not v then v = {B = {}}; l.V["g" .. guid] = v end
    return v
end

function M.Rest(l) l.V = {}; l.N = l.N + 1 end
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
        else
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
