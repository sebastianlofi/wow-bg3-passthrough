-- Prices.lua: which BG3 value makes BG3's trade screen show WoW's price. No game calls: tools/test_prices.py
-- runs this. BG3's rule, read from its code (bg3 4.8.400's trade price function) and data:
--   P     = max(1, TRADE_PRICE_MODIFIER - PriceBarterCoefficient * persuasion - PriceAttitudeCoefficient * attitude)
--   buy   = max(1, round(value * P)) each, sell = max(1, round(value / P)) each, a value of 0 is a price of 0
--   a stack costs its amount times the price of one. round: halves go up.
-- The value is eoc::ValueComponent's integer, per item, so what a merchant holds and what the party holds
-- can differ. TRADE_PRICE_MODIFIER is the ruleset's (Explorer 2, Balanced 2.5, Tactician and Honour 3, Custom 1 to 4);
-- the coefficients are Data.txt's. "No discount" on the screen is P equal to that modifier.
local M = {}

M.DEFAULT_BASE = 2.5 -- TRADE_PRICE_MODIFIER's default, RulesetModifiers.lsx
M.BARTER, M.ATTITUDE = 0.1, 0.005 -- Data.txt

function M.Round(x) return math.floor(x + 0.5) end

function M.Multiplier(base, persuasion, attitude)
    return math.max(1, base - M.BARTER * (persuasion or 0) - M.ATTITUDE * (attitude or 0))
end

-- What BG3 shows for one item of this value: bought from a merchant, or sold to one.
function M.Bought(value, p)
    if value == 0 then return 0 end
    return math.max(1, M.Round(value * p))
end
function M.Sold(value, p)
    if value == 0 then return 0 end
    return math.max(1, M.Round(value / p))
end

-- The value of a ware: the closest BG3 can show to WoW's buy price (the value is whole, so at 2.5 the
-- screen can only show 3, 5, 8, 10, 13...), the dearer when two are as close. bp 0 stays 0.
function M.Ware(bp, base)
    if (bp or 0) <= 0 then return 0 end
    local mid, best, bestOff = M.Round(bp / base), nil, nil
    for v = math.max(1, mid - 1), mid + 1 do
        local off = math.abs(M.Bought(v, base) - bp)
        if not best or off <= bestOff then best, bestOff = v, off end
    end
    return best
end

-- The value of what the party holds: BG3 shows WoW's sell price exactly. No sell price, no money for it;
-- an item with no buy price stays at 0, as it always was.
function M.Held(sp, bp, base)
    if (sp or 0) <= 0 or (bp or 0) <= 0 then return 0 end
    return math.max(1, M.Round(sp * base))
end

return M
